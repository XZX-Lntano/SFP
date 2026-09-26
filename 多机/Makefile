CC ?= cc
CFLAGS ?= -O3 -march=native -std=c11 -Wall -Wextra -Werror
DPDK_CFLAGS := $(shell pkg-config --cflags libdpdk)
DPDK_LIBS := $(shell pkg-config --libs libdpdk)

all: bench_random

bench_random: bench_random.c sfp_lib.c sfp_lib.h bridge_protocol.h
	$(CC) $(DPDK_CFLAGS) $(CFLAGS) -pthread bench_random.c sfp_lib.c -o $@ $(DPDK_LIBS) -pthread

clean:
	rm -f bench_random

.PHONY: all clean
