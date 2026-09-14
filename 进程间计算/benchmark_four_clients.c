#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <math.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "bridge_protocol.h"
#define BATCH 32
static double now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return t.tv_sec+t.tv_nsec*1e-9;
}
typedef struct {
    uint32_t id;
    int busy;
    uint64_t ordinal;
    uint8_t data[WORKER_APP_MAX];
} Pending;
static Pending pending[MAX_WINDOW];
int main(int argc,char **argv) {
    const char *host="127.0.0.1";
    int rank=-1,cpu=-1,port=10000,workers=4,rounds=16,window=16;
    double duration=30,drain=2;
    uint64_t seed=0x10000,requests=0;
    int fixed=0;
    static struct option opts[]={
        {"host",1,0,'h'},{"rank",1,0,'R'},{"cpu",1,0,'c'},
        {"port",1,0,'p'},{"workers",1,0,'w'},{"rounds",1,0,'r'},
        {"window",1,0,'W'},{"duration",1,0,'d'},
        {"requests",1,0,'N'},
        {"drain-timeout",1,0,'t'},{"request-id-start",1,0,'i'},
        {"help",0,0,'?'},{0,0,0,0}};
    int opt;
    while((opt=getopt_long(argc,argv,"",opts,NULL))!=-1) {
        switch(opt) {
        case 'h':host=optarg;break;case 'R':rank=atoi(optarg);break;
        case 'c':cpu=atoi(optarg);break;case 'p':port=atoi(optarg);break;
        case 'w':workers=atoi(optarg);break;case 'r':rounds=atoi(optarg);break;
        case 'W':window=atoi(optarg);break;case 'd':duration=atof(optarg);break;
        case 'N': {
            char *tail;errno=0;requests=strtoull(optarg,&tail,10);fixed=1;
            if(errno||*tail||optarg[0]=='-'||!requests){fputs("Invalid --requests\n",stderr);return 2;}
            break;
        }
        case 't':drain=atof(optarg);break;case 'i':seed=strtoull(optarg,NULL,0);break;
        default:puts("--rank 0..3 [--cpu N] [--host IPv4] [--port 10000] [--workers 4] [--rounds 16] [--window 16] [--duration 30 | --requests N] [--drain-timeout 2] [--request-id-start 0x10000]");return 2;
        }
    }
    if(cpu==-1)cpu=4+rank;
    if(rank<0||rank>3||cpu<0||cpu>=CPU_SETSIZE||port<1||port>65532||
       workers<2||workers>4||rounds<1||rounds>16||window<1||window>16||
       !isfinite(duration)||duration<=0||!isfinite(drain)||drain<=0||
       seed>UINT32_MAX-255||(seed&255)) {
        fputs("Invalid arguments; --rank 0..3 is required.\n",stderr);return 2;
    }
    if(fixed) {
        uint64_t capacity=((UINT32_MAX-255-seed)/256+1)*(uint64_t)window;
        if(requests>capacity){fputs("Request IDs would overflow\n",stderr);return 2;}
    }
    cpu_set_t set;CPU_ZERO(&set);CPU_SET(cpu,&set);
    if(sched_setaffinity(0,sizeof set,&set)){perror("CPU affinity");return 2;}
    int fd=socket(AF_INET,SOCK_DGRAM|SOCK_NONBLOCK,0),buf=16<<20;
    if(fd<0){perror("socket");return 2;}
    setsockopt(fd,SOL_SOCKET,SO_SNDBUF,&buf,sizeof buf);
    setsockopt(fd,SOL_SOCKET,SO_RCVBUF,&buf,sizeof buf);
    struct sockaddr_in peer={.sin_family=AF_INET,.sin_port=htons(port+rank)};
    if(inet_pton(AF_INET,host,&peer.sin_addr)!=1||connect(fd,(void *)&peer,sizeof peer)) {
        fputs("Invalid/unreachable peer\n",stderr);close(fd);return 2;
    }
    size_t bytes=(size_t)rounds*512,sendlen=APP_HEADER+(rank<workers?bytes:0);
    uint8_t expected[MAX_ROUNDS*512];
    for(size_t j=0;j<bytes/8;j++) {
        uint64_t sum=0;
        for(int w=0;w<workers;w++)sum+=100000u*(w+1)+j;
        put64(expected+8*j,sum);
    }
    for(int k=0;k<window;k++) {
        Pending *p=&pending[k];p->id=(uint32_t)seed+k*16;p->ordinal=k;
        app_header(p->data,MSG_REQUEST,p->id,rounds,workers,0);
        put16(p->data+4,WORKER_APP_VERSION);put32(p->data+20,rank);
        for(size_t j=0;j<bytes/8;j++)put64(p->data+APP_HEADER+8*j,100000u*(rank+1)+j);
    }
    uint8_t replies[BATCH][WORKER_APP_MAX+1];
    struct mmsghdr rx[BATCH]={0};struct iovec ri[BATCH];
    for(int j=0;j<BATCH;j++) {
        ri[j]=(struct iovec){replies[j],sizeof replies[j]};
        rx[j].msg_hdr.msg_iov=&ri[j];rx[j].msg_hdr.msg_iovlen=1;
    }
    uint64_t sent=0,ok=0,invalid=0,late=0,status_errors=0,txbytes=0,rxbytes=0;
    int active=0,halt=0,ioerror=0;
    double start=now(),end=start+duration,drain_end=0,last_progress=start;
    printf("independent client rank=%d CPU=%d peer=%s:%d window=%d\n",rank,sched_getcpu(),host,port+rank,window);fflush(stdout);
    printf("mode=%s requests=%"PRIu64" drain_timeout=%.3f s\n",fixed?"fixed requests":"duration",requests,drain);
    while(1) {
        double t=now();
        if(fixed&&!halt&&sent<requests&&t-last_progress>=drain) {
            fputs("No progress before send completion; stopping and draining pending requests\n",stderr);
            halt=1;
        }
        int sending=!halt&&(fixed?sent<requests:t<end);
        if(!sending&&!drain_end)drain_end=t+drain;
        if(!sending&&(!active||t>=drain_end))break;
        if(sending) {
            struct mmsghdr tx[MAX_WINDOW]={0};struct iovec ti[MAX_WINDOW];int owners[MAX_WINDOW],n=0;
            for(int k=0;k<window;k++)if(!pending[k].busy&&(!fixed||pending[k].ordinal<requests)) {
                Pending *p=&pending[k];put32(p->data+8,p->id);
                put64(p->data+APP_HEADER,100000u*(rank+1)+(uint64_t)p->id);
                owners[n]=k;ti[n]=(struct iovec){p->data,sendlen};
                tx[n].msg_hdr.msg_iov=&ti[n];tx[n].msg_hdr.msg_iovlen=1;n++;
            }
            if(n) {
                int z=sendmmsg(fd,tx,n,MSG_DONTWAIT);
                if(z<0&&errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR) {
                    perror("sendmmsg");ioerror=halt=1;
                }
                for(int j=0;j<z;j++){pending[owners[j]].busy=1;active++;sent++;last_progress=now();txbytes+=tx[j].msg_len;}
            }
        }
        for(int j=0;j<BATCH;j++){rx[j].msg_hdr.msg_flags=0;rx[j].msg_len=0;}
        int n=recvmmsg(fd,rx,BATCH,MSG_DONTWAIT,NULL);
        if(n<0&&errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR) {
            perror("recvmmsg");ioerror=halt=1;break;
        }
        for(int j=0;j<n;j++) {
            uint8_t *b=replies[j];rxbytes+=rx[j].msg_len;
            if((rx[j].msg_hdr.msg_flags&MSG_TRUNC)||!app_worker_response(b,rx[j].msg_len)||
               get32(b+20)!=(unsigned)rank||get16(b+12)!=rounds||get16(b+14)!=workers) {invalid++;continue;}
            uint32_t id=get32(b+8);unsigned k=(id>>4)&15;
            if(k>=(unsigned)window||!pending[k].busy||pending[k].id!=id){late++;continue;}
            if(get16(b+18)) {
                status_errors++;pending[k].busy=0;active--;halt=1;
                fprintf(stderr,"rank%d request_id=%"PRIu32" status=%u\n",rank,id,get16(b+18));
                continue;
            }
            if(get64(b+APP_HEADER)!=get64(expected)+(uint64_t)workers*id||
               memcmp(b+APP_HEADER+8,expected+8,bytes-8)){invalid++;continue;}
            pending[k].busy=0;active--;ok++;last_progress=now();pending[k].ordinal+=window;
            if(id>UINT32_MAX-256){if(!fixed||pending[k].ordinal<requests)halt=1;}else pending[k].id+=256;
        }
    }
    double elapsed=now()-start;
    if(fixed)printf("target=%"PRIu64" unsent=%"PRIu64"\n",requests,requests-sent);
    printf("elapsed=%.6f s sent=%"PRIu64" completed=%"PRIu64" unresolved=%d invalid=%"PRIu64" late=%"PRIu64" status_errors=%"PRIu64" io_error=%d\n",elapsed,sent,ok,active,invalid,late,status_errors,ioerror);
    printf("validated result bandwidth: %.3f Mbit/s\n",ok*(double)bytes*8/elapsed/1e6);
    printf("observed app UDP TX: %.3f Mbit/s RX: %.3f Mbit/s\n",txbytes*8.0/elapsed/1e6,rxbytes*8.0/elapsed/1e6);
    puts("Result bandwidth is per client, includes drain time, and excludes headers; four broadcast copies must not be summed as unique results.");
    close(fd);return (fixed&&sent!=requests)||active||invalid||status_errors||ioerror||!ok?1:0;
}
