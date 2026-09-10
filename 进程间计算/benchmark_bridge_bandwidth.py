#!/usr/bin/env python3
"""Launch the compiled sliding-window generator without a Python per-packet loop."""
import os
from pathlib import Path
import subprocess
import sys

if __name__ == "__main__":
    root = Path(__file__).resolve().parent
    subprocess.run(["make", "benchmark_bridge_native"], cwd=root, check=True)
    os.execv(str(root / "benchmark_bridge_native"), ["benchmark_bridge_native", *sys.argv[1:]])
