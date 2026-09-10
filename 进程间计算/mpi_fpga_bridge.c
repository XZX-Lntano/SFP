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

typedef struct {
    uint32_t base, rounds, workers;
    uint8_t values[MAX_ROUNDS*ENTRIES*8];
} Work;
typedef struct {
    uint32_t status, base, rounds, workers;
    uint8_t values[MAX_ROUNDS*ENTRIES*8];
} Reply;
typedef struct {
    int busy, sent, complete, work_ready, early_valid;
    double started;
    Work work[4];
    Reply replies[4];
    Reply early;
    MPI_Request jobs[3], results[3], incoming, outgoing;
    struct sockaddr_in peer;
} Slot;
static Slot slots[MAX_WINDOW];
static volatile sig_atomic_t stopping=0;
static void stop_handler(int sig) {(void)sig;stopping=1;}
static double now(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;
}
static int cpu_number(void) {
    cpu_set_t allowed;CPU_ZERO(&allowed);
    if(sched_getaffinity(0,sizeof allowed,&allowed)<0) return -1;
    for(int i=0;i<CPU_SETSIZE;i++) if(CPU_ISSET(i,&allowed)) {
        cpu_set_t one;CPU_ZERO(&one);CPU_SET(i,&one);
        if(sched_setaffinity(0,sizeof one,&one)<0) return -1;
        return i;
    }
    return -1;
}
static void post_work(Slot *s,int slot) {
    MPI_Irecv(&s->work[0],sizeof(Work),MPI_BYTE,0,100+slot,MPI_COMM_WORLD,&s->incoming);
}
static void init_reply(Slot *s,int rank,unsigned status) {
    Reply *r=&s->replies[rank];Work *w=&s->work[0];
    r->base=w->base;r->rounds=w->rounds;r->workers=w->workers;r->status=status;
}
static void app_reply(int fd,const Slot *s,unsigned status) {
    uint8_t b[APP_HEADER+MAX_ROUNDS*ENTRIES*8];const Work *w=&s->work[0];
    size_t n=app_size(w->rounds,w->workers,1);
    app_header(b,MSG_RESPONSE,w->base,w->rounds,w->workers,status);
    if(status) memset(b+APP_HEADER,0,n-APP_HEADER);
    else memcpy(b+APP_HEADER,s->replies[0].values,n-APP_HEADER);
    if(sendto(fd,b,n,MSG_DONTWAIT,(const struct sockaddr *)&s->peer,sizeof(s->peer))!=(ssize_t)n)
        fprintf(stderr,"application response dropped: %s\n",strerror(errno));
}
static int parse_mac(const char *str,uint8_t dst[6]) {
    unsigned d[6];char extra;
    if(sscanf(str,"%x:%x:%x:%x:%x:%x%c",d,d+1,d+2,d+3,d+4,d+5,&extra)!=6) return -1;
    for(int i=0;i<6;i++) {if(d[i]>255) return -1;dst[i]=d[i];}return 0;
}
int main(int argc,char **argv) {
    MPI_Init(&argc,&argv);
    int rank,size;MPI_Comm_rank(MPI_COMM_WORLD,&rank);MPI_Comm_size(MPI_COMM_WORLD,&size);
    int simulate=0,probe=0,port=10000,window=MAX_WINDOW;double timeout=0.5;
    uint8_t dst[6]={0xff,0xff,0xff,0xff,0xff,0xff};
    const char *bdfs[4]={"0000:01:00.3","0000:01:00.2","0000:01:00.1","0000:01:00.0"};
    static struct option opts[]={{"simulate",0,0,'s'},{"probe",0,0,'P'},{"port",1,0,'p'},{"window",1,0,'w'},
        {"timeout-ms",1,0,'t'},{"dst-mac",1,0,'m'},{"bdf0",1,0,1000},
        {"bdf1",1,0,1001},{"bdf2",1,0,1002},{"bdf3",1,0,1003},{0,0,0,0}};
    int c;
    while((c=getopt_long(argc,argv,"",opts,NULL))!=-1) {
        if(c=='s') simulate=1;else if(c=='P') probe=1;else if(c=='p') port=atoi(optarg);
        else if(c=='w') window=atoi(optarg);else if(c=='t') timeout=atof(optarg)/1000;
        else if(c=='m') {if(parse_mac(optarg,dst)) MPI_Abort(MPI_COMM_WORLD,2);}
        else if(c>=1000 && c<=1003) bdfs[c-1000]=optarg;else MPI_Abort(MPI_COMM_WORLD,2);
    }
    if(size!=4 || port<1 || port>65535 || window<1 || window>MAX_WINDOW || timeout<0.2) {
        if(!rank) fprintf(stderr,"Use mpirun -np 4 --bind-to core --map-by core ./mpi_fpga_bridge [--simulate] [--window 1..16] [--timeout-ms >=200]\n");
        MPI_Abort(MPI_COMM_WORLD,2);
    }
    int cpu=cpu_number();if(cpu<0) MPI_Abort(MPI_COMM_WORLD,2);
    DpdkPort dp={0};
    if(!simulate && dpdk_open(&dp,bdfs,rank,cpu,!probe)) MPI_Abort(MPI_COMM_WORLD,3);
    fprintf(stderr,"rank%d: pinned CPU=%d transport=%s\n",rank,cpu,simulate?"SIMULATION":"DPDK");
    if(probe) {if(!simulate) dpdk_close(&dp);MPI_Finalize();return 0;}
    int fd=-1;
    if(!rank) {
        fd=socket(AF_INET,SOCK_DGRAM|SOCK_NONBLOCK,0);int buf=16*1024*1024;
        setsockopt(fd,SOL_SOCKET,SO_RCVBUF,&buf,sizeof buf);setsockopt(fd,SOL_SOCKET,SO_SNDBUF,&buf,sizeof buf);
        struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons(port),.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
        if(fd<0 || bind(fd,(struct sockaddr *)&a,sizeof a)<0) {perror("app bind");MPI_Abort(MPI_COMM_WORLD,4);}
        fprintf(stderr,"bridge v4: 127.0.0.1:%d, window=%d batches, 256 round slots, %s\n",port,window,simulate?"SIMULATION ONLY":"FPGA");
    }
    for(int i=0;i<MAX_WINDOW;i++) {
        slots[i].incoming=slots[i].outgoing=MPI_REQUEST_NULL;
        for(int j=0;j<3;j++) slots[i].jobs[j]=slots[i].results[j]=MPI_REQUEST_NULL;
        if(rank) post_work(&slots[i],i);
    }
    signal(SIGINT,stop_handler);signal(SIGTERM,stop_handler);
    uint64_t completed=0,failed=0,rejected=0;int active=0;
    uint8_t app_rx[MAX_WINDOW][MAX_APP+1],frame_app[APP_HEADER+4*MAX_ROUNDS*ENTRIES*8];
    struct mmsghdr messages[MAX_WINDOW];struct iovec iov[MAX_WINDOW];
    struct sockaddr_in peers[MAX_WINDOW];
    memset(messages,0,sizeof messages);
    for(int i=0;i<MAX_WINDOW;i++) {
        iov[i]=(struct iovec){app_rx[i],sizeof app_rx[i]};
        messages[i].msg_hdr.msg_iov=&iov[i];messages[i].msg_hdr.msg_iovlen=1;
        messages[i].msg_hdr.msg_name=&peers[i];
    }
    while(!stopping || active) {
        int received_messages=0;
        if(!rank && !stopping) {
            for(int i=0;i<MAX_WINDOW;i++) {
                messages[i].msg_hdr.msg_namelen=sizeof peers[i];messages[i].msg_hdr.msg_flags=0;
            }
            received_messages=recvmmsg(fd,messages,MAX_WINDOW,MSG_DONTWAIT,NULL);
            if(received_messages<0 && errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR) stopping=1;
        }
        for(int burst=0;burst<received_messages;burst++) {
            uint8_t *app=app_rx[burst];size_t n=messages[burst].msg_len;
            struct sockaddr_in peer=peers[burst];
            if(n==APP_HEADER && get32(app)==APP_MAGIC && get16(app+4)==4 && get16(app+6)==MSG_STOP) {
                stopping=1;continue;
            }
            if(messages[burst].msg_hdr.msg_flags&MSG_TRUNC) {rejected++;continue;}
            if(!app_valid(app,n,0)) {rejected++;continue;}
            unsigned base=get32(app+8),rounds=get16(app+12),workers=get16(app+14);
            int index=(base>>4)&(MAX_WINDOW-1);Slot *s=&slots[index];
            if(s->busy || active>=window) {rejected++;continue;}
            s->busy=1;s->sent=0;s->complete=0;s->work_ready=1;s->started=now();s->peer=peer;active++;
            size_t bytes=rounds*ENTRIES*8;
            for(int w=0;w<4;w++) {
                s->work[w].base=base;s->work[w].rounds=rounds;s->work[w].workers=workers;
                if((unsigned)w<workers) memcpy(s->work[w].values,app+APP_HEADER+w*bytes,bytes);
                else memset(s->work[w].values,0,bytes);
            }
            if(simulate) for(unsigned i=0;i<rounds*ENTRIES;i++) {
                uint64_t sum=0;for(unsigned w=0;w<workers;w++) sum+=get64(app+APP_HEADER+w*bytes+i*8);
                for(int w=0;w<4;w++) put64(s->work[w].values+i*8,sum);
            }
            init_reply(s,0,0);
            for(int w=1;w<4;w++) {
                MPI_Isend(&s->work[w],offsetof(Work,values)+bytes,MPI_BYTE,w,100+index,MPI_COMM_WORLD,&s->jobs[w-1]);
                MPI_Irecv(&s->replies[w],sizeof(Reply),MPI_BYTE,w,1000+index,MPI_COMM_WORLD,&s->results[w-1]);
            }
        }
        if(rank) for(int i=0;i<MAX_WINDOW;i++) {
            Slot *s=&slots[i];int done=0;
            if(!s->busy) {
                MPI_Test(&s->incoming,&done,MPI_STATUS_IGNORE);
                if(done) {
                    if(!s->work[0].rounds) {stopping=1;break;}
                    s->busy=1;s->sent=0;s->complete=0;s->work_ready=1;s->started=now();active++;
                    init_reply(s,rank,0);
                    if(s->early_valid) {
                        if(s->early.base==s->work[0].base && s->early.rounds==s->work[0].rounds &&
                           s->early.workers==s->work[0].workers) {
                            s->replies[rank]=s->early;s->complete=1;
                        } else dp.rx_invalid++;
                        s->early_valid=0;
                    }
                }
            }
        }
        // Burst TX: only accepted mbufs transfer ownership to the PMD.
        struct rte_mbuf *tx[MAX_WINDOW];int txslot[MAX_WINDOW],ntx=0;
        for(int i=0;i<MAX_WINDOW;i++) {
            Slot *s=&slots[i];if(!s->busy || !s->work_ready || s->sent || s->complete) continue;
            Work *w=&s->work[0];
            if(simulate) {
                memcpy(s->replies[rank].values,w->values,w->rounds*ENTRIES*8);
                s->sent=1;s->complete=1;continue;
            }
            if((unsigned)rank>=w->workers) {s->sent=1;continue;}
            struct rte_mbuf *m=rte_pktmbuf_alloc(dp.pool);if(!m) break;
            size_t len=FPGA_HEADER+w->rounds*ROUND_BYTES;
            uint8_t *b=(uint8_t *)rte_pktmbuf_append(m,len);
            if(!b) {rte_pktmbuf_free(m);break;}
            app_header(frame_app,MSG_REQUEST,w->base,w->rounds,w->workers,0);
            memcpy(frame_app+APP_HEADER,w->values,w->rounds*ENTRIES*8);
            fpga_frame(b,frame_app,0,dst,0xc0a80a01u+rank,0xc0a80a65u+rank);put16(b+34,4000+rank);
            tx[ntx]=m;txslot[ntx++]=i;
        }
        if(ntx) {
            uint16_t sent=rte_eth_tx_burst(dp.id,0,tx,ntx);dp.tx_packets+=sent;
            if(sent<ntx) dp.tx_short++;
            for(int j=0;j<ntx;j++) {
                if(j<sent) slots[txslot[j]].sent=1;else rte_pktmbuf_free(tx[j]);
            }
        }
        if(!simulate) {
            struct rte_mbuf *rx[32];uint16_t n=rte_eth_rx_burst(dp.id,0,rx,32);dp.rx_packets+=n;
            for(unsigned j=0;j<n;j++) {
                uint8_t scratch[MAX_FRAME],values[MAX_ROUNDS*ENTRIES*8];
                uint32_t base;unsigned rounds,workers;
                const uint8_t *b=NULL;unsigned len=rte_pktmbuf_pkt_len(rx[j]);
                if(len<=MAX_FRAME) b=rte_pktmbuf_read(rx[j],0,len,scratch);
                if(b && fpga_result(b,len,values,&base,&rounds,&workers)) {
                    Slot *s=&slots[(base>>4)&(MAX_WINDOW-1)];
                    if(s->busy && !s->complete && s->work[0].base==base &&
                        s->work[0].rounds==rounds && s->work[0].workers==workers) {
                        memcpy(s->replies[rank].values,values,rounds*ENTRIES*8);s->complete=1;
                    } else if(rank && !s->busy && !s->early_valid) {
                        // Broadcast can beat MPI work notification on an inactive worker.
                        s->early=(Reply){.status=0,.base=base,.rounds=rounds,.workers=workers};
                        memcpy(s->early.values,values,rounds*ENTRIES*8);s->early_valid=1;
                    } else dp.rx_invalid++;
                } else dp.rx_invalid++;
                rte_pktmbuf_free(rx[j]);
            }
        }
        for(int i=0;i<MAX_WINDOW;i++) {
            Slot *s=&slots[i];if(!s->busy) continue;
            if(!s->complete && now()-s->started>timeout) {
                s->replies[rank].status=s->sent?STATUS_TIMEOUT:STATUS_SEND;s->complete=1;
            }
            if(!s->complete) continue;
            size_t bytes=s->work[0].rounds*ENTRIES*8;
            if(rank) {
                if(s->work_ready) {
                    MPI_Isend(&s->replies[rank],offsetof(Reply,values)+bytes,MPI_BYTE,0,1000+i,MPI_COMM_WORLD,&s->outgoing);
                    s->work_ready=0;
                }
                int done;MPI_Test(&s->outgoing,&done,MPI_STATUS_IGNORE);
                if(done) {s->busy=0;active--;post_work(s,i);}
            } else {
                int sent,received;
                MPI_Testall(3,s->jobs,&sent,MPI_STATUSES_IGNORE);MPI_Testall(3,s->results,&received,MPI_STATUSES_IGNORE);
                if(!sent || !received) continue;
                unsigned status=s->replies[0].status;
                for(int w=1;w<4;w++) {
                    if(s->replies[w].status>status) status=s->replies[w].status;
                    if(!status && (s->replies[w].base!=s->work[0].base ||
                        s->replies[w].rounds!=s->work[0].rounds || s->replies[w].workers!=s->work[0].workers ||
                        memcmp(s->replies[0].values,s->replies[w].values,bytes))) status=STATUS_MISMATCH;
                }
                if(status) fprintf(stderr,"batch %u failed: status=%u rank_status=%u,%u,%u,%u\n",
                    s->work[0].base,status,s->replies[0].status,s->replies[1].status,
                    s->replies[2].status,s->replies[3].status);
                app_reply(fd,s,status);if(status) failed++;else completed++;
                s->busy=0;active--;
            }
        }
    }
    if(!rank) {
        Work stop={0};MPI_Request req[3];
        for(int w=1;w<4;w++) MPI_Isend(&stop,offsetof(Work,values),MPI_BYTE,w,100,MPI_COMM_WORLD,&req[w-1]);
        MPI_Waitall(3,req,MPI_STATUSES_IGNORE);
        fprintf(stderr,"bridge completed=%lu failed=%lu rejected=%lu\n",completed,failed,rejected);close(fd);
    } else for(int i=0;i<MAX_WINDOW;i++) if(slots[i].incoming!=MPI_REQUEST_NULL) {
        MPI_Cancel(&slots[i].incoming);MPI_Wait(&slots[i].incoming,MPI_STATUS_IGNORE);
    }
    if(!simulate) dpdk_close(&dp);
    MPI_Finalize();return 0;
}
