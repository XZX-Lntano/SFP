#define _GNU_SOURCE
#include <getopt.h>
#include <mpi.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "dpdk_port.h"

typedef struct {
    uint8_t frame[MAX_FRAME];
    uint32_t base;
    uint64_t sequence;
    double started;
    int busy,sent;
} Pending;
static Pending slots[MAX_WINDOW];
static double now(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;
}
static void transmit_only(DpdkPort *port,int rank,unsigned workers,unsigned rounds,uint64_t batches) {
    uint8_t app[APP_HEADER+MAX_ROUNDS*ENTRIES*8]={0},frame[MAX_FRAME];
    const uint8_t dst[6]={255,255,255,255,255,255};
    app_header(app,MSG_REQUEST,0,rounds,workers,0);
    size_t len=fpga_frame(frame,app,0,dst,0xc0a80a01+rank,0xc0a80a65+rank);
    put16(frame+12,0x88b5); // Experimental Ethernet type: FPGA discards without aggregation.
    struct rte_eth_stats before={0},after={0};rte_eth_stats_get(port->id,&before);
    MPI_Request ready;MPI_Ibarrier(MPI_COMM_WORLD,&ready);MPI_Wait(&ready,MPI_STATUS_IGNORE);
    double start=now(),deadline=start+10+batches*len*8/1e9;
    uint64_t sent=0;
    if((unsigned)rank<workers) {
        while(sent<batches) {
            struct rte_mbuf *packets[32];unsigned count=0;
            while(count<32 && sent+count<batches) {
                struct rte_mbuf *m=rte_pktmbuf_alloc(port->pool);if(!m) break;
                void *data=rte_pktmbuf_append(m,len);
                if(!data) {rte_pktmbuf_free(m);break;}
                memcpy(data,frame,len);packets[count++]=m;
            }
            if(count) {
                unsigned accepted=rte_eth_tx_burst(port->id,0,packets,count);
                sent+=accepted;port->tx_packets+=accepted;
                if(accepted<count) port->tx_short++;
                for(unsigned i=accepted;i<count;i++) rte_pktmbuf_free(packets[i]);
            }
            if(now()>deadline) MPI_Abort(MPI_COMM_WORLD,6);
        }
        // Include the descriptor drain; count completed NIC transmissions, not enqueue calls.
        do {
            rte_eth_stats_get(port->id,&after);
            if(now()>deadline) MPI_Abort(MPI_COMM_WORLD,6);
        } while(after.opackets-before.opackets<batches);
        double elapsed=now()-start;
        printf("TX ONLY rank%d: NIC packets=%lu bytes=%lu elapsed=%.6f wire estimate=%.3f Gbit/s; no aggregation/results\n",
            rank,after.opackets-before.opackets,after.obytes-before.obytes,elapsed,batches*(len+24)*8/elapsed/1e9);
    }
    MPI_Ibarrier(MPI_COMM_WORLD,&ready);MPI_Wait(&ready,MPI_STATUS_IGNORE);
}
static void next_frame(Pending *p,int rank,unsigned rounds) {
    for(unsigned r=0;r<rounds;r++) put32(p->frame+48+r*ROUND_BYTES,p->base+r);
    put64(p->frame+56,100000u*(rank+1)+(uint64_t)p->base);
    p->busy=1;p->sent=0;p->started=now();
}
int main(int argc,char **argv) {
    MPI_Init(&argc,&argv);
    int rank,size;MPI_Comm_rank(MPI_COMM_WORLD,&rank);MPI_Comm_size(MPI_COMM_WORLD,&size);
    unsigned workers=4,rounds=16,window=16;uint64_t batches=100000;uint32_t seed=0x10000;int tx_only=0;
    double timeout=1;
    const char *bdf[4]={"0000:01:00.3","0000:01:00.2","0000:01:00.1","0000:01:00.0"};
    static struct option opts[]={{"workers",1,0,'w'},{"rounds",1,0,'r'},{"window",1,0,'W'},
        {"batches",1,0,'b'},{"timeout",1,0,'t'},{"request-id-start",1,0,'i'},
        {"bdf0",1,0,1000},{"bdf1",1,0,1001},{"bdf2",1,0,1002},{"bdf3",1,0,1003},{"tx-only",0,0,'T'},{0,0,0,0}};
    int opt;
    while((opt=getopt_long(argc,argv,"",opts,NULL))!=-1) {
        if(opt=='w') workers=atoi(optarg);else if(opt=='r') rounds=atoi(optarg);
        else if(opt=='W') window=atoi(optarg);else if(opt=='b') batches=strtoull(optarg,NULL,0);
        else if(opt=='t') timeout=atof(optarg);else if(opt=='i') seed=strtoul(optarg,NULL,0);
        else if(opt=='T') tx_only=1;
        else if(opt>=1000 && opt<=1003) bdf[opt-1000]=optarg;else MPI_Abort(MPI_COMM_WORLD,2);
    }
    if(size!=4 || workers<2 || workers>4 || rounds<1 || rounds>16 || !window || window>16 ||
        !batches || batches>(UINT32_MAX-seed)/256 || (seed&255) || timeout<=0) MPI_Abort(MPI_COMM_WORLD,2);
    cpu_set_t allowed;CPU_ZERO(&allowed);sched_getaffinity(0,sizeof allowed,&allowed);
    int cpu=-1;for(int i=0;i<CPU_SETSIZE;i++) if(CPU_ISSET(i,&allowed)) {cpu=i;break;}
    if(cpu<0) MPI_Abort(MPI_COMM_WORLD,2);
    cpu_set_t one;CPU_ZERO(&one);CPU_SET(cpu,&one);
    if(sched_setaffinity(0,sizeof one,&one)) MPI_Abort(MPI_COMM_WORLD,2);
    DpdkPort port={0};if(dpdk_open(&port,bdf,rank,cpu,1)) MPI_Abort(MPI_COMM_WORLD,3);
    if(tx_only) {transmit_only(&port,rank,workers,rounds,batches);dpdk_close(&port);MPI_Finalize();return 0;}
    size_t bytes=rounds*ENTRIES*8,frame_bytes=FPGA_HEADER+rounds*ROUND_BYTES;
    uint8_t app[APP_HEADER+MAX_ROUNDS*ENTRIES*8],expected[MAX_ROUNDS*ENTRIES*8];
    app_header(app,MSG_REQUEST,seed,rounds,workers,0);
    for(unsigned i=0;i<rounds*ENTRIES;i++) {
        put64(app+APP_HEADER+i*8,100000u*(rank+1)+i);
        uint64_t sum=0;for(unsigned w=0;w<workers;w++) sum+=100000u*(w+1)+i;
        put64(expected+i*8,sum);
    }
    const uint8_t dst[6]={255,255,255,255,255,255};
    for(unsigned i=0;i<window && i<batches;i++) {
        Pending *p=&slots[i];p->base=seed+i*16;p->sequence=i;
        fpga_frame(p->frame,app,0,dst,0xc0a80a01+rank,0xc0a80a65+rank);put16(p->frame+34,4000+rank);
        next_frame(p,rank,rounds);
    }
    // One startup synchronization only; the packet loop contains no MPI data movement.
    MPI_Request start_request;MPI_Ibarrier(MPI_COMM_WORLD,&start_request);
    int ready=0;while(!ready) MPI_Test(&start_request,&ready,MPI_STATUS_IGNORE);
    double start=now();
    for(unsigned i=0;i<window;i++) slots[i].started=start;
    uint64_t complete=0,errors=0;
    while(complete<batches) {
        struct rte_mbuf *tx[MAX_WINDOW];unsigned ids[MAX_WINDOW],count=0;
        for(unsigned i=0;i<window;i++) {
            Pending *p=&slots[i];if(!p->busy || p->sent) continue;
            if((unsigned)rank>=workers) {p->sent=1;continue;}
            struct rte_mbuf *m=rte_pktmbuf_alloc(port.pool);if(!m) break;
            uint8_t *b=(uint8_t *)rte_pktmbuf_append(m,frame_bytes);
            if(!b) {rte_pktmbuf_free(m);break;}
            memcpy(b,p->frame,frame_bytes);tx[count]=m;ids[count++]=i;
        }
        if(count) {
            unsigned sent=rte_eth_tx_burst(port.id,0,tx,count);port.tx_packets+=sent;
            if(sent<count) port.tx_short++;
            for(unsigned j=0;j<count;j++) {
                if(j<sent) slots[ids[j]].sent=1;else rte_pktmbuf_free(tx[j]);
            }
        }
        struct rte_mbuf *rx[32];unsigned n=rte_eth_rx_burst(port.id,0,rx,32);port.rx_packets+=n;
        for(unsigned j=0;j<n;j++) {
            uint8_t scratch[MAX_FRAME];
            unsigned len=rte_pktmbuf_pkt_len(rx[j]),got_rounds,got_workers;uint32_t base;
            const uint8_t *b=len<=MAX_FRAME?rte_pktmbuf_read(rx[j],0,len,scratch):NULL;
            if(b && fpga_result_view(b,len,&base,&got_rounds,&got_workers)) {
                unsigned i=(base>>4)&15;Pending *p=&slots[i];
                if(i<window && p->busy && p->base==base && got_rounds==rounds && got_workers==workers) {
                    int bad=get64(b+56)!=get64(expected)+(uint64_t)workers*base;
                    for(unsigned r=0;r<rounds;r++) {
                        unsigned skip=r?0:8;
                        bad|=memcmp(b+FPGA_HEADER+r*ROUND_BYTES+8+skip,
                                    expected+r*ENTRIES*8+skip,ENTRIES*8-skip)!=0;
                    }
                    if(bad) errors++;
                    complete++;p->sequence+=window;p->busy=0;
                    if(p->sequence<batches) {p->base+=256;next_frame(p,rank,rounds);}
                } else port.rx_invalid++;
            } else port.rx_invalid++;
            rte_pktmbuf_free(rx[j]);
        }
        for(unsigned i=0;i<window;i++) if(slots[i].busy && now()-slots[i].started>timeout) {
            struct rte_eth_stats stats={0};rte_eth_stats_get(port.id,&stats);
            fprintf(stderr,"rank%d: tx=%lu rx=%lu invalid=%lu NIC tx=%lu rx=%lu missed=%lu errors=%lu\n",
                    rank,port.tx_packets,port.rx_packets,port.rx_invalid,stats.opackets,stats.ipackets,stats.imissed,stats.ierrors);
            fprintf(stderr,"rank%d: timeout base=%u completed=%lu; stop all generators to avoid stale-slot reuse\n",rank,slots[i].base,complete);
            MPI_Abort(MPI_COMM_WORLD,6);
        }
    }
    double elapsed=now()-start,max_elapsed=0;uint64_t all_errors=0;
    MPI_Request requests[2];
    MPI_Ireduce(&elapsed,&max_elapsed,1,MPI_DOUBLE,MPI_MAX,0,MPI_COMM_WORLD,&requests[0]);
    MPI_Ireduce(&errors,&all_errors,1,MPI_UINT64_T,MPI_SUM,0,MPI_COMM_WORLD,&requests[1]);
    MPI_Waitall(2,requests,MPI_STATUSES_IGNORE);
    if(!rank) {
        printf("DPDK direct FPGA: batches=%lu rounds=%u workers=%u window=%u elapsed=%.6f errors=%lu\n",batches,rounds,workers,window,max_elapsed,all_errors);
        printf("useful result %.3f Gbit/s; per active input/output wire estimate %.3f Gbit/s\n",
               batches*bytes*8/max_elapsed/1e9,batches*(frame_bytes+24)*8/max_elapsed/1e9);
        puts("No application UDP or per-batch MPI transfer in this measurement; compare with end-to-end benchmark.");
    }
    dpdk_close(&port);MPI_Finalize();return errors?1:0;
}
