CC = gcc
# _GNU_SOURCE: -std=c11 is strict ISO C and hides POSIX (sigaction, setitimer,
# swapcontext); this exposes them without dropping to -std=gnu11.
CFLAGS = -std=c11 -D_GNU_SOURCE -Wall -Wextra -g -O2 -Iinclude
LDFLAGS = -pthread

# Default backend: the hand-written x86-64 switch. The ucontext build exists
# alongside it so the benchmark can measure one against the other.
LIB = src/gt.o src/sync.o src/switch_x86_64.o
LIB_UCTX = src/gt_uctx.o src/sync.o

BINS = bin/demo bin/preempt_demo bin/io_test bin/mt_test bin/sync_test bin/echo_server \
       bin/switch_bench bin/switch_bench_uctx bin/scaling_bench

.PHONY: all check bench clean

all: $(BINS)

bin:
	mkdir -p bin

# The ucontext build needs its own objects: the macro changes both the
# scheduler's backend and the label the benchmark prints.
src/gt_uctx.o: src/gt.c
	$(CC) $(CFLAGS) -DGT_UCONTEXT_SWITCH -c -o $@ $<

bench/switch_bench_uctx.o: bench/switch_bench.c
	$(CC) $(CFLAGS) -DGT_UCONTEXT_SWITCH -c -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.S
	$(CC) $(CFLAGS) -c -o $@ $<

bin/demo: bin $(LIB) tests/demo.o
	$(CC) $(CFLAGS) -o $@ $(LIB) tests/demo.o $(LDFLAGS)

bin/preempt_demo: bin $(LIB) tests/preempt_demo.o
	$(CC) $(CFLAGS) -o $@ $(LIB) tests/preempt_demo.o $(LDFLAGS)

bin/io_test: bin $(LIB) tests/io_test.o
	$(CC) $(CFLAGS) -o $@ $(LIB) tests/io_test.o $(LDFLAGS)

bin/mt_test: bin $(LIB) tests/mt_test.o
	$(CC) $(CFLAGS) -o $@ $(LIB) tests/mt_test.o $(LDFLAGS)

bin/sync_test: bin $(LIB) tests/sync_test.o
	$(CC) $(CFLAGS) -o $@ $(LIB) tests/sync_test.o $(LDFLAGS)

bin/echo_server: bin $(LIB) tests/echo_server.o
	$(CC) $(CFLAGS) -o $@ $(LIB) tests/echo_server.o $(LDFLAGS)

bin/scaling_bench: bin $(LIB) bench/scaling_bench.o
	$(CC) $(CFLAGS) -o $@ $(LIB) bench/scaling_bench.o $(LDFLAGS)

bin/switch_bench: bin $(LIB) bench/switch_bench.o
	$(CC) $(CFLAGS) -o $@ $(LIB) bench/switch_bench.o $(LDFLAGS)

bin/switch_bench_uctx: bin $(LIB_UCTX) bench/switch_bench_uctx.o
	$(CC) $(CFLAGS) -o $@ $(LIB_UCTX) bench/switch_bench_uctx.o $(LDFLAGS)

check: all
	./bin/demo
	./bin/preempt_demo
	./bin/io_test
	./bin/mt_test 1
	./bin/mt_test 4
	./bin/sync_test 1
	./bin/sync_test 4

bench: bin/switch_bench bin/switch_bench_uctx
	@echo "=== ucontext (swapcontext) ==="
	@./bin/switch_bench_uctx
	@echo
	@echo "=== hand-written x86-64 switch ==="
	@./bin/switch_bench

clean:
	rm -f src/*.o tests/*.o bench/*.o
	rm -rf bin
