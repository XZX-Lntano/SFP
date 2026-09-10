#!/usr/bin/env python3
"""Reversible host setup for this machine's four dedicated X710 ports."""
import argparse
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent
STATE = ROOT / "reports" / "host_before_dpdk.json"
BDFS = [f"0000:01:00.{i}" for i in range(4)]
SYSCTLS = {"net.core.rmem_max": 16777216, "net.core.wmem_max": 16777216}


def run(*args):
    return subprocess.check_output(args, text=True).strip()


def save(state):
    STATE.parent.mkdir(exist_ok=True)
    STATE.write_text(json.dumps(state, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("buffers", "dpdk", "restore"))
    args = parser.parse_args()
    if os.geteuid():
        parser.error("run with sudo; original state is saved for restore")
    state = json.loads(STATE.read_text()) if STATE.exists() else {"sysctls": {}, "ports": {}}
    if args.mode == "restore":
        for bdf, original in state["ports"].items():
            run("dpdk-devbind.py", "--bind=" + original["driver"], bdf)
            for interface, cfg in original["interfaces"].items():
                run("ip", "link", "set", interface, "mtu", str(cfg["mtu"]))
                run("ip", "link", "set", interface, "up" if cfg["up"] else "down")
        for key, value in state["sysctls"].items():
            run("sysctl", "-w", f"{key}={value}")
        if state.get("mounted_hugetlbfs"):
            run("umount", "/dev/hugepages")
        if "hugepages" in state:
            run("sysctl", "-w", f"vm.nr_hugepages={state['hugepages']}")
        STATE.rename(STATE.with_suffix(".restored.json"))
        print("Restored original drivers, interface state, buffers and hugepages")
        return
    for key, value in SYSCTLS.items():
        state["sysctls"].setdefault(key, int(run("sysctl", "-n", key)))
        save(state)
        run("sysctl", "-w", f"{key}={value}")
    if args.mode == "buffers":
        print("Socket buffer maxima set to 16 MiB; existing sockets must be reopened")
        return
    # Management eno1 is deliberately outside the exact PCI allowlist.
    for bdf in BDFS:
        dev = Path("/sys/bus/pci/devices") / bdf
        if (dev / "vendor").read_text().strip() != "0x8086" or (dev / "device").read_text().strip() != "0x1572":
            raise RuntimeError(f"Unexpected NIC at {bdf}")
        if not (dev / "iommu_group").exists():
            raise RuntimeError(f"No IOMMU group for {bdf}; enable intel_iommu=on at boot")
        for member in (dev / "iommu_group" / "devices").iterdir():
            if member.name not in BDFS and not (member/"class").read_text().startswith("0x0604"):
                raise RuntimeError(f"IOMMU group includes unrelated device {member.name}")
        if bdf not in state["ports"]:
            interfaces = {}
            for iface in (dev / "net").glob("*"):
                if json.loads(run("ip", "-j", "route", "show", "dev", iface.name)):
                    raise RuntimeError(f"{iface.name} has a route; refusing to detach an active routed interface")
                link = json.loads(run("ip", "-j", "link", "show", iface.name))[0]
                interfaces[iface.name] = {"mtu": link["mtu"], "up": "UP" in link["flags"]}
            state["ports"][bdf] = {"driver": (dev/"driver").resolve().name, "interfaces": interfaces}
    state.setdefault("hugepages", int(run("sysctl", "-n", "vm.nr_hugepages")))
    state.setdefault("mounted_hugetlbfs", not os.path.ismount("/dev/hugepages"))
    save(state)
    run("sysctl", "-w", f"vm.nr_hugepages={max(256, state['hugepages'])}")
    if int(run("sysctl", "-n", "vm.nr_hugepages")) < 256:
        raise RuntimeError("Could not allocate 512 MiB of 2 MiB hugepages")
    Path("/dev/hugepages").mkdir(exist_ok=True)
    if not os.path.ismount("/dev/hugepages"):
        run("mount", "-t", "hugetlbfs", "nodev", "/dev/hugepages")
    run("modprobe", "vfio-pci")
    for original in state["ports"].values():
        for iface in original["interfaces"]:
            if (Path("/sys/class/net") / iface).exists():
                run("ip", "link", "set", iface, "mtu", "9000")
                run("ip", "link", "set", iface, "down")
    run("dpdk-devbind.py", "--bind=vfio-pci", *BDFS)
    print(run("dpdk-devbind.py", "--status"))
    print(f"Original state: {STATE}")


if __name__ == "__main__":
    main()
