#!/usr/bin/env python3
"""Check real C v4 serialization against independent Python wire expectations.

This is a protocol test, not formal RTL equivalence or a timing sign-off.
Run test_batch_rtl.py separately to execute the actual RTL in XSim.
"""
import ctypes
from pathlib import Path
import random
import struct
import subprocess
import tempfile
from bridge_app_client import build_message, decode_message, HEADER

ROOT = Path(__file__).resolve().parent


def fixture(directory):
    library = Path(directory) / "protocol_fixture.so"
    subprocess.run(["cc", "-shared", "-fPIC", "-O2", "-Wall", "-Wextra", "-Werror",
                    str(ROOT / "protocol_fixture.c"), "-o", str(library)], check=True)
    lib = ctypes.CDLL(str(library))
    lib.fixture_frame.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint]
    lib.fixture_frame.restype = ctypes.c_size_t
    lib.fixture_parse.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
    lib.fixture_app.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]
    return lib


def make_frame(lib, request, worker):
    result = ctypes.create_string_buffer(9000)
    n = lib.fixture_frame(result, request, worker)
    return result.raw[:n]


def main():
    rng = random.Random(415)
    cases = 0
    with tempfile.TemporaryDirectory() as tmp:
        lib = fixture(tmp)
        for count in (2, 3, 4):
            for rounds in (1, 2, 7, 16):
                workers = [[[rng.getrandbits(64) for _ in range(64)] for _ in range(rounds)] for _ in range(count)]
                for base in (0, 0x12345670, 0xFFFFFFF0):
                    request = build_message(base, workers)
                    assert lib.fixture_app(request, len(request), 0)
                    for worker in range(count):
                        frame = make_frame(lib, request, worker)
                        assert len(frame) == 48 + rounds * 520
                        assert len(frame) - 14 <= 9000
                        assert struct.unpack_from("!H", frame, 16)[0] == len(frame) - 14
                        assert struct.unpack_from("!H", frame, 38)[0] == len(frame) - 34
                        assert frame[42:48] == struct.pack("!HBBBB", 0x4147, 4, count, rounds, 1)
                        for r in range(rounds):
                            assert frame[48+r*520:56+r*520] == struct.pack("!II", base+r, 0)
                            assert frame[56+r*520:568+r*520] == struct.pack("!64Q", *workers[worker][r])
                    result = bytearray(make_frame(lib, request, 0))
                    result[47] = 2
                    expected = [[sum(w[r][i] for w in workers) & ((1 << 64)-1) for i in range(64)] for r in range(rounds)]
                    for r in range(rounds):
                        result[56+r*520:568+r*520] = struct.pack("!64Q", *expected[r])
                    values = ctypes.create_string_buffer(8192)
                    assert lib.fixture_parse(bytes(result), len(result), values)
                    payload = b"".join(struct.pack("!64Q", *v) for v in expected)
                    assert values.raw[:rounds*512] == payload
                    response = HEADER.pack(0x4D504247, 4, 2, base, rounds, count, 64, 0, 0) + payload
                    assert decode_message(response)["results"] == expected
                    assert lib.fixture_app(response, len(response), 1)
                    for bad in (result[:-1], result + b"\0"):
                        assert not lib.fixture_parse(bytes(bad), len(bad), values)
                    for offset in (14, 23, 36, 42, 44, 47, 51, 52):
                        bad = result.copy(); bad[offset] ^= 1
                        assert not lib.fixture_parse(bytes(bad), len(bad), values), offset
                    cases += 1
    print(f"PASS: {cases} C/Python v4 cases; 2/3/4 workers, 1..16 rounds, u64 overflow, invalid frames")
    print("Frame = 48 + 520*rounds bytes; values = 56 + 520*round + 8*entry. No per-entry index.")
    print("FIFO DEPTH in verilog-axis is in bytes at KEEP_ENABLE=1, not 64-bit beats.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
