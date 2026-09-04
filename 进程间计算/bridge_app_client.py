#!/usr/bin/env python3
import argparse
import socket
import struct
import sys

BRIDGE_MAGIC = 0x4D504247
BRIDGE_VERSION = 3
MAX_ENTRIES = 64
MAX_WORKERS = 4
APP_MSG_REQUEST = 1
APP_MSG_RESPONSE = 2
APP_MSG_STOP = 3
APP_MSG_ERROR = 4
WIRE_FMT = "!IHHIHHHH" + ("Q" * MAX_ENTRIES) * (MAX_WORKERS + 1)
STATUS = {
    0: "OK",
    1: "BAD_MAGIC",
    2: "BAD_VERSION",
    3: "BAD_TYPE",
    4: "BAD_ENTRY_COUNT",
    5: "RECV_FAILED",
    6: "FPGA_TIMEOUT",
    7: "FPGA_SEND_FAILED",
    8: "BAD_WORKER_LAYOUT",
    9: "BAD_INDEX_RANGE",
    10: "DUPLICATE_INDEX",
    11: "FPGA_BAD_RESULT",
    12: "BAD_WORKER_COUNT",
    13: "RESULT_MISMATCH",
}


def parse_value(text):
    try:
        value = int(text, 0)
    except ValueError as exc:
        raise ValueError(f"无效 value {text!r}，格式应为整数") from exc
    if not 0 <= value <= 0xffffffffffffffff:
        raise ValueError(f"value={value} 超出 uint64 范围")
    return value


def normalize(values):
    if len(values) > MAX_ENTRIES:
        raise ValueError(f"每个 worker 最多 {MAX_ENTRIES} 个 value")
    return list(values)


def pad(values):
    return list(values) + [0] * (MAX_ENTRIES - len(values))


def build_message(request_id, workers, msg_type=APP_MSG_REQUEST):
    worker_count = len(workers)
    if not 2 <= worker_count <= MAX_WORKERS:
        raise ValueError("必须提供 2、3 或 4 路 worker")
    normalized = [normalize(worker) for worker in workers]
    entry_count = len(normalized[0])
    if any(len(worker) != entry_count for worker in normalized):
        raise ValueError("所有 worker 必须提供相同数量的 value")
    wire_workers = [pad(worker) for worker in normalized]
    while len(wire_workers) < MAX_WORKERS:
        wire_workers.append([0] * MAX_ENTRIES)
    fields = []
    for worker in wire_workers:
        fields.extend(worker)
    fields.extend([0] * MAX_ENTRIES)
    return struct.pack(
        WIRE_FMT,
        BRIDGE_MAGIC,
        BRIDGE_VERSION,
        msg_type,
        request_id,
        entry_count,
        worker_count,
        0,
        0,
        *fields,
    )


def decode_message(raw):
    if len(raw) != struct.calcsize(WIRE_FMT):
        expected_size = struct.calcsize(WIRE_FMT)
        raise ValueError(f"响应长度 {len(raw)}，期望 {expected_size}")
    values = struct.unpack(WIRE_FMT, raw)
    (
        magic,
        version,
        msg_type,
        request_id,
        entry_count,
        worker_count,
        status,
        reserved,
    ) = values[:8]
    cursor = 8
    workers = []
    for _ in range(MAX_WORKERS):
        workers.append(
            [
                values[cursor + i] for i in range(MAX_ENTRIES)
            ]
        )
        cursor += MAX_ENTRIES
    result = [
        values[cursor + i] for i in range(MAX_ENTRIES)
    ]
    return {
        "magic": magic,
        "version": version,
        "msg_type": msg_type,
        "request_id": request_id,
        "entry_count": entry_count,
        "worker_count": worker_count,
        "status": status,
        "batch_size": reserved,
        "workers": workers,
        "result_entries": result,
    }


def show(values, count):
    return "[" + ", ".join(str(v) for v in values[:count]) + "]"


def main():
    parser = argparse.ArgumentParser(
        description="2/3/4 路 MPI-FPGA 64 项聚合客户端"
    )
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=10000)
    parser.add_argument(
        "--request-id", type=lambda value: int(value, 0), default=1
    )

    for worker in range(MAX_WORKERS):
        parser.add_argument(f"--worker{worker}-values", nargs="+", metavar="VALUE")
    parser.add_argument("--timeout", type=float, default=8.0)
    parser.add_argument("--stop", action="store_true")
    args = parser.parse_args()
    supplied = [
        getattr(args, f"worker{worker}_values")
        for worker in range(MAX_WORKERS)
    ]

    if args.stop:
        supplied = [["0"], ["0"]]
    else:
        missing_required_worker = not supplied[0] or not supplied[1]
        non_contiguous_workers = any(
            supplied[worker]
            for worker in range(2, MAX_WORKERS)
            if not all(supplied[:worker])
        )
        if missing_required_worker or non_contiguous_workers:
            parser.error(
                "worker 必须从 worker0 起连续提供，至少 worker0 和 worker1"
            )

    try:
        workers = [
            [parse_value(item) for item in entries]
            for entries in supplied
            if entries
        ]
        message_type = APP_MSG_STOP if args.stop else APP_MSG_REQUEST
        request = build_message(args.request_id, workers, message_type)

    except ValueError as exc:
        print(exc, file=sys.stderr)
        return 2
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(args.timeout)
    try:
        sock.sendto(request, (args.host, args.port))
        raw, peer = sock.recvfrom(struct.calcsize(WIRE_FMT) + 64)
    except socket.timeout:
        print("等待桥接程序响应超时", file=sys.stderr)
        return 1
    finally:
        sock.close()
    try:
        message = decode_message(raw)
    except ValueError as exc:
        print(exc, file=sys.stderr)
        return 1
    print(f"peer        : {peer[0]}:{peer[1]}")
    print(f"request_id  : {message['request_id']}")
    print(f"worker_count: {message['worker_count']}")
    print(f"entry_count : {message['entry_count']}")
    print(f"batch_size  : {message['batch_size']}")
    print(
        f"status      : {message['status']} "
        f"({STATUS.get(message['status'], 'UNKNOWN')})"
    )
    
    for worker, entries in enumerate(workers):
        print(f"worker{worker:<5}: {show(entries, message['entry_count'])}")
    print(f"result      : {show(message['result_entries'], message['entry_count'])}")
    response_is_valid = (
        message["magic"] == BRIDGE_MAGIC
        and message["version"] == BRIDGE_VERSION
        and message["status"] == 0
    )
    return 0 if response_is_valid else 1


if __name__ == "__main__":
    raise SystemExit(main())
