#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "bridge_protocol.h"

typedef struct {
    uint8_t request[MAX_APP];
    uint32_t base;
    double started;
    int busy;
    unsigned sent_mask;
} Pending;
static Pending pending[MAX_WINDOW];
static double now(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;
}
static void rate(const char *label,uint64_t bytes,double elapsed) {
    printf("%-25s %.3f Mbit/s (%.3f MiB/s)\n",label,bytes*8/elapsed/1e6,bytes/elapsed/1048576);
}
int main(int argc,char **argv) {
    const char *host="127.0.0.1";int port=10000,workers=4,rounds=16,window=16,cpu=-1;
    double duration=10,timeout=2;uint32_t seed=0x10000;
    static struct option opts[]={{"host",1,0,'h'},{"port",1,0,'p'},{"workers",1,0,'w'},
        {"rounds",1,0,'r'},{"window",1,0,'W'},{"duration",1,0,'d'},
        {"timeout",1,0,'t'},{"cpu",1,0,'c'},{"request-id-start",1,0,'i'},{0,0,0,0}};
    int opt;
    while((opt=getopt_long(argc,argv,"",opts,NULL))!=-1) {
        switch(opt) {
            case 'h':host=optarg;break;case 'p':port=atoi(optarg);break;
            case 'w':workers=atoi(optarg);break;case 'r':rounds=atoi(optarg);break;
            case 'W':window=atoi(optarg);break;case 'd':duration=atof(optarg);break;
            case 't':timeout=atof(optarg);break;case 'c':cpu=atoi(optarg);break;
            case 'i':seed=strtoul(optarg,NULL,0);break;default:return 2;
        }
    }
    if(port<1 || port>65532 || workers<2 || workers>4 || rounds<1 || rounds>16 ||
        window<1 || window>16 || duration<=0 || timeout<=0 || (seed&255) || cpu>=CPU_SETSIZE) return 2;
    if(cpu>=0) {cpu_set_t set;CPU_ZERO(&set);CPU_SET(cpu,&set);if(sched_setaffinity(0,sizeof set,&set)) {perror("CPU affinity");return 2;}}
    int fd=socket(AF_INET,SOCK_DGRAM|SOCK_NONBLOCK,0),buf=16*1024*1024;
    if(fd<0) {perror("socket");return 2;}
    setsockopt(fd,SOL_SOCKET,SO_SNDBUF,&buf,sizeof buf);setsockopt(fd,SOL_SOCKET,SO_RCVBUF,&buf,sizeof buf);
    struct sockaddr_in peer={.sin_family=AF_INET,.sin_port=htons(port)};
    if(inet_pton(AF_INET,host,&peer.sin_addr)!=1) {perror("connect");return 2;}
    struct sockaddr_in destinations[4];for(int w=0;w<4;w++){destinations[w]=peer;destinations[w].sin_port=htons(port+w);}
    uint64_t app_tx_bytes=0,app_rx_bytes=0;
    size_t bytes=rounds*ENTRIES*8;
    uint8_t expected[MAX_ROUNDS*ENTRIES*8];
    for(int i=0;i<rounds*ENTRIES;i++) {
        uint64_t sum=0;for(int w=0;w<workers;w++) sum+=100000u*(w+1)+i;
        put64(expected+i*8,sum);
    }
    for(int s=0;s<window;s++) {
        Pending *p=&pending[s];p->base=seed+s*16;
        app_header(p->request,MSG_REQUEST,p->base,rounds,workers,0);
        for(int w=0;w<workers;w++) for(int i=0;i<rounds*ENTRIES;i++)
            put64(p->request+APP_HEADER+w*bytes+i*8,100000u*(w+1)+i);
    }
    uint8_t replies[32][APP_HEADER+MAX_ROUNDS*ENTRIES*8+1];
    struct mmsghdr rx[32];struct iovec rxiov[32];struct sockaddr_in sources[32];
    memset(rx,0,sizeof rx);
    for(int i=0;i<32;i++) {rxiov[i]=(struct iovec){replies[i],sizeof replies[i]};rx[i].msg_hdr.msg_iov=&rxiov[i];rx[i].msg_hdr.msg_iovlen=1;rx[i].msg_hdr.msg_name=&sources[i];}
    uint64_t sent=0,ok=0,failed=0,late=0,timeouts=0,latency_hist[10000]={0};
    double latency_sum=0,start=now(),deadline=start+duration;int active=0,halt=0;
    while((now()<deadline && !halt) || active) {
        double current=now();
        if(!halt) {
            for(int slot=0;slot<window;slot++) {
                Pending *p=&pending[slot];
                if(!p->busy&&current>=deadline)continue;
                if(!p->busy){p->busy=1;p->sent_mask=0;p->started=current;active++;sent++;}
                if(p->sent_mask==15)continue;
                for(int w=0;w<4;w++)if(!(p->sent_mask&(1u<<w))){
                    uint8_t header[APP_HEADER];app_header(header,MSG_REQUEST,p->base,rounds,workers,0);
                    put16(header+4,WORKER_APP_VERSION);put32(header+20,w);
                    if(w<workers)put64(p->request+APP_HEADER+w*bytes,100000u*(w+1)+(uint64_t)p->base);
                    struct iovec vec[2]={{header,APP_HEADER},{p->request+APP_HEADER+w*bytes,w<workers?bytes:0}};
                    struct msghdr msg={.msg_name=&destinations[w],.msg_namelen=sizeof peer,.msg_iov=vec,.msg_iovlen=2};
                    ssize_t n=sendmsg(fd,&msg,MSG_DONTWAIT);
                    if(n>=0){p->sent_mask|=1u<<w;app_tx_bytes+=n;}
                    else if(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR){perror("sendmsg");halt=1;failed++;}
                }
            }
        }

        for(int i=0;i<32;i++) {rx[i].msg_hdr.msg_flags=0;rx[i].msg_len=0;rx[i].msg_hdr.msg_namelen=sizeof sources[i];}
        int got=recvmmsg(fd,rx,32,MSG_DONTWAIT,NULL);
        if(got<0 && errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR) {perror("recvmmsg");halt=1;}
        for(int i=0;i<got;i++) {
            uint8_t *b=replies[i];size_t n=rx[i].msg_len;
            if((rx[i].msg_hdr.msg_flags&MSG_TRUNC) || !app_worker_response(b,n) || sources[i].sin_addr.s_addr!=peer.sin_addr.s_addr || ntohs(sources[i].sin_port)!=port+get32(b+20)) {failed++;continue;}
            app_rx_bytes+=n;
            uint32_t base=get32(b+8);int s=(base>>4)&15;
            if(s>=window || !pending[s].busy || pending[s].base!=base) {late++;continue;}
            Pending *p=&pending[s];double elapsed=now()-p->started;
            uint64_t first=get64(expected)+(uint64_t)workers*base;
            if(get16(b+18) || get16(b+12)!=rounds || get16(b+14)!=workers ||
                get64(b+APP_HEADER)!=first || memcmp(b+APP_HEADER+8,expected+8,bytes-8)) {failed++;continue;}
            else {ok++;latency_sum+=elapsed;unsigned us=elapsed*1e6;latency_hist[us<10000?us:9999]++;}
            p->busy=0;active--;if(p->base>UINT32_MAX-256){halt=1;}else p->base+=256;
        }
        for(int s=0;s<window;s++) if(pending[s].busy && now()-pending[s].started>timeout) {
            pending[s].busy=0;active--;failed++;timeouts++;halt=1;
        }
    }
    double elapsed=now()-start;close(fd);
    printf("native four-port v5: workers=%d rounds/frame=%d window=%d CPU=%d\n",workers,rounds,window,sched_getcpu());
    printf("elapsed=%.6f s sent=%lu completed=%lu failed=%lu timeout=%lu late=%lu\n",elapsed,sent,ok,failed,timeouts,late);
    printf("batches/s=%.2f rounds/s=%.2f mean RTT=%.3f us\n",ok/elapsed,ok*rounds/elapsed,ok?latency_sum/ok*1e6:0);
    uint64_t seen=0;unsigned p99=0;
    for(;p99<9999;p99++) {seen+=latency_hist[p99];if(seen>=((ok*99+99)/100)) break;}
    printf("p99 RTT %s %u us\n",p99==9999?">=":"<=",p99+1);
    rate("useful aggregate result",ok*bytes,elapsed);
    rate("per active input payload",ok*(6+rounds*ROUND_BYTES),elapsed);
    rate("all input payload",ok*workers*(6+rounds*ROUND_BYTES),elapsed);
    rate("four output payload",ok*4*(6+rounds*ROUND_BYTES),elapsed);
    // 8-byte preamble/SFD + 4-byte FCS + 12-byte IFG, excluded from app payload.
    rate("per output wire estimate",ok*(MAX_FRAME-(16-rounds)*ROUND_BYTES+24),elapsed);
    rate("app UDP bytes observed",app_tx_bytes+app_rx_bytes,elapsed);
    puts("FPGA rates are inferred from successful replies, not physical-port counters.");
    return failed || !ok ? 1 : 0;
}
