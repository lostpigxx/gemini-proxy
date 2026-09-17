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

With --expect-close the run also doubles as the graceful-shutdown check. RESP
has no way for a server to say "this is my last reply", so a drain is judged by
where the close lands relative to a phase:

  drained  clean EOF seen at a phase boundary, before the next batch went out.
           This is the good case and counts as a success.
  raced    clean EOF right after a batch was sent, with *none* of it answered.
           The proxy had already closed at a boundary; our bytes crossed its
           FIN on the wire and it never read them. Unavoidable without a
           protocol-level goodbye, and a retry on a new connection is correct.
  error    EOF partway through a batch's replies (the proxy answered part of
           what it had accepted and dropped the rest), or a reset anywhere — a
           reset also destroys replies we had not read yet.

Boundaries are polled rather than inferred from a failed write, because writing
into a socket the peer already closed turns its FIN into a reset and would hide
which of the two happened.

  ./scripts/cluster-loadcheck.py [--port 6380] [--clients 50]
                                 [--pipeline 16] [--seconds 30]
                                 [--expect-close]

Exit status is 0 only when errors and mismatches are both zero — and, under
--expect-close, only if at least one connection actually saw the close, so a
TERM that never arrived cannot pass by default.
"""

import argparse
import select
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

    def closed_by_peer(self):
        """True at a phase boundary if the peer sent FIN and nothing else."""
        if self.buf:
            return False  # a reply we have not consumed: not a boundary
        r, _, _ = select.select([self.sock], [], [], 0)
        if not r:
            return False
        chunk = self.sock.recv(65536)
        if chunk:
            self.buf += chunk  # unsolicited data; let the caller trip over it
            return False
        return True

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
        self.closed = 0
        self.raced = 0
        self.samples = []

    def add(self, ops, errors, mismatches, closed, raced, samples):
        with self.lock:
            self.ops += ops
            self.errors += errors
            self.mismatches += mismatches
            self.closed += closed
            self.raced += raced
            for s in samples:
                if len(self.samples) < 10:
                    self.samples.append(s)


def worker(wid, args, stats, deadline, barrier):
    ops = errors = mismatches = closed = raced = 0
    samples = []
    try:
        try:
            c = Conn(args.host, args.port)
        except Exception:
            # Nobody else can start, so say so: without this a refused connect
            # leaves every other thread — and main — waiting out the barrier.
            barrier.abort()
            raise
        barrier.wait()
        seq = 0

        def phase(label, keys, want):
            """Reads one reply per key. Returns True if the phase completed,
            False if the connection closed cleanly before answering any of it
            — that one is the send that raced the drain, not a lost request."""
            nonlocal ops, errors, mismatches, raced
            got = 0
            try:
                for k, v in zip(keys, want):
                    kind, payload = c.reply()
                    got += 1
                    ops += 1
                    if kind == "err":
                        errors += 1
                        samples.append(
                            "%s %s -> %s" % (label, k.decode(), payload.decode())
                        )
                    elif v is not None and payload != v:
                        mismatches += 1
                        samples.append(
                            "%s %s -> %r, want %r" % (label, k.decode(), payload, v)
                        )
            except ConnectionError as e:
                # A partial phase means the proxy answered some of a batch it
                # had accepted and dropped the rest — a real lost request. So
                # does a reset, which can destroy replies we never read.
                if not args.expect_close or got != 0 or isinstance(
                    e, ConnectionResetError
                ):
                    raise
                raced += 1
                return False
            return True

        while time.monotonic() < deadline:
            if args.expect_close and c.closed_by_peer():
                closed = 1  # drained between requests: exactly what we want
                break
            keys, values = [], []
            for _ in range(args.pipeline):
                seq += 1
                keys.append(b"lc:%d:%d" % (wid, seq))
                values.append(b"v%d" % seq)

            c.send([[b"SET", k, v] for k, v in zip(keys, values)])
            if not phase("SET", keys, [None] * len(keys)):
                break

            if args.expect_close and c.closed_by_peer():
                closed = 1  # the other boundary: phase 1 answered in full
                break

            c.send([[b"GET", k] for k in keys])
            if not phase("GET", keys, values):
                break
    except Exception as e:  # a dropped connection is itself a failure
        errors += 1
        samples.append("worker %d: %s: %s" % (wid, type(e).__name__, e))
    stats.add(ops, errors, mismatches, closed, raced, samples)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=6380)
    p.add_argument("--clients", type=int, default=50)
    p.add_argument("--pipeline", type=int, default=16)
    p.add_argument("--seconds", type=float, default=30.0)
    p.add_argument(
        "--expect-close",
        action="store_true",
        help="treat a clean EOF at a phase boundary as a drained connection "
        "rather than an error (for the SIGTERM acceptance run)",
    )
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
    try:
        barrier.wait()  # start measuring only once every connection is up
    except threading.BrokenBarrierError:
        for t in threads:
            t.join()
        print("connect failed: %s" % (stats.samples[0] if stats.samples else "?"))
        return 1
    started = time.monotonic()
    for t in threads:
        t.join()
    elapsed = time.monotonic() - started

    line = "ops=%d  elapsed=%.1fs  rps=%.0f  errors=%d  mismatches=%d" % (
        stats.ops,
        elapsed,
        stats.ops / elapsed,
        stats.errors,
        stats.mismatches,
    )
    if args.expect_close:
        line += "  drained=%d/%d  raced=%d" % (stats.closed, args.clients, stats.raced)
    print(line)
    for s in stats.samples:
        print("  !", s)
    if stats.errors or stats.mismatches:
        return 1
    if args.expect_close and stats.closed + stats.raced == 0:
        print("  ! --expect-close given but no connection was ever closed")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
