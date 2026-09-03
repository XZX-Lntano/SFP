#!/usr/bin/env python3
"""Benchmark end-to-end MPI-FPGA aggregation goodput with 64 entries."""

import argparse
import socket
import struct
import time

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

FPGA_PAYLOAD_BYTES = 2 + MAX_ENTRIES * 8
ETHERNET_IPV4_UDP_BYTES = 14 + 20 + 8
RESULT_BYTES = MAX_ENTRIES * 8


def format_rate(byte_count, seconds):
    if seconds <= 0:
        return "n/a"
    mib_per_second = byte_count / seconds / (1024 * 1024)
    mbit_per_second = byte_count * 8 / seconds / 1_000_000
    return f"{mib_per_second:.3f} MiB/s ({mbit_per_second:.3f} Mbit/s)"


def build_workers(worker_count):
    return [
        [
            1000 * (worker + 1) + index
            for index in range(1, MAX_ENTRIES + 1)
        ]
        for worker in range(worker_count)
    ]


def check_reply(reply, request_id, worker_count):
    header_is_valid = (
        reply["magic"] == BRIDGE_MAGIC
        and reply["version"] == BRIDGE_VERSION
        and reply["request_id"] == request_id
        and reply["worker_count"] == worker_count
        and reply["entry_count"] == MAX_ENTRIES
        and reply["status"] == 0
    )
    if not header_is_valid:
        return False
    result = reply["result_entries"]
    return all(result[index - 1] == sum(1000 * (worker + 1) + index for worker in range(worker_count)) for index in range(1, MAX_ENTRIES + 1))


def send_request(sock, peer, payload, request_id, worker_count):
    start = time.perf_counter()
    sock.sendto(payload, peer)
    raw, _ = sock.recvfrom(struct.calcsize(WIRE_FMT) + 64)
    elapsed = time.perf_counter() - start
    reply = decode_message(raw)
    return check_reply(reply, request_id, worker_count), elapsed, reply


def run_warmup(sock, peer, worker_count, rounds):
    workers = build_workers(worker_count)
    for request_id in range(1, rounds + 1):
        payload = build_message(request_id, workers, APP_MSG_REQUEST)
        ok, _, reply = send_request(
            sock, peer, payload, request_id, worker_count
        )
        if not ok:
            status = STATUS.get(reply["status"], "UNKNOWN")
            raise RuntimeError(f"warmup failed: status={reply['status']}({status})")


def main():
    parser = argparse.ArgumentParser(
        description="Benchmark 64-entry end-to-end MPI-FPGA aggregation goodput"
    )
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=10000)
    parser.add_argument("--workers", type=int, choices=(2, 3, 4), default=4)
    parser.add_argument("--duration", type=float, default=30.0)
    parser.add_argument("--warmup", type=int, default=5)
    parser.add_argument("--timeout", type=float, default=8.0)
    parser.add_argument(
        "--request-id-start",
        type=lambda value: int(value, 0),
        default=0x9000,
    )
    args = parser.parse_args()

    if args.duration <= 0:
        parser.error("--duration 必须大于 0")
    if args.warmup < 0:
        parser.error("--warmup 不能小于 0")

    peer = (args.host, args.port)
    workers = build_workers(args.workers)
    wire_bytes = struct.calcsize(WIRE_FMT)
    successes = 0
    failures = 0
    latencies = []
    failure_statuses = {}

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.settimeout(args.timeout)
        if args.warmup:
            print(f"warmup: {args.warmup} rounds")
            run_warmup(sock, peer, args.workers, args.warmup)

        print(
            f"benchmark: workers={args.workers}, entries={MAX_ENTRIES}, "
            f"duration={args.duration:.1f}s, peer={args.host}:{args.port}"
        )
        start = time.perf_counter()
        deadline = start + args.duration
        request_id = args.request_id_start

        while time.perf_counter() < deadline:
            payload = build_message(request_id, workers, APP_MSG_REQUEST)
            try:
                ok, latency, reply = send_request(
                    sock, peer, payload, request_id, args.workers
                )
            except (socket.timeout, ValueError, OSError) as exc:
                failures += 1
                failure_statuses[type(exc).__name__] = (
                    failure_statuses.get(type(exc).__name__, 0) + 1
                )
            else:
                if ok:
                    successes += 1
                    latencies.append(latency)
                else:
                    failures += 1
                    name = STATUS.get(reply["status"], "INVALID_REPLY")
                    failure_statuses[name] = failure_statuses.get(name, 0) + 1
            request_id = (request_id + 1) & 0xFFFFFFFF

        elapsed = time.perf_counter() - start

    active_input_bytes = successes * args.workers * FPGA_PAYLOAD_BYTES
    broadcast_output_bytes = successes * 4 * FPGA_PAYLOAD_BYTES
    result_goodput_bytes = successes * RESULT_BYTES
    app_round_trip_bytes = successes * wire_bytes * 2
    average_latency_ms = (
        sum(latencies) / len(latencies) * 1000 if latencies else 0.0
    )
    p99_latency_ms = (
        sorted(latencies)[int((len(latencies) - 1) * 0.99)] * 1000
        if latencies
        else 0.0
    )

    print("\nresult")
    print(f"elapsed             : {elapsed:.3f} s")
    print(f"completed rounds    : {successes}")
    print(f"failed rounds       : {failures}")
    print(f"round rate          : {successes / elapsed:.2f} rounds/s")
    print(f"avg round-trip      : {average_latency_ms:.3f} ms")
    print(f"p99 round-trip      : {p99_latency_ms:.3f} ms")
    print(f"system effective BW : {format_rate(result_goodput_bytes, elapsed)}")
    print(f"  useful result     : {RESULT_BYTES} B/round (64 uint64 values)")
    print(f"FPGA input payload  : {format_rate(active_input_bytes, elapsed)}")
    print(f"FPGA broadcast load : {format_rate(broadcast_output_bytes, elapsed)}")
    if broadcast_output_bytes:
        print(f"broadcast efficiency: {result_goodput_bytes / broadcast_output_bytes:.2%}")
    print(f"app UDP round-trip  : {format_rate(app_round_trip_bytes, elapsed)}")
    print(
        "payload definition  : result=64*8 B/round; "
        "FPGA payload=(round_id + 64*8 B) per physical frame"
    )
    if failure_statuses:
        print(f"failures by type    : {failure_statuses}")


if __name__ == "__main__":
    main()
