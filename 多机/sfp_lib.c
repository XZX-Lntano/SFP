#define _GNU_SOURCE
#include "sfp_lib.h"
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SFP_SLOTS MAX_WINDOW
#define SFP_POOL_N 2047
#define SFP_POOL_PREFIX "sfp-jumbo"
#define SFP_LOCAL_PORTS 2

enum { SLOT_FREE, SLOT_WRITING, SLOT_READY, SLOT_INFLIGHT };
typedef struct {
    _Atomic int state;
    sfp_job_t job;
    const double *in;
    double *out;
    sfp_callback_fn cb;
    void *opaque;
    uint64_t submitted_ns;
    uint64_t timeout_ns; /* 0 = use cfg.timeout_ms */
    struct rte_mbuf *tx;
    int rx_done, tx_sent;
} slot_t;

typedef struct {
    sfp_cfg_t cfg;
    uint16_t port;
    int local_idx;
    struct rte_mempool *pool;
    slot_t slots[SFP_SLOTS];
    pthread_t thread;
    _Atomic int stop;
    _Atomic int started;
    sfp_stats_t stats;
    unsigned active;
    _Atomic int failed;
    _Atomic uint32_t last_base;
    _Atomic uint64_t probe_timeout_ns;
} sfp_ctx_t;

static sfp_ctx_t G;
static struct rte_mempool *primary_pools[SFP_LOCAL_PORTS];

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec*1000000000ull + (uint64_t)t.tv_nsec;
}
static void pin_thread(int cpu) {
    if (cpu < 0) return;
    cpu_set_t set; CPU_ZERO(&set); CPU_SET((unsigned)cpu, &set);
    (void)pthread_setaffinity_np(pthread_self(), sizeof set, &set);
}
static void stat_add(uint64_t *p, uint64_t v) {
    __atomic_fetch_add(p, v, __ATOMIC_RELAXED);
}
static int build_native(uint8_t *b, const sfp_job_t *j, unsigned worker,
                        const double *in) {
    if (worker >= j->workers || j->rounds < 1 || j->rounds > MAX_ROUNDS ||
        j->workers < 2 || j->workers > 4 || (j->base & 15)) return 0;
    const double limit = 0x1p63 / j->workers;
    size_t n = fpga_work_header(b, j->rounds, j->workers, worker,
                                (const uint8_t[6]){255,255,255,255,255,255},
                                0xc0a80a01u + worker, 0xc0a80a65u + worker);
    for (unsigned r=0; r<j->rounds; r++) {
        uint8_t *q = b + FPGA_HEADER + r*ROUND_BYTES;
        put32(q, j->base+r); put32(q+4, 0);
        for (unsigned i=0; i<ENTRIES; i++) {
            uint64_t bits;
            if (!double_to_fixed_limit(in[(size_t)r*ENTRIES+i], limit, &bits))
                return 0;
            put64(q+8+i*8, bits);
        }
    }
    return (int)n;
}
static void complete_slot(slot_t *s, int status) {
    uint64_t end = now_ns();
    sfp_job_t job = s->job;
    if (status == STATUS_OK) {
        stat_add(&G.stats.completed, 1);
        stat_add(&G.stats.rtt_ns_sum, end-s->submitted_ns); stat_add(&G.stats.rtt_ns_count, 1);
        unsigned b = 0; uint64_t us = (end-s->submitted_ns)/1000;
        while (us > 1 && b < 31) { us >>= 1; b++; }
        stat_add(&G.stats.rtt_hist[b], 1);
    }
    sfp_callback_fn cb = s->cb; void *opaque = s->opaque;
    atomic_store_explicit(&s->state, SLOT_FREE, memory_order_release);
    if (cb) cb(&job, status, opaque);
}
static void *worker_main(void *arg) {
    (void)arg; pin_thread(G.cfg.dpdk_cpu);
    atomic_store(&G.started, 1);
    struct rte_mbuf *rx[32];
    while (!atomic_load_explicit(&G.stop, memory_order_acquire)) {
        struct rte_mbuf *batch[16];
        slot_t *owners[16];
        uint16_t count = 0;
        /* Only this thread touches this port's TX queue and retained mbufs. */
        int cleanup_rc=rte_eth_tx_done_cleanup(G.port, 0, 0);
        if (cleanup_rc<0 && !atomic_exchange(&G.failed,1))
            fprintf(stderr,"rank=%d TX completion unsupported/error: %d\n",G.cfg.rank,cleanup_rc);

        for (unsigned si=0; si<G.active && count<16; si++) {
            slot_t *s = &G.slots[si];
            if (atomic_load_explicit(&s->state, memory_order_acquire) != SLOT_READY)
                continue;
            if ((unsigned)G.cfg.rank >= s->job.workers) {
                atomic_store(&s->state, SLOT_INFLIGHT);
                continue;
            }
            struct rte_mbuf *m = s->tx;
            /* One library reference remains after the PMD releases its own.
             * A received result alone does not prove DMA has released m. */
            if (rte_mbuf_refcnt_read(m) != 1) continue;
            uint8_t *d = rte_pktmbuf_mtod(m, uint8_t *);
            int len = build_native(d, &s->job, (unsigned)G.cfg.rank, s->in);
            if (!len) {
                stat_add(&G.stats.invalid,1);
                complete_slot(s, STATUS_INVALID); continue;
            }
            m->data_len = (uint16_t)len; m->pkt_len = (uint32_t)len;
            rte_mbuf_refcnt_update(m, 1); /* PMD reference, in addition to ours */
            batch[count] = m; owners[count++] = s;
        }
        if (count) {
            stat_add(&G.stats.tx_batches,1);
            if (count>__atomic_load_n(&G.stats.tx_batch_max,__ATOMIC_RELAXED))
                __atomic_store_n(&G.stats.tx_batch_max,count,__ATOMIC_RELAXED);
            uint16_t sent = rte_eth_tx_burst(G.port, 0, batch, count);
            stat_add(&G.stats.tx_packets, sent);
            for (uint16_t i=0; i<count; i++) {
                if (i<sent) {
                    stat_add(&G.stats.tx_bytes, batch[i]->pkt_len);
                    stat_add(&G.stats.tx_reuse, 1);
                    owners[i]->tx_sent=1;
                    atomic_store(&owners[i]->state, SLOT_INFLIGHT);
                } else {
                    /* Unsent mbufs never belonged to the PMD. Keep READY
                     * for retry; never allocate or free a TX mbuf here. */
                    rte_mbuf_refcnt_update(batch[i], -1);
                }
            }
            if (sent<count) stat_add(&G.stats.short_burst, 1);
        }
        uint16_t n = rte_eth_rx_burst(G.port, 0, rx, 32);
        for (uint16_t k=0; k<n; k++) {
            struct rte_mbuf *m = rx[k];
            const uint8_t *b = rte_pktmbuf_mtod(m, const uint8_t *);
            size_t len = rte_pktmbuf_pkt_len(m);
            uint32_t base; unsigned rounds, workers;
            int ok = rte_pktmbuf_is_contiguous(m) &&
                fpga_result_view(b, len, &base, &rounds, &workers);
            if (!ok) { stat_add(&G.stats.invalid,1); rte_pktmbuf_free(m); continue; }
            atomic_store_explicit(&G.last_base, base, memory_order_relaxed);
            slot_t *hit = NULL;
            for (unsigned i=0; i<SFP_SLOTS; i++) {
                slot_t *s = &G.slots[i];
                if (atomic_load_explicit(&s->state,memory_order_acquire)==SLOT_INFLIGHT &&
                    s->job.base==base && s->job.rounds==rounds &&
                    s->job.workers==workers) { hit=s; break; }
            }
            if (hit && hit->rx_done) { stat_add(&G.stats.duplicate,1); rte_pktmbuf_free(m); continue; }
            if (!hit) { stat_add(&G.stats.late,1); rte_pktmbuf_free(m); continue; }
            for (unsigned r=0; r<rounds; r++) {
                const uint8_t *q=b+FPGA_HEADER+r*ROUND_BYTES;
                for (unsigned i=0; i<ENTRIES; i++)
                    hit->out[(size_t)r*ENTRIES+i] =
                        fixed_to_double(get64(q+8+i*8));
            }
            stat_add(&G.stats.rx_packets,1); stat_add(&G.stats.rx_bytes,len);
            rte_pktmbuf_free(m); hit->rx_done=1;
        }
        uint64_t t = now_ns();
        for (unsigned i=0; i<SFP_SLOTS; i++) {
            slot_t *s=&G.slots[i];
            int state=atomic_load_explicit(&s->state,memory_order_acquire);
            if (state!=SLOT_INFLIGHT && state!=SLOT_READY) continue;
            int tx_done=!s->tx_sent || rte_mbuf_refcnt_read(s->tx)==1;
            if (s->tx_sent==1 && tx_done) {
                s->tx_sent=2; stat_add(&G.stats.tx_completed,1);
            }
            if (s->rx_done && tx_done) { complete_slot(s,STATUS_OK); continue; }
            uint64_t deadline = s->timeout_ns ? s->timeout_ns :
                (uint64_t)(G.cfg.timeout_ms ? G.cfg.timeout_ms : 500)*1000000ull;
            int expired = (t>=s->submitted_ns && t-s->submitted_ns > deadline);
            /* Probe slots (short join-probe timeout) expire silently: a lost
             * probe batch is an expected event, not a stall, and it must not
             * latch the failed flag (which would -EIO the next probe).
             * G.failed does NOT time out other in-flight slots: one lost
             * frame (a flaky link blip) must not cascade into the other 15
             * slots of the window, whose frames may still arrive. The latch
             * only stops NEW submits (sfp_submit -> -EIO); callers that
             * expect a live stream (warmup/verify/keep-alive/observer
             * main loop) re-arm and retry. */
            if (expired) {
                if (!s->timeout_ns && !atomic_exchange(&G.failed,1))
                    fprintf(stderr,"rank=%d stalled base=%u rx_done=%d tx_sent=%d refcnt=%u cleanup=%d\n",
                        G.cfg.rank,s->job.base,s->rx_done,s->tx_sent,rte_mbuf_refcnt_read(s->tx),cleanup_rc);
                stat_add(&G.stats.timeout,1); complete_slot(s,STATUS_TIMEOUT);
            }
        }
    }
    return NULL;
}
/* Configure both ports of this machine. Only the DPDK primary calls this. */
static int configure_local(const sfp_cfg_t *c) {
    for (int i=0; i<SFP_LOCAL_PORTS; i++) {
        uint16_t p;
        if (rte_eth_dev_get_port_by_name(c->local_bdfs[i], &p) < 0) return -1;
        struct rte_eth_dev_info info;
        if (rte_eth_dev_info_get(p,&info)<0) return -1;
        struct rte_eth_conf ec={0}; ec.rxmode.mtu=JUMBO_MTU;
        if (rte_eth_dev_configure(p,1,1,&ec)<0 || rte_eth_dev_set_mtu(p,JUMBO_MTU)<0) return -1;
        int socket=rte_eth_dev_socket_id(p); if (socket<0) socket=0;
        char name[32]; snprintf(name,sizeof name,"%s%d",SFP_POOL_PREFIX,c->local_ranks[i]);
        struct rte_mempool *pool=rte_pktmbuf_pool_create(name,SFP_POOL_N,32,0,
            9728+RTE_PKTMBUF_HEADROOM,socket);
        if (!pool) return -1;
        primary_pools[i]=pool;
        uint16_t nrx=256, ntx=128;
        struct rte_eth_txconf txconf=info.default_txconf;
        /* Scalar TX paths support reference-counted mbufs; RS on every
         * packet lets a partial/final batch release all retained references. */
        txconf.tx_rs_thresh=1;
        txconf.tx_free_thresh=32;
        txconf.offloads &= ~RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;
        if (rte_eth_dev_adjust_nb_rx_tx_desc(p,&nrx,&ntx)<0 ||
            rte_eth_rx_queue_setup(p,0,nrx,socket,&info.default_rxconf,pool)<0 ||
            rte_eth_tx_queue_setup(p,0,ntx,socket,&txconf)<0 ||
            rte_eth_dev_start(p)<0) return -1;
        (void)rte_eth_promiscuous_enable(p);
    }
    return 0;
}
int sfp_init(const sfp_cfg_t *cfg) {
    if (!cfg || !cfg->local_bdfs || !cfg->local_ranks || cfg->rank<0 || cfg->rank>3) return -EINVAL;
    int my_idx=-1;
    for (int i=0;i<SFP_LOCAL_PORTS;i++) {
        if (cfg->local_ranks[i]==cfg->rank) {
            if (my_idx>=0) return -EINVAL;
            my_idx=i;
        }
    }
    if (my_idx<0) return -EINVAL;
    if (cfg->local_peer<0 || cfg->local_peer>3 || cfg->local_peer==cfg->rank) return -EINVAL;
    if (cfg->local_ranks[my_idx^1]!=cfg->local_peer) return -EINVAL;
    memset(&G,0,sizeof G); G.cfg=*cfg; G.local_idx=my_idx;
    G.active=(cfg->window && cfg->window<=SFP_SLOTS)?cfg->window:SFP_SLOTS;
    char core[32]; snprintf(core,sizeof core,"%d",cfg->app_cpu);
    char *args[]={"sfp","-l",core,"-n","4","--huge-dir","/dev/hugepages",
        "-a",(char*)cfg->local_bdfs[0],"-a",(char*)cfg->local_bdfs[1],
        "--file-prefix",(char*)(cfg->file_prefix?cfg->file_prefix:"sfp-shared"),
        cfg->is_primary?"--proc-type=primary":"--proc-type=secondary","--log-level=5",NULL};
    int argc=0; while(args[argc]) argc++;
    if (rte_eal_init(argc,args)<0) return -1;
    if (cfg->is_primary && configure_local(cfg)<0) return -1;
    char pool_name[32]; snprintf(pool_name,sizeof pool_name,"%s%d",SFP_POOL_PREFIX,cfg->rank);
    G.pool=rte_mempool_lookup(pool_name); if (!G.pool) return -1;
    if (rte_eth_dev_get_port_by_name(cfg->local_bdfs[my_idx],&G.port)<0) return -1;
    if (cfg->require_link) {
        /* SFP+ PCS lock can take a few seconds after a fresh port init, so
         * wait up to 10 s before declaring the link dead. */
        struct rte_eth_link link={0};
        int ok=0;
        for (int t=0;t<100;t++) {
            if (rte_eth_link_get(G.port,&link)==0 && link.link_status) { ok=1; break; }
            usleep(100000);
        }
        if (!ok) {
            fprintf(stderr,"rank=%d: no link on %s after 10 s\n",cfg->rank,cfg->local_bdfs[my_idx]);
            return -1;
        }
    }
    for (unsigned i=0;i<G.active;i++) {
        atomic_store(&G.slots[i].state,SLOT_FREE);
        G.slots[i].tx=rte_pktmbuf_alloc(G.pool);
        if (!G.slots[i].tx) return -ENOMEM;
    }
    pin_thread(cfg->app_cpu);
    atomic_store(&G.stop,0);
    if (pthread_create(&G.thread,NULL,worker_main,NULL)!=0) return -1;
    while (!atomic_load(&G.started)) sched_yield();
    return 0;
}
int sfp_submit(int worker, const double *in, double *out, const sfp_job_t *j,
               sfp_callback_fn cb, void *opaque) {
    if (atomic_load(&G.failed)) return -EIO;
    if (!in || !out || !j || !cb || worker != G.cfg.rank || worker<0 || worker>3) return -EINVAL;
    if (j->workers<2 || j->workers>4 || j->rounds<1 || j->rounds>MAX_ROUNDS || (j->base&15)) return -EINVAL;
    for (unsigned i=0;i<G.active;i++) {
        slot_t *s=&G.slots[i]; int expected=SLOT_FREE;
        if (atomic_compare_exchange_strong(&s->state,&expected,SLOT_WRITING)) {
            s->rx_done=0; s->tx_sent=0;
            s->job=*j; s->in=in; s->out=out; s->cb=cb; s->opaque=opaque; s->submitted_ns=now_ns();
            s->timeout_ns=atomic_load_explicit(&G.probe_timeout_ns, memory_order_relaxed);
            stat_add(&G.stats.submitted,1);
            atomic_store_explicit(&s->state, SLOT_READY, memory_order_release);
            return 0;
        }
    }
    return -EAGAIN;
}
int sfp_stats(sfp_stats_t *out) {
    if (!out) return -EINVAL;
    /* Atomic counters permit live snapshots without a per-packet mutex.
     * A live snapshot is approximate; after quiescence it is exact. */
#define SNAP(name) out->name=__atomic_load_n(&G.stats.name,__ATOMIC_RELAXED)
    SNAP(submitted); SNAP(completed); SNAP(tx_packets); SNAP(rx_packets);
    SNAP(tx_completed); SNAP(tx_batches); SNAP(tx_batch_max);
    SNAP(tx_bytes); SNAP(rx_bytes); SNAP(tx_reuse); SNAP(tx_alloc_fail);
    SNAP(invalid); SNAP(late); SNAP(duplicate); SNAP(timeout); SNAP(io_error);
    SNAP(short_burst); SNAP(rtt_ns_sum); SNAP(rtt_ns_count);
    for (unsigned i=0;i<32;i++) SNAP(rtt_hist[i]);
#undef SNAP
    out->mbuf_in_use=G.active;
    struct rte_eth_stats es={0}; if (G.pool) rte_eth_stats_get(G.port,&es);
    out->rx_missed=es.imissed; out->rx_errors=es.ierrors; out->rx_nombuf=es.rx_nombuf;
    return 0;
}
void sfp_rearm(void) {
    if (G.pool) atomic_store(&G.failed, 0);
}
uint32_t sfp_latest_base(void) {
    return atomic_load_explicit(&G.last_base, memory_order_relaxed);
}
void sfp_probe_timeout(uint64_t ns) {
    atomic_store_explicit(&G.probe_timeout_ns, ns, memory_order_relaxed);
}
void sfp_fini(void) {
    if (!G.pool) return;
    atomic_store(&G.stop,1); pthread_join(G.thread,NULL);
    (void)rte_eth_tx_done_cleanup(G.port,0,UINT32_MAX);
    for (unsigned i=0;i<G.active;i++) {
        /* Drop only our reference; any outstanding PMD reference is returned
         * when the primary stops and closes the port. */
        rte_pktmbuf_free(G.slots[i].tx); G.slots[i].tx=NULL;
    }
    if (G.cfg.is_primary) {
        for (int i=0;i<SFP_LOCAL_PORTS;i++) {
            uint16_t p;
            if (rte_eth_dev_get_port_by_name(G.cfg.local_bdfs[i],&p)==0) {
                rte_eth_dev_stop(p); rte_eth_dev_close(p);
            }
        }
        for (int i=0; i<SFP_LOCAL_PORTS; i++) {
            if (primary_pools[i]) {
                fprintf(stderr,"sfp: freeing primary mempool rank %d\n",G.cfg.local_ranks[i]);
                rte_mempool_free(primary_pools[i]); primary_pools[i]=NULL;
            }
        }
    } else { /* secondary must not stop/close a device owned by the primary */ }
    int cleanup_rc=rte_eal_cleanup();
    fprintf(stderr,"sfp: rte_eal_cleanup rc=%d rank=%d\n",cleanup_rc,G.cfg.rank);
    memset(&G,0,sizeof G);
}
