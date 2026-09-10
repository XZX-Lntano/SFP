#!/usr/bin/env python3
"""Execute the real streaming RTL with C-produced requests and Python scoreboarding."""
from pathlib import Path
import random
import shutil
import struct
import subprocess
import tempfile
from bridge_app_client import build_message
from pre_synth_equivalence_check import fixture, make_frame

ROOT = Path(__file__).resolve().parent
RTL = ROOT.parent / "corundum/fpga/mqnic/ZCU102/fpga/rtl/axis_udp_batch_aggregator.v"
CYCLES = 180000


def main():
    rng = random.Random(975)
    output = ROOT / "reports" / "rtl_test"
    output.mkdir(parents=True, exist_ok=True)
    vectors = [[0] * CYCLES for _ in range(4)]
    expected = {}
    with tempfile.TemporaryDirectory() as tmp:
        lib = fixture(tmp)

        def packet(base, rounds, workers, expect=True):
            data = [[[rng.getrandbits(64) for _ in range(64)] for _ in range(rounds)] for _ in range(workers)]
            req = build_message(base, data)
            frames = [make_frame(lib, req, w) for w in range(workers)]
            if expect:
                result = bytearray(frames[0]); result[47] = 2
                for r in range(rounds):
                    result[56+r*520:568+r*520] = struct.pack("!64Q", *[sum(w[r][i] for w in data) % (1 << 64) for i in range(64)])
                expected[base] = bytes(result)
            return frames

        def send(port, start, frame, gaps=False, bad_keep=False):
            for offset in range(0, len(frame), 8):
                word = int.from_bytes(frame[offset:offset+8], "little")
                final = offset + 8 == len(frame)
                keep = 0x7f if final and bad_keep else 0xff
                assert not vectors[port][start]
                vectors[port][start] = word | keep << 64 | int(final) << 72 | 1 << 73
                start += 1 + (rng.randrange(3) if gaps else 0)
            return start + 2

        # All 256 slots occupied on three ports before the fourth starts.
        cursor = [100, 100, 100, 18000]
        batches = [packet(i*16, 16, 4) for i in range(16)]
        for w in range(4):
            order = list(range(16)) if w!=3 else list(reversed(range(16)))
            for i in order:
                cursor[w] = send(w, cursor[w], batches[i][w])
        # Duplicate worker contributions are discarded, never double-added.
        send(0, 16900, batches[0][0])
        for i, (rounds, workers) in enumerate(((1,2),(3,3),(16,4))):
            frames = packet(256+i*16, rounds, workers)
            for w, frame in enumerate(frames): send(w, 45000+i*4000+w*31, frame, gaps=True)
        # Incomplete batch expires; a newer generation reuses the same slot.
        incomplete = packet(0x1030, 1, 2, expect=False)
        send(0, 60000, incomplete[0])
        recovery = packet(0x1130, 2, 2)
        for w, frame in enumerate(recovery): send(w, 120000+w*71, frame)
        # Bad headers, wrong record ID, truncated records and bad final keep.
        bad_frames = packet(0x1040, 2, 2, expect=False)
        for j, offset in enumerate((12, 14, 23, 36, 42, 44, 47, 568)):
            bad = bytearray(bad_frames[0]);bad[offset] ^= 1
            send(0, 130000+j*1000, bad)
        send(0, 140000, bad_frames[0][:-8])
        send(0, 141000, bad_frames[0], bad_keep=True)
        for w in range(4):
            (output / f"input{w}.mem").write_text("".join(f"{v:020x}\n" for v in vectors[w]))
        for command in (["xvlog", "--sv", str(RTL), str(ROOT/"test_batch_rtl.sv")],
                        ["xelab", "test_batch_rtl", "-s", "batch_test"],
                        ["xsim", "batch_test", "-runall"]):
            if not shutil.which(command[0]):
                raise SystemExit(f"{command[0]} not found; source Vivado settings64.sh")
            log = output / (command[0] + ".log")
            with log.open("w") as f:
                subprocess.run(command, cwd=output, stdout=f, stderr=subprocess.STDOUT, check=True, timeout=180)
        received = {}
        frame = bytearray()
        for line in (output/"output.txt").read_text().splitlines():
            word, last = line.split()
            frame.extend(int(word,16).to_bytes(8,"little"))
            if int(last):
                base = struct.unpack_from("!I",frame,48)[0]
                assert base not in received, f"duplicate output {base}"
                received[base] = bytes(frame);frame.clear()
        assert not frame, "incomplete output frame"
        assert received.keys() == expected.keys(), (received.keys(), expected.keys())
        for base, result in expected.items():
            actual = received[base]
            assert actual == result, (base, len(actual), len(result), next((i for i,(a,b) in enumerate(zip(actual,result)) if a!=b),None))
        print(f"PASS: {len(expected)} RTL output frames, all 256 slots, four parallel inputs, reordered batches, 2/3/4 workers, random u64 overflow, input gaps, AXIS backpressure, duplicates, invalid frames, timeout/reuse")


if __name__ == "__main__":
    main()
