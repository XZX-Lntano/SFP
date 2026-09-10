#define _GNU_SOURCE
#include <stdio.h>
#include <mpi.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <unistd.h>
#include <time.h>
#include "dpdk_port.h"

static struct rte_mempool *pools[4];
static int process_rank;

static double startup_time(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;
}
static int verify_datapath(DpdkPort *p,int rank) {
    uint8_t app[APP_HEADER+ENTRIES*8]={0},frame[FPGA_HEADER+ROUND_BYTES];
    const uint8_t dst[6]={255,255,255,255,255,255};
    for(unsigned attempt=0;attempt<8;attempt++) {
        uint32_t base=0xfffffe00+attempt*16;
        app_header(app,MSG_REQUEST,base,1,4,0);
        size_t len=fpga_frame(frame,app,0,dst,0xc0a80a01+rank,0xc0a80a65+rank);
        struct rte_mbuf *m=rte_pktmbuf_alloc(p->pool);
        if(!m) return -1;
        void *data=rte_pktmbuf_append(m,len);
        if(!data) {rte_pktmbuf_free(m);return -1;}
        memcpy(data,frame,len);
        if(rte_eth_tx_burst(p->id,0,&m,1)!=1) rte_pktmbuf_free(m);
        int received=0,all_received=0;double deadline=startup_time()+0.25;
        while(!received && startup_time()<deadline) {
            struct rte_mbuf *rx[32];unsigned count=rte_eth_rx_burst(p->id,0,rx,32);
            for(unsigned j=0;j<count;j++) {
                uint8_t scratch[MAX_FRAME],values[MAX_ROUNDS*ENTRIES*8];
                unsigned n=rte_pktmbuf_pkt_len(rx[j]),rounds,workers;uint32_t id;
                const uint8_t *b=n<=MAX_FRAME?rte_pktmbuf_read(rx[j],0,n,scratch):NULL;
                if(b && fpga_result(b,n,values,&id,&rounds,&workers) && id==base && rounds==1 && workers==4) {
                    received=1;
                    for(unsigned k=0;k<ENTRIES*8;k++) if(values[k]) received=0;
                }
                rte_pktmbuf_free(rx[j]);
            }
        }
        MPI_Request request;
        MPI_Iallreduce(&received,&all_received,1,MPI_INT,MPI_MIN,MPI_COMM_WORLD,&request);
        MPI_Wait(&request,MPI_STATUS_IGNORE);
        if(all_received) {
            if(!rank) fprintf(stderr,"FPGA startup handshake passed on all four ports (attempt %u); excluded from benchmark\n",attempt+1);
            return 0;
        }
        // A missing contribution expires in 100 ms; do not reuse its slot.
        usleep(150000);
    }
    fprintf(stderr,"rank%d: FPGA startup handshake failed\n",rank);return -1;
}

static int configure_port(uint16_t id,int rank) {
    char pool_name[32];snprintf(pool_name,sizeof pool_name,"sfp-mbuf-%d",rank);
    struct rte_eth_dev_info info;
    if(rte_eth_dev_info_get(id,&info)<0 || info.max_mtu<JUMBO_MTU) return -1;
    struct rte_eth_conf conf={0};conf.rxmode.mtu=JUMBO_MTU;
    if(rte_eth_dev_configure(id,1,1,&conf)<0 || rte_eth_dev_set_mtu(id,JUMBO_MTU)<0) return -1;
    int socket=rte_eth_dev_socket_id(id);if(socket<0) socket=0;
    pools[rank]=rte_pktmbuf_pool_create(pool_name,511,32,0,9728+RTE_PKTMBUF_HEADROOM,socket);
    if(!pools[rank]) {fprintf(stderr,"mbuf pool: %s\n",rte_strerror(rte_errno));return -1;}
    // At most 16 result frames are in flight; keep the four jumbo rings cache resident.
    uint16_t rx=64,tx=128;
    if(rte_eth_dev_adjust_nb_rx_tx_desc(id,&rx,&tx)<0 ||
        rte_eth_rx_queue_setup(id,0,rx,socket,&info.default_rxconf,pools[rank])<0 ||
        rte_eth_tx_queue_setup(id,0,tx,socket,&info.default_txconf)<0 ||
        rte_eth_dev_start(id)<0 || rte_eth_promiscuous_enable(id)<0) return -1;
    return 0;
}

int dpdk_open(DpdkPort *p, const char *const bdfs[4], int rank, int cpu, int require_link) {
    process_rank=rank;
    char cores[32];
    snprintf(cores,sizeof cores,"%d",cpu);
    char *args[]={"mpi_fpga_bridge","-l",cores,"-n","4",
                  "-a",(char *)bdfs[0],"-a",(char *)bdfs[1],"-a",(char *)bdfs[2],"-a",(char *)bdfs[3],
                  "--file-prefix","sfp-shared",rank?"--proc-type=secondary":"--proc-type=primary","--log-level=5",NULL};
    int primary_status=0;
    if(!rank) {
        if(rte_eal_init(17,args)<0 || rte_eth_dev_count_avail()!=4) primary_status=-1;
        for(int w=0;w<4 && !primary_status;w++) {
            uint16_t id;
            if(rte_eth_dev_get_port_by_name(bdfs[w],&id)<0 || configure_port(id,w)) primary_status=-1;
        }
    }
    // All four PFs share one IOMMU group; only the primary owns the VFIO container.
    MPI_Request request;MPI_Ibcast(&primary_status,1,MPI_INT,0,MPI_COMM_WORLD,&request);
    MPI_Wait(&request,MPI_STATUS_IGNORE);
    if(primary_status) return -1;
    if(rank && rte_eal_init(17,args)<0) return -1;
    if(rte_eth_dev_get_port_by_name(bdfs[rank],&p->id)<0) return -1;
    char pool_name[32];snprintf(pool_name,sizeof pool_name,"sfp-mbuf-%d",rank);
    p->pool=rte_mempool_lookup(pool_name);if(!p->pool) return -1;
    struct rte_eth_link link={0};
    if(rte_eth_link_get(p->id,&link)<0 || (require_link && !link.link_status)) {
        fprintf(stderr,"rank%d: no link on %s\n",rank,bdfs[rank]);return -1;
    }
    // Local link-up can precede the remote FPGA PCS reset release after PF start.
    MPI_Ibarrier(MPI_COMM_WORLD,&request);MPI_Wait(&request,MPI_STATUS_IGNORE);
    if(require_link) {
        for(int stable=0;stable<10;stable++) {
            usleep(100000);
            if(rte_eth_link_get_nowait(p->id,&link)<0 || !link.link_status) {
                fprintf(stderr,"rank%d: link did not remain stable on %s\n",rank,bdfs[rank]);return -1;
            }
        }
    }
    fprintf(stderr,"rank%d: %s, CPU %d, MTU %d, link %u Mbit/s\n",rank,bdfs[rank],cpu,JUMBO_MTU,link.link_speed);
    if(require_link && verify_datapath(p,rank)) return -1;
    return 0;
}
void dpdk_close(DpdkPort *p) {
    struct rte_eth_stats s={0};rte_eth_stats_get(p->id,&s);
    fprintf(stderr,"DPDK tx=%lu rx=%lu short_bursts=%lu rejected=%lu missed=%lu errors=%lu no_mbuf=%lu\n",
            p->tx_packets,p->rx_packets,p->tx_short,p->rx_invalid,s.imissed,s.ierrors,s.rx_nombuf);
    MPI_Request request;MPI_Ibarrier(MPI_COMM_WORLD,&request);MPI_Wait(&request,MPI_STATUS_IGNORE);
    if(process_rank) rte_eal_cleanup();
    MPI_Ibarrier(MPI_COMM_WORLD,&request);MPI_Wait(&request,MPI_STATUS_IGNORE);
    if(!process_rank) {
        uint16_t id;RTE_ETH_FOREACH_DEV(id) {rte_eth_dev_stop(id);rte_eth_dev_close(id);}
        for(int w=0;w<4;w++) if(pools[w]) rte_mempool_free(pools[w]);
        rte_eal_cleanup();
    }
}
