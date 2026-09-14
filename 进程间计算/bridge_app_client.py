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


def build_worker_messages(request_id, workers):
    """One payload per contributing worker; inactive ports receive metadata only."""
    combined = build_message(request_id, workers)
    rounds, count = len(workers[0]), len(workers)
    size = rounds * 512
    return [HEADER.pack(BRIDGE_MAGIC, 5, 1, request_id, rounds, count, 64, 0, rank)
            + (combined[24+rank*size:24+(rank+1)*size] if rank < count else b"")
            for rank in range(4)]


class FourPortClient:
    """Bounded outstanding request table; first valid OK response consumes an ID."""
    def __init__(self, host="127.0.0.1", port=10000, window=16, timeout=2):
        import selectors
        if not 1 <= window <= 16 or not 1 <= port <= 65532 or timeout <= 0:
            raise ValueError("window must be 1..16; valid base port and positive timeout required")
        self.pending = {}
        self.window, self.timeout = window, timeout
        self.selector = selectors.DefaultSelector()
        self.sockets = []
        for rank in range(4):
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.connect((host, port+rank)); sock.setblocking(False)
            self.sockets.append(sock); self.selector.register(sock, selectors.EVENT_READ, rank)

    def close(self):
        self.pending.clear()
        self.selector.close()
        for sock in self.sockets: sock.close()

    def submit(self, request_id, workers):
        import time
        if self.expire(): raise TimeoutError("expired requests removed; stop before reusing FPGA credits")
        if request_id in self.pending: raise ValueError("request_id already pending")
        if len(self.pending) >= self.window: raise BufferError("request window full")
        if any((rid >> 4) % 16 == (request_id >> 4) % 16 for rid in self.pending):
            raise BufferError("FPGA batch slot already in flight")
        messages = build_worker_messages(request_id, workers)
        # Register before sending, so fast responses always have an owner.
        self.pending[request_id] = (time.monotonic(), len(workers[0]), len(workers))
        try:
            for sock, message in zip(self.sockets, messages): sock.send(message)
        except OSError:
            self.pending.pop(request_id, None)
            raise

    def expire(self):
        import time
        expired = [rid for rid, meta in self.pending.items() if time.monotonic()-meta[0] >= self.timeout]
        for rid in expired: del self.pending[rid]
        return expired

    def receive(self, timeout=None):
        import time
        deadline = time.monotonic() + (self.timeout if timeout is None else timeout)
        while self.pending:
            if self.expire(): raise TimeoutError("request expired; outstanding entry removed")
            left = deadline-time.monotonic()
            if left <= 0: return None
            for key, _ in self.selector.select(min(left, .05)):
                try: raw = key.fileobj.recv(65536)
                except BlockingIOError: continue
                if len(raw) < 24: continue
                magic, ver, kind, rid, rounds, workers, entries, status, rank = HEADER.unpack_from(raw)
                meta = self.pending.get(rid)
                if (not meta or (magic, ver, kind, entries, rank) != (BRIDGE_MAGIC, 5, 2, 64, key.data)
                        or status or rounds != meta[1] or workers != meta[2] or len(raw) != 24+rounds*512):
                    continue
                # No payload comparison with other copies; late copies are ignored.
                del self.pending[rid]
                values = struct.unpack_from(f"!{rounds*64}Q", raw, 24)
                return dict(request_id=rid, round_count=rounds, worker_count=workers, status=0, rank=rank,
                            results=[list(values[r*64:(r+1)*64]) for r in range(rounds)])
        return None

    def stop(self):
        for rank, sock in enumerate(self.sockets):
            sock.send(HEADER.pack(BRIDGE_MAGIC, 5, 3, 0, 0, 0, 64, 0, rank))


def main():
    parser = argparse.ArgumentParser(description="Four-port application client; first OK result wins")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=10000, help="rank ports: base..base+3")
    parser.add_argument("--request-id", type=lambda s:int(s,0), default=0x1000)
    parser.add_argument("--rounds", type=int, choices=range(1,17), default=1)
    parser.add_argument("--timeout", type=float, default=2)
    parser.add_argument("--stop", action="store_true")
    for w in range(4): parser.add_argument(f"--worker{w}-values", nargs="+", type=lambda s:int(s,0))
    args=parser.parse_args()
    supplied=[getattr(args,f"worker{w}_values") for w in range(4)]
    count=sum(v is not None for v in supplied)
    if not args.stop and (count<2 or any(v is None for v in supplied[:count])):
        parser.error("provide 2..4 contiguous workers")
    client=FourPortClient(args.host,args.port,timeout=args.timeout)
    try:
        if args.stop: client.stop();return 0
        client.submit(args.request_id,[[v]*args.rounds for v in supplied[:count]])
        reply=client.receive()
        if reply is None: raise TimeoutError("no usable response")
        print(f"request_id={reply['request_id']} rank={reply['rank']} status=OK pending={len(client.pending)}")
        for r,values in enumerate(reply['results']): print(f"round_id={args.request_id+r}: {values[:len(supplied[0])]}")
        return 0
    except (ValueError,OSError,TimeoutError,BufferError) as exc:
        print(exc);return 1
    finally: client.close()


if __name__ == "__main__":
    raise SystemExit(main())
