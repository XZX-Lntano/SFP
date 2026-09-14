from pathlib import Path
import sys, subprocess, struct, json, re
ROOT=Path('~/Desktop/SFP/进程间计算');OUT=Path(__file__).resolve().parent;SIM=OUT/'rtl';SIM.mkdir(exist_ok=True)
sys.path.insert(0,str(ROOT))
from pre_synth_equivalence_check import fixture, make_frame
from bridge_app_client import build_message
import tempfile
expected={}
with tempfile.TemporaryDirectory() as tmp:
 lib=fixture(tmp);files=[(SIM/f'frames{p}.mem').open('w') for p in range(4)]
 for b in range(64):
  data=[[[100000*(p+1)+r*64+i+b for i in range(64)] for r in range(16)] for p in range(4)]
  req=build_message(b*16,data);frames=[make_frame(lib,req,p) for p in range(4)]
  result=bytearray(frames[0]);result[47]=2
  for r in range(16):result[56+r*520:568+r*520]=struct.pack('!64Q',*[sum(data[p][r][i] for p in range(4)) for i in range(64)])
  expected[b*16]=bytes(result)
  for p in range(4):files[p].write(''.join(f'{int.from_bytes(frames[p][i:i+8],"little"):016x}\n' for i in range(0,len(frames[p]),8)))
 for f in files:f.close()
rtl=ROOT.parent/'corundum/fpga/mqnic/ZCU102/fpga/rtl/axis_udp_batch_aggregator.v';axis=ROOT.parent/'corundum/fpga/lib/axis/rtl'
def cmd(args,log):
 with (SIM/log).open('w') as f:subprocess.run(args,cwd=SIM,stdout=f,stderr=subprocess.STDOUT,check=True,timeout=180)
rows=json.loads((OUT/'rtl_results.json').read_text())
for slots,depth in [(512,65536)]:
 top=(OUT/'fifo_tb.sv').read_text().replace('parameter SLOTS=256, DEPTH=16384;',f'parameter SLOTS={slots}, DEPTH={depth};')
 (SIM/'top.sv').write_text(top)
 cmd(['xvlog','--sv',str(rtl),str(axis/'axis_broadcast.v'),str(axis/'axis_async_fifo.v'),'top.sv'],f'compile_{slots}_{depth}.log')
 snap=f's{slots}d{depth}'
 cmd(['xelab','fifo_tb','-s',snap],f'elab_{snap}.log')
 for pause,skew in [(0,0),(30,0),(120,0),(0,120)]:
  label=f'{snap}_p{pause}_k{skew}';cmd(['xsim',snap,'-testplusarg',f'PAUSE={pause}','-testplusarg',f'SKEW={skew}','-runall'],label+'.log')
  log=(SIM/(label+'.log')).read_text();lines=[l for l in log.splitlines() if l.startswith(('AGG ','FIFO '))]
  row={'slots':slots,'depth':depth,'pause_us':pause,'skew_us':skew,'counters':lines,'ports':[]}
  for p in range(4):
   got={};frame=bytearray();bad=0;duplicates=0
   for l in (SIM/f'out{p}.txt').read_text().splitlines():
    word,last=l.split();frame.extend(int(word,16).to_bytes(8,'little'))
    if int(last):
     base=struct.unpack_from('!I',frame,48)[0];duplicates+=base in got;bad+=bytes(frame)!=expected.get(base);got[base]=True;frame.clear()
   row['ports'].append({'received':len(got),'bad':bad,'duplicates':duplicates,'partial':len(frame)})
   (SIM/f'out{p}.txt').unlink()
  rows.append(row);(OUT/'rtl_results.json').write_text(json.dumps(rows,indent=2));print(label,lines,row['ports'],flush=True)
