#!/usr/bin/env python3
"""Open-loop, common-duration validation of the four-stream lookup service."""

import argparse
from array import array
from collections import deque
from datetime import datetime, timezone
import hashlib
import json
import math
import multiprocessing
from multiprocessing.connection import Connection, wait
import os
from pathlib import Path
import select
import socket
import struct
import subprocess
import sys
import time


NANOSECONDS = 1_000_000_000
REQUEST = struct.Struct("!QQ")
RESPONSE = struct.Struct("!QII")
KEY_COUNT = 65536
VALUE_BYTES = 64
RESPONSE_BYTES = RESPONSE.size + VALUE_BYTES


def make_values(count: int = KEY_COUNT) -> tuple[bytes, ...]:
    mask = (1 << 64) - 1
    return tuple(
        struct.pack("!8Q", *(key ^ ((0x9E3779B97F4A7C15 * word) & mask) for word in range(8)))
        for key in range(count)
    )


class ArrivalStream:
    """Keeps intended arrivals independent of response completion and queue capacity."""

    def __init__(
        self, thread: int, rate: int, duration_ns: int, start_ns: int,
        max_outstanding: int, values: tuple[bytes, ...],
    ) -> None:
        self.thread = thread
        self.rate = rate
        self.start_ns = start_ns
        self.end_ns = start_ns + duration_ns
        self.total = (duration_ns * rate + NANOSECONDS - 1) // NANOSECONDS
        self.max_outstanding = max_outstanding
        self.values = values
        self.offered = 0
        self.admitted = 0
        self.rejected = 0
        self.completed = 0
        self.completed_in_window = 0
        self.bytes_sent = 0
        self.pending_ids: deque[int] = deque()
        self.send_buffer = bytearray()
        self.receive_buffer = bytearray()
        self.latencies = array("Q")
        self.peak_outstanding = 0
        self.max_generation_lag_ns = 0
        self.generation_late_over_1ms = 0

    def arrival_ns(self, request_id: int) -> int:
        return self.start_ns + (request_id * NANOSECONDS + self.rate - 1) // self.rate

    def key(self, request_id: int) -> int:
        return (request_id * 11400714819323198485 + self.thread) % len(self.values)

    def offer_due(self, now_ns: int) -> None:
        if now_ns < self.start_ns:
            return
        due = min(self.total, (now_ns - self.start_ns) * self.rate // NANOSECONDS + 1)
        while self.offered < due:
            request_id = self.offered
            self.offered += 1
            lag = now_ns - self.arrival_ns(request_id)
            self.max_generation_lag_ns = max(self.max_generation_lag_ns, lag)
            if lag > 1_000_000:
                self.generation_late_over_1ms += 1
            if len(self.pending_ids) >= self.max_outstanding:
                self.rejected += 1
                continue
            self.pending_ids.append(request_id)
            self.send_buffer.extend(REQUEST.pack(request_id, self.key(request_id)))
            self.admitted += 1
            self.peak_outstanding = max(self.peak_outstanding, len(self.pending_ids))

    def accept_bytes(self, data: bytes, observed_ns: int) -> None:
        self.receive_buffer.extend(data)
        consumed = 0
        while len(self.receive_buffer) - consumed >= RESPONSE_BYTES:
            request_id, status, length = RESPONSE.unpack_from(self.receive_buffer, consumed)
            if not self.pending_ids or request_id != self.pending_ids[0]:
                raise ValueError(f"T{self.thread}: unexpected or duplicate response ID {request_id}")
            value = self.receive_buffer[consumed + RESPONSE.size:consumed + RESPONSE_BYTES]
            if status != 0 or length != VALUE_BYTES or value != self.values[self.key(request_id)]:
                raise ValueError(f"T{self.thread}: invalid lookup response for {request_id}")
            latency = observed_ns - self.arrival_ns(request_id)
            if latency < 0:
                raise ValueError("A response precedes its intended arrival")
            self.pending_ids.popleft()
            self.latencies.append(latency)
            self.completed += 1
            if observed_ns < self.end_ns:
                self.completed_in_window += 1
            consumed += RESPONSE_BYTES
        if consumed:
            del self.receive_buffer[:consumed]

    def summary(self, duration_ns: int, cpu_seconds: float) -> dict:
        ordered = sorted(self.latencies)
        percentile = (
            ordered[(99 * len(ordered) + 99) // 100 - 1] / 1_000_000 if ordered else None
        )
        unanswered = self.admitted - self.completed
        if self.offered != self.total or self.offered != self.rejected + self.admitted:
            raise ValueError("Arrival accounting does not cover the complete window")
        return {
            "thread": self.thread,
            "requested_requests_per_second": self.rate,
            "offered_requests": self.offered,
            "admitted_requests": self.admitted,
            "sent_requests": self.bytes_sent // REQUEST.size,
            "completed_requests": self.completed,
            "completed_in_window": self.completed_in_window,
            "completed_after_window": self.completed - self.completed_in_window,
            "client_rejected_requests": self.rejected,
            "unanswered_after_drain": unanswered,
            "dropped_requests": self.rejected + unanswered,
            "backlog_at_window_end": self.admitted - self.completed_in_window,
            "peak_outstanding_requests": self.peak_outstanding,
            "useful_requests_per_second_in_window": self.completed_in_window * NANOSECONDS / duration_ns,
            "p99_completed_latency_ms": percentile,
            "max_generation_lag_ms": self.max_generation_lag_ns / 1_000_000,
            "generation_late_over_1ms": self.generation_late_over_1ms,
            "generator_cpu_seconds": cpu_seconds,
        }


def load_worker(
    control: Connection, thread: int, cpu: int, port: int, rate: int,
    duration_ns: int, drain_ns: int, max_outstanding: int, values: tuple[bytes, ...],
) -> None:
    try:
        os.sched_setaffinity(0, {cpu})
        if os.sched_getaffinity(0) != {cpu}:
            raise RuntimeError(f"T{thread}: client singleton affinity failed")
        with socket.create_connection(("127.0.0.1", port), timeout=10) as connection:
            connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            connection.setblocking(False)
            control.send({"type": "ready", "thread": thread, "client_cpu": cpu})
            start_ns = control.recv()
            stream = ArrivalStream(thread, rate, duration_ns, start_ns, max_outstanding, values)
            deadline_ns = stream.end_ns + drain_ns
            start_cpu = time.process_time_ns()
            while True:
                now_ns = time.monotonic_ns()
                stream.offer_due(now_ns)
                if now_ns >= deadline_ns:
                    break
                if stream.send_buffer:
                    try:
                        sent = connection.send(stream.send_buffer)
                        if sent == 0:
                            raise RuntimeError(f"T{thread}: socket send returned zero")
                    except BlockingIOError:
                        sent = 0
                    if sent:
                        del stream.send_buffer[:sent]
                        stream.bytes_sent += sent
                try:
                    data = connection.recv(65536)
                except BlockingIOError:
                    data = None
                if data == b"":
                    raise RuntimeError(f"T{thread}: service closed before the client finished")
                if data:
                    stream.accept_bytes(data, time.monotonic_ns())
                if now_ns >= stream.end_ns and not stream.pending_ids:
                    break
                next_wake = (
                    stream.arrival_ns(stream.offered) if stream.offered < stream.total
                    else stream.end_ns if now_ns < stream.end_ns else deadline_ns
                )
                timeout = max(0.0, min(0.05, (min(next_wake, deadline_ns) - time.monotonic_ns()) / NANOSECONDS))
                select.select(
                    [connection], [connection] if stream.send_buffer else [], [], timeout
                )
            cpu_seconds = (time.process_time_ns() - start_cpu) / NANOSECONDS
        result = stream.summary(duration_ns, cpu_seconds)
        result["type"] = "result"
        result["client_cpu"] = cpu
        result["client_affinity_after"] = sorted(os.sched_getaffinity(0))
        control.send(result)
    except (OSError, ValueError, RuntimeError) as error:
        control.send({"type": "error", "thread": thread, "error": str(error)})
        raise
    finally:
        control.close()


def collect_messages(connections: list[Connection], expected_type: str, timeout: float) -> list[dict]:
    pending = set(connections)
    messages = []
    deadline = time.monotonic() + timeout
    while pending:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError(f"Timed out waiting for client {expected_type} messages")
        for connection in wait(pending, timeout=remaining):
            try:
                message = connection.recv()
            except EOFError as error:
                raise RuntimeError("Client exited without its result") from error
            if message.get("type") != expected_type:
                raise RuntimeError(f"Client failure: {message}")
            pending.remove(connection)
            messages.append(message)
    return sorted(messages, key=lambda message: message["thread"])


def read_ready(process: subprocess.Popen) -> dict:
    readable, _, _ = select.select([process.stdout], [], [], 10)
    if not readable:
        raise TimeoutError("Service did not become ready within 10 seconds")
    line = process.stdout.readline()
    if not line:
        _, error = process.communicate(timeout=5)
        raise RuntimeError(f"Service exited before readiness ({process.returncode}): {error.strip()}")
    ready = json.loads(line)
    if (
        ready.get("type") != "ready" or ready.get("schema_version") != 1
        or ready.get("key_count") != KEY_COUNT or ready.get("value_bytes") != VALUE_BYTES
    ):
        raise ValueError(f"Unexpected service readiness: {ready}")
    return ready


def policy_snapshot(cpus: list[int]) -> list[dict]:
    result = []
    for cpu in sorted(set(cpus)):
        policies = {"cpu": cpu}
        for property_name in ("scaling_min_freq", "scaling_max_freq", "scaling_governor"):
            path = Path(f"/sys/devices/system/cpu/cpu{cpu}/cpufreq/{property_name}")
            try:
                policies[property_name] = {"value": path.read_text().strip(), "error": None}
            except OSError as error:
                policies[property_name] = {"value": None, "error": str(error)}
        result.append(policies)
    return result


def run_condition(options: argparse.Namespace, placement: str, layout: str, values: tuple[bytes, ...]) -> dict:
    arguments = [
        str(options.server), "--cpus", ",".join(map(str, options.cpus)),
        "--placement", placement, "--layout", layout, "--base-port", str(options.base_port),
    ]
    before = policy_snapshot(options.cpus)
    original_affinity = os.sched_getaffinity(0)
    process = subprocess.Popen(arguments, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    clients = []
    connections = []
    try:
        # The service inherits its allowed mask before the controller moves to client CPUs.
        os.sched_setaffinity(0, set(options.client_cpus))
        ready = read_ready(process)
        context = multiprocessing.get_context("fork")
        duration_ns = round(options.duration * NANOSECONDS)
        for index, rate in enumerate(options.rates):
            parent, child = context.Pipe()
            worker = context.Process(
                target=load_worker,
                args=(
                    child, index + 1, options.client_cpus[index % len(options.client_cpus)],
                    options.base_port + index, rate, duration_ns, round(options.drain_timeout * NANOSECONDS),
                    options.max_outstanding, values,
                ),
            )
            worker.start()
            child.close()
            clients.append(worker)
            connections.append(parent)
        collect_messages(connections, "ready", 15)
        start_ns = time.monotonic_ns() + 250_000_000
        for connection in connections:
            connection.send(start_ns)
        results = collect_messages(connections, "result", options.duration + options.drain_timeout + 30)
        for worker in clients:
            worker.join(timeout=5)
            if worker.exitcode != 0:
                raise RuntimeError(f"Client process {worker.pid} failed or did not exit")
        output, error = process.communicate(timeout=10)
        if process.returncode != 0:
            raise RuntimeError(f"Service failed ({process.returncode}): {error.strip()}")
        lines = output.splitlines()
        if len(lines) != 1:
            raise ValueError("Expected exactly one service result after readiness")
        service = json.loads(lines[0])
        if service.get("type") != "result" or service.get("placement") != placement or service.get("layout") != layout:
            raise ValueError("Service result does not match the selected condition")
        if len(service.get("threads", [])) != 4:
            raise ValueError("Service did not report four workers")
        cpu_seconds = 0.0
        for client, server in zip(results, service["threads"]):
            if (
                client["thread"] != server["thread"]
                or client["completed_requests"] > server["completed_requests"]
                or server["completed_requests"] > client["admitted_requests"]
            ):
                raise ValueError("Client/service completed-work accounting mismatch")
            expected_cpu = options.cpus[
                ([2, 1, 0, 3] if placement == "colocated" else [0, 1, 2, 3])[client["thread"] - 1]
            ]
            if server["start_cpu"] != expected_cpu or server["end_cpu"] != expected_cpu:
                raise ValueError("Service worker endpoint affinity mismatch")
            latency = client["p99_completed_latency_ms"]
            client["service_objectives_met"] = (
                client["dropped_requests"] == 0 and server["connection_error"] == 0
                and latency is not None and latency <= options.p99_budget_ms
            )
            client["service"] = server
            cpu_seconds += server["cpu_seconds"]
        completed = sum(client["completed_requests"] for client in results)
        return {
            "schema_version": 1,
            "experiment": "tcp-key-value-request-workload",
            "recorded_at_utc": datetime.now(timezone.utc).isoformat(),
            "placement": placement,
            "layout": layout,
            "arrival_window_seconds": options.duration,
            "drain_timeout_seconds": options.drain_timeout,
            "rates": options.rates,
            "p99_budget_ms": options.p99_budget_ms,
            "drop_budget_requests": 0,
            "max_outstanding_per_worker": options.max_outstanding,
            "cpu_slots": options.cpus,
            "client_cpus": options.client_cpus,
            "total_service_cpu_seconds": cpu_seconds,
            "cpu_ns_per_completed_request": cpu_seconds * NANOSECONDS / completed if completed else None,
            "useful_requests_per_second_in_window": sum(
                client["useful_requests_per_second_in_window"] for client in results
            ),
            "service_objectives_met": all(client["service_objectives_met"] for client in results),
            "threads": results,
            "service_metadata": service,
            "readiness": ready,
            "frequency_policy_before": before,
            "frequency_policy_after": policy_snapshot(options.cpus),
            "service_binary_sha256": options.binary_hash,
            "driver_sha256": options.driver_hash,
            "arguments": sys.argv,
            "parent_control_affinity": sorted(os.sched_getaffinity(0)),
            "kernel": os.uname().release,
            "architecture": os.uname().machine,
        }
    finally:
        for worker in clients:
            if worker.is_alive():
                worker.terminate()
            worker.join(timeout=5)
            if worker.is_alive():
                worker.kill()
                worker.join()
        for connection in connections:
            connection.close()
        if process.poll() is None:
            process.terminate()
            try:
                process.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate()
        os.sched_setaffinity(0, original_affinity)


def positive_float(text: str) -> float:
    value = float(text)
    if not math.isfinite(value) or value <= 0:
        raise argparse.ArgumentTypeError("Must be a finite positive number")
    return value


def integer_list(text: str) -> list[int]:
    entries = text.split(",")
    if not entries or any(not entry.isascii() or not entry.isdecimal() for entry in entries):
        raise argparse.ArgumentTypeError("Use comma-separated nonnegative integers")
    return [int(entry) for entry in entries]


def file_hash(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def parse_options() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--cpus", type=integer_list, default=[0, 0, 1, 1])
    parser.add_argument("--client-cpus", type=integer_list, default=[2, 3])
    parser.add_argument("--rates", type=integer_list, default=[20000, 5000, 5000, 20000])
    parser.add_argument("--duration", type=positive_float, default=60)
    parser.add_argument("--p99-budget-ms", type=positive_float, default=5)
    parser.add_argument("--drain-timeout", type=positive_float, default=10)
    parser.add_argument("--max-outstanding", type=int, default=10000)
    parser.add_argument("--base-port", type=int, default=19000)
    parser.add_argument("--placement", choices=("baseline", "colocated"))
    parser.add_argument("--layout", choices=("shared", "padded"))
    parser.add_argument("--output", type=Path, required=True)
    options = parser.parse_args()
    if not hasattr(os, "sched_getaffinity"):
        parser.error("This workload requires Linux CPU affinity")
    if len(options.cpus) != 4 or len(options.rates) != 4 or any(rate <= 0 for rate in options.rates):
        parser.error("Supply four CPU slots and four positive per-worker rates")
    if not options.client_cpus or set(options.cpus) & set(options.client_cpus):
        parser.error("Client CPUs must be nonempty and disjoint from service CPUs")
    allowed = os.sched_getaffinity(0)
    if not set(options.cpus + options.client_cpus) <= allowed:
        parser.error("Requested CPUs are outside the current allowed mask")
    service_cores = {
        Path(f"/sys/devices/system/cpu/cpu{cpu}/topology/thread_siblings_list").read_text().strip()
        for cpu in options.cpus
    }
    client_cores = {
        Path(f"/sys/devices/system/cpu/cpu{cpu}/topology/thread_siblings_list").read_text().strip()
        for cpu in options.client_cpus
    }
    if service_cores & client_cores:
        parser.error("Client CPUs must also be on different physical cores from service CPUs")
    if not 1024 <= options.base_port <= 65532 or options.max_outstanding <= 0:
        parser.error("Base port must be 1024-65532 and max-outstanding must be positive")
    if any(options.duration * rate > 10_000_000 for rate in options.rates) or (
        round(options.duration * NANOSECONDS) <= 0
    ):
        parser.error("Use a representable window with at most 10M requests per worker")
    if (options.placement is None) != (options.layout is None):
        parser.error("Specify both --placement and --layout, or neither for all four conditions")
    options.server = options.server.resolve()
    if not options.server.is_file() or not os.access(options.server, os.X_OK):
        parser.error("--server must identify an executable service binary")
    options.binary_hash = file_hash(options.server)
    options.driver_hash = file_hash(Path(__file__))
    return options


def main() -> int:
    options = parse_options()
    values = make_values()
    conditions = (
        [(options.placement, options.layout)] if options.placement is not None else
        [("baseline", "shared"), ("colocated", "shared"), ("baseline", "padded"), ("colocated", "padded")]
    )
    objectives_met = True
    with options.output.open("a", encoding="utf-8") as output:
        for placement, layout in conditions:
            record = run_condition(options, placement, layout, values)
            output.write(json.dumps(record, allow_nan=False, separators=(",", ":")) + "\n")
            output.flush()
            objectives_met = objectives_met and record["service_objectives_met"]
            print(
                f"{placement}/{layout}: CPU={record['total_service_cpu_seconds']:.6f}s "
                f"in-window throughput={record['useful_requests_per_second_in_window']:.1f}/s "
                f"service_objectives_met={record['service_objectives_met']}",
                file=sys.stderr,
            )
    return 0 if objectives_met else 2


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"request-workload: {error}", file=sys.stderr)
        sys.exit(1)
