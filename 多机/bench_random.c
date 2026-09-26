#define _GNU_SOURCE
/* Multi-machine benchmark driver.
 *
 * Two independent processes run on each of two machines (ranks 0..3 overall).
 * Each machine's DPDK primary configures that machine's two ports.
 *
 * Start-up alignment has two selectable modes (--sync):
 *   handshake (default): every participating rank submits ONE job at the same
 *     fixed base and retries that base until the FPGA completes the batch and
 *     broadcasts it. All ranks receive that first broadcast within a few
 *     microseconds, so it is the common "go" edge and no wall clock is
 *     involved. A rank that starts early simply spins on the same base; a rank
 *     that starts seconds late is caught by a later retry.
 *   grid: the legacy NTP-aligned 600 ms wall-clock grid, kept as a fallback
 *     and for A/B comparison.
 * Either way the measured stream is a 16-deep pipeline and the FPGA's 100 ms
 * incomplete-batch window absorbs the residual skew, so no per-job wait ever
 * happens. The finish barrier is machine-local (primary waits for its own peer
 * only), because DPDK shared memory never crosses machines.
 */
#include "sfp_lib.h"
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <time.h>
#include <sched.h>
#include <errno.h>
#include <stdatomic.h>
#include <dirent.h>
#include <rte_pause.h>

typedef struct {
    double *in, *out;
    _Atomic int busy;
    int status;
} app_buffer_t;
static _Atomic unsigned done_count, error_count;
/* 1 for non-participating ranks: their non-OK completions (lost frames,
 * stream-end timeouts) are handled by the main loop's skip-ahead and
 * stream-end detection, not counted as run errors. */
static _Atomic int observer_mode;
/* 1 while a participating rank is in the fixed-base handshake: the expected
 * per-attempt slot timeouts are not run errors and must not spam the log. */
static _Atomic int handshake_mode;

/* ---- start-up synchronisation --------------------------------------------
 * handshake (default): all participants submit the SAME fixed base and retry
 *   it (no rotation: independently rotating ranks could never agree on a base
 *   again). The retry period HANDSHAKE_PROBE_NS is just above the RTL's 100 ms
 *   incomplete-batch reclaim, so a retry always finds the slot free; the FPGA
 *   completes the batch as soon as every participating bank holds that base
 *   inside one 100 ms window and broadcasts the result to all four ports. That
 *   first broadcast is the common "go" edge.
 * grid: legacy NTP wall-clock grid, selected with --sync=grid.
 */
enum { SYNC_HANDSHAKE = 0, SYNC_GRID = 1 };
#define SYNC_BASE 0x0fff00u          /* one fixed sync frame, inside warmup区 */
#define HANDSHAKE_PROBE_NS 130000000ull /* > RTL 100 ms stale, < 500 ms normal */
#define WARMUP_REGION_LO 0x0ffe00u   /* observers accept any base >= this */
#define WARMUP_REGION_HI 0x0fffffu

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec*1000000000ull + (uint64_t)t.tv_nsec;
}
static void clear_finish_files(const char *prefix, int a, int b) {
    char path[256];
    int rs[2]={a,b};
    for (int i=0;i<2;i++) {
        snprintf(path,sizeof path,"/tmp/%s.done.%d",prefix,rs[i]); unlink(path);
        snprintf(path,sizeof path,"/tmp/%s.clean.%d",prefix,rs[i]); unlink(path);
    }
    snprintf(path,sizeof path,"/tmp/%s.release",prefix); unlink(path);
}
/* Machine-local two-rank barrier:
 *   secondary: write done, wait for release (then fini and write clean)
 *   primary:   wait for peer done, write release, wait for peer clean
 * The DPDK primary must outlive the secondary EAL user.
 */
static int finish_barrier(const char *prefix, int rank, int is_primary, int peer) {
    char own[256], release[256], peerdone[256];
    snprintf(own, sizeof own, "/tmp/%s.done.%d", prefix, rank);
    snprintf(release, sizeof release, "/tmp/%s.release", prefix);
    snprintf(peerdone, sizeof peerdone, "/tmp/%s.done.%d", prefix, peer);
    const uint64_t deadline=(uint64_t)time(NULL)+30;
    if (is_primary) {
        for (;;) {
            if (access(peerdone,F_OK)==0) break;
            if ((uint64_t)time(NULL)>=deadline) return 0;
            usleep(10000);
        }
        FILE *f=fopen(release,"w"); if (f) fclose(f);
        return 1;
    }
    FILE *f=fopen(own,"w"); if (f) fclose(f);
    while (access(release,F_OK)!=0) {
        if ((uint64_t)time(NULL)>=deadline) return 0;
        usleep(10000);
    }
    return 1;
}
static void finish_secondary_ack(const char *prefix, int rank) {
    char path[256];
    snprintf(path,sizeof path,"/tmp/%s.clean.%d",prefix,rank);
    FILE *f=fopen(path,"w"); if (f) fclose(f);
}
static int finish_primary_wait_clean(const char *prefix, int peer) {
    char path[256];
    snprintf(path,sizeof path,"/tmp/%s.clean.%d",prefix,peer);
    const uint64_t deadline=(uint64_t)time(NULL)+30;
    for (;;) {
        if (access(path,F_OK)==0) return 1;
        if ((uint64_t)time(NULL)>=deadline) return 0;
        usleep(10000);
    }
}
static void done_cb(const sfp_job_t *j, int status, void *opaque);
static void cleanup_hugepage_files(const char *prefix);
/* Coordinated abort: a rank may leave only after the machine-local peer has
 * also stopped, because the DPDK primary's EAL cleanup destroys shared
 * resources the secondary still uses (a primary that finis first while the
 * secondary is still in its warmup/finish loop crashes the secondary).
 * Mirrors the normal finish order: barrier, secondary fini + clean ack,
 * primary wait-clean + fini + cleanup. */
static void abort_with_peer(const char *prefix, int rank, int primary, int peer) {
    (void)finish_barrier(prefix, rank, primary, peer);
    if (!primary) {
        sfp_fini();
        finish_secondary_ack(prefix, rank);
    } else {
        (void)finish_primary_wait_clean(prefix, peer);
        sfp_fini();
        cleanup_hugepage_files(prefix);
        clear_finish_files(prefix, rank, peer);
    }
}
/* Keep-alive step: submit the next job of the continued main stream
 * (base sequence 0x100000 + n*16, n >= requests) into a free buffer.
 * Bounded EAGAIN retry; on failure the buffer is released and the error
 * latch re-armed (a peer bank that disappeared latches G.failed -> -EIO).
 * The submit rate self-limits to the 16-deep window's completion rate. */
static void ka_step(int rank, app_buffer_t *b, uint32_t kbase,
                    unsigned rounds, int workers,
                    uint32_t *kbase_next, unsigned *kn) {
    if (atomic_load_explicit(&b->busy,memory_order_acquire)) return;
    for (unsigned k=0;k<rounds*ENTRIES;k++) b->in[k]=0.0;
    sfp_job_t j={kbase,rounds,(uint16_t)workers};
    atomic_store(&b->busy,1);
    int rc; uint64_t sg=now_ns()+100000000ull;
    while ((rc=sfp_submit(rank,b->in,b->out,&j,done_cb,b)) == -EAGAIN)
        if (now_ns()>sg) break; else rte_pause();
    if (rc) { atomic_store(&b->busy,0); if (rc==-EIO) sfp_rearm(); return; }
    *kbase_next=kbase+MAX_ROUNDS; (*kn)++;
}
/* DPDK 21.11 keeps legacy hugetlbfs map files unless --huge-unlink is used.
 * That option is unsafe for a multi-process EAL setup, so remove only this
 * completed job's files after every rank on this machine has left EAL. */
static void cleanup_hugepage_files(const char *prefix) {
    DIR *dir = opendir("/dev/hugepages");
    if (!dir) return;
    char stem[96];
    int n = snprintf(stem, sizeof stem, "%smap_", prefix);
    if (n <= 0 || (size_t)n >= sizeof stem) { closedir(dir); return; }
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, stem, (size_t)n) != 0) continue;
        char path[256];
        int m = snprintf(path, sizeof path, "/dev/hugepages/%s", ent->d_name);
        if (m > 0 && (size_t)m < sizeof path) (void)unlink(path);
    }
    closedir(dir);
}
static uint64_t now_rt_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (uint64_t)t.tv_sec*1000000000ull + (uint64_t)t.tv_nsec;
}
static void sleep_until_rt(uint64_t abs_ns) {
    struct timespec t={.tv_sec=(time_t)(abs_ns/1000000000ull),
                       .tv_nsec=(long)(abs_ns%1000000000ull)};
    while (clock_nanosleep(CLOCK_REALTIME,TIMER_ABSTIME,&t,NULL) == EINTR) {}
}
static void done_cb(const sfp_job_t *j, int status, void *opaque) {
    (void)j;
    app_buffer_t *buffer=opaque;
    buffer->status=status;
    if (status != STATUS_OK && !observer_mode) {
        unsigned errors=atomic_fetch_add(&error_count, 1);
        /* Handshake retries time out on purpose while peers are still
         * arriving; count them (they are discarded before the measured phase)
         * but keep the log readable. */
        if (errors<8 && !handshake_mode) fprintf(stderr, "completion status=%d\n", status);
    }
    atomic_fetch_add(&done_count, 1);
    atomic_store_explicit(&buffer->busy, 0, memory_order_release);
}
static double sample(unsigned *state, uint32_t id, unsigned pos) {
    *state = *state * 1664525u + 1013904223u;
    double u = (double)(*state) / 4294967296.0;
    double span = ((pos + id) % 100 == 0) ? 460.0 : 20.0;
    return (u - .5) * span;
}
int main(int argc, char **argv) {
    int rank=-1,cpu=-1,dpcpu=-1,primary=0,peer=-1,requests=3000000;
    unsigned rounds=16, workers=4, seed=1, verify_const=0;
    int sync_mode=SYNC_HANDSHAKE;
    const char *bdfs_arg=NULL, *ranks_arg=NULL;
    char bdf0[16]={0}, bdf1[16]={0};
    const char *bdfs[2]={bdf0,bdf1};
    int local_ranks[2]={-1,-1};
    char prefix[48]; snprintf(prefix,sizeof prefix,"sfp-mm-%ld",(long)getpid());
    static const struct option opts[]={
        {"rank",1,0,'r'},{"bdfs",1,0,'b'},{"ranks",1,0,'k'},{"primary",0,0,'P'},
        {"peer",1,0,'x'},{"cpu",1,0,'c'},{"dpdk-cpu",1,0,'d'},
        {"requests",1,0,'n'},{"rounds",1,0,'o'},{"workers",1,0,'w'},
        {"seed",1,0,'s'},{"file-prefix",1,0,'f'},{"verify-const",0,0,'v'},
        {"sync",1,0,'S'},{0,0,0,0}};
    int ch; while ((ch=getopt_long(argc,argv,"r:b:k:Px:c:d:n:o:w:s:f:S:v",opts,NULL))!=-1) {
        switch(ch) {
        case 'r':rank=atoi(optarg);break;
        case 'b':bdfs_arg=optarg;break;
        case 'k':ranks_arg=optarg;break;
        case 'P':primary=1;break;
        case 'x':peer=atoi(optarg);break;
        case 'c':cpu=atoi(optarg);break;
        case 'd':dpcpu=atoi(optarg);break;
        case 'n':requests=atoi(optarg);break;
        case 'o':rounds=(unsigned)atoi(optarg);break;
        case 'w':workers=(unsigned)atoi(optarg);break;
        case 's':seed=(unsigned)strtoul(optarg,NULL,0);break;
        case 'f':snprintf(prefix,sizeof prefix,"%s",optarg);break;
        case 'v':verify_const=1;break;
        case 'S':
            if (!strcmp(optarg,"handshake")) sync_mode=SYNC_HANDSHAKE;
            else if (!strcmp(optarg,"grid")) sync_mode=SYNC_GRID;
            else { fputs("--sync must be handshake or grid\n",stderr); return 2; }
            break;
        default:return 2;
        }
    }
    /* Parse and validate the machine description. */
    if (rank<0 || rank>3 || peer<0 || peer>3 || peer==rank ||
        !bdfs_arg || !ranks_arg) {
        fputs("usage: --rank 0..3 --bdfs BDF0,BDF1 --ranks R0,R1 "
              "[--primary] --peer P --cpu N --dpdk-cpu N "
              "[--requests N] [--rounds N] [--workers N] [--seed N] [--file-prefix P]\n",stderr);
        return 2;
    }
    {
        const char *comma=strchr(bdfs_arg,',');
        if (!comma || (size_t)(comma-bdfs_arg)>=sizeof bdf0 || strlen(comma+1)>=sizeof bdf1)
            return 2;
        memcpy(bdf0,bdfs_arg,(size_t)(comma-bdfs_arg)); bdf0[comma-bdfs_arg]='\0';
        strcpy(bdf1,comma+1);
        comma=strchr(ranks_arg,',');
        if (!comma) return 2;
        char *end;
        local_ranks[0]=strtol(ranks_arg,&end,10);
        if (end!=comma) return 2;
        local_ranks[1]=strtol(comma+1,&end,10);
        if (*end) return 2;
        if (local_ranks[0]<0 || local_ranks[0]>3 || local_ranks[1]<0 || local_ranks[1]>3 ||
            local_ranks[0]==local_ranks[1]) return 2;
        if (local_ranks[0]!=rank && local_ranks[1]!=rank) return 2;
    }
    if (cpu<0 || cpu>=CPU_SETSIZE || dpcpu<0 || dpcpu>=CPU_SETSIZE ||
        rounds<1 || rounds>MAX_ROUNDS || workers<2 || workers>4 ||
        requests<1 || (uint64_t)requests>(UINT32_MAX-0x100000u)/MAX_ROUNDS ||
        strspn(prefix,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")!=strlen(prefix)) {
        fprintf(stderr,"invalid benchmark arguments\n"); return 2;
    }
    app_buffer_t buffers[MAX_WINDOW]={0};
    for (unsigned i=0;i<MAX_WINDOW;i++) {
        buffers[i].in=aligned_alloc(64,(size_t)rounds*ENTRIES*sizeof(double));
        buffers[i].out=aligned_alloc(64,(size_t)rounds*ENTRIES*sizeof(double));
        if (!buffers[i].in || !buffers[i].out) return 1;
    }
    if (primary) clear_finish_files(prefix, rank, peer);
    cpu_set_t app_set;
    CPU_ZERO(&app_set); CPU_SET((unsigned)cpu, &app_set);
    if (sched_setaffinity(0, sizeof app_set, &app_set) != 0) {
        perror("sched_setaffinity"); return 1;
    }
    sfp_cfg_t cfg={.rank=rank,.is_primary=primary,.local_bdfs=bdfs,.local_ranks=local_ranks,
                   .app_cpu=cpu,.dpdk_cpu=dpcpu,.require_link=1,.window=MAX_WINDOW,
                   .timeout_ms=500,.file_prefix=prefix,.local_peer=peer};
    if (sfp_init(&cfg)) { fprintf(stderr,"sfp_init failed\n"); return 1; }
    /* Start-up synchronisation (--sync, default handshake).
     *
     * handshake: each participating rank submits ONE job at the fixed base
     *   SYNC_BASE and retries the SAME base until its completion is OK. The
     *   FPGA completes the batch only when all `workers` banks hold that base
     *   inside one 100 ms window and then broadcasts the result to all four
     *   ports; every rank sees that first broadcast within microseconds, so it
     *   is the common "go" edge. The retry period is > the RTL's 100 ms
     *   incomplete-batch reclaim, so a new attempt always finds the slot free.
     *   No base rotation (ranks that rotated independently would never agree)
     *   and no wall clock.
     *
     * grid: the legacy NTP-aligned 600 ms absolute wall-clock grid. All ranks
     *   join the same grid point and one OK tick is the common go edge.
     *
     * Both modes are excluded from the measured statistics. */
    unsigned state=seed+rank;
    const uint64_t tick_ns=600000000ull;
    const uint64_t rt_deadline=now_rt_ns()+60000000000ull;
    int warm_ok=0;
    if ((unsigned)rank >= workers) {
        /* Non-participating rank (rank >= workers): sends no data, so it
         * cannot form a batch on its own. The tick-grid warmup below is only
         * meaningful for it while the participants' grid phase stays in
         * sync; if the participants slip once (late FPGA port wake-up, a
         * missed tick), the non-participant's fixed base sequence
         * (0x0FFF00+i*16) can never line up again with what the
         * participants send next (verify 0x0FFE00+i*16, main 0x100000+n*16)
         * and its warmup is lost for the whole 60 s deadline. Instead it
         * simply waits for ANY valid result broadcast: that validates the
         * one property it needs (FPGA -> this port delivery works) and is
         * self-healing regardless of when the first frame arrives. */
        uint32_t lb=0;
        while (now_rt_ns() < rt_deadline) {
            lb=sfp_latest_base();
            if (lb >= WARMUP_REGION_LO) break;
            usleep(1000);
        }
        warm_ok = (lb >= WARMUP_REGION_LO);
        if (warm_ok)
            fprintf(stderr,"rank=%d passive observer: first FPGA frame base=%u t=%lldms\n",
                    rank,lb,(long long)(now_rt_ns()/1000000ull));
    } else if (sync_mode == SYNC_GRID) for (;;) {
        uint64_t now_rt=now_rt_ns();
        if (now_rt>=rt_deadline) break;
        uint64_t T=(now_rt/tick_ns+1)*tick_ns;
        if (T>rt_deadline) break;
        sleep_until_rt(T);
        uint32_t wbase=(uint32_t)(0x0fff00ull + ((T/tick_ns)%8)*16ull);
        sfp_job_t warm={wbase,(uint16_t)rounds,(uint16_t)workers};
        app_buffer_t *first=&buffers[0];
        for (unsigned k=0;k<rounds*ENTRIES;k++) first->in[k]=sample(&state,wbase,k);
        sfp_rearm();
        unsigned err_before=atomic_load(&error_count);
        atomic_store(&first->busy, 1);
        uint64_t guard=now_ns()+2000000000ull;
        int wrc;
        while ((wrc=sfp_submit(rank,first->in,first->out,&warm,done_cb,first)) == -EAGAIN) {
            if (now_ns()>guard) { fprintf(stderr,"warmup submit stuck\n"); return 1; }
            rte_pause();
        }
        if (wrc) { fprintf(stderr,"warmup submit failed: %d\n",wrc); return 1; }
        while (atomic_load_explicit(&first->busy,memory_order_acquire)) {
            if (now_ns()>guard) { fprintf(stderr,"warmup completion stuck\n"); return 1; }
        }
        {
            unsigned e=atomic_load(&error_count);
            int ok=(e==err_before);
            /* The batch that completed is the same broadcast for every rank,
             * so the first OK tick is common to all ranks and is a clean
             * start edge for the measured run. */
            if (ok) warm_ok=1;
            fprintf(stderr,"rank=%d warm t=%lldms base=%u -> %s (err_delta=%u)\n",
                    rank,(long long)(now_rt_ns()/1000000ull),wbase,ok?"OK":"fail",(unsigned)(e-err_before));
            if (ok) break;
        }
    } else {
        /* Fixed-base handshake (default). One frame occupies one of the 16
         * hardware batch slots, so the sync needs exactly ONE job per rank.
         * Each rank retries the same base; the FPGA only emits the broadcast
         * when every participating bank holds that base inside one 100 ms
         * window, so the first broadcast proves that all participants are
         * live and in the same slot. Every rank receives it within a few
         * microseconds -> a common, clock-free "go" edge. */
        app_buffer_t *first=&buffers[0];
        const uint32_t wbase=SYNC_BASE;
        unsigned tries=0;
        handshake_mode=1;
        while (now_rt_ns() < rt_deadline) {
            for (unsigned k=0;k<rounds*ENTRIES;k++) first->in[k]=sample(&state,wbase,k);
            first->status=-1;
            atomic_store(&first->busy, 1);
            /* Short per-slot timeout: a failed attempt must free the buffer in
             * ~130 ms (not the 500 ms normal timeout) and must not latch the
             * library's failed flag. 130 ms > the RTL's 100 ms slot reclaim, so
             * the next attempt at the same base finds the slot free. */
            sfp_probe_timeout(HANDSHAKE_PROBE_NS);
            sfp_rearm();
            sfp_job_t warm={wbase,(uint16_t)rounds,(uint16_t)workers};
            uint64_t guard=now_ns()+2000000000ull;
            int wrc;
            while ((wrc=sfp_submit(rank,first->in,first->out,&warm,done_cb,first)) == -EAGAIN) {
                if (now_ns()>guard) { fprintf(stderr,"handshake submit stuck\n"); return 1; }
                rte_pause();
            }
            if (wrc) { fprintf(stderr,"handshake submit failed: %d\n",wrc); return 1; }
            while (atomic_load_explicit(&first->busy,memory_order_acquire)) {
                if (now_ns()>guard) { fprintf(stderr,"handshake completion stuck\n"); return 1; }
                rte_pause();
            }
            tries++;
            if (first->status==STATUS_OK) {
                warm_ok=1;
                fprintf(stderr,"rank=%d handshake OK t=%lldms base=%u tries=%u\n",
                        rank,(long long)(now_rt_ns()/1000000ull),wbase,tries);
                break;
            }
            /* Expected timeout: at least one peer is not in the slot yet.
             * Re-arm and retry the SAME base (no rotation). */
            if ((tries & 7u)==1u) {
                sfp_stats_t hst; sfp_stats(&hst);
                fprintf(stderr,"rank=%d handshake retry %u t=%lldms latest_base=%u timeout=%lu late=%lu invalid=%lu\n",
                        rank,tries,(long long)(now_rt_ns()/1000000ull),sfp_latest_base(),
                        hst.timeout,hst.late,hst.invalid);
            }
        }
        sfp_probe_timeout(0); /* restore the normal 500 ms slot timeout */
        handshake_mode=0;
    }
    if (!warm_ok) {
        fprintf(stderr,"rank=%d warmup never completed; aborting\n",rank);
        abort_with_peer(prefix, rank, primary, peer);
        return 1;
    }
    fprintf(stderr,"rank=%d warm done t=%lldms (post-warmup phases follow)\n",rank,(long long)(now_rt_ns()/1000000ull));
    /* Optional end-to-end numeric self-check: every entry of every rank is a
     * constant (double)rank, so the FPGA sum over the `workers` participating
     * banks must equal workers*(workers-1)/2 on every returned entry. This
     * also validates the port<->worker mapping for 2/3-worker modes. */
    if (verify_const) {
        double expect = (double)workers * (double)(workers-1) / 2.0;
        if ((unsigned)rank >= workers) {
            /* Passive observer: the FPGA broadcasts the identical frame to
             * every port, so the participants' numeric check covers this
             * rank's view as well (and the tick-coupled check here would
             * suffer the same grid-phase fragility as the warmup). */
            printf("rank=%d verify-const: skipped (passive observer; %d participants validate the FPGA sum)\n",
                   rank,workers);
        } else {
        double cv = (double)rank;
        int vfail = 0;
        for (unsigned i=0;i<16 && !vfail;i++) {
            sfp_job_t vj={(uint32_t)(0x0ffe00u + i*16u),(uint16_t)rounds,(uint16_t)workers};
            app_buffer_t *b=&buffers[i%MAX_WINDOW];
            for (unsigned k=0;k<rounds*ENTRIES;k++) b->in[k]=cv;
            int vok=0;
            for (unsigned tr=0; tr<6 && !vok; tr++) {
                b->status=-1;
                atomic_store(&b->busy, 1);
                sfp_rearm();
                int vrc; uint64_t vguard=now_ns()+3000000000ull;
                while ((vrc=sfp_submit(rank,b->in,b->out,&vj,done_cb,b)) == -EAGAIN) {
                    if (now_ns()>vguard) break;
                    rte_pause();
                }
                if (vrc) { fprintf(stderr,"rank=%d verify job %u submit rc=%d (retry %u)\n",rank,i,vrc,tr); continue; }
                while (atomic_load_explicit(&b->busy,memory_order_acquire)) {
                    if (now_ns()>vguard) break;
                }
                if (b->status == STATUS_OK) vok=1;
            }
            if (!vok) {
                fprintf(stderr,"rank=%d verify job %u failed after retries status=%d\n",rank,i,b->status); vfail=1; break;
            }
            for (unsigned k=0;k<rounds*ENTRIES;k++) {
                if (fabs(b->out[k]-expect) > 1e-9) {
                    fprintf(stderr,"rank=%d verify MISMATCH entry %u: got %.17g want %.17g\n",
                            rank,k,b->out[k],expect);
                    vfail=1; break;
                }
            }
        }
        if (vfail) { fprintf(stderr,"rank=%d verify-const FAILED\n",rank); abort_with_peer(prefix, rank, primary, peer); return 1; }
        printf("rank=%d verify-const passed: 16 jobs x %u rounds x 64 entries all == %.1f\n",
               rank,rounds,expect);
        } /* participant verify-const */
    }
    sfp_stats_t before; sfp_stats(&before);
    atomic_store(&done_count, 0);
    unsigned warm_errors=atomic_exchange(&error_count, 0);
    struct timespec measure_start, measure_end;
    clock_gettime(CLOCK_MONOTONIC, &measure_start);
    unsigned outstanding=0, max_outstanding=0;
    int submit_error=0;
    uint64_t base=0x100000;
    int n_start=0, n_first=0, n_hi=requests;
    if ((unsigned)rank >= workers) {
        observer_mode = 1;
        /* Non-participating rank: join the participants' main stream, which
         * is usually already flowing (cross-machine delay; early job-0
         * broadcasts are already gone). The FPGA broadcasts each job's
         * result EXACTLY ONCE, at line rate (~152k jobs/s, 6.6 us/job), so
         * the joiner's first 16 slots must be INFLIGHT before their frames
         * arrive. Two hazards make a tight-margin probe unreliable in
         * practice: (a) the poll may see a stale last_base, so the first
         * probe frames may already be gone; (b) a scheduler preemption
         * between probe submits lets some frames beat their slot marks.
         * Both are handled structurally: the probe targets TWO windows
         * (32 jobs) ahead of the seen position, so the first probe frame
         * is ~211 us out and even a ~150 us preemption cannot beat the
         * marks; and if the poll was stale (frames already gone), the
         * probe's first slot times out in 300 us and it RETRIES from a
         * fresh position. The main loop starts as soon as the FIRST probe
         * slot lands OK: the other 15 probe slots are still in flight and
         * form the initial 16-deep pipeline (waiting for all 16 would
         * consume the pipeline margin and lose every main-loop frame).
         * Probe slots use a 300 us short timeout (sfp_probe_timeout;
         * covers the 211 us first-frame distance plus submit preemption)
         * and no in[] fill (a non-participant never transmits, so marking
         * takes ~2 us/job). Probe timeouts are expected noise: they do
         * not latch the failed flag and are cleared before the measured
         * loop. If a frame is still lost later (link blip), the main loop
         * detects the timed-out slot and skips ahead to the live frontier
         * (counted as a skip event, worth extra rc slack). The 30 s
         * window covers the participants' verify phase, the measured run
         * start, and their 5 s keep-alive grace (late FPGA port wake-up).
         * If this port wakes up AFTER the measured window has already
         * passed (short runs: a 1000-job window is 7 ms, the remote FPGA
         * port takes ~300 ms to wake), the participants' keep-alive stream
         * continues the same base sequence for ~5 s: the join then targets
         * that stream (n_hi extends past `requests`) and the rc requires a
         * 1000-job sample instead of the measured-window quota. */
        uint64_t jt=now_rt_ns();
        unsigned tries=0;
        uint32_t jlb=0; uint64_t jstall=0;
        for (;;) {
            uint32_t lb=sfp_latest_base();
            if (lb>=0x100000u) {
                /* Stream-dead check: if the frontier stopped advancing,
                 * no probe can ever land (the stream is over, not just
                 * slow) -> fail fast instead of retrying 30 s. */
                if (lb!=jlb) { jlb=lb; jstall=0; }
                else if (!jstall) jstall=now_rt_ns();
                else if (now_rt_ns()-jstall > 1000000000ull) {
                    fprintf(stderr,"rank=%d non-participant: stream ended before join\n",rank);
                    abort_with_peer(prefix, rank, primary, peer);
                    return 1;
                }
                int n0=(int)(((uint64_t)lb-0x100000ull)/MAX_ROUNDS + 2*MAX_WINDOW);
                if (n0>=requests && n_hi==requests) {
                    /* Measured window is over: join the keep-alive stream. */
                    n_hi=requests+1000000;
                }
                if (n0>=n_hi) { n_start=n_hi; n_first=n_hi; break; }
                tries++;
                sfp_probe_timeout(300000ull);
                int probed=0;
                for (int m=0;m<MAX_WINDOW;m++) {
                    app_buffer_t *b=&buffers[(unsigned)(n0+m)%MAX_WINDOW];
                    if (atomic_load_explicit(&b->busy,memory_order_acquire)) break;
                    sfp_job_t j={(uint32_t)(base+(uint64_t)(n0+m)*MAX_ROUNDS),rounds,(uint16_t)workers};
                    atomic_store(&b->busy,1);
                    int rc; uint64_t sg=now_ns()+100000000ull;
                    while ((rc=sfp_submit(rank,b->in,b->out,&j,done_cb,b)) == -EAGAIN)
                        if (now_ns()>sg) break; else rte_pause();
                    if (rc) { atomic_store(&b->busy,0); if (rc==-EIO) sfp_rearm(); probed=-1; break; }
                    probed++;
                }
                if (probed==MAX_WINDOW) {
                    /* Start as soon as the FIRST probe slot settles: the
                     * remaining 15 frames (<=210 us apart from it) then
                     * keep the pipeline full for the main loop's first 15
                     * submits. If the first slot's frame was already gone
                     * (stale poll), it times out at 300 us -> retry. */
                    app_buffer_t *b0=&buffers[(unsigned)n0%MAX_WINDOW];
                    uint64_t w0=now_ns();
                    while (atomic_load_explicit(&b0->busy,memory_order_acquire))
                        if (now_ns()-w0 > 600000ull) break;
                    if (b0->status==STATUS_OK) {
                        n_start=n0; n_first=n0+MAX_WINDOW;
                        /* Main-loop slots: 2 ms timeout (a matching frame
                         * arrives within 105 us of the previous one, so 2 ms
                         * only ever fires on a genuinely lost frame, and it
                         * bounds the skip-ahead loss to ~270 jobs at line
                         * rate instead of ~67k with the 500 ms default). */
                        sfp_probe_timeout(2000000ull);
                        fprintf(stderr,"rank=%d non-participant: joined %s at job %d (probe try %u)\n",
                                rank, n0>=requests?"keep-alive stream":"main stream",n_start,tries);
                        atomic_exchange(&error_count, 0); /* drop probe-timeout noise */
                        atomic_store(&done_count, 0);    /* recount from the main loop */
                        break;
                    }
                    /* b0 lost: wait for the other 15 probe slots to free
                     * (each settles within its 300 us timeout) and retry
                     * from a fresh position. */
                    uint64_t w_end=now_ns()+700000ull;
                    for (unsigned m=1;m<MAX_WINDOW;m++) {
                        app_buffer_t *b=&buffers[(unsigned)(n0+m)%MAX_WINDOW];
                        while (atomic_load_explicit(&b->busy,memory_order_acquire))
                            if (now_ns()>w_end) break;
                    }
                }
                sfp_probe_timeout(0);
            }
            if (now_rt_ns()-jt > 30000000000ull) {
                fprintf(stderr,"rank=%d non-participant: main stream never started\n",rank);
                abort_with_peer(prefix, rank, primary, peer);
                return 1;
            }
            rte_pause();
        }
    }
    /* Warmup errors are not fatal: the retry loop above already guarantees a
     * clean batch before the measured run. Non-participants start at n_first
     * (their 16-job join probe, if it succeeded, already covered n_start..
     * n_first-1) and skip the in[] fill: a non-participant never transmits,
     * so the worker marks its slots INFLIGHT without touching the input. */
    int n_lo = ((unsigned)rank >= workers) ? n_first : n_start;
    unsigned eio_streak=0, skip_events=0;
    uint32_t m_lb=0; uint64_t m_stall=0;
    for (int n=n_lo;n<n_hi;n++) {
        /* The same ring index is reused only after its callback. At most
         * 16 consecutive hardware generations can be outstanding. */
        app_buffer_t *buffer=&buffers[(unsigned)n % MAX_WINDOW];
        while (atomic_load_explicit(&buffer->busy,memory_order_acquire)) rte_pause();
        if ((unsigned)rank >= workers) {
            /* Stream-end detection: the frontier (last broadcast job) stops
             * advancing when the keep-alive finishes or the FPGA goes quiet.
             * Without this, a late-joining observer would keep submitting
             * doomed jobs for the whole n_hi range. */
            uint32_t lb3=sfp_latest_base();
            if (lb3!=m_lb) { m_lb=lb3; m_stall=0; }
            else if (!m_stall) m_stall=now_ns();
            else if (now_ns()-m_stall > 100000000ull) {
                fprintf(stderr,"rank=%d non-participant: stream ended at job %d\n",rank,n);
                break;
            }
        }
        if ((unsigned)rank >= workers && buffer->status==STATUS_TIMEOUT) {
            /* The previous occupant of this ring slot timed out: its
             * broadcast is gone, and job n's frame (16 jobs later) is very
             * likely gone as well (it arrived before its slot could be
             * marked). If the live frontier has passed job n, skip ahead
             * to 16-deep ahead of it. With the 2 ms observer slot timeout
             * the frontier is at most ~270 jobs past job n at detection
             * time, so each demonstrated skip loses <=~300 jobs and earns
             * that much extra rc slack. */
            uint32_t lb2=sfp_latest_base();
            if (lb2>=0x100000u) {
                int n2=(int)(((uint64_t)lb2-0x100000ull)/MAX_ROUNDS + MAX_WINDOW) + 1;
                if (n2>n) { skip_events++; n=n2-1; continue; }
            }
        }
        if ((unsigned)rank < workers)
            for (unsigned k=0;k<rounds*ENTRIES;k++)
                buffer->in[k]=sample(&state,(uint32_t)n,k);
        sfp_job_t j={(uint32_t)(base+(uint32_t)n*MAX_ROUNDS),rounds,(uint16_t)workers};
        atomic_store(&buffer->busy, 1);
        int rc;
        while ((rc=sfp_submit(rank,buffer->in,buffer->out,&j,done_cb,buffer)) == -EAGAIN)
            rte_pause();
        while (rc == -EIO && (unsigned)rank >= workers && eio_streak < 8) {
            /* Observer: an earlier lost frame latched G.failed. Re-arm and
             * retry: the stream may well still be alive. A truly dead stream
             * costs one 500 ms slot timeout per job (the re-submitted job
             * times out and re-latches), so bound the streak at 8 (~4 s). */
            eio_streak++;
            sfp_rearm();
            while ((rc=sfp_submit(rank,buffer->in,buffer->out,&j,done_cb,buffer)) == -EAGAIN)
                rte_pause();
        }
        if (rc == 0) eio_streak=0;
        if (rc) { fprintf(stderr,"submit failed: %d\n",rc); atomic_store(&buffer->busy,0); submit_error=1; break; }
        /* Non-participants submitted 16 extra probe jobs before n_lo (their
         * completions are counted in done_count after the post-join reset). */
        outstanding=(unsigned)(n-n_lo)+1u+(n_first?(unsigned)MAX_WINDOW:0u)-atomic_load(&done_count);
        if (outstanding>max_outstanding) max_outstanding=outstanding;
    }
    for (unsigned i=0;i<MAX_WINDOW;i++)
        while (atomic_load_explicit(&buffers[i].busy,memory_order_acquire)) rte_pause();
    clock_gettime(CLOCK_MONOTONIC, &measure_end);
    sfp_stats_t st; sfp_stats(&st);
    double elapsed=(double)(measure_end.tv_sec-measure_start.tv_sec)+
        (double)(measure_end.tv_nsec-measure_start.tv_nsec)*1e-9;
    double useful=(elapsed>0.0 && st.completed>before.completed)
        ? (double)(st.completed-before.completed)*rounds*ENTRIES*8.0*8.0/elapsed/1e9 : 0.0;
    double wire=(elapsed>0.0 && st.tx_packets>before.tx_packets)
        ? (double)(st.tx_packets-before.tx_packets)*(FPGA_HEADER+rounds*ROUND_BYTES+24)*8.0/elapsed/1e9 : 0.0;
    uint64_t rtt_count=st.rtt_ns_count-before.rtt_ns_count;
    double rtt=rtt_count ? (double)(st.rtt_ns_sum-before.rtt_ns_sum)/rtt_count/1000.0 : 0.0;
    printf("rank=%d submitted=%lu completed=%lu tx=%lu rx=%lu invalid=%lu late=%lu timeout=%lu\n",
           rank,st.submitted,st.completed,st.tx_packets,st.rx_packets,st.invalid,st.late,st.timeout);
    printf("elapsed=%.6f s useful_result=%.3f Gbit/s wire_estimate=%.3f Gbit/s avg_rtt=%.3f us\n",
           elapsed,useful,wire,rtt);
    printf("window=16 max_outstanding=%u retained_tx_mbufs=%lu tx_reuse=%lu errors=%u warm_errors=%u\n",
           max_outstanding,st.mbuf_in_use,st.tx_reuse,atomic_load(&error_count),warm_errors);
    printf("tx_completed=%lu tx_batches=%lu tx_batch_max=%lu\n",st.tx_completed,st.tx_batches,st.tx_batch_max);
    /* Measured-phase errors are final here; anything that happens during the
     * keep-alive stream below (e.g. slot timeouts after a peer bank is gone)
     * is not a run error. */
    unsigned final_errors=atomic_load(&error_count);
    /* ---- finish ----
     * Participants keep the result stream flowing through the machine-local
     * finish barrier and for a 5 s grace after it, but only when non-
     * participants exist: a non-participant's FPGA port may come up seconds
     * after the measured run, and a stopped stream would leave it stuck at
     * "main stream never started". Keep-alive jobs continue the measured
     * base sequence (0x100000 + n*16, n >= requests), so they are
     * indistinguishable from the main stream for a late joiner. Excluded
     * from the measured statistics above. */
    int ka_done=0;
    if ((unsigned)rank < workers && workers < 4) {
        uint32_t kbase=(uint32_t)(0x100000ull + (uint64_t)requests*MAX_ROUNDS);
        unsigned kn=0;
        char f_own[256], f_rel[256], f_peer[256];
        snprintf(f_own,sizeof f_own,"/tmp/%s.done.%d",prefix,rank);
        snprintf(f_rel,sizeof f_rel,"/tmp/%s.release",prefix);
        snprintf(f_peer,sizeof f_peer,"/tmp/%s.done.%d",prefix,peer);
        const uint64_t t0=(uint64_t)time(NULL);
        int release_written=0;
        if (!primary) { FILE *f=fopen(f_own,"w"); if (f) fclose(f); }
        for (;;) { /* machine-local barrier, streaming while waiting */
            for (unsigned s=0;s<64;s++)
                ka_step(rank,&buffers[kn%MAX_WINDOW],kbase,rounds,workers,&kbase,&kn);
            int ok = primary ? (access(f_peer,F_OK)==0) : (access(f_rel,F_OK)==0);
            if (ok) {
                if (primary && !release_written) {
                    FILE *f=fopen(f_rel,"w"); if (f) fclose(f);
                    release_written=1;
                }
                break;
            }
            if ((uint64_t)time(NULL)-t0 >= 30) {
                fprintf(stderr,"rank=%d finish barrier timed out\n",rank);
                abort_with_peer(prefix, rank, primary, peer);
                return 1;
            }
        }
        uint64_t g0=now_ns();
        while (now_ns()-g0 < 5000000000ull) { /* 5 s grace, streaming */
            for (unsigned s=0;s<64;s++)
                ka_step(rank,&buffers[kn%MAX_WINDOW],kbase,rounds,workers,&kbase,&kn);
        }
        for (unsigned i=0;i<MAX_WINDOW;i++) { /* bounded drain */
            uint64_t d0=now_ns();
            while (atomic_load_explicit(&buffers[i].busy,memory_order_acquire))
                if (now_ns()-d0 > 1000000000ull) break;
        }
        ka_done=1;
    }
    if (!ka_done && !finish_barrier(prefix, rank, primary, peer)) {
        fprintf(stderr,"rank=%d finish barrier timed out\n",rank); return 1;
    }
    if (!primary) {
        sfp_fini();
        finish_secondary_ack(prefix, rank);
    } else {
        if (!finish_primary_wait_clean(prefix, peer)) {
            fprintf(stderr,"peer cleanup unconfirmed; preserve shared resources\n");
            return 1;
        }
        sfp_fini();
        cleanup_hugepage_files(prefix);
        clear_finish_files(prefix, rank, peer);
    }
    for (unsigned i=0;i<MAX_WINDOW;i++) { free(buffers[i].in); free(buffers[i].out); }
    /* Participants must complete exactly `requests`; non-participants join
     * mid-stream, so their count starts at n_start (a few may be lost at the
     * join edge). */
    uint64_t done=st.completed-before.completed;
    /* Observer quota: measured-window join -> cover requests-n_start;
     * keep-alive join (measured window already over when this port woke)
     * -> a 1000-job sample of the keep-alive stream; nothing left to
     * measure (stream over before join) -> 0. */
    uint64_t quota;
    if ((unsigned)rank >= workers && n_start>=n_hi) quota=0;
    else if ((unsigned)rank >= workers && n_start>=requests) quota=1000;
    else quota=(uint64_t)requests-(unsigned)n_start;
    int count_ok = ((unsigned)rank < workers)
        ? (done == (uint64_t)requests)
        : (done + 32 + 300ull*skip_events >= quota);
    return !submit_error && final_errors==0 && count_ok ? 0 : 1;
}
