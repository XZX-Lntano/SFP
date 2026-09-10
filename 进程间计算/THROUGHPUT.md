# ZCU102 aggregation v4

This version replaces the single-round stop-and-wait datapath. The FPGA bitstream,
bridge, client and benchmarks must all use v4. The old raw-socket source is retained
as `mpi_fpga_bridge_v3_legacy.c` for reference; it is not part of the default build.

## Hardware and capacity

The target is `xczu9eg-ffvb1156-2-e`. Vivado reports 912 BRAM36 tiles, 274080 LUTs
and 548160 flip-flops. The selected design has 256 round slots (16 batch slots,
each reserving 16 rounds), with four independently written BRAM banks. Each bank
stores 256 * 64 * 8 bytes. Four banks occupy 512 KiB of data storage.

The actual 256-slot aggregation module uses 114 BRAM tiles, 6759 LUTs and 7940
registers after OOC optimization. Its 300 MHz synthesis estimate has WNS +0.222 ns.
Full-board synthesis uses 457.5/912 BRAM tiles and 67274/274080 LUTs. The full
2026-09-07 implementation passed all specified timing constraints: setup WNS
+0.025 ns, hold WHS +0.010 ns, and zero failing endpoints. The sign-off report is
`../corundum/fpga/mqnic/ZCU102/fpga/fpga/fpga.runs/impl_1/fpga_timing_summary_routed.rpt`.
The implemented utilization is 66319 LUTs (24.20%), 68492 registers (12.49%) and
457.5 BRAM tiles (50.16%). The deployed binary SHA256 is
`ea9619684afb1487881027f245e4186ae9183373d06b9659620652c0f5e0dc6a`;
its hardware build timestamp is 2026-09-07 12:09:45 UTC.

At unchanged non-aggregation resource usage, 512 slots would need approximately
571.5 total BRAM tiles, and 1024 approximately 799.5. These larger configurations
are estimates, not synthesized or timing-validated configurations. 256 slots are
the implemented choice, leaving about half the device's BRAM available.

The RX paths parse one 64-bit beat per cycle independently. Synchronous BRAM reads
have an extra registered output stage; two registered pair sums feed a registered
final sum. An AXIS output register and the broadcast block distribute identical
frames to all four ports. Backpressure freezes the read/sum/output pipeline, while
the four ingress writers remain independent. There is no byte-at-a-time loop in
the FPGA datapath. `axis_fifo` DEPTH is measured in bytes with KEEP_ENABLE, not
64-bit beats. The existing 16384-byte frame FIFOs accommodate an 8368-byte frame.

All operands and results are unsigned 64-bit integers. Addition wraps modulo 2^64.
A round is identified by an explicit 32-bit ID. A batch starts on a multiple of 16
and contains sequential IDs. Batch slot = `(base_round >> 4) & 15`; values are
addressed by batch slot, round position and entry position. A committed worker
contribution is never overwritten by a duplicate. Only fully validated frames
become visible to the reduction scheduler. An incomplete batch expires after
30000000 cycles (100 ms at 300 MHz). The sender stops on timeout rather than
automatically reusing a possibly active slot. This is not a reliable transport or
an arbitrary sparse-ID hash table; applications must obey slot credits and must
not run independent generators against the same FPGA at the same time.

## Packet formats

All multibyte fields are network byte order. No VLAN or IPv4 options are supported.

FPGA Ethernet frame:

| Offset | Bytes | Field |
|---|---:|---|
| 0 | 14 | Ethernet II, IPv4 EtherType |
| 14 | 20 | IPv4, unfragmented, normal TTL 64 |
| 34 | 8 | UDP, destination 0x2345, checksum zero |
| 42 | 2 | Magic 0x4147 |
| 44 | 1 | Version 4 |
| 45 | 1 | Worker count, 2..4 |
| 46 | 1 | Round count, 1..16 |
| 47 | 1 | Message type, request 1 / response 2 |
| 48+520*r | 4 | Explicit round ID = base+r |
| 52+520*r | 4 | Reserved, zero |
| 56+520*r+8*i | 8 | Value/result for entry i, 0..63 |

Frame size excluding FCS = `48 + 520 * rounds`. At 16 rounds: 8368 bytes;
IPv4 length = 8354, UDP payload = 8326, useful result = 8192 bytes. Use MTU 9000
on the FPGA ports and DPDK ports. At 10G line rate the maximum useful result rate
for this packet layout is approximately `10 * 8192 / (8368+24) = 9.762 Gbit/s`,
accounting for preamble/SFD, FCS and IFG. Four broadcast copies are not four times
the useful aggregate result. MAC/IPv4 addressing is reflected from worker0;
the DPDK receiver identifies replies by the v4 response marker and round IDs.

Application UDP protocol, intended for localhost:

| Offset | Type | Field |
|---|---|---|
| 0 | u32 | Magic 0x4d504247 |
| 4 | u16 | Version 4 |
| 6 | u16 | Request 1, response 2, stop 3 |
| 8 | u32 | Base round ID, multiple of 16 |
| 12 | u16 | Round count |
| 14 | u16 | Worker count |
| 16 | u16 | Entry count, fixed 64 |
| 18 | u16 | Status, zero in requests |
| 20 | u32 | Reserved, zero |
| 24 | u64[] | Request: worker-major, then round-major, then entry-major |
| 24 | u64[] | Response: round-major, then entry-major |

Request size = `24 + workers*rounds*512`; response size = `24 + rounds*512`.
The largest application request is 32792 bytes, which fits localhost UDP. Sending
that application datagram over an external MTU-9000 network would fragment it;
the FPGA-facing DPDK frames are separately packed and do not fragment. The bridge
listens on 127.0.0.1. Adapt the application transport separately for remote clients.
The Python client pads each supplied round to 64 values. `--rounds N` repeats its
CLI values for N rounds; its API accepts different values for every round.

## Build and validate

```bash
cd /home/antl/Desktop/SFP/进程间计算
make
python3 pre_synth_equivalence_check.py
python3 test_batch_rtl.py
python3 test_software_pipeline.py --window 16
```

XSim requires the Vivado tools on PATH. The protocol test compiles and calls the
actual C serializer/parser, comparing it to independent Python expectations.
The RTL test drives real Verilog with 256 occupied slots, different worker orders,
u64 overflow, input gaps, output stalls, duplicates, malformed frames and timeout
recovery. Software simulation explicitly computes sums in software and is not a
measurement of the FPGA. Test logs and resource reports are in `reports/`.

Build the board image with the existing flow:

```bash
cd /home/antl/Desktop/SFP/corundum/fpga/mqnic/ZCU102/fpga/fpga
make app
```

Check the implemented timing report before deployment. Deploy `app/fpga.bin`,
`app/overlay.dtbo`, and `app/shell.json` together. On the board, the new isolated
source checkout is `/home/ubuntu/sfp-v4`; the old firmware backup is
`/home/ubuntu/mqnic-backup-before-v4`.

```bash
sudo rmmod mqnic                  # only when the module is currently loaded
sudo xmutil unloadapp             # only when an application is active
sudo xmutil loadapp mqnic
sudo insmod /home/ubuntu/sfp-v4/corundum/modules/mqnic/mqnic.ko
sudo ip link set eth1 mtu 9000 up
sudo ip link set eth2 mtu 9000 up
sudo ip link set eth3 mtu 9000 up
sudo ip link set eth4 mtu 9000 up
```

## Host setup and port ownership

| Rank | Former Linux interface | PCI address | Board |
|---|---|---|---|
| 0 | enp1s0f3np3 | 0000:01:00.3 | eth1 |
| 1 | enp1s0f2np2 | 0000:01:00.2 | eth2 |
| 2 | enp1s0f1np1 | 0000:01:00.1 | eth3 |
| 3 | enp1s0f0np0 | 0000:01:00.0 | eth4 |

The four PFs share IOMMU group 1. Rank0 is the DPDK primary and configures all
devices, queues and pools; ranks1..3 are secondary processes. Each rank exclusively
polls one RX/TX queue pair. They share the `sfp-shared` EAL file prefix. Four
independent primaries would conflict over this IOMMU group. Do not run the bridge
and direct benchmark concurrently. Run all ranks on the same machine.
The final per-port configuration uses 64 RX descriptors, 128 TX descriptors and
511 jumbo mbufs with a cache of 32. The 16-frame credit limit bounds outstanding
traffic; smaller rings reduce the four-port memory footprint.

Normal startup checks link stability and performs a four-worker zero-sum handshake
before opening the application socket or starting measurement. Local link-up was
observed to precede a working end-to-end packet path after PF restart. The handshake
uses fresh IDs, bounded retries and waits longer than the FPGA incomplete-batch
timeout after failure; it is excluded from measured traffic. The bridge also holds
an early broadcast response if it arrives before an inactive rank processes its MPI
work notification. Benchmark traffic itself is not silently retried after timeout.

```bash
cd /home/antl/Desktop/SFP/进程间计算
sudo python3 setup_host.py dpdk
sudo /usr/local/openmpi/bin/mpirun --allow-run-as-root -np 4 \
  --bind-to core --map-by core ./mpi_fpga_bridge --probe
```

Setup saves the original state, enables 512 MiB of hugepages, raises UDP buffer
maxima to 16 MiB and binds only the four listed X710 functions to vfio-pci. The
management interface eno1 is excluded. The four former Linux SFP interface names
disappear while VFIO owns them; DPDK sets MTU and starts the ports. Both hardware
programs are run with sudo to provide VFIO and locked-memory privileges. The
setup makes no boot-time configuration changes. To restore the old host state:

```bash
sudo python3 setup_host.py restore
```

## Measurements

End-to-end application -> MPI -> FPGA -> MPI -> application:

```bash
sudo /usr/local/openmpi/bin/mpirun --allow-run-as-root -np 4 \
  --bind-to core --map-by core ./mpi_fpga_bridge --window 16

# A second terminal, using a different physical CPU core:
python3 bridge_app_client.py --request-id 0x1000 \
  --worker0-values 10 20 30 40 50 --worker1-values 1 2 3 4 6
python3 benchmark_bridge_bandwidth.py --workers 4 --rounds 16 \
  --window 16 --duration 30 --cpu 4

# Drain outstanding work and stop the bridge before the direct benchmark:
python3 bridge_app_client.py --stop
```

The Python benchmark entry point runs the native binary. It uses preallocated
packet templates, `sendmmsg`/`recvmmsg`, a bounded window, response ID matching,
numeric validation and latency histograms. It includes the final drain in elapsed
time and labels payload, useful results and estimated physical-wire traffic
separately. Kernel socket limits must be raised before opening the sockets.
The MPI bridge uses nonblocking point-to-point operations with independent buffers
and outstanding requests per slot, instead of blocking broadcasts/barriers per
batch. The FPGA performs the numeric reduction; rank0 compares all four replies.

Direct DPDK -> FPGA -> DPDK, excluding application and per-batch MPI copying:

```bash
sudo /usr/local/openmpi/bin/mpirun --allow-run-as-root -np 4 \
  --bind-to core --map-by core ./benchmark_dpdk \
  --workers 4 --rounds 16 --window 16 --batches 100000
```

This generator has one process/core per physical port, prebuilt frame templates,
burst RX/TX, a 16-batch window and full numeric validation. MPI is used only at
startup and for final statistics, not in the packet loop. Each generation changes
a payload value to detect stale result reuse. Timeout terminates all generators.
Run this separately from the bridge to distinguish FPGA/NIC throughput from
application/MPI overhead. DPDK port counters include missing packets, RX errors,
mbuf exhaustion and short TX bursts.

Independent sender capacity, with no aggregation or response validation:

```bash
sudo /usr/local/openmpi/bin/mpirun --allow-run-as-root -np 4 \
  --bind-to core --map-by core ./benchmark_dpdk --tx-only \
  --workers 4 --rounds 16 --batches 1000000
```

This mode sends experimental EtherType 0x88b5 frames, which the FPGA parser discards.
It waits for actual NIC transmit counters to reach the requested packet count,
including the final queue drain. It measures sender capacity only. Short TX bursts
are normal line-rate backpressure here: unaccepted mbufs are freed and the remaining
packet count is sent later. They are not counted as successfully transmitted packets.

## Recorded board results, 2026-09-07

Host: i7-10700, X710-4 at PCIe Gen3 x8, four physical 10G links. Ranks use CPU0..3;
the native application generator uses CPU4. All rates below are Gbit/s. Useful
results count each aggregate once, regardless of the four returned copies.

| Measurement | Workers | Rounds/frame | Window | Useful result | Per-port wire estimate |
|---|---:|---:|---:|---:|---:|
| End-to-end | 4 | 1 | 1 | 0.161 | 0.186 |
| End-to-end | 4 | 16 | 1 | 1.055 | 1.081 |
| End-to-end | 2 | 16 | 16 | 3.925 | 4.021 |
| End-to-end | 3 | 16 | 16 | 3.649 | 3.738 |
| End-to-end | 4 | 16 | 16 | 3.585 | 3.673 |
| Direct DPDK aggregation | 2 | 16 | 16 | 8.446 | 8.652 |
| Direct DPDK aggregation | 3 | 16 | 16 | 7.776 | 7.966 |
| Direct DPDK aggregation | 4 | 16 | 16 | 6.694 | 6.858 |
| TX-only, four simultaneous ports | 4 | 16 | N/A | N/A | 9.799..9.800 |

End-to-end cases ran for five seconds each, with zero failed or timed-out requests.
The four-worker window16 case completed 273548 batches, mean RTT 291.843 us and
p99 <=535 us. Each direct aggregation case completed 100000 batches, with zero
numeric errors, NIC misses, RX errors or mbuf exhaustion. The nine numeric cases
also cover 2/3/4 workers, 1/3/16 rounds and unsigned overflow. TX-only sent exactly
1000000 frames / 8368000000 Ethernet bytes on each of all four ports.
Logs are `reports/hardware_*.log`; run `sudo python3 test_hardware_pipeline.py`
to reproduce the bounded matrix. Values vary with host load and are not guarantees.

The sender reaches near 10G, but four-worker aggregation and the complete application
path do not reach 10G. This measurement does not isolate all remaining CPU, MPI,
memory, NIC and FPGA backpressure costs. A separate million-frame stress run observed
one NIC RX error on rank2 near batch 992755 and stopped on timeout; the protocol has
no general retransmission/replay mechanism. The final million-frame repeat passed
on all four ports with zero errors or losses, measuring 6.701 Gbit/s useful results
over 9.780518 seconds (`reports/hardware_long_w4.log`). Do not infer indefinite loss-free service
from the shorter passing runs. Retain the timeout check and investigate persistent
physical-link errors before using this as a reliable transport.

The final localhost software-only regression (16-round frames, window16, CPU4 for
the client) also passed all numeric and sliding-window cases. It measured about
2.74 Gbit/s with four workers during that run. This includes simulated CPU addition
and must not be presented as a physical FPGA link measurement.
