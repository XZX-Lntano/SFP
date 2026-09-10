#!/usr/bin/env python3
"""V4 functional client. Use the native benchmark for throughput measurements."""
import argparse
import socket
import struct

BRIDGE_MAGIC = 0x4D504247
BRIDGE_VERSION = 4
MAX_ENTRIES = 64
MAX_WORKERS = 4
MAX_ROUNDS = 16
APP_MSG_REQUEST = 1
APP_MSG_RESPONSE = 2
HEADER = struct.Struct("!IHHIHHHHI")
STATUS = {0: "OK", 1: "INVALID", 6: "FPGA_TIMEOUT", 7: "FPGA_SEND_FAILED", 13: "RESULT_MISMATCH"}


def build_message(request_id, workers, msg_type=APP_MSG_REQUEST):
    """workers[worker][round][entry], padding each round to 64 values."""
    if not 0 <= request_id <= 0xFFFFFFF0 or request_id & 15:
        raise ValueError("request-id must be a uint32 multiple of 16")
    if not 2 <= len(workers) <= 4:
        raise ValueError("2..4 contiguous workers required")
    rounds = len(workers[0])
    if not 1 <= rounds <= MAX_ROUNDS or any(len(w) != rounds for w in workers):
        raise ValueError("each worker must have the same 1..16 rounds")
    flat = []
    for worker in workers:
        for values in worker:
            if not 1 <= len(values) <= MAX_ENTRIES or any(not 0 <= v < 1 << 64 for v in values):
                raise ValueError("each round needs 1..64 uint64 values")
            flat.extend(values)
            flat.extend([0] * (MAX_ENTRIES - len(values)))
    return HEADER.pack(BRIDGE_MAGIC, 4, msg_type, request_id, rounds, len(workers), 64, 0, 0) + struct.pack(f"!{len(flat)}Q", *flat)


def decode_message(raw):
    if len(raw) < HEADER.size:
        raise ValueError("short response")
    magic, version, kind, base, rounds, workers, entries, status, reserved = HEADER.unpack_from(raw)
    if (magic, version, kind, entries, reserved) != (BRIDGE_MAGIC, 4, 2, 64, 0):
        raise ValueError("invalid response header")
    if base & 15 or not 1 <= rounds <= 16 or not 2 <= workers <= 4 or len(raw) != 24 + rounds * 512:
        raise ValueError("invalid response layout")
    values = struct.unpack_from(f"!{rounds * 64}Q", raw, 24)
    return dict(magic=magic, version=version, msg_type=kind, request_id=base,
                round_count=rounds, worker_count=workers, entry_count=entries,
                status=status, results=[list(values[r*64:(r+1)*64]) for r in range(rounds)])


def main():
    parser = argparse.ArgumentParser(description="V4 MPI-FPGA batch client")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=10000)
    parser.add_argument("--request-id", type=lambda s: int(s, 0), default=0x1000)
    parser.add_argument("--rounds", type=int, choices=range(1, 17), default=1)
    parser.add_argument("--timeout", type=float, default=2)
    parser.add_argument("--stop", action="store_true")
    for w in range(4):
        parser.add_argument(f"--worker{w}-values", nargs="+", type=lambda s: int(s, 0))
    args = parser.parse_args()
    if args.stop:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.sendto(HEADER.pack(BRIDGE_MAGIC, 4, 3, 0, 0, 0, 64, 0, 0), (args.host, args.port))
        print("Stop requested; the bridge drains outstanding batches before exit.")
        return 0
    supplied = [getattr(args, f"worker{w}_values") for w in range(4)]
    count = sum(v is not None for v in supplied)
    if count < 2 or any(v is None for v in supplied[:count]):
        parser.error("workers must be contiguous from worker0")
    if len({len(v) for v in supplied[:count]}) != 1:
        parser.error("workers must have the same value count")
    try:
        payload = build_message(args.request_id, [[v] * args.rounds for v in supplied[:count]])
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.settimeout(args.timeout)
            sock.connect((args.host, args.port))
            sock.send(payload)
            reply = decode_message(sock.recv(65536))
        if reply['request_id'] != args.request_id or reply['round_count'] != args.rounds or reply['worker_count'] != count:
            raise ValueError("response does not match request")
        print(f"request_id={reply['request_id']} workers={count} rounds={args.rounds} status={STATUS.get(reply['status'], reply['status'])}")
        for r, values in enumerate(reply['results']):
            print(f"round_id={args.request_id+r}: {values[:len(supplied[0])]}")
        return int(reply['status'] != 0)
    except (ValueError, OSError) as exc:
        print(exc)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
