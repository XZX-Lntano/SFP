from pathlib import Path
import subprocess,json
R=Path('~/Desktop/SFP/进程间计算');O=Path(__file__).resolve().parent
M=['/usr/local/openmpi/bin/mpirun','--allow-run-as-root','-np','4','--bind-to','core','--map-by','core']
cflags=subprocess.check_output(['pkg-config','--cflags','libdpdk'],text=True).split();libs=subprocess.check_output(['pkg-config','--libs','libdpdk'],text=True).split()
for ring in [64,512]:
 dp=(O/f'dpdk_{ring}.c').read_text().replace('pool_name,511,32,','pool_name,2047,32,');(O/f'pool2047_rx{ring}.c').write_text(dp)
 subprocess.run(['/usr/local/openmpi/bin/mpicc',*cflags,'-O3','-march=native','-std=c11','-Wall','-Wextra','-Werror','-I',str(R),str(R/'benchmark_dpdk.c'),str(O/f'pool2047_rx{ring}.c'),'-o',str(O/f'clean_rx{ring}'),*libs],check=True)
rows=[]
for rep in range(2):
 for ring in ([64,512] if not rep else [512,64]):
  for window in [8,16]:
   label=f'clean_r{ring}_w{window}_rep{rep}';p=subprocess.run(M+[str(O/f'clean_rx{ring}'),'--window',str(window),'--batches','200000'],cwd=R,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=60)
   (O/(label+'.log')).write_text(p.stdout);rows.append({'name':label,'returncode':p.returncode});(O/'control_runs.json').write_text(json.dumps(rows,indent=2))
   print(label,p.returncode,' '.join(x for x in p.stdout.splitlines() if 'useful result' in x or 'missed=' in x),flush=True)
   if p.returncode:raise SystemExit('control failed')
