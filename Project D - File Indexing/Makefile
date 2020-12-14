CC=gcc
CFLAGS= -std=gnu99 -Wall
LDLIBS = -lpthread -lm

all: mole
main: mole.c
	gcc -o mole mole.c -lpthread -lm
	
.PHONY: clean all
clean:
	rm mole temp