#!/usr/bin/env python3
"""Concurrent load client for the green-thread echo server.

Opens N connections, holds them all open simultaneously, sends R rounds of a
message on each and checks every echo comes back byte-identical. Holding the
connections open at once is the point: it proves the server is multiplexing
many live sockets on one OS thread, not serving them one after another.

Usage: python3 bench/echo_client.py [connections] [rounds] [port]
"""

import socket
import sys
import time

CONNS = int(sys.argv[1]) if len(sys.argv) > 1 else 50
ROUNDS = int(sys.argv[2]) if len(sys.argv) > 2 else 20
PORT = int(sys.argv[3]) if len(sys.argv) > 3 else 9000


def main():
    socks = []
    for i in range(CONNS):
        s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        socks.append(s)
    print(f"{len(socks)} connections open simultaneously")

    sent = 0
    start = time.perf_counter()
    for r in range(ROUNDS):
        for i, s in enumerate(socks):
            msg = f"conn{i:04d}-round{r:04d}".encode()
            s.sendall(msg)
            got = s.recv(len(msg))
            if got != msg:
                print(f"MISMATCH on conn {i}: sent {msg!r}, got {got!r}")
                return 1
            sent += 1
    elapsed = time.perf_counter() - start

    for s in socks:
        s.close()

    print(f"OK: {sent} echo round-trips, all byte-identical")
    print(f"    {elapsed:.3f}s total -> {sent / elapsed:,.0f} round-trips/sec")
    return 0


if __name__ == "__main__":
    sys.exit(main())
