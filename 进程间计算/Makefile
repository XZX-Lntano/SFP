CC = cc
MPICC ?= mpicc
CFLAGS ?= -O3 -march=native -std=c11 -Wall -Wextra -Werror
DPDK_CFLAGS := $(shell pkg-config --cflags libdpdk)
DPDK_LIBS := $(shell pkg-config --libs libdpdk)

all: mpi_fpga_bridge benchmark_bridge_native benchmark_dpdk

mpi_fpga_bridge: mpi_fpga_bridge.c dpdk_port.c dpdk_port.h bridge_protocol.h
	$(MPICC) $(DPDK_CFLAGS) $(CFLAGS) mpi_fpga_bridge.c dpdk_port.c -o $@ $(DPDK_LIBS)

benchmark_bridge_native: benchmark_bridge_native.c bridge_protocol.h
	$(CC) $(CFLAGS) $< -o $@

benchmark_dpdk: benchmark_dpdk.c dpdk_port.c dpdk_port.h bridge_protocol.h
	$(MPICC) $(DPDK_CFLAGS) $(CFLAGS) benchmark_dpdk.c dpdk_port.c -o $@ $(DPDK_LIBS)

check:
	python3 pre_synth_equivalence_check.py

.PHONY: all check
