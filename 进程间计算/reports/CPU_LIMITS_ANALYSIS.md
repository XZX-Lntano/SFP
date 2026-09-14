# MPI/DPDK bridge CPU experiment — 2026-09-09

Scope: real four-worker FPGA aggregation through mpi_fpga_bridge and the unchanged
native application benchmark, 16 rounds/frame, window16, ranks CPU0..3, client CPU4.
No production bridge source, binary, FPGA image or NIC configuration was changed.

## Findings

Rank0 has the strongest evidence of CPU-side processing pressure, dominated by
copying and comparing data. There is no evidence that all four ranks have exhausted
their useful processing capacity. Ranks1..3 spend substantial sampled time in MPI
progress/test functions even under load. These functions include both waiting and
useful progress; their sample percentage is not an exact idle percentage.

The experiment supports prioritizing rank0 data movement and RX service latency.
It does not prove a CPU arithmetic/clock ceiling, memory bandwidth saturation, or
that optimizing rank0 alone would deliver 10G. The application sender was unchanged
and is not independently certified to support 10G effective results by this test.

## User-mode cycle sampling

Six seconds per rank at 199 Hz, cycles:u, no call-stack unwinding. No lost samples.
Source: cpu_limits_20260909_211135/idle_r*.txt and loaded_r*.txt.

| Rank | Loaded memcpy/memmove samples | Loaded memcmp samples |
|---|---:|---:|
| 0 | 51.38% | 8.84% |
| 1 | 17.11% | below report threshold |
| 2 | 15.65% | below report threshold |
| 3 | 17.90% | below report threshold |

Idle ranks1..3 spend approximately 40% in mca_part_persist_progress and 20% in
ompi_request_default_test alone. Under load those shares drop but remain material.
Percentages describe sampled user-mode cycles, not total process wall time or
fraction of business work. Some executable addresses could not be symbolized;
therefore no precise source-line or MPI-internal-versus-application copy attribution
is claimed. Kernel UDP costs are not fully represented by cycles:u.

## Frequency experiment — invalid intervention

The sysfs max-frequency writes were accepted, but measured frequencies stayed near
4.6 GHz rather than the requested 2.3 GHz. Do not interpret these throughput numbers
as frequency sensitivity. All four original 4800000 kHz limits were restored.
Raw records: cpu_limits_20260909_211135/results.json.

## Same-core contention

Bounded competitors consume CPU on selected rank cores, client CPU4 unchanged.
Process CPU accounting verifies approximately 50% CPU time for affected ranks.
First full sequence, useful Gbit/s:

| Case | Throughput |
|---|---:|
| Normal | 3.976 |
| Rank0 receives ~50% CPU time | 1.987 |
| Rank1..3 each receive ~50% CPU time | 0.600 |
| All ranks receive ~50% CPU time | 0.288 |
| Normal after removing competitors | 3.635 |

This proves sensitivity to CPU availability/scheduling, not saturation from useful
computation: preemption delays the slowest-worker completion and consumes window
credits. The second sequence stopped on one rank0 missed RX packet and timeout;
do not count it as a clean completed performance experiment.
Raw records: cpu_limits_20260909_211346/.

## Per-batch CPU work without deliberate preemption

A temporary binary adds an active monotonic-clock loop for 2 or 4 microseconds
once per batch after work dispatch/receipt on selected ranks. It neither sleeps nor
changes packet data. The compiler flags match the production optimization flags.
It is a sensitivity test, not an instruction-throughput benchmark: injected work
also changes request timing and cache/pipeline overlap. The diagnostic source is
saved; its executable was temporary and removed after testing.

| Case | Repeat 1 Gbit/s | Repeat 2 Gbit/s |
|---|---:|---:|
| Normal before | 3.332 | 2.995 |
| Rank0 +2 us/batch | 3.205 | 3.175 |
| Rank0 +4 us/batch | 2.969 | 2.903 |
| Rank1..3 each +4 us/batch | 3.327 | 3.185 |
| Normal after | 3.401 (failed request; exclude) | 3.342 |

Rank0 +4 us causes a repeatable lower result than the adjacent clean baselines,
about 8–11% using the first baseline and the second repeat's before/after midpoint.
Rank1..3 +4 us did not produce a comparable reduction. The +2 us effect is smaller
than run-to-run variation and is inconclusive. Baseline variation prohibits precise
headroom percentages or a maximum achievable CPU-limited throughput prediction.

One nominal baseline encountered rank0 imissed=1 (RX queue missed packet), not a
NIC CRC/ierrors event. The bridge returned STATUS_TIMEOUT; other ranks received
their result. This shows that RX service/queue headroom remains a reliability issue,
but the current instrumentation cannot attribute that single miss to a specific
scheduler pause or application function.
Raw records: cpu_work_20260909_211619/results.json and individual bridge/client logs.

## Next implementation targets

1. Remove intermediate Work -> frame_app -> mbuf and receive result copies in rank0.
2. Profile copy call stacks to separate MPI internal transfers from application copies.
3. Measure rank0 RX polling gaps and queue occupancy; reassess the 64-entry RX ring
   under the full bridge workload rather than relying on direct-DPDK tests alone.
4. Independently test the native application generator before claiming 10G end-to-end.

Scripts: test_cpu_limits.py (sampling, frequency or --contention), test_cpu_work.py
(isolated per-batch work). These require sudo and exclusive ownership of the four
DPDK ports; each run starts/stops its own bridge. No CPU stress process is retained.
