MPICC ?= /usr/local/openmpi/bin/mpicc
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=gnu11
DPDK_FLAGS := $(shell pkg-config --cflags --libs libdpdk)

.PHONY: all dpdk clean test

all: mpi_fpga_bridge

mpi_fpga_bridge: mpi_fpga_bridge.c
	$(MPICC) $(CFLAGS) -o $@ $<

dpdk: mpi_fpga_bridge.c
	$(MPICC) $(CFLAGS) -DUSE_DPDK -o mpi_fpga_bridge_dpdk $< $(DPDK_FLAGS)

clean:
	rm -f mpi_fpga_bridge mpi_fpga_bridge_dpdk test_batch_protocol

test_batch_protocol: test_batch_protocol.c mpi_fpga_bridge.c
	$(MPICC) $(CFLAGS) -o $@ test_batch_protocol.c

test: all dpdk test_batch_protocol
	./test_batch_protocol
	python3 -m py_compile bridge_app_client.py benchmark_bridge_bandwidth.py
