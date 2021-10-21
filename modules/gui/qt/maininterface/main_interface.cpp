/*****************************************************************************
 * main_interface.cpp : Main interface
 ****************************************************************************
 * Copyright (C) 2006-2011 VideoLAN and AUTHORS
 *
 * Authors: Clément Stenac <zorglub@videolan.org>
 *          Jean-Baptiste Kempf <jb@videolan.org>
 *          Ilkka Ollakka <ileoo@videolan.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "qt.hpp"

#include "maininterface/main_interface.hpp"
#include "compositor.hpp"
#include "player/player_controller.hpp"                    // Creation
#include "util/renderer_manager.hpp"

#include "widgets/native/customwidgets.hpp"               // qtEventToVLCKey, QVLCStackedWidget
#include "util/qt_dirs.hpp"                     // toNativeSeparators
#include "util/imagehelper.hpp"
#include "util/color_scheme_model.hpp"

#include "widgets/native/interface_widgets.hpp"     // bgWidget, videoWidget

#include "playlist/playlist_model.hpp"
#include "playlist/playlist_controller.hpp"
#include <vlc_playlist.h>

#include "videosurface.hpp"

#include "menus/menus.hpp"                            // Menu creation

#include "vlc_media_library.h"

#include "dialogs/toolbar/controlbar_profile_model.hpp"

#include <QCloseEvent>
#include <QKeyEvent>

#include <QUrl>
#include <QSize>
#include <QDate>
#include <QMimeData>

#include <QWindow>
#include <QMenuBar>
#include <QStatusBar>
#include <QLabel>
#include <QStackedWidget>
#include <QScreen>
#include <QStackedLayout>
#ifdef _WIN32
#include <QFileInfo>
#endif


#include <QtGlobal>
#include <QTimer>

#include <vlc_actions.h>                    /* Wheel event */
#include <vlc_vout_window.h>                /* VOUT_ events */

#define VLC_REFERENCE_SCALE_FACTOR 96.

using  namespace vlc::playlist;

// #define DEBUG_INTF

/* Callback prototypes */
static int PopupMenuCB( vlc_object_t *p_this, const char *psz_variable,
                        vlc_value_t old_val, vlc_value_t new_val, void *param );
static int IntfShowCB( vlc_object_t *p_this, const char *psz_variable,
                       vlc_value_t old_val, vlc_value_t new_val, void *param );
static int IntfBossCB( vlc_object_t *p_this, const char *psz_variable,
                       vlc_value_t old_val, vlc_value_t new_val, void *param );
static int IntfRaiseMainCB( vlc_object_t *p_this, const char *psz_variable,
                           vlc_value_t old_val, vlc_value_t new_val,
                           void *param );

const QEvent::Type MainInterface::ToolbarsNeedRebuild =
        (QEvent::Type)QEvent::registerEventType();

namespace
{

template <typename T>
T loadVLCOption(vlc_object_t *obj, const char *name);

template <>
float loadVLCOption<float>(vlc_object_t *obj, const char *name)
{
    return var_InheritFloat(obj, name);
}

template <>
int loadVLCOption<int>(vlc_object_t *obj, const char *name)
{
    return var_InheritInteger(obj, name);
}

template <>
bool loadVLCOption<bool>(vlc_object_t *obj, const char *name)
{
    return var_InheritBool(obj, name);
}

}

MainInterface::MainInterface(qt_intf_t *_p_intf)
    : p_intf(_p_intf)
{
    /**
     *  Configuration and settings
     *  Pre-building of interface
     **/

    settings = getSettings();
    m_colorScheme = new ColorSchemeModel(this);

    loadPrefs(false);
    loadFromSettingsImpl(false);

    /* Get the available interfaces */
    m_extraInterfaces = new VLCVarChoiceModel(VLC_OBJECT(p_intf->intf), "intf-add", this);

    vlc_medialibrary_t* ml = vlc_ml_instance_get( p_intf );
    b_hasMedialibrary = (ml != NULL);
    if (b_hasMedialibrary) {
        m_medialib = new MediaLib(p_intf, this);
    }

    /* Controlbar Profile Model Creation */
    m_controlbarProfileModel = new ControlbarProfileModel(p_intf, this);

    m_dialogFilepath = getSettings()->value( "filedialog-path", QVLCUserDir( VLC_HOME_DIR ) ).toString();

    QString platformName = QGuiApplication::platformName();

#ifdef QT5_HAS_WAYLAND
    b_hasWayland = platformName.startsWith(QLatin1String("wayland"), Qt::CaseInsensitive);
#endif

    /*********************************
     * Create the Systray Management *
     *********************************/
    //postpone systray initialisation to speedup starting time
    QMetaObject::invokeMethod(this, &MainInterface::initSystray, Qt::QueuedConnection);

    /*************************************************************
     * Connect the input manager to the GUI elements it manages  *
     * Beware initSystray did some connects on input manager too *
     *************************************************************/
    /**
     * Connects on nameChanged()
     * Those connects are different because options can impeach them to trigger.
     **/
    /* Main Interface statusbar */
    /* and title of the Main Interface*/
    connect( THEMIM, &PlayerController::inputChanged, this, &MainInterface::onInputChanged );

    /* END CONNECTS ON IM */

    /* VideoWidget connects for asynchronous calls */
    connect( this, &MainInterface::askToQuit, THEDP, &DialogsProvider::quit, Qt::QueuedConnection  );

    connect(this, &MainInterface::interfaceFullScreenChanged, this, &MainInterface::useClientSideDecorationChanged);

    /** END of CONNECTS**/


    /************
     * Callbacks
     ************/
    libvlc_int_t* libvlc = vlc_object_instance(p_intf);
    var_AddCallback( libvlc, "intf-toggle-fscontrol", IntfShowCB, p_intf );
    var_AddCallback( libvlc, "intf-boss", IntfBossCB, p_intf );
    var_AddCallback( libvlc, "intf-show", IntfRaiseMainCB, p_intf );

    /* Register callback for the intf-popupmenu variable */
    var_AddCallback( libvlc, "intf-popupmenu", PopupMenuCB, p_intf );

    if( config_GetInt("qt-privacy-ask") )
    {
        //postpone dialog call, as composition might not be ready yet
        QMetaObject::invokeMethod(this, [this](){
            THEDP->firstRunDialog();
        }, Qt::QueuedConnection);
    }
}

MainInterface::~MainInterface()
{
    RendererManager::killInstance();

    /* Save states */

    settings->beginGroup("MainWindow");
    settings->setValue( "pl-dock-status", b_playlistDocked );
    settings->setValue( "ShowRemainingTime", m_showRemainingTime );
    settings->setValue( "interface-scale", m_intfUserScaleFactor );
    settings->setValue( "pin-video-controls", m_pinVideoControls );

    /* Save playlist state */
    settings->setValue( "playlist-visible", playlistVisible );
    settings->setValue( "playlist-width-factor", playlistWidthFactor);

    settings->setValue( "grid-view", m_gridView );
    settings->setValue( "color-scheme", m_colorScheme->currentScheme() );
    /* Save the stackCentralW sizes */
    settings->endGroup();

    if( var_InheritBool( p_intf, "qt-recentplay" ) )
        getSettings()->setValue( "filedialog-path", m_dialogFilepath );
    else
        getSettings()->remove( "filedialog-path" );

    /* Unregister callbacks */
    libvlc_int_t* libvlc = vlc_object_instance(p_intf);
    var_DelCallback( libvlc, "intf-boss", IntfBossCB, p_intf );
    var_DelCallback( libvlc, "intf-show", IntfRaiseMainCB, p_intf );
    var_DelCallback( libvlc, "intf-toggle-fscontrol", IntfShowCB, p_intf );
    var_DelCallback( libvlc, "intf-popupmenu", PopupMenuCB, p_intf );

    p_intf->p_mi = NULL;
}

bool MainInterface::hasVLM() const {
#ifdef ENABLE_VLM
    return true;
#else
    return false;
#endif
}

bool MainInterface::useClientSideDecoration() const
{
    //don't show CSD when interface is fullscreen
    return m_clientSideDecoration && m_windowVisibility != QWindow::FullScreen;
}

bool MainInterface::hasFirstrun() const {
    return config_GetInt( "qt-privacy-ask" );
}

/*****************************
 *   Main UI handling        *
 *****************************/

void MainInterface::loadPrefs(const bool callSignals)
{
    const auto loadFromVLCOption = [this, callSignals](auto &variable, const char *name
            , const std::function<void(MainInterface *)> signal)
    {
        using variableType = std::remove_reference_t<decltype(variable)>;

        const auto value =  loadVLCOption<variableType>(VLC_OBJECT(p_intf), name);
        if (value == variable)
            return;

        variable = value;
        if (callSignals && signal)
            signal(this);
    };

    /* Are we in the enhanced always-video mode or not ? */
    loadFromVLCOption(b_minimalView, "qt-minimal-view", nullptr);

    /* Do we want anoying popups or not */
    loadFromVLCOption(i_notificationSetting, "qt-notification", nullptr);

    /* Should the UI stays on top of other windows */
    loadFromVLCOption(b_interfaceOnTop, "video-on-top", [this](MainInterface *)
    {
        emit interfaceAlwaysOnTopChanged(b_interfaceOnTop);
    });

    loadFromVLCOption(m_hasToolbarMenu, "qt-menubar", &MainInterface::hasToolbarMenuChanged);

#if QT_CLIENT_SIDE_DECORATION_AVAILABLE
    loadFromVLCOption(m_clientSideDecoration, "qt-titlebar" , &MainInterface::useClientSideDecorationChanged);
#endif
}

void MainInterface::loadFromSettingsImpl(const bool callSignals)
{
    const auto loadFromSettings = [this, callSignals](auto &variable, const char *name
            , const auto defaultValue, auto signal)
    {
        using variableType = std::remove_reference_t<decltype(variable)>;

        const auto value = getSettings()->value(name, defaultValue).template value<variableType>();
        if (value == variable)
            return;

        variable = value;
        if (callSignals && signal)
            (this->*signal)(variable);
    };

    loadFromSettings(b_playlistDocked, "MainWindow/pl-dock-status", true, &MainInterface::playlistDockedChanged);

    loadFromSettings(playlistVisible, "MainWindow/playlist-visible", false, &MainInterface::playlistVisibleChanged);

    loadFromSettings(playlistWidthFactor, "MainWindow/playlist-width-factor", 4.0 , &MainInterface::playlistWidthFactorChanged);

    loadFromSettings(m_gridView, "MainWindow/grid-view", true, &MainInterface::gridViewChanged);

    loadFromSettings(m_showRemainingTime, "MainWindow/ShowRemainingTime", false, &MainInterface::showRemainingTimeChanged);

    loadFromSettings(m_pinVideoControls, "MainWindow/pin-video-controls", false, &MainInterface::pinVideoControlsChanged);

    const auto colorScheme = static_cast<ColorSchemeModel::ColorScheme>(getSettings()->value( "MainWindow/color-scheme", ColorSchemeModel::System ).toInt());
    if (m_colorScheme->currentScheme() != colorScheme)
        m_colorScheme->setCurrentScheme(colorScheme);

    /* user interface scale factor */
    auto userIntfScaleFactor = var_InheritFloat(p_intf, "qt-interface-scale");
    if (userIntfScaleFactor == -1)
        userIntfScaleFactor = getSettings()->value( "MainWindow/interface-scale", 1.0).toDouble();
    if (m_intfUserScaleFactor != userIntfScaleFactor)
    {
        m_intfUserScaleFactor = userIntfScaleFactor;
        updateIntfScaleFactor();
    }
}

void MainInterface::reloadPrefs()
{
    loadPrefs(true);
}

void MainInterface::onInputChanged( bool hasInput )
{
    if( hasInput == false )
        return;
    int autoRaise = var_InheritInteger( p_intf, "qt-auto-raise" );
    if ( autoRaise == MainInterface::RAISE_NEVER )
        return;
    if( THEMIM->hasVideoOutput() == true )
    {
        if( ( autoRaise & MainInterface::RAISE_VIDEO ) == 0 )
            return;
    }
    else if ( ( autoRaise & MainInterface::RAISE_AUDIO ) == 0 )
        return;
    emit askRaise();
}

#ifdef KeyPress
#undef KeyPress
#endif
void MainInterface::sendHotkey(Qt::Key key , Qt::KeyboardModifiers modifiers)
{
    QKeyEvent event(QEvent::KeyPress, key, modifiers );
    int vlckey = qtEventToVLCKey(&event);
    var_SetInteger(vlc_object_instance(p_intf), "key-pressed", vlckey);
}

void MainInterface::updateIntfScaleFactor()
{
    m_intfScaleFactor = m_intfUserScaleFactor;
    if (QWindow* window = p_intf->p_compositor ? p_intf->p_compositor->interfaceMainWindow() : nullptr)
    {
        QScreen* screen = window->screen();
        if (screen)
        {
            qreal dpi = screen->logicalDotsPerInch();
            m_intfScaleFactor = m_intfUserScaleFactor * dpi / VLC_REFERENCE_SCALE_FACTOR;
        }
    }
    emit intfScaleFactorChanged();
}

void MainInterface::onWindowVisibilityChanged(QWindow::Visibility visibility)
{
    m_windowVisibility = visibility;
}

void MainInterface::setUseAcrylicBackground(const bool v)
{
    if (m_useAcrylicBackground == v)
        return;

    m_useAcrylicBackground = v;
    emit useAcrylicBackgroundChanged();
}

void MainInterface::setHasAcrylicSurface(const bool v)
{
    if (m_hasAcrylicSurface == v)
        return;

    m_hasAcrylicSurface = v;
    emit hasAcrylicSurfaceChanged();
}

void MainInterface::incrementIntfUserScaleFactor(bool increment)
{
    if (increment)
        setIntfUserScaleFactor(m_intfUserScaleFactor + 0.1);
    else
        setIntfUserScaleFactor(m_intfUserScaleFactor - 0.1);
}

void MainInterface::setIntfUserScaleFactor(double newValue)
{
    m_intfUserScaleFactor = qBound(getMinIntfUserScaleFactor(), newValue, getMaxIntfUserScaleFactor());
    updateIntfScaleFactor();
}

void MainInterface::setPinVideoControls(bool pinVideoControls)
{
    if (m_pinVideoControls == pinVideoControls)
        return;

    m_pinVideoControls = pinVideoControls;
    emit pinVideoControlsChanged(m_pinVideoControls);
}

inline void MainInterface::initSystray()
{
    bool b_systrayAvailable = QSystemTrayIcon::isSystemTrayAvailable();
    bool b_systrayWanted = var_InheritBool( p_intf, "qt-system-tray" );

    if( var_InheritBool( p_intf, "qt-start-minimized") )
    {
        if( b_systrayAvailable )
        {
            b_systrayWanted = true;
            b_hideAfterCreation = true;
        }
        else
            msg_Err( p_intf, "cannot start minimized without system tray bar" );
    }

    if( b_systrayAvailable && b_systrayWanted )
        createSystray();
}


void MainInterface::setPlaylistDocked( bool docked )
{
    b_playlistDocked = docked;

    emit playlistDockedChanged(docked);
}

void MainInterface::setPlaylistVisible( bool visible )
{
    playlistVisible = visible;

    emit playlistVisibleChanged(visible);
}

void MainInterface::setPlaylistWidthFactor( double factor )
{
    if (factor > 0.0)
    {
        playlistWidthFactor = factor;
        emit playlistWidthFactorChanged(factor);
    }
}

void MainInterface::setShowRemainingTime( bool show )
{
    m_showRemainingTime = show;
    emit showRemainingTimeChanged(show);
}

void MainInterface::setGridView(bool asGrid)
{
    m_gridView = asGrid;
    emit gridViewChanged( asGrid );
}

void MainInterface::setInterfaceAlwaysOnTop( bool on_top )
{
    b_interfaceOnTop = on_top;
    emit interfaceAlwaysOnTopChanged(on_top);
}

bool MainInterface::hasEmbededVideo() const
{
    return m_videoSurfaceProvider && m_videoSurfaceProvider->hasVideoEmbed();
}

void MainInterface::setVideoSurfaceProvider(VideoSurfaceProvider* videoSurfaceProvider)
{
    if (m_videoSurfaceProvider)
        disconnect(m_videoSurfaceProvider, &VideoSurfaceProvider::hasVideoEmbedChanged, this, &MainInterface::hasEmbededVideoChanged);
    m_videoSurfaceProvider = videoSurfaceProvider;
    if (m_videoSurfaceProvider)
        connect(m_videoSurfaceProvider, &VideoSurfaceProvider::hasVideoEmbedChanged,
                this, &MainInterface::hasEmbededVideoChanged,
                Qt::QueuedConnection);
    emit hasEmbededVideoChanged(m_videoSurfaceProvider && m_videoSurfaceProvider->hasVideoEmbed());
}

VideoSurfaceProvider* MainInterface::getVideoSurfaceProvider() const
{
    return m_videoSurfaceProvider;
}

/*****************************************************************************
 * Systray Icon and Systray Menu
 *****************************************************************************/
/**
 * Create a SystemTray icon and a menu that would go with it.
 * Connects to a click handler on the icon.
 **/
void MainInterface::createSystray()
{
    QIcon iconVLC;
    if( QDate::currentDate().dayOfYear() >= QT_XMAS_JOKE_DAY && var_InheritBool( p_intf, "qt-icon-change" ) )
        iconVLC = QIcon::fromTheme( "vlc-xmas", QIcon( ":/logo/vlc128-xmas.png" ) );
    else
        iconVLC = QIcon::fromTheme( "vlc", QIcon( ":/logo/vlc256.png" ) );
    sysTray = new QSystemTrayIcon( iconVLC, this );
    sysTray->setToolTip( qtr( "VLC media player" ));

    systrayMenu.reset(new QMenu( qtr( "VLC media player") ));
    systrayMenu->setIcon( iconVLC );

    VLCMenuBar::updateSystrayMenu( this, p_intf, true );
    sysTray->show();

    connect( sysTray, &QSystemTrayIcon::activated,
             this, &MainInterface::handleSystrayClick );

    /* Connects on nameChanged() */
    connect( THEMIM, &PlayerController::nameChanged,
             this, &MainInterface::updateSystrayTooltipName );
    /* Connect PLAY_STATUS on the systray */
    connect( THEMIM, &PlayerController::playingStateChanged,
             this, &MainInterface::updateSystrayTooltipStatus );
}

/**
 * Updates the Systray Icon's menu and toggle the main interface
 */
void MainInterface::toggleUpdateSystrayMenu()
{
    emit toggleWindowVisibility();
    if( sysTray )
        VLCMenuBar::updateSystrayMenu( this, p_intf );
}

/* First Item of the systray menu */
void MainInterface::showUpdateSystrayMenu()
{
    emit setInterfaceVisibible(true);
    VLCMenuBar::updateSystrayMenu( this, p_intf );
}

/* First Item of the systray menu */
void MainInterface::hideUpdateSystrayMenu()
{
    emit setInterfaceVisibible(false);
    VLCMenuBar::updateSystrayMenu( this, p_intf );
}

/* Click on systray Icon */
void MainInterface::handleSystrayClick(
                                    QSystemTrayIcon::ActivationReason reason )
{
    switch( reason )
    {
        case QSystemTrayIcon::Trigger:
        case QSystemTrayIcon::DoubleClick:
#ifdef Q_OS_MAC
            VLCMenuBar::updateSystrayMenu( this, p_intf );
#else
            toggleUpdateSystrayMenu();
#endif
            break;
        case QSystemTrayIcon::MiddleClick:
            sysTray->showMessage( qtr( "VLC media player" ),
                    qtr( "Control menu for the player" ),
                    QSystemTrayIcon::Information, 3000 );
            break;
        default:
            break;
    }
}

/**
 * Updates the name of the systray Icon tooltip.
 * Doesn't check if the systray exists, check before you call it.
 **/
void MainInterface::updateSystrayTooltipName( const QString& name )
{
    if( name.isEmpty() )
    {
        sysTray->setToolTip( qtr( "VLC media player" ) );
    }
    else
    {
        sysTray->setToolTip( name );
        if( ( i_notificationSetting == NOTIFICATION_ALWAYS ) ||
            ( i_notificationSetting == NOTIFICATION_MINIMIZED && (m_windowVisibility == QWindow::Hidden || m_windowVisibility == QWindow::Minimized)))
        {
            sysTray->showMessage( qtr( "VLC media player" ), name,
                    QSystemTrayIcon::NoIcon, 3000 );
        }
    }

    VLCMenuBar::updateSystrayMenu( this, p_intf );
}

/**
 * Updates the status of the systray Icon tooltip.
 * Doesn't check if the systray exists, check before you call it.
 **/
void MainInterface::updateSystrayTooltipStatus( PlayerController::PlayingState )
{
    VLCMenuBar::updateSystrayMenu( this, p_intf );
}


/************************************************************************
 * D&D Events
 ************************************************************************/

/**
 * dropEventPlay
 *
 * Event called if something is dropped onto a VLC window
 * \param event the event in question
 * \param b_play whether to play the file immediately
 * \return nothing
 */
void MainInterface::dropEventPlay( QDropEvent *event, bool b_play )
{
    if( event->possibleActions() & ( Qt::CopyAction | Qt::MoveAction | Qt::LinkAction ) )
       event->setDropAction( Qt::CopyAction );
    else
        return;

    const QMimeData *mimeData = event->mimeData();

    /* D&D of a subtitles file, add it on the fly */
    if( mimeData->urls().count() == 1 && THEMIM->hasInput() )
    {
        if( !THEMIM->AddAssociatedMedia(SPU_ES, mimeData->urls()[0].toString(), true, true, true) )
        {
            event->accept();
            return;
        }
    }

    QVector<vlc::playlist::Media> medias;
    for( const QUrl &url: mimeData->urls() )
    {
        if( url.isValid() )
        {
            QString mrl = toURI( url.toEncoded().constData() );
#ifdef _WIN32
            QFileInfo info( url.toLocalFile() );
            if( info.exists() && info.isSymLink() )
            {
                QString target = info.symLinkTarget();
                QUrl url;
                if( QFile::exists( target ) )
                {
                    url = QUrl::fromLocalFile( target );
                }
                else
                {
                    url.setUrl( target );
                }
                mrl = toURI( url.toEncoded().constData() );
            }
#endif
            if( mrl.length() > 0 )
                medias.push_back( vlc::playlist::Media{ mrl, QString {} });
        }
    }

    /* Browsers give content as text if you dnd the addressbar,
       so check if mimedata has valid url in text and use it
       if we didn't get any normal Urls()*/
    if( !mimeData->hasUrls() && mimeData->hasText() &&
        QUrl(mimeData->text()).isValid() )
    {
        QString mrl = toURI( mimeData->text() );
        medias.push_back( vlc::playlist::Media{ mrl, QString {} });
    }
    if (!medias.empty())
        THEMPL->append(medias, b_play);
    event->accept();
}

/************************************************************************
 * Events stuff
 ************************************************************************/

bool MainInterface::onWindowClose( QWindow* )
{
    PlaylistControllerModel* playlistController = p_intf->p_mainPlaylistController;
    PlayerController* playerController = p_intf->p_mainPlayerController;

    if (m_videoSurfaceProvider)
        m_videoSurfaceProvider->onWindowClosed();
    //We need to make sure that noting is playing anymore otherwise the vout will be closed
    //after the main interface, and it requires (at least with OpenGL) that the OpenGL context
    //from the main window is still valid.
    //vout_window_ReportClose is currently stubbed
    if (playerController && playerController->hasVideoOutput()) {
        connect(playerController, &PlayerController::playingStateChanged, [this](PlayerController::PlayingState state){
            if (state == PlayerController::PLAYING_STATE_STOPPED) {
                emit askToQuit();
            }
        });
        playlistController->stop();
        return false;
    }
    else
    {
        emit askToQuit(); /* ask THEDP to quit, so we have a unique method */
        return true;
    }
}

void MainInterface::toggleInterfaceFullScreen()
{
    emit setInterfaceFullScreen( m_windowVisibility != QWindow::FullScreen );
}

void MainInterface::emitBoss()
{
    emit askBoss();
}

void MainInterface::emitShow()
{
    emit askShow();
}

void MainInterface::emitRaise()
{
    emit askRaise();
}

VLCVarChoiceModel* MainInterface::getExtraInterfaces()
{
    return m_extraInterfaces;
}

/*****************************************************************************
 * PopupMenuCB: callback triggered by the intf-popupmenu playlist variable.
 *  We don't show the menu directly here because we don't want the
 *  caller to block for a too long time.
 *****************************************************************************/
static int PopupMenuCB( vlc_object_t *, const char *,
                        vlc_value_t, vlc_value_t new_val, void *param )
{
    qt_intf_t *p_intf = (qt_intf_t *)param;

    if( p_intf->pf_show_dialog )
    {
        p_intf->pf_show_dialog( p_intf->intf, INTF_DIALOG_POPUPMENU,
                                new_val.b_bool, NULL );
    }

    return VLC_SUCCESS;
}

/*****************************************************************************
 * IntfShowCB: callback triggered by the intf-toggle-fscontrol libvlc variable.
 *****************************************************************************/
static int IntfShowCB( vlc_object_t *, const char *,
                       vlc_value_t, vlc_value_t, void *param )
{
    qt_intf_t *p_intf = (qt_intf_t *)param;
    p_intf->p_mi->emitShow();

    return VLC_SUCCESS;
}

/*****************************************************************************
 * IntfRaiseMainCB: callback triggered by the intf-show-main libvlc variable.
 *****************************************************************************/
static int IntfRaiseMainCB( vlc_object_t *, const char *,
                            vlc_value_t, vlc_value_t, void *param )
{
    qt_intf_t *p_intf = (qt_intf_t *)param;
    p_intf->p_mi->emitRaise();

    return VLC_SUCCESS;
}

/*****************************************************************************
 * IntfBossCB: callback triggered by the intf-boss libvlc variable.
 *****************************************************************************/
static int IntfBossCB( vlc_object_t *, const char *,
                       vlc_value_t, vlc_value_t, void *param )
{
    qt_intf_t *p_intf = (qt_intf_t *)param;
    p_intf->p_mi->emitBoss();

    return VLC_SUCCESS;
}
