CC=gcc
CFLAGS= -std=gnu99 -Wall
LDLIBS = -lpthread -lm -lncurses

all: tsnake 
mole: tsnake.c
	gcc -o tsnake tsnake.c -lpthread -lm -lncurses
	
.PHONY: clean all
clean:
	rm tsnake