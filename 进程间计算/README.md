# MPI-FPGA bridge v4

This directory contains the index-free 2/3/4-worker aggregation bridge.  The
application protocol is still one 2580-byte UDP message per round.  The bridge
coalesces 1 to 16 outstanding application requests into one physical Jumbo
Frame per worker.  The v4 bridge and v4 FPGA bitstream must be used together;
they are intentionally incompatible with the old one-round v3 bitstream.

The full wire format is documented in `BATCH_PROTOCOL.md`.  A maximum batch is
8368 Ethernet bytes without FCS and fits in a 9000-byte MTU.

## Build

```bash
make clean
make
make dpdk
make test
```

`make test` round-trips both one-round and full 16-round physical frames
through the C serializer/parser and compiles both Python clients.

Both targets use `/usr/local/openmpi/bin/mpicc`.  The DPDK target is selected
with `MPI_FPGA_DPDK=1`; rank 0 owns all four X710 functions in their shared
IOMMU group, while all four ranks exchange their worker input and broadcast
result with non-blocking MPI collectives.

## Raw-socket functional run

Bind the four ports to `i40e`, then configure Jumbo Frames:

```bash
sudo ./setup_raw_jumbo.sh
sudo env MPI_FPGA_CPU_BASE=4 /usr/local/openmpi/bin/mpirun \
  --allow-run-as-root --bind-to none --tag-output -np 4 \
  ./mpi_fpga_bridge \
  10000 ff:ff:ff:ff:ff:ff \
  enp1s0f3np3 192.168.10.1 192.168.20.1 \
  enp1s0f2np2 192.168.10.2 192.168.20.2 \
  enp1s0f1np1 192.168.10.3 192.168.20.3 \
  enp1s0f0np0 192.168.10.4 192.168.20.4 \
  0x2345 4000 4001 4002 4003 5000
```

## DPDK run

After binding all four `0000:01:00.[0-3]` functions to `vfio-pci` and mounting
2 MiB hugepages, use the same arguments with the DPDK binary:

```bash
sudo env MPI_FPGA_DPDK=1 MPI_FPGA_CPU_BASE=4 \
  /usr/local/openmpi/bin/mpirun --allow-run-as-root --bind-to none \
  --tag-output -np 4 ./mpi_fpga_bridge_dpdk \
  10000 ff:ff:ff:ff:ff:ff \
  enp1s0f3np3 192.168.10.1 192.168.20.1 \
  enp1s0f2np2 192.168.10.2 192.168.20.2 \
  enp1s0f1np1 192.168.10.3 192.168.20.3 \
  enp1s0f0np0 192.168.10.4 192.168.20.4 \
  0x2345 4000 4001 4002 4003 5000
```

The DPDK backend configures every hardware port to MTU 9000 itself.

## Four-worker functional check

Run this after programming the matching v4 bitstream and starting either
bridge backend:

```bash
python3 bridge_app_client.py \
  --host 127.0.0.1 --port 10000 --request-id 4660 \
  --worker0-values 10 20 30 40 50 \
  --worker1-values 1 2 3 4 5 \
  --worker2-values 1 1 1 1 1 \
  --worker3-values 2 2 2 2 2
```

The expected result is `[14, 25, 36, 47, 58]`.  The same application format
also supports two or three workers; only the first `worker_count` physical
inputs are sent, while the FPGA result is still received on all four ports.

For a pipelined load test, keep at least 16 outstanding requests so the bridge
can fill physical batches:

```bash
python3 benchmark_bridge_bandwidth.py \
  --host 127.0.0.1 --port 10000 --workers 4 \
  --duration 60 --warmup 10 --window 256 --timeout 8 \
  --request-id-start 0x9000
```

## FPGA datapath and diagnostics

`axis_udp_batch_aggregator.v` has four independent one-value-per-cycle parsers,
four worker BRAM banks, a three-stage pairwise adder, result BRAM, and 16 live
round slots.  Output is broadcast through the existing four per-port TX async
FIFOs.  Packet/byte, invalid-frame, slot-collision, FIFO-overflow, output
backpressure, and output packet/byte counters carry `MARK_DEBUG`.

After `synth_1`, run `insert_batch_ila.tcl` before implementation to attach all
of those counters to a 1024-sample ILA.

## FPGA build and verification

From `corundum/fpga/mqnic/ZCU102/fpga/fpga`:

```bash
/home/antl/vivado/2025.2/Vivado/bin/vivado -mode batch -source run_synth.tcl
/home/antl/vivado/2025.2/Vivado/bin/vivado -mode batch -source report_batch_post_synth.tcl
/home/antl/vivado/2025.2/Vivado/bin/vivado -mode batch -source insert_batch_ila.tcl
/home/antl/vivado/2025.2/Vivado/bin/vivado -mode batch -source run_impl.tcl
/home/antl/vivado/2025.2/Vivado/bin/vivado -mode batch -source generate_bit.tcl
```

The generated programming and debug files are:

- `fpga.runs/impl_1/fpga.bit`
- `fpga.runs/impl_1/fpga.bin`
- `fpga.runs/impl_1/fpga.ltx`
- `fpga.xsa`

The standalone SystemVerilog testbench is in
`corundum/fpga/mqnic/ZCU102/fpga/tb/test_axis_udp_batch_aggregator.sv`.  Vivado
successfully compiles and elaborates it.  On this development host the XSim
runtime itself exits with an internal Tcl exception, including for a trivial
smoke test, so dynamic XSim execution is not counted as a passing test.  The C
serializer/parser test, complete FPGA RTL elaboration, synthesis, DRC, routing,
and post-route timing are the required local gates before programming hardware.

The verified Vivado 2025.2 build uses 10 `RAMB36E2` primitives for the five
aggregation memories (four worker banks plus one result bank).  The optional
22×64-bit, 1024-sample ILA uses another 39 `RAMB36E2` and two `RAMB18E2`.
With the ILA present, the complete design uses 379.5 of 912 BRAM tiles
(41.61%).  Final routing has no failed nets and meets timing at 300 MHz with
setup WNS `+0.042 ns` and hold slack `+0.010 ns`.
