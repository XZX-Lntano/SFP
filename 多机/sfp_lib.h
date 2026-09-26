#ifndef SFP_LIB_H
#define SFP_LIB_H
#include <stdint.h>
#include <stddef.h>
#include "bridge_protocol.h"

/* Multi-machine configuration:
 * - two processes run on each machine, 2 machines in total (ranks 0..3 overall);
 * - exactly one process per machine is the DPDK primary and configures both of
 *   the machine's ports; the other is a DPDK secondary;
 * - local_bdfs[i] / local_ranks[i] describe the machine's two ports:
 *   local_bdfs[i] is owned by the global rank local_ranks[i].
 * - local_peer is the other global rank living on this machine.
 */
typedef struct {
    int rank;              /* global worker id 0..3; FPGA port == rank (eth{rank+1}) */
    int is_primary;        /* 1: configure this machine's two ports (DPDK primary) */
    const char *const *local_bdfs; /* this machine's two BDF strings */
    const int *local_ranks;        /* global rank owning each local BDF */
    int app_cpu;
    int dpdk_cpu;
    int require_link;
    unsigned window;
    unsigned timeout_ms;
    const char *file_prefix;
    int local_peer;        /* global rank of the other process on this machine */
} sfp_cfg_t;

typedef struct {
    uint32_t base;
    uint16_t rounds;
    uint16_t workers;
} sfp_job_t;

typedef void (*sfp_callback_fn)(const sfp_job_t *, int status, void *);

typedef struct {
    uint64_t submitted, completed, tx_packets, rx_packets;
    uint64_t tx_bytes, rx_bytes, tx_reuse, tx_alloc_fail;
    uint64_t tx_completed, tx_batches, tx_batch_max;
    uint64_t invalid, late, duplicate, timeout, io_error;
    uint64_t short_burst, rx_missed, rx_errors, rx_nombuf;
    uint64_t rtt_ns_sum, rtt_ns_count, mbuf_in_use;
    uint64_t rtt_hist[32];
} sfp_stats_t;

int sfp_init(const sfp_cfg_t *);
int sfp_submit(int worker, const double *in, double *out,
               const sfp_job_t *, sfp_callback_fn, void *);
int sfp_stats(sfp_stats_t *);
/* Clear the failed latch after a warmup timeout so a fresh batch base can be
 * submitted. Only call between jobs, after the FPGA has expired the old slot
 * (>=100 ms) and no slot is in flight. */
void sfp_rearm(void);
/* Base of the most recent FPGA result frame seen on this port (0 before the
 * first one). Used by non-participating ranks to join an already-running
 * result stream at its current position. */
uint32_t sfp_latest_base(void);
/* Short per-slot timeout (ns) for the slots submitted while set (0 = normal
 * timeout_ms). Non-participating ranks probe a join position with 16 slots
 * that time out in 150 us so a lost probe batch can be retried in ~ms
 * instead of the 500 ms slot timeout. Probe-slot timeouts do not print the
 * "stalled base" stall line (they are expected) and do not latch failed. */
void sfp_probe_timeout(uint64_t ns);
void sfp_fini(void);
#endif
