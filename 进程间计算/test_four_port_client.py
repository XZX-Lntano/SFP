"""Network-level client tests with controllable response ordering; no FPGA emulation claim."""
import socket,struct,threading,time
from bridge_app_client import FourPortClient,HEADER,BRIDGE_MAGIC

def main():
    servers=[]
    for port in range(24000,30000,4):
        try:
            for w in range(4):
                s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);s.bind(('127.0.0.1',port+w));s.settimeout(3);servers.append(s)
            break
        except OSError:
            for s in servers:s.close()
            servers=[]
    assert len(servers)==4
    errors=[]
    def serve():
        try:
            for batch in range(3):
                messages=[s.recvfrom(9000) for s in servers]
                ids=[HEADER.unpack_from(raw)[3] for raw,addr in messages];assert len(set(ids))==1
                for w,(raw,_) in enumerate(messages):assert HEADER.unpack_from(raw)[-1]==w
                if batch==2:continue # timeout cleanup
                base=ids[0];reply=lambda rank,status=0: HEADER.pack(BRIDGE_MAGIC,5,2,base,1,4,64,status,rank)+struct.pack('!64Q',*[10]*64)
                servers[0].sendto(reply(0,6),messages[0][1]) # error must not consume ID
                servers[3].sendto(reply(3),messages[3][1]) # fourth port is first usable
                time.sleep(.03)
                for w in range(3):servers[w].sendto(reply(w),messages[w][1]) # stale copies
        except Exception as e:errors.append(e)
    t=threading.Thread(target=serve);t.start();c=FourPortClient(port=port,timeout=.2,window=1)
    try:
        for base in [0x1000,0x1100]:
            c.submit(base,[[[w+1]] for w in range(4)])
            try:c.submit(base+16,[[[1]]]*4);raise AssertionError('no bound')
            except BufferError:pass
            result=c.receive();assert result['rank']==3 and result['results'][0][0]==10 and not c.pending
        c.submit(0x1200,[[[1]]]*4)
        try:c.receive();raise AssertionError('no timeout')
        except TimeoutError:assert not c.pending
    finally:c.close();t.join();[s.close() for s in servers]
    if errors:raise errors[0]
    print('PASS: four shards, first OK from rank3, error ignored, late duplicates, bounded table, timeout deletion')
if __name__=='__main__':main()
