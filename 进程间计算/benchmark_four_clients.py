#!/usr/bin/env python3
"""Four independent, CPU-pinned application clients driving the four MPI ranks."""
import argparse, multiprocessing as mp, os, socket, struct, time
MAGIC=0x4d504247; HDR=24; ENTRIES=64; VERSION=5; REQUEST=1; RESPONSE=2
def header(base, rounds, workers, rank, typ=REQUEST):
    return struct.pack('!IHHIHHHHI', MAGIC, VERSION, typ, base, rounds, workers, ENTRIES, 0, rank)
def client(rank, a, out):
    try: os.sched_setaffinity(0,{a.cpu+rank})
    except (AttributeError,OSError): pass
    s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.settimeout(a.timeout); s.setsockopt(socket.SOL_SOCKET,socket.SO_SNDBUF,16<<20); s.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,16<<20)
    peer=(a.host,a.port+rank); rounds=a.rounds; payload=b''.join(struct.pack('!Q',100000*(rank+1)+i) for i in range(rounds*ENTRIES)) if rank<a.workers else b''
    sent=ok=fail=0; start=time.monotonic(); end=start+a.duration; base=a.request_id_start
    window=a.window
    while time.monotonic()<end:
        for k in range(window): s.sendto(header(base+k*16,rounds,a.workers,rank)+payload,peer); sent+=1
        for k in range(window):
            try: data,_=s.recvfrom(9000)
            except (socket.timeout,OSError): fail+=1; continue
            if len(data)>=HDR and struct.unpack_from('!IHHIHHHHI',data)[0:3]==(MAGIC,VERSION,RESPONSE) and struct.unpack_from('!I',data,20)[0]==rank and struct.unpack_from('!H',data,18)[0]==0: ok+=1
            else: fail+=1
        base+=window*16
    # Drain replies for packets already submitted before the duration expired.
    drain_end=time.monotonic()+max(2.0,a.timeout*4)
    while sent-ok-fail>0 and time.monotonic()<drain_end:
        s.settimeout(max(0.01,drain_end-time.monotonic()))
        try: data,_=s.recvfrom(9000)
        except (socket.timeout,OSError): break
        if len(data)>=HDR and struct.unpack_from('!IHHIHHHHI',data)[0:3]==(MAGIC,VERSION,RESPONSE) and struct.unpack_from('!I',data,20)[0]==rank and struct.unpack_from('!H',data,18)[0]==0: ok+=1
        else: fail+=1
    out.put((rank,sent,ok,fail,time.monotonic()-start)); s.close()
def main():
    p=argparse.ArgumentParser(); p.add_argument('--host',default='127.0.0.1'); p.add_argument('--port',type=int,default=10000); p.add_argument('--workers',type=int,default=4); p.add_argument('--rounds',type=int,default=16); p.add_argument('--window',type=int,default=16); p.add_argument('--duration',type=float,default=30); p.add_argument('--timeout',type=float,default=.5); p.add_argument('--cpu',type=int,default=4, help='first client CPU (default 4; MPI uses CPU 0-3)'); p.add_argument('--request-id-start',type=lambda x:int(x,0),default=0x10000); a=p.parse_args(); q=mp.Queue(); ps=[mp.Process(target=client,args=(r,a,q)) for r in range(4)]; [x.start() for x in ps]; vals=[]
    for _ in ps:
        try: vals.append(q.get(timeout=a.duration+max(2,a.timeout)+5))
        except Exception: vals.append((len(vals),0,0,1,a.duration))
    [x.join(timeout=2) for x in ps]; elapsed=max(v[4] for v in vals); sent=sum(v[1] for v in vals); ok=sum(v[2] for v in vals); fail=sum(v[3] for v in vals); useful=ok/4*a.rounds*ENTRIES*8*8/elapsed/1e6; print(f'four clients: workers={a.workers} rounds/frame={a.rounds} window={a.window} CPUs={a.cpu}-{a.cpu+3}'); print(f'elapsed={elapsed:.3f}s batches={ok/4} sent={sent} replies={ok} failed={fail}'); print(f'useful aggregate result {useful:.3f} Mbit/s')
if __name__=='__main__': main()
