#!/usr/bin/env python3
"""Pipelined load generator that validates every reply.

The M4 acceptance criterion is "resharding under load, zero client errors", and
redis-benchmark cannot measure it: it has no error-reporting flag at all (7.0.15
`--help` lists none) and discards error replies, so a run where every request
came back `-MOVED` still prints a happy rps number. Hence this.

Each worker owns one connection and alternates two phases:

  1. pipeline N SETs with distinct keys, read N replies, require +OK
  2. pipeline N GETs for those same keys, read N replies, require the value back

Phase 2 is only sent after every phase-1 reply has arrived, so the check never
depends on execution order between two in-flight commands. That matters during
resharding: the proxy guarantees *reply* order per client, not execution order
across nodes, so a GET pipelined behind a SET of the same key could legitimately
overtake it while the SET is bouncing through a redirect.

Any `-ERR`/`-MOVED`/`-ASK`/`-CROSSSLOT` reaching the client is a failure — the
whole point of the proxy is that redirects stay invisible.

  ./scripts/cluster-loadcheck.py [--port 6380] [--clients 50]
                                 [--pipeline 16] [--seconds 30]

Exit status is 0 only when errors and mismatches are both zero.
"""

import argparse
import socket
import sys
import threading
import time


class Conn:
    """Minimal RESP client: pipelined writes, buffered reply reads."""

    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port))
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = b""

    def send(self, parts):
        out = []
        for args in parts:
            out.append(b"*%d\r\n" % len(args))
            for a in args:
                out.append(b"$%d\r\n%s\r\n" % (len(a), a))
        self.sock.sendall(b"".join(out))

    def _line(self):
        while True:
            nl = self.buf.find(b"\r\n")
            if nl >= 0:
                line, self.buf = self.buf[:nl], self.buf[nl + 2 :]
                return line
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("server closed the connection")
            self.buf += chunk

    def _exact(self, n):
        while len(self.buf) < n + 2:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("server closed the connection")
            self.buf += chunk
        body, self.buf = self.buf[:n], self.buf[n + 2 :]
        return body

    def reply(self):
        """Returns (kind, payload); kind is one of 'ok', 'err', 'nil', 'bulk'."""
        line = self._line()
        tag, rest = line[:1], line[1:]
        if tag == b"+":
            return "ok", rest
        if tag == b"-":
            return "err", rest
        if tag == b":":
            return "ok", rest
        if tag == b"$":
            n = int(rest)
            if n < 0:
                return "nil", b""
            return "bulk", self._exact(n)
        raise ValueError("unexpected reply type %r" % line[:32])


class Stats:
    def __init__(self):
        self.lock = threading.Lock()
        self.ops = 0
        self.errors = 0
        self.mismatches = 0
        self.samples = []

    def add(self, ops, errors, mismatches, samples):
        with self.lock:
            self.ops += ops
            self.errors += errors
            self.mismatches += mismatches
            for s in samples:
                if len(self.samples) < 10:
                    self.samples.append(s)


def worker(wid, args, stats, deadline, barrier):
    ops = errors = mismatches = 0
    samples = []
    try:
        c = Conn(args.host, args.port)
        barrier.wait()
        seq = 0
        while time.monotonic() < deadline:
            keys, values = [], []
            for _ in range(args.pipeline):
                seq += 1
                keys.append(b"lc:%d:%d" % (wid, seq))
                values.append(b"v%d" % seq)

            c.send([[b"SET", k, v] for k, v in zip(keys, values)])
            for k in keys:
                kind, payload = c.reply()
                ops += 1
                if kind == "err":
                    errors += 1
                    samples.append("SET %s -> %s" % (k.decode(), payload.decode()))

            c.send([[b"GET", k] for k in keys])
            for k, v in zip(keys, values):
                kind, payload = c.reply()
                ops += 1
                if kind == "err":
                    errors += 1
                    samples.append("GET %s -> %s" % (k.decode(), payload.decode()))
                elif payload != v:
                    mismatches += 1
                    samples.append(
                        "GET %s -> %r, want %r" % (k.decode(), payload, v)
                    )
    except Exception as e:  # a dropped connection is itself a failure
        errors += 1
        samples.append("worker %d: %s: %s" % (wid, type(e).__name__, e))
    stats.add(ops, errors, mismatches, samples)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=6380)
    p.add_argument("--clients", type=int, default=50)
    p.add_argument("--pipeline", type=int, default=16)
    p.add_argument("--seconds", type=float, default=30.0)
    args = p.parse_args()

    stats = Stats()
    barrier = threading.Barrier(args.clients + 1)
    deadline = time.monotonic() + args.seconds
    threads = [
        threading.Thread(target=worker, args=(i, args, stats, deadline, barrier))
        for i in range(args.clients)
    ]
    for t in threads:
        t.start()
    barrier.wait()  # start measuring only once every connection is up
    started = time.monotonic()
    for t in threads:
        t.join()
    elapsed = time.monotonic() - started

    print(
        "ops=%d  elapsed=%.1fs  rps=%.0f  errors=%d  mismatches=%d"
        % (stats.ops, elapsed, stats.ops / elapsed, stats.errors, stats.mismatches)
    )
    for s in stats.samples:
        print("  !", s)
    return 0 if stats.errors == 0 and stats.mismatches == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
