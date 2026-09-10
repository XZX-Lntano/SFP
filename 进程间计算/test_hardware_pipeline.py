#!/usr/bin/env python3
"""Bounded real-NIC/FPGA validation. Requires v4 firmware and configured VFIO ports."""
import os
from pathlib import Path
import signal
import socket
import subprocess
import time
from bridge_app_client import HEADER

ROOT=Path(__file__).resolve().parent
REPORTS=ROOT/"reports"
MPI=["/usr/local/openmpi/bin/mpirun","--allow-run-as-root","-np","4","--bind-to","core","--map-by","core"]


def run(label,command,timeout=90):
    result=subprocess.run(command,cwd=ROOT,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=timeout)
    (REPORTS/(label+".log")).write_text(result.stdout)
    print(label+":",flush=True)
    print(result.stdout,flush=True)
    if result.returncode:
        raise RuntimeError(f"{label}: exit {result.returncode}")


def main():
    if os.geteuid(): raise SystemExit("Run with sudo for VFIO access")
    REPORTS.mkdir(exist_ok=True)
    run("hardware_smoke",MPI+[str(ROOT/"benchmark_dpdk"),"--workers","2","--rounds","1","--window","1","--batches","4"])
    with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.1",0));port=probe.getsockname()[1]
    report=REPORTS/"hardware_bridge.log"
    with report.open("w") as log:
        server=subprocess.Popen(MPI+[str(ROOT/"mpi_fpga_bridge"),"--port",str(port)],
                                cwd=ROOT,stdout=log,stderr=log,start_new_session=True)
        try:
            deadline=time.monotonic()+30
            while "bridge v4:" not in report.read_text():
                if server.poll() is not None or time.monotonic()>deadline:
                    raise RuntimeError(report.read_text())
                time.sleep(0.1)
            run("hardware_numeric",["python3",str(ROOT/"test_bridge_64_entries.py"),"--port",str(port)])
            cases=[(4,1,1),(4,16,1),(2,16,16),(3,16,16),(4,16,16)]
            for workers,rounds,window in cases:
                run(f"hardware_app_w{workers}_r{rounds}_q{window}",
                    [str(ROOT/"benchmark_bridge_native"),"--port",str(port),"--workers",str(workers),
                     "--rounds",str(rounds),"--window",str(window),"--duration","5","--cpu","4"])
            with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as sock:
                sock.sendto(HEADER.pack(0x4D504247,4,3,0,0,0,64,0,0),("127.0.0.1",port))
            server.wait(timeout=15)
            if server.returncode: raise RuntimeError(report.read_text())
        finally:
            if server.poll() is None:
                os.killpg(server.pid,signal.SIGTERM)
                try: server.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    os.killpg(server.pid,signal.SIGKILL);server.wait()
    for workers in (2,3,4):
        run(f"hardware_direct_w{workers}",MPI+[str(ROOT/"benchmark_dpdk"),"--workers",str(workers),
            "--rounds","16","--window","16","--batches","100000"])
    run("hardware_tx_only",MPI+[str(ROOT/"benchmark_dpdk"),"--tx-only","--workers","4",
        "--rounds","16","--batches","1000000"])
    print("PASS: hardware numeric tests, end-to-end matrix, and direct DPDK tests",flush=True)


if __name__=="__main__": main()
