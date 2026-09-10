#!/usr/bin/env python3
"""End-to-end v4 numeric checks against a running bridge (hardware or explicit simulation)."""
import argparse
import socket
from bridge_app_client import build_message, decode_message


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=10000)
    args = parser.parse_args()
    base = 0x8000
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.settimeout(3)
        sock.connect((args.host, args.port))
        for count in (2, 3, 4):
            for rounds in (1, 3, 16):
                workers = [[[((1 << 64)-17 if i==63 else (w+1)*1000+r*64+i) for i in range(64)]
                            for r in range(rounds)] for w in range(count)]
                sock.send(build_message(base, workers))
                reply = decode_message(sock.recv(65536))
                assert (reply['request_id'], reply['status'], reply['worker_count'], reply['round_count']) == (base, 0, count, rounds), reply
                assert reply['results'] == [[sum(w[r][i] for w in workers) % (1 << 64) for i in range(64)] for r in range(rounds)]
                print(f"PASS: workers={count} rounds={rounds} entries=64 including overflow")
                base += 16


if __name__ == "__main__":
    main()
