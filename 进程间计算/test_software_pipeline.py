#!/usr/bin/env python3
"""Start four simulated MPI ranks; test protocol and the real native generator."""
import os
import argparse
from pathlib import Path
import signal
import socket
import subprocess
import time
from bridge_app_client import HEADER

ROOT = Path(__file__).resolve().parent


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--window",type=int,default=16)
    args=parser.parse_args()
    subprocess.run(["make", "all"],cwd=ROOT,check=True)
    with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.1",0))
        port=probe.getsockname()[1]
    report=ROOT/"reports"/"software_pipeline.log"
    report.parent.mkdir(exist_ok=True)
    with report.open("w") as log:
        server=subprocess.Popen(["mpirun","-np","4","--bind-to","core","--map-by","core",
                                 str(ROOT/"mpi_fpga_bridge"),"--simulate","--port",str(port),"--window",str(args.window)],
                                stdout=log,stderr=log,start_new_session=True,cwd=ROOT)
        try:
            deadline=time.monotonic()+20
            while "bridge v4:" not in report.read_text():
                if server.poll() is not None or time.monotonic()>deadline:
                    raise RuntimeError(report.read_text())
                time.sleep(0.05)
            subprocess.run(["python3",str(ROOT/"test_bridge_64_entries.py"),"--port",str(port)],check=True,timeout=30)
            for workers in (2,3,4):
                run=subprocess.run([str(ROOT/"benchmark_bridge_native"),"--port",str(port),"--workers",str(workers),
                                    "--rounds","16","--window",str(args.window),"--duration","2","--cpu","4"],
                                   text=True,capture_output=True,timeout=15)
                print(run.stdout,end="")
                (ROOT/"reports"/f"software_workers{workers}_window{args.window}.txt").write_text(run.stdout+run.stderr)
                if run.returncode: raise RuntimeError(run.stderr+run.stdout)
            with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as sock:
                sock.sendto(HEADER.pack(0x4D504247,4,3,0,0,0,64,0,0),("127.0.0.1",port))
            server.wait(timeout=10)
            assert server.returncode==0, report.read_text()
            print("PASS: four-rank nonblocking MPI simulation; functional and sliding-window tests. Not FPGA throughput.")
        finally:
            if server.poll() is None:
                os.killpg(server.pid,signal.SIGTERM)
                try: server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(server.pid,signal.SIGKILL);server.wait()


if __name__ == "__main__":
    main()
