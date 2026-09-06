CC = gcc
# _GNU_SOURCE: -std=c11 is strict ISO C and hides POSIX (sigaction, setitimer,
# swapcontext); this exposes them without dropping to -std=gnu11.
CFLAGS = -std=c11 -D_GNU_SOURCE -Wall -Wextra -g -Iinclude
LDFLAGS = -pthread

SRC = src/gt.c
OBJ = $(SRC:.c=.o)

.PHONY: all demo preempt_demo io_test echo_server check bench clean

all: demo preempt_demo io_test echo_server

demo: bin $(OBJ) tests/demo.o
	$(CC) $(CFLAGS) -o bin/demo $(OBJ) tests/demo.o $(LDFLAGS)

preempt_demo: bin $(OBJ) tests/preempt_demo.o
	$(CC) $(CFLAGS) -o bin/preempt_demo $(OBJ) tests/preempt_demo.o $(LDFLAGS)

io_test: bin $(OBJ) tests/io_test.o
	$(CC) $(CFLAGS) -o bin/io_test $(OBJ) tests/io_test.o $(LDFLAGS)

echo_server: bin $(OBJ) tests/echo_server.o
	$(CC) $(CFLAGS) -o bin/echo_server $(OBJ) tests/echo_server.o $(LDFLAGS)

check: all
	./bin/demo
	./bin/preempt_demo
	./bin/io_test

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

bin:
	mkdir -p bin

clean:
	rm -f $(OBJ) tests/*.o
	rm -rf bin
