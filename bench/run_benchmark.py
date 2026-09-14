#!/usr/bin/env python3
"""Throughput/latency/memory sweep for a real sensorlink-server.

For each N in N_VALUES: start a real server, start one
sensorlink-bench-subscriber measuring the feed, then launch N separate
sensorlink-bench-generator processes -- each one a full TCP connection doing
a real HELLO handshake, exactly like a real device -- all streaming SAMPLE
frames on the same metric for DURATION_S seconds. Record:
  - frames/sec actually delivered to the subscriber
  - how many of the frames the generators sent never showed up (drops)
  - p50/p99 latency from a generator's send() to the subscriber parsing it
  - the server process's peak resident memory (VmRSS) during the run

Prerequisite (this script does not build anything itself):
    cmake --preset host && cmake --build build/host

Run from anywhere:
    python3 bench/run_benchmark.py
"""

import re
import resource
import socket
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]

SERVER_BIN = REPO_ROOT / "build/host/server/sensorlink-server"
GENERATOR_BIN = REPO_ROOT / "build/host/bench/sensorlink-bench-generator"
SUBSCRIBER_BIN = REPO_ROOT / "build/host/bench/sensorlink-bench-subscriber"

# How many simulated devices to sweep across: 10 is a near-idle baseline,
# 50 is a plausible real deployment (one supermarket's worth of units),
# 200 is where the throughput/latency/memory curves start actually moving,
# and 1000 is a deliberate stress point -- picked to stay under the ~1024
# file-descriptor ceiling most Linux distributions ship as the default soft
# RLIMIT_NOFILE, even after this script raises it (see raise_fd_limit()
# below).
N_VALUES = [10, 50, 200, 1000]

# How long each generator streams for. Long enough that per-connection HELLO
# handshakes and process startup jitter are a small fraction of the window,
# short enough that sweeping all of N_VALUES does not take all day.
DURATION_S = 5

# The subscriber is given more time than DURATION_S, not because the
# measurement window is longer, but because spawning up to 1000 separate
# processes is itself not instant -- the last generator can start a couple
# of seconds after the first. This script measures elapsed wall time itself
# (see run_one()'s use of time.monotonic()), so the subscriber's own
# --seconds only has to be generous enough to still be listening when the
# slowest-to-start generator finishes, not exact.
SUBSCRIBER_MARGIN_S = 8

METRIC_NAME = "pressure"
METRIC_ID = 1

# Paced to the server's own kMaxSampleRateHz (server/device_session.cpp) --
# the fastest any real device is ever allowed to sample after its HELLO
# handshake. Sending faster than this does not model a real device; it
# models a device ignoring the server's own limits, which is a different
# (and separately documented) failure mode -- see bench/load_generator.cpp's
# --rate-hz 0.
RATE_HZ = 10

INGEST_HOST = "127.0.0.1"
SUBSCRIBE_HOST = "127.0.0.1"


def raise_fd_limit():
    """Both this script and the server it launches need one file descriptor
    per simulated device at N=1000, plus the usual handful for listening
    sockets, pipes, and stdio -- comfortably past the 1024 many Linux
    distributions set as the default soft RLIMIT_NOFILE. Raising the soft
    limit here (before any subprocess is started) is inherited by every
    child this script forks, so it covers the server too without needing a
    separate preexec_fn. Best-effort: if the hard limit will not allow it,
    print a warning and let the N=1000 row fail on its own rather than
    aborting the whole sweep."""
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    desired = min(hard, 4096)
    if desired <= soft:
        return
    try:
        resource.setrlimit(resource.RLIMIT_NOFILE, (desired, hard))
    except (ValueError, OSError) as exc:
        print(f"warning: could not raise RLIMIT_NOFILE ({soft} -> {desired}): {exc}")


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait_for_port(host, port, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with socket.create_connection((host, port), timeout=0.5):
                return True
        except OSError:
            time.sleep(0.1)
    return False


def read_vmrss_kb(pid):
    try:
        with open(f"/proc/{pid}/status") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
    except (FileNotFoundError, ProcessLookupError):
        pass
    return None


def check_binaries_exist():
    missing = [p for p in (SERVER_BIN, GENERATOR_BIN, SUBSCRIBER_BIN) if not p.exists()]
    if missing:
        lines = "\n".join(f"  - {p}" for p in missing)
        print(f"missing build outputs, build the host preset first:\n{lines}")
        sys.exit(1)


def parse_kv_lines(text):
    """bench/latency_subscriber.cpp and bench/load_generator.cpp both print
    "KEY value" lines on their last bit of stdout -- this turns that into a
    dict, tolerating any warm-up noise a process printed before it."""
    result = {}
    for line in text.splitlines():
        match = re.match(r"^(\w+) (-?\d+)$", line.strip())
        if match:
            result[match.group(1)] = int(match.group(2))
    return result


def run_one(n):
    ingest_port = free_port()
    subscribe_port = free_port()

    server = subprocess.Popen(
        [str(SERVER_BIN), "--ingest", str(ingest_port), "--subscribe", str(subscribe_port)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    subscriber = None
    generators = []
    try:
        if not wait_for_port(INGEST_HOST, ingest_port, timeout=5):
            raise RuntimeError("server never opened its ingest port")
        if not wait_for_port(SUBSCRIBE_HOST, subscribe_port, timeout=5):
            raise RuntimeError("server never opened its subscribe port")

        subscriber = subprocess.Popen(
            [
                str(SUBSCRIBER_BIN),
                "--connect", f"{SUBSCRIBE_HOST}:{subscribe_port}",
                "--metric", METRIC_NAME,
                "--seconds", str(DURATION_S + SUBSCRIBER_MARGIN_S),
            ],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
        )

        for i in range(n):
            generators.append(subprocess.Popen(
                [
                    str(GENERATOR_BIN),
                    "--connect", f"{INGEST_HOST}:{ingest_port}",
                    "--device-id", str(2000 + i),
                    "--metric", str(METRIC_ID),
                    "--seconds", str(DURATION_S),
                    "--rate-hz", str(RATE_HZ),
                ],
                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True,
            ))

        peak_rss_kb = 0
        while any(g.poll() is None for g in generators):
            rss = read_vmrss_kb(server.pid)
            if rss is not None:
                peak_rss_kb = max(peak_rss_kb, rss)
            time.sleep(0.2)
        total_sent = 0
        for g in generators:
            out, _ = g.communicate(timeout=5)
            total_sent += parse_kv_lines(out).get("SENT", 0)

        try:
            sub_out, _ = subscriber.communicate(timeout=DURATION_S + SUBSCRIBER_MARGIN_S + 5)
        except subprocess.TimeoutExpired:
            subscriber.kill()
            sub_out, _ = subscriber.communicate()
        sub_result = parse_kv_lines(sub_out)
        count = sub_result.get("COUNT", 0)
        if count == 0:
            raise RuntimeError("subscriber received nothing -- check the server/subscriber logs")

        return {
            "n": n,
            # Deliberately count / DURATION_S, not count / measured wall
            # time: measured wall time includes this script's own process-
            # spawn overhead (a couple of seconds at N=1000), which would
            # dilute the rate with a Python startup cost that has nothing to
            # do with the server. Each generator paces itself to DURATION_S
            # from its own connection time (bench/load_generator.cpp), so
            # the aggregate sustained rate the server actually handled is
            # total delivered / DURATION_S.
            "frames_per_sec": count / DURATION_S,
            "drops": max(total_sent - count, 0),
            "p50_ms": sub_result.get("P50_MS"),
            "p99_ms": sub_result.get("P99_MS"),
            "server_rss_kb": peak_rss_kb,
        }
    finally:
        for g in generators:
            if g.poll() is None:
                g.kill()
        if subscriber is not None and subscriber.poll() is None:
            subscriber.kill()
        server.terminate()
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()


def print_markdown_table(rows):
    header = "| N | frames/sec | drops | p50 (ms) | p99 (ms) | server RSS (MB) |"
    separator = "|---|---|---|---|---|---|"
    lines = [header, separator]
    for row in rows:
        if "error" in row:
            lines.append(f"| {row['n']} | error: {row['error']} | | | | |")
            continue
        lines.append(
            f"| {row['n']} | {row['frames_per_sec']:.0f} | {row['drops']} | "
            f"{row['p50_ms']} | {row['p99_ms']} | {row['server_rss_kb'] / 1024:.1f} |"
        )
    table = "\n".join(lines)
    print(table)
    (REPO_ROOT / "bench/results.md").write_text(table + "\n")


def main():
    check_binaries_exist()
    raise_fd_limit()

    rows = []
    for n in N_VALUES:
        print(f"running N={n} for {DURATION_S}s ...")
        try:
            rows.append(run_one(n))
        except RuntimeError as exc:
            print(f"N={n} failed: {exc}")
            rows.append({"n": n, "error": str(exc)})

    print()
    print_markdown_table(rows)


if __name__ == "__main__":
    main()
