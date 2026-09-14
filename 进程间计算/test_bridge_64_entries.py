"""Real four-port client numeric checks against a running v5 bridge."""
import argparse,time
from bridge_app_client import FourPortClient

def main():
    p=argparse.ArgumentParser();p.add_argument('--port',type=int,default=10000);a=p.parse_args()
    c=FourPortClient(port=a.port)
    try:
        for count in (2,3,4):
            for rounds in (1,3,16):
                base=0x8000+count*512+rounds*16
                values=[[[((1<<64)-1 if i==0 else w+i+r) for i in range(64)] for r in range(rounds)] for w in range(count)]
                c.submit(base,values);reply=c.receive()
                expected=[[sum(values[w][r][i] for w in range(count))%(1<<64) for i in range(64)] for r in range(rounds)]
                assert reply and reply['results']==expected and not c.pending
                print(f'PASS workers={count} rounds={rounds} winner={reply["rank"]}')
                time.sleep(.01)
    finally:c.close()
if __name__=='__main__':main()
