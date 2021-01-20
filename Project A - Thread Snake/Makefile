CC=gcc
CFLAGS= -std=gnu99 -Wall
LDLIBS = -lpthread -lm

all: tsnake 
mole: tsnake.c
	gcc -o tsnake tsnake.c -lpthread -lm
	
.PHONY: clean all
clean:
	rm tsnake