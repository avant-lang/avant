#!/usr/bin/env python3
"""How a server's listening sockets spread new connections over its event
loops, read from outside: no server change, no tracing.

Usage: spread.py PID PORT CONNECTIONS [TRIALS]

For a server with one epoll set per loop thread (Avant's std.http and the
harness's h2o server: one SO_REUSEPORT listener per loop, the kernel picks
the listener by a hash of the connection's addresses and ports). Each trial
reads the members of every epoll set of the process from
/proc/PID/fdinfo/<epoll fd> (the `tfd:` lines), opens CONNECTIONS idle
connections to 127.0.0.1:PORT, reads the sets again and prints the
difference per set, largest first. The connections are then closed; the
next trial gets new source ports, so a new draw of the hash.
"""
import os
import socket
import sys
import time


def epoll_sets(pid):
    sets = {}
    fd_dir = "/proc/%d/fd" % pid
    for name in os.listdir(fd_dir):
        try:
            if os.readlink(os.path.join(fd_dir, name)) != "anon_inode:[eventpoll]":
                continue
            with open("/proc/%d/fdinfo/%s" % (pid, name)) as f:
                sets[int(name)] = sum(1 for line in f if line.startswith("tfd:"))
        except OSError:
            pass
    return sets


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    pid, port, count = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
    trials = int(sys.argv[4]) if len(sys.argv) > 4 else 5
    for trial in range(1, trials + 1):
        before = epoll_sets(pid)
        conns = [socket.create_connection(("127.0.0.1", port), timeout=5) for _ in range(count)]
        time.sleep(0.5)
        after = epoll_sets(pid)
        per = sorted((after.get(fd, 0) - before.get(fd, 0) for fd in after), reverse=True)
        loops = len(per)
        ideal = count / loops if loops else 0
        print("connections=%d trial=%d loops=%d sum=%d max=%d min=%d empty=%d ideal=%.1f max/ideal=%.2f per_loop=%s" % (
            count, trial, loops, sum(per), per[0] if per else 0, per[-1] if per else 0,
            sum(1 for n in per if n == 0), ideal, (per[0] / ideal) if ideal else 0, ",".join(str(n) for n in per)))
        for c in conns:
            c.close()
        time.sleep(0.5)


if __name__ == "__main__":
    main()
