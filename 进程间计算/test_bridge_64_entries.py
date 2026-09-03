#!/usr/bin/env python3
"""对已烧录的四端口聚合 FPGA 执行 2/3/4 worker、每路 64 项端到端校验。"""
import argparse
import socket
import struct
import sys

from bridge_app_client import (
    APP_MSG_REQUEST,
    BRIDGE_MAGIC,
    BRIDGE_VERSION,
    MAX_ENTRIES,
    STATUS,
    WIRE_FMT,
    build_message,
    decode_message,
)


def request(host, port, payload):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.settimeout(12.0)
        sock.sendto(payload, (host, port))
        return sock.recvfrom(struct.calcsize(WIRE_FMT) + 64)[0]


def run_case(host, port, worker_count):
    workers = [
        [
            1000 * (worker + 1) + index
            for index in range(1, MAX_ENTRIES + 1)
        ]
        for worker in range(worker_count)
    ]
    request_id = 0x4000 + worker_count
    try:
        payload = build_message(request_id, workers, APP_MSG_REQUEST)
        reply = decode_message(request(host, port, payload))
    except (socket.timeout, ValueError) as exc:
        print(f"{worker_count} worker: FAIL ({exc})")
        return False
    checks = [
        ("magic", reply["magic"] == BRIDGE_MAGIC),
        ("version", reply["version"] == BRIDGE_VERSION),
        ("status", reply["status"] == 0),
        ("request_id", reply["request_id"] == request_id),
        ("worker_count", reply["worker_count"] == worker_count),
        ("entry_count", reply["entry_count"] == MAX_ENTRIES),
    ]
    result = reply["result_entries"]
    for index in range(1, MAX_ENTRIES + 1):
        expected = sum(
            1000 * (worker + 1) + index
            for worker in range(worker_count)
        )
        checks.append((f"index={index}", result[index - 1] == expected))
    failed = [name for name, ok in checks if not ok]
    if failed:
        print(
            f"{worker_count} worker: FAIL, "
            f"status={reply['status']}({STATUS.get(reply['status'])}), "
            f"failed={failed[:8]}"
        )
        return False
    print(
        f"{worker_count} worker: PASS, 64 entries, "
        f"value1={result[0]}, value64={result[63]}"
    )
    return True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=10000)
    parser.add_argument(
        "--workers",
        type=int,
        choices=(2, 3, 4),
        help="只测试指定 worker 数；默认依次测试 2/3/4",
    )
    args = parser.parse_args()
    cases = (args.workers,) if args.workers else (2, 3, 4)
    passed = all(run_case(args.host, args.port, count) for count in cases)
    print("整体结果:", "PASS" if passed else "FAIL")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
