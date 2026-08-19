CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -g -Iinclude
LDFLAGS = -pthread

SRC = src/gt.c
OBJ = $(SRC:.c=.o)

.PHONY: all demo bench clean

all: demo

demo: bin $(OBJ) tests/demo.o
	$(CC) $(CFLAGS) -o bin/demo $(OBJ) tests/demo.o $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

bin:
	mkdir -p bin

clean:
	rm -f $(OBJ) tests/*.o
	rm -rf bin
