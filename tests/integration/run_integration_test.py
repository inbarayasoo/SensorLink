#!/usr/bin/env python3
"""End-to-end integration test: a real sensorlink-server, a real firmware
image running under QEMU, and the real sensorlink-client CLI, all talking to
each other over actual TCP sockets -- nothing simulated, nothing mocked.

What this proves, in order:
  1. The handshake works: the firmware's HELLO reaches the server, the
     server's CONFIG reaches the firmware, and SAMPLE frames start flowing.
  2. sensorlink-client, run as a subprocess exactly the way an operator
     would run it, actually receives and prints them.
  3. Backpressure closes the loop end to end: a subscriber that never drains
     its buffer causes the server to send SLOW_DOWN, and the firmware's
     real sample rate actually drops -- not just the unit-tested pieces in
     isolation (device_session.slow_down, ingest.send_slow_down), but the
     whole chain, including the 1 Hz housekeeping sweep in server/main.cpp
     and node/tasks/command_task.cpp actually decoding the frame.

Prerequisites (this script does not build anything itself):
    cmake --preset host && cmake --build build/host
    cmake --preset firmware && cmake --build build/firmware

Run from anywhere:
    python3 tests/integration/run_integration_test.py
"""

import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

SERVER_BIN = REPO_ROOT / "build/host/server/sensorlink-server"
CLIENT_BIN = REPO_ROOT / "build/host/client/sensorlink-client"
LOAD_GENERATOR_BIN = REPO_ROOT / "build/host/tests/integration/sensorlink-load-generator"
FIRMWARE_ELF = REPO_ROOT / "build/firmware/node/sensorlink-node.elf"

# How many samples the real firmware sends per second before anything has
# asked it to slow down (see node/shared_state.hpp's DeviceConfig default).
BASELINE_RATE_HZ = 2

# How long to watch the feed in each half of the comparison. 6s at 2 Hz is
# ~12 samples -- enough that a little scheduling jitter around QEMU boot or
# housekeeping's 1s tick does not make a real rate change hard to see.
MEASUREMENT_WINDOW_S = 6

# How long the load generator floods the "pressure" metric for. Reaching
# the server's 64 KiB per-connection high-water mark (server/connection.cpp)
# over a loopback socket, with nothing draining it, takes well under a
# second in practice -- this is generous margin, not a tuned minimum.
LOAD_DURATION_S = 4

# After the flood, how long to wait before measuring again: one full
# housekeeping tick (server/main.cpp runs it every 1s) has to fire and
# notice the congestion, SLOW_DOWN has to reach the firmware over the
# emulated UART, and command_task has to decode it and update the shared
# config sensor_task reads.
SETTLE_TIME_S = 3

INGEST_HOST = "127.0.0.1"
SUBSCRIBE_HOST = "127.0.0.1"


class IntegrationTestFailure(Exception):
    pass


def free_port():
    """Asks the OS for an unused TCP port by binding to port 0 and reading
    back what it picked, then releasing it immediately. There is a small
    window where something else could grab the same port before the server
    binds it -- an accepted, standard trade-off for test tooling, and far
    less error-prone than hard-coding a port number and hoping nothing else
    on the machine (or a previous, still-lingering run) is using it."""
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


class LineReader:
    """Reads a subprocess's stdout line by line on a background thread, and
    timestamps every line with time.monotonic() as it arrives -- the same
    problem client/line_protocol.hpp solves for a raw socket, one level up:
    here it is sensorlink-client's own stdout we cannot afford to block on
    while the rest of the test keeps running."""

    def __init__(self, stream):
        self._records = []
        self._lock = threading.Lock()
        self._thread = threading.Thread(target=self._run, args=(stream,), daemon=True)
        self._thread.start()

    def _run(self, stream):
        for raw_line in stream:
            with self._lock:
                self._records.append((time.monotonic(), raw_line.rstrip("\n")))

    def count_since(self, prefix, since):
        with self._lock:
            return sum(1 for ts, line in self._records if ts >= since and line.startswith(prefix))

    def tail(self, n=20):
        with self._lock:
            return [line for _, line in self._records[-n:]]


def check_binaries_exist():
    missing = [p for p in (SERVER_BIN, CLIENT_BIN, LOAD_GENERATOR_BIN, FIRMWARE_ELF) if not p.exists()]
    if missing:
        lines = "\n".join(f"  - {p}" for p in missing)
        raise IntegrationTestFailure(
            "missing build outputs, build both presets first:\n"
            f"{lines}\n"
            "  cmake --preset host && cmake --build build/host\n"
            "  cmake --preset firmware && cmake --build build/firmware"
        )


def main():
    check_binaries_exist()

    ingest_port = free_port()
    subscribe_port = free_port()
    log_dir = Path(tempfile.mkdtemp(prefix="sensorlink-integration-"))
    print(f"log directory: {log_dir}")

    processes = []
    stuck_subscriber_socket = None

    def start(name, args, **kwargs):
        log_file = open(log_dir / f"{name}.log", "w")
        proc = subprocess.Popen(args, stdout=log_file, stderr=subprocess.STDOUT, **kwargs)
        processes.append((name, proc, log_file))
        return proc

    def dump_log(name):
        for entry_name, _, log_file in processes:
            if entry_name == name:
                log_file.flush()
                print(f"--- {name}.log ---")
                print((log_dir / f"{name}.log").read_text())

    try:
        print(f"starting sensorlink-server (ingest={ingest_port}, subscribe={subscribe_port})")
        start("server", [str(SERVER_BIN), "--ingest", str(ingest_port), "--subscribe", str(subscribe_port)])
        if not wait_for_port(INGEST_HOST, ingest_port, timeout=5):
            dump_log("server")
            raise IntegrationTestFailure("server never opened its ingest port")
        if not wait_for_port(SUBSCRIBE_HOST, subscribe_port, timeout=5):
            dump_log("server")
            raise IntegrationTestFailure("server never opened its subscribe port")

        print("starting QEMU with the real firmware image")
        start(
            "qemu",
            [
                "qemu-system-arm",
                "-M", "mps2-an385",
                "-nographic",
                "-semihosting",
                "-monitor", "none",
                "-serial", f"tcp:{INGEST_HOST}:{ingest_port}",
                "-kernel", str(FIRMWARE_ELF),
            ],
        )

        print("starting sensorlink-client as the observer (subscribed to temp)")
        observer = subprocess.Popen(
            [str(CLIENT_BIN), "--connect", f"{SUBSCRIBE_HOST}:{subscribe_port}", "--subscribe", "temp"],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
        processes.append(("client", observer, None))
        reader = LineReader(observer.stdout)

        print("waiting for the handshake and the first few samples")
        deadline = time.monotonic() + 20
        while reader.count_since("temp:", 0) < 3:
            if time.monotonic() > deadline:
                dump_log("qemu")
                dump_log("server")
                raise IntegrationTestFailure(
                    "no samples reached the client within 20s -- handshake or wiring is broken. "
                    f"client output so far: {reader.tail()}"
                )
            time.sleep(0.5)

        print(f"measuring baseline rate for {MEASUREMENT_WINDOW_S}s")
        baseline_start = time.monotonic()
        time.sleep(MEASUREMENT_WINDOW_S)
        baseline_count = reader.count_since("temp:", baseline_start)
        print(f"baseline: {baseline_count} sample(s) in {MEASUREMENT_WINDOW_S}s")

        print("connecting a subscriber that will never drain its buffer")
        stuck_subscriber_socket = socket.create_connection((SUBSCRIBE_HOST, subscribe_port), timeout=5)
        stuck_subscriber_socket.sendall(b"SUBSCRIBE pressure\n")

        print(f"flooding the 'pressure' metric for {LOAD_DURATION_S}s to build backpressure")
        load_generator = start(
            "load_generator",
            [
                str(LOAD_GENERATOR_BIN),
                "--connect", f"{INGEST_HOST}:{ingest_port}",
                "--metric", "1",
                "--seconds", str(LOAD_DURATION_S),
            ],
        )
        load_generator.wait(timeout=LOAD_DURATION_S + 10)

        print(f"waiting {SETTLE_TIME_S}s for housekeeping to notice and SLOW_DOWN to take effect")
        time.sleep(SETTLE_TIME_S)

        print(f"measuring post-backpressure rate for {MEASUREMENT_WINDOW_S}s")
        after_start = time.monotonic()
        time.sleep(MEASUREMENT_WINDOW_S)
        after_count = reader.count_since("temp:", after_start)
        print(f"after backpressure: {after_count} sample(s) in {MEASUREMENT_WINDOW_S}s")

        if after_count == 0:
            raise IntegrationTestFailure(
                "the firmware stopped sending entirely -- that is not a slow-down, it looks dead"
            )
        if after_count > baseline_count * 0.75:
            raise IntegrationTestFailure(
                f"sample rate did not drop: {baseline_count} before vs {after_count} after "
                f"(expected after <= {baseline_count * 0.75:.1f})"
            )

        print("PASS: handshake worked, samples flowed, and SLOW_DOWN measurably lowered the real rate")
        return 0

    except IntegrationTestFailure as exc:
        print(f"FAIL: {exc}")
        return 1

    finally:
        if stuck_subscriber_socket is not None:
            stuck_subscriber_socket.close()
        for name, proc, log_file in processes:
            proc.terminate()
        for name, proc, log_file in processes:
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
            if log_file is not None:
                log_file.close()


if __name__ == "__main__":
    sys.exit(main())
