#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <mpi.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "dpdk_port.h"
/* Separate bounded software records let a slow rank drain an old response even
 * after another rank has completed that request at the client. */
#define RECORDS (MAX_WINDOW*4)

typedef struct {
    int busy, sent, complete;
    uint32_t base; 
    unsigned rounds,workers;
    double started;
    struct sockaddr_in peer;
    struct rte_mbuf *tx;
    uint8_t reply[WORKER_APP_MAX];
} 
Slot;

static Slot slots[RECORDS];
static Slot early[RECORDS];

static volatile sig_atomic_t stopping;

static void stop_handler(int sig){
    (void)sig;
    stopping=1;
}

static double now(void){
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC,&t);
    return t.tv_sec+t.tv_nsec*1e-9;
}

static Slot *lookup(uint32_t base){
    for(int i=0;i<RECORDS;i++)
    if(slots[i].busy&&slots[i].base==base)
    return &slots[i];
    return NULL;
}

static void finish(Slot *s,unsigned status,int rank){
    app_header(s->reply,MSG_RESPONSE,s->base,s->rounds,s->workers,status);
    put16(s->reply+4,WORKER_APP_VERSION);
    put32(s->reply+20,rank);
    if(status)memset(s->reply+APP_HEADER,0,s->rounds*512);
    s->complete=1;
}

int main(int argc,char **argv){
    MPI_Init(&argc,&argv);
    int rank,size;
    MPI_Comm_rank(MPI_COMM_WORLD,&rank);
    MPI_Comm_size(MPI_COMM_WORLD,&size);
    int port=10000,probe=0,window=MAX_WINDOW;
    double timeout=.5;
    const char *bdfs[4]={"0000:01:00.3","0000:01:00.2","0000:01:00.1","0000:01:00.0"};
    uint8_t dst[6]={255,255,255,255,255,255};

    static struct option opts[]={
        {"port",1,0,'p'},
        {"window",1,0,'w'},
        {"timeout-ms",1,0,'t'},
        {"probe",0,0,'P'},
        {"dst-mac",1,0,'m'},
        {"bdf0",1,0,1000},
        {"bdf1",1,0,1001},
        {"bdf2",1,0,1002},
        {"bdf3",1,0,1003},
        {0,0,0,0}};

    int c;
    while((c=getopt_long(argc,argv,"",opts,NULL))!=-1){
        if(c=='p')port=atoi(optarg);
        else if(c=='w')window=atoi(optarg);
        else if(c=='t')timeout=atof(optarg)/1000;
        else if(c=='P')probe=1;
        else if(c>=1000&&c<=1003)bdfs[c-1000]=optarg;
        else if(c=='m'){
            unsigned v[6];
            char end;
            if(sscanf(optarg,"%x:%x:%x:%x:%x:%x%c",v,v+1,v+2,v+3,v+4,v+5,&end)!=6)
            MPI_Abort(MPI_COMM_WORLD,2);
            for(int j=0;j<6;j++){
                if(v[j]>255)
                MPI_Abort(MPI_COMM_WORLD,2);
                dst[j]=v[j];
            }
        }
        else MPI_Abort(MPI_COMM_WORLD,2);
    }

    if(size!=4||port<1||port>65532||window<1||window>MAX_WINDOW||timeout<.2)
    MPI_Abort(MPI_COMM_WORLD,2);
    cpu_set_t set;
    CPU_ZERO(&set);
    sched_getaffinity(0,sizeof set,&set);
    int cpu=-1;

    for(int j=0;j<CPU_SETSIZE;j++)
    if(CPU_ISSET(j,&set)){
        cpu=j;break;
    }

    if(cpu<0)
    MPI_Abort(MPI_COMM_WORLD,2);
    CPU_ZERO(&set);
    CPU_SET(cpu,&set);

    if(sched_setaffinity(0,sizeof set,&set))
    MPI_Abort(MPI_COMM_WORLD,2);
    DpdkPort dp={0};

    if(dpdk_open(&dp,bdfs,rank,cpu,!probe))
    MPI_Abort(MPI_COMM_WORLD,3);

    if(probe){
        dpdk_close(&dp);
        MPI_Finalize();
        return 0;
    }

    int fd=socket(AF_INET,SOCK_DGRAM|SOCK_NONBLOCK,0),buf=16*1024*1024;
    if(fd<0)
    MPI_Abort(MPI_COMM_WORLD,4);
    setsockopt(fd,SOL_SOCKET,SO_RCVBUF,&buf,sizeof buf);
    setsockopt(fd,SOL_SOCKET,SO_SNDBUF,&buf,sizeof buf);

    struct sockaddr_in addr={
        .sin_family=AF_INET,
        .sin_port=htons(port+rank),
        .sin_addr.s_addr=htonl(INADDR_LOOPBACK)
    };

    if(bind(fd,(struct sockaddr *)&addr,sizeof addr)){
        perror("bind");
        MPI_Abort(MPI_COMM_WORLD,4);
    }

    signal(SIGINT,stop_handler);
    signal(SIGTERM,stop_handler);
    fprintf(stderr,"bridge v5 rank%d: 127.0.0.1:%d FPGA slots=%d window<=%d\n",rank,port+rank,MAX_WINDOW,window);

    uint64_t completed=0,failed=0,rejected=0,late=0;
    double stop_at=0;
    int active=0;
    uint8_t input[32][WORKER_APP_MAX+1];
    struct sockaddr_in peers[32];
    struct mmsghdr msgs[32]={0};
    struct iovec io[32];

    for(int j=0;j<32;j++){
        io[j]=(struct iovec){
            input[j],
            sizeof input[j]
        };
        msgs[j].msg_hdr.msg_iov=&io[j];
        msgs[j].msg_hdr.msg_iovlen=1;
        msgs[j].msg_hdr.msg_name=&peers[j];
    }

    while(!stopping||active){
        if(stopping&&!stop_at)stop_at=now();
        if(stop_at&&now()-stop_at>timeout*2)break;
        /* Drain FPGA first so slow application work does not delay RX service. */
        struct rte_mbuf *rx[32];
        unsigned nr=rte_eth_rx_burst(dp.id,0,rx,32);
        dp.rx_packets+=nr;

        for(unsigned j=0;j<nr;j++){
            uint8_t scratch[MAX_FRAME];
            unsigned len=rte_pktmbuf_pkt_len(rx[j]),rounds,workers;
            uint32_t base;
            const uint8_t *b=len<=MAX_FRAME?rte_pktmbuf_read(rx[j],0,len,scratch):NULL;
            if(b&&fpga_result_view(b,len,&base,&rounds,&workers)){
                Slot *s=lookup(base);
                if(s&&!s->complete&&s->rounds==rounds&&s->workers==workers){
                    for(unsigned r=0;r<rounds;r++)
                    memcpy(s->reply+APP_HEADER+r*512,b+FPGA_HEADER+r*ROUND_BYTES+8,512);
                    finish(s,0,rank);
                }
                else {
                    late++;
                    if(!s && (unsigned)rank>=workers){
                        Slot *e=&early[(base>>4)%RECORDS];
                        *e=(Slot){
                            .busy=1,
                            .base=base,
                            .rounds=rounds,
                            .workers=workers,
                            .started=now()
                        };
                        for(unsigned r=0;r<rounds;r++)
                        memcpy(e->reply+APP_HEADER+r*512,b+FPGA_HEADER+r*ROUND_BYTES+8,512);
                        finish(e,0,rank);
                    }
                }
            }else dp.rx_invalid++;
            rte_pktmbuf_free(rx[j]);
        }
        for(int j=0;j<32;j++){
            msgs[j].msg_hdr.msg_namelen=sizeof peers[j];
            msgs[j].msg_hdr.msg_flags=0;
        }

        int n=stopping?0:recvmmsg(fd,msgs,32,MSG_DONTWAIT,NULL);
        if(n<0&&errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR){
            perror("recv");
            MPI_Abort(MPI_COMM_WORLD,4);
        }

        for(int j=0;j<n;j++){
            uint8_t *b=input[j];
            unsigned len=msgs[j].msg_len;
            if(len==APP_HEADER&&get32(b)==APP_MAGIC&&get16(b+4)==WORKER_APP_VERSION&&get16(b+6)==MSG_STOP){
                stopping=1;continue;
            }

            if((msgs[j].msg_hdr.msg_flags&MSG_TRUNC)||!app_valid_worker(b,len)||get32(b+20)!=(unsigned)rank){
                rejected++;continue;
            }

            uint32_t base=get32(b+8);
            if(lookup(base)){
                rejected++;
                continue;
            }

            Slot *s=NULL;
            for(int k=0;k<RECORDS;k++)
            if(!slots[k].busy){
                s=&slots[k];
                break;
            }
            if(!s){
                rejected++;
                continue;
            }
            *s=(Slot){
                .busy=1,
                .base=base,
                .rounds=get16(b+12),
                .workers=get16(b+14),
                .started=now(),
                .peer=peers[j]};
                active++;

            if((unsigned)rank<s->workers){
                s->tx=rte_pktmbuf_alloc(dp.pool);
                uint8_t *frame=s->tx?(uint8_t *)rte_pktmbuf_append(s->tx,FPGA_HEADER+s->rounds*ROUND_BYTES):NULL;
                if(!frame){
                    if(s->tx)rte_pktmbuf_free(s->tx);
                    s->tx=NULL;
                    finish(s,STATUS_SEND,rank);
                }
                else fpga_frame_work(frame,base,s->rounds,s->workers,b+APP_HEADER,rank,dst,0xc0a80a01u+rank,0xc0a80a65u+rank);
            }else {
                s->sent=1;
                Slot *e=&early[(base>>4)%RECORDS];
                if(e->busy&&e->base==base&&e->rounds==s->rounds&&e->workers==s->workers&&now()-e->started<timeout){
                    memcpy(s->reply,e->reply,APP_HEADER+s->rounds*512);
                    s->complete=1;
                    e->busy=0;
                }
            }
        }
        struct rte_mbuf *tx[RECORDS];
        Slot *owners[RECORDS];
        unsigned nt=0;

        for(int j=0;j<RECORDS;j++)
        if(slots[j].busy&&slots[j].tx&&!slots[j].complete){
            owners[nt]=&slots[j];
            tx[nt++]=slots[j].tx;
        }

        if(nt){
            unsigned sent=rte_eth_tx_burst(dp.id,0,tx,nt);
            dp.tx_packets+=sent;if(sent<nt)dp.tx_short++;
            for(unsigned j=0;j<sent;j++){
                owners[j]->tx=NULL;
                owners[j]->sent=1;
            }
        }

        for(int j=0;j<RECORDS;j++){
            Slot *s=&slots[j];if(!s->busy)continue;
            if(!s->complete&&now()-s->started>timeout)
            finish(s,STATUS_TIMEOUT,rank);
            if(!s->complete)
            continue;
            size_t len=APP_HEADER+s->rounds*512;
            ssize_t sent=sendto(fd,s->reply,len,MSG_DONTWAIT,(struct sockaddr *)&s->peer,sizeof s->peer);
            if(sent<0&&(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR)&&now()-s->started<timeout)
            continue;
            if(sent==(ssize_t)len&&!get16(s->reply+18))
            completed++;
        else failed++;
            if(s->tx){
                rte_pktmbuf_free(s->tx);
                s->tx=NULL;
            }
            s->busy=0;
            active--;
        }
    }

    for(int j=0;j<RECORDS;j++)
    if(slots[j].tx)
    rte_pktmbuf_free(slots[j].tx);
    fprintf(stderr,"rank%d completed=%lu failed=%lu rejected=%lu late=%lu\n",rank,completed,failed,rejected,late);
    close(fd);
    dpdk_close(&dp);
    MPI_Finalize();
    return failed?1:0;
}
