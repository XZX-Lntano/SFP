from pathlib import Path
import subprocess, os, json, re, time, signal, socket, struct
ROOT=Path('~/Desktop/SFP/进程间计算'); OUT=Path(__file__).resolve().parent
MPI=['/usr/local/openmpi/bin/mpirun','--allow-run-as-root','-np','4','--bind-to','core','--map-by','core']
def run(name,args,timeout=90):
    t=time.time()
    try:
        p=subprocess.run(args,cwd=ROOT,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=timeout)
        code=p.returncode; log=p.stdout
    except subprocess.TimeoutExpired as e:
        code=124; log=(e.stdout or b'').decode() if isinstance(e.stdout,bytes) else (e.stdout or '')
    (OUT/(name+'.log')).write_text(log)
    row={'name':name,'returncode':code,'seconds':time.time()-t}
    rows.append(row);(OUT/'host_runs.json').write_text(json.dumps(rows,indent=2))
    print(name,code,' '.join(x for x in log.splitlines() if any(k in x for k in ['useful','DIAG','missed=','latency','timeout','DPDK direct'])),flush=True)
    return code
rows=[]
# Diagnostic copies only; preserve production executables and source.
src=(ROOT/'benchmark_dpdk.c').read_text()
src=src.replace('uint64_t complete=0,errors=0;', 'uint64_t complete=0,errors=0,loops=0,empty=0,rxmax=0,short_calls=0; double rttsum=0,rttmax=0,lastpoll=now(),gapmax=0;')
src=src.replace('while(complete<batches) {','while(complete<batches) {\n        loops++;')
src=src.replace('if(sent<count) port.tx_short++;','if(sent<count) {port.tx_short++;short_calls++;}')
src=src.replace('struct rte_mbuf *rx[32];unsigned n=', 'double polltime=now(); if(polltime-lastpoll>gapmax) gapmax=polltime-lastpoll; lastpoll=polltime;\n        struct rte_mbuf *rx[32];unsigned n=')
src=src.replace('port.rx_packets+=n;', 'port.rx_packets+=n; if(!n) empty++; if(n>rxmax) rxmax=n;')
src=src.replace('complete++;p->sequence', 'double rtt=now()-p->started;rttsum+=rtt;if(rtt>rttmax)rttmax=rtt;\n                    complete++;p->sequence')
src=src.replace('double elapsed=now()-start,max_elapsed=0;', 'fprintf(stderr,"DIAG rank=%d rtt_mean_us=%.3f rtt_max_us=%.3f poll_gap_max_us=%.3f rx_burst_max=%lu empty_loops=%lu loops=%lu short=%lu\\n",rank,rttsum/complete*1e6,rttmax*1e6,gapmax*1e6,rxmax,empty,loops,short_calls);\n    double elapsed=now()-start,max_elapsed=0;')
(OUT/'benchmark_diag.c').write_text(src)
cf=subprocess.check_output(['pkg-config','--cflags','libdpdk'],text=True).split();libs=subprocess.check_output(['pkg-config','--libs','libdpdk'],text=True).split()
for ring in [64,512]:
    dp=(ROOT/'dpdk_port.c').read_text().replace('uint16_t rx=64,tx=128;',f'uint16_t rx={ring},tx=128;')
    dp=dp.replace('struct rte_eth_link link={0};', 'struct rte_eth_link link={0}; for(int waitlink=0;waitlink<50;waitlink++){rte_eth_link_get_nowait(p->id,&link);if(link.link_status)break;usleep(100000);}')
    if ring==512:dp=dp.replace('pool_name,511,32,','pool_name,2047,32,')
    (OUT/f'dpdk_{ring}.c').write_text(dp)
    for name,source in [('direct','benchmark_diag.c'),('bridge',str(ROOT/'mpi_fpga_bridge.c'))]:
        subprocess.run(['/usr/local/openmpi/bin/mpicc',*cf,'-O3','-march=native','-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT),str(OUT/source),str(OUT/f'dpdk_{ring}.c'),'-o',str(OUT/f'{name}_{ring}'),*libs],check=True)
for repeat in range(2):
    for window in ([1,2,4,8,16] if repeat==0 else [16,8,4,2,1]):
        if run(f'direct_r64_w{window}_rep{repeat}',MPI+[str(OUT/'direct_64'),'--window',str(window),'--batches','100000']):raise SystemExit('direct failed')
for window in [8,16]:
    if run(f'direct_r512_w{window}',MPI+[str(OUT/'direct_512'),'--window',str(window),'--batches','100000']):raise SystemExit('direct ring512 failed')
for ring in [64,512]:
    port=10120
    with (OUT/f'bridge_r{ring}.log').open('w') as log:
        server=subprocess.Popen(MPI+[str(OUT/f'bridge_{ring}'),'--port',str(port)],cwd=ROOT,stdout=log,stderr=log,start_new_session=True)
        try:
            deadline=time.monotonic()+45
            while 'bridge v4:' not in (OUT/f'bridge_r{ring}.log').read_text():
                if server.poll() is not None or time.monotonic()>deadline:raise RuntimeError('bridge startup failed')
                time.sleep(.1)
            for w in [1,2,4,8,16,16,8]:
                if run(f'app_r{ring}_w{w}_n{len(rows)}',[str(ROOT/'benchmark_bridge_native'),'--port',str(port),'--workers','4','--rounds','16','--window',str(w),'--duration','3','--cpu','4']):raise RuntimeError('app failed')
            with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as sk:sk.sendto(struct.pack('!IHHIHHHHI',0x4d504247,4,3,0,0,0,64,0,0),('127.0.0.1',port))
            server.wait(timeout=15)
        finally:
            if server.poll() is None:
                os.killpg(server.pid,signal.SIGTERM)
                try:server.wait(timeout=8)
                except subprocess.TimeoutExpired:os.killpg(server.pid,signal.SIGKILL);server.wait()
print('HOST MATRIX COMPLETE',flush=True)
