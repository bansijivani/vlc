SRC=$(wildcard *.c)

test: $(SRC)
	-gcc -o $@ $^
