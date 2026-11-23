#!/usr/bin/env python3
"""Measure PostgreSQL parallel-worker scheduling candidates without PMUs."""

import argparse
import bisect
import json
import math
import os
from pathlib import Path
import re
import select
import shlex
import subprocess
import sys
import tempfile
import time


NANOSECONDS = 1_000_000_000
RECEIVE_WAITS = {"MessageQueueReceive", "ExecuteGather"}
EVENT = re.compile(
    r"^\s*\[(\d+)\]\s+(\d+)\.(\d{9}):\s+"
    r"sched:(sched_switch|sched_wakeup_new|sched_wakeup|sched_process_exit):\s+(.*)$"
)
SWITCH = re.compile(
    r"\bprev_pid=(\d+)\s+prev_prio=\d+\s+prev_state=(\S+)\s+"
    r"==>\s+next_comm=.*?\s+next_pid=(\d+)\s+next_prio=\d+"
)
PID = re.compile(r"\bpid=(\d+)\s+prio=\d+")
SESSION_SQL = """
SET statement_timeout = '5s';
SET application_name = 'dependency-scheduling-observer';
SET timezone = 'UTC';
SELECT json_build_object(
    'server_version_num', current_setting('server_version_num')::integer,
    'observer_backend_pid', pg_backend_pid(),
    'all_stats_visible', pg_has_role(current_user, 'pg_read_all_stats', 'USAGE')
);
"""
SAMPLE_SQL = """
SELECT COALESCE(json_agg(json_build_object(
    'pid', pid, 'leader_pid', leader_pid, 'backend_type', backend_type,
    'state', state, 'wait_event_type', wait_event_type,
    'wait_event', wait_event, 'query_start', query_start::text
) ORDER BY pid), '[]'::json)
FROM pg_stat_activity
WHERE datname = current_database()
  AND pid <> pg_backend_pid()
  AND backend_type IN ('client backend', 'parallel worker')
  AND (state = 'active' OR backend_type = 'parallel worker');
"""


def positive_float(text):
    value = float(text)
    if not math.isfinite(value) or value <= 0:
        raise argparse.ArgumentTypeError("Must be finite and positive")
    return value


def emit(handle, value):
    handle.write(json.dumps(value, separators=(",", ":"), allow_nan=False) + "\n")
    handle.flush()


def cpu_ticks():
    fields = Path("/proc/stat").read_text().splitlines()[0].split()
    if fields[0] != "cpu" or len(fields) < 9:
        raise ValueError("Missing aggregate CPU/steal accounting in /proc/stat")
    return {"total": sum(map(int, fields[1:9])), "steal": int(fields[8])}


def postgres_query(process, error_file, sql):
    process.stdin.write(sql.encode("utf-8") + b"\n")
    process.stdin.flush()
    deadline = time.monotonic() + 10
    data = bytearray()
    while b"\n" not in data:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("Timed out waiting for psql")
        ready, _, _ = select.select([process.stdout], [], [], remaining)
        if not ready:
            raise TimeoutError("Timed out waiting for psql")
        try:
            chunk = os.read(process.stdout.fileno(), 65536)
        except BlockingIOError:
            continue
        if not chunk:
            error_file.seek(0)
            raise RuntimeError("psql closed its output: " + error_file.read().strip())
        data.extend(chunk)
    line, remainder = data.split(b"\n", 1)
    if remainder.strip():
        raise ValueError("Expected exactly one JSON result from psql")
    return json.loads(line)


def collect(options):
    if sys.platform != "linux":
        raise RuntimeError("Collection requires Linux")
    import resource

    with options.output.open("x", encoding="utf-8") as output, tempfile.TemporaryFile(
        mode="w+t", encoding="utf-8"
    ) as errors:
        process = subprocess.Popen(
            ["psql", "-X", "-w", "-q", "-A", "-t", "-v", "ON_ERROR_STOP=1",
             "--dbname", options.database],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=errors,
        )
        try:
            os.set_blocking(process.stdout.fileno(), False)
            session = postgres_query(process, errors, SESSION_SQL)
            if session["server_version_num"] < 130000:
                raise ValueError("PostgreSQL 13 or newer is required for leader_pid")
            if not session["all_stats_visible"]:
                raise ValueError("The monitoring role requires pg_read_all_stats")
            backend_pid = session["observer_backend_pid"]
            if Path(f"/proc/{backend_pid}/comm").read_text().strip() != "postgres":
                raise ValueError("Observer must share PostgreSQL's host and PID namespace")
            interval_ns = round(options.interval * NANOSECONDS)
            duration_ns = round(options.duration * NANOSECONDS)
            if interval_ns < 1 or duration_ns < 1:
                raise ValueError("Duration and interval must be at least one nanosecond")
            start_ns = time.monotonic_ns()
            next_ns = start_ns
            start_cpu = time.process_time()
            child_cpu_before = resource.getrusage(resource.RUSAGE_CHILDREN)
            ticks_before = cpu_ticks()
            emit(output, {
                "type": "context", "schema_version": 1,
                "experiment": "postgres-dependency-scheduling-baseline",
                "clock": "CLOCK_MONOTONIC", "start_ns": start_ns,
                "requested_duration_ns": duration_ns, "sample_interval_ns": interval_ns,
                "session": session, "kernel": os.uname().release,
                "observer_affinity": sorted(os.sched_getaffinity(0)),
                "pid_namespace": os.readlink("/proc/self/ns/pid"),
                "clock_ticks_per_second": os.sysconf("SC_CLK_TCK"),
            })
            count = 0
            missed = 0
            while time.monotonic_ns() < start_ns + duration_ns:
                before_ns = time.monotonic_ns()
                backends = postgres_query(process, errors, SAMPLE_SQL)
                after_ns = time.monotonic_ns()
                if not isinstance(backends, list):
                    raise ValueError("Expected a PostgreSQL activity array")
                emit(output, {
                    "type": "sample", "begin_ns": before_ns,
                    "end_ns": after_ns, "backends": backends,
                })
                count += 1
                next_ns += interval_ns
                now_ns = time.monotonic_ns()
                if next_ns < now_ns:
                    skipped = (now_ns - next_ns) // interval_ns + 1
                    missed += skipped
                    next_ns += skipped * interval_ns
                wake_ns = min(next_ns, start_ns + duration_ns)
                if wake_ns > now_ns:
                    time.sleep((wake_ns - now_ns) / NANOSECONDS)
            process.stdin.write(b"\\q\n")
            process.stdin.flush()
            process.stdin.close()
            if process.wait(timeout=10) != 0:
                errors.seek(0)
                raise RuntimeError("psql failed: " + errors.read().strip())
            errors.seek(0)
            child_cpu_after = resource.getrusage(resource.RUSAGE_CHILDREN)
            emit(output, {
                "type": "summary", "samples": count,
                "elapsed_ns": time.monotonic_ns() - start_ns,
                "missed_poll_deadlines": missed,
                "observer_cpu_seconds": time.process_time() - start_cpu,
                "psql_cpu_seconds": (
                    child_cpu_after.ru_utime + child_cpu_after.ru_stime
                    - child_cpu_before.ru_utime - child_cpu_before.ru_stime
                ),
                "cpu_ticks_before": ticks_before, "cpu_ticks_after": cpu_ticks(),
                "psql_stderr": errors.read(),
            })
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            if not process.stdin.closed:
                try:
                    process.stdin.close()
                except BrokenPipeError:
                    pass
            process.stdout.close()
    print(f"Collected {count} activity samples; no scheduling policy changed.")


def read_samples(path):
    with path.open(encoding="utf-8") as handle:
        rows = [json.loads(line) for line in handle if line.strip()]
    if (
        len(rows) < 3 or rows[0].get("type") != "context"
        or rows[-1].get("type") != "summary"
        or rows[0].get("schema_version") != 1
        or rows[0].get("clock") != "CLOCK_MONOTONIC"
    ):
        raise ValueError("Incomplete or unsupported activity recording")
    samples = rows[1:-1]
    previous_end = -1
    for sample in samples:
        if (
            sample.get("type") != "sample"
            or sample["begin_ns"] < previous_end
            or sample["end_ns"] < sample["begin_ns"]
        ):
            raise ValueError("Invalid or out-of-order activity sample")
        pids = [backend["pid"] for backend in sample["backends"]]
        if len(set(pids)) != len(pids):
            raise ValueError("Duplicate backend PID in an activity sample")
        previous_end = sample["end_ns"]
    if len(samples) != rows[-1]["samples"]:
        raise ValueError("Activity sample count does not match its summary")
    return rows[0], samples, rows[-1]


def runqueue_intervals(lines, worker_pids):
    pending = {}
    running = set()
    intervals = {pid: [] for pid in worker_pids}
    first_ns = None
    last_ns = None
    unobserved_enqueue = 0
    abandoned_enqueue = 0
    clock_id = None
    for number, line in enumerate(lines, 1):
        if re.search(r"PERF_RECORD_LOST|LOST\s+\d+\s+events", line, re.IGNORECASE):
            raise ValueError(f"Lost scheduler events at trace line {number}")
        command = re.match(r"^\s*#\s*cmdline\s*:\s*(.*)$", line)
        if command:
            arguments = shlex.split(command.group(1))
            for index, argument in enumerate(arguments):
                if argument == "--clockid" and index + 1 < len(arguments):
                    clock_id = arguments[index + 1]
                elif argument.startswith("--clockid="):
                    clock_id = argument.split("=", 1)[1]
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        event = EVENT.match(line.rstrip())
        if event is None:
            raise ValueError(f"Unsupported trace line {number}; use the documented perf export")
        _, seconds, nanos, kind, fields = event.groups()
        timestamp = int(seconds) * NANOSECONDS + int(nanos)
        if last_ns is not None and timestamp < last_ns:
            raise ValueError("Scheduler trace is not chronologically ordered")
        if first_ns is None:
            first_ns = timestamp
        last_ns = timestamp
        if kind == "sched_switch":
            switch = SWITCH.search(fields)
            if switch is None:
                raise ValueError(f"Malformed sched_switch at trace line {number}")
            previous, state, following = switch.groups()
            previous, following = int(previous), int(following)
            running.discard(previous)
            if previous in worker_pids and previous != following:
                if state in {"R", "R+"}:
                    pending[previous] = timestamp
                else:
                    pending.pop(previous, None)
            if following in worker_pids:
                queued_at = pending.pop(following, None)
                if queued_at is None:
                    unobserved_enqueue += 1
                else:
                    intervals[following].append((queued_at, timestamp))
                running.add(following)
        else:
            match = PID.search(fields)
            if match is None:
                raise ValueError(f"Malformed {kind} at trace line {number}")
            pid = int(match.group(1))
            if pid not in worker_pids:
                continue
            if kind == "sched_process_exit":
                abandoned_enqueue += int(pid in pending)
                pending.pop(pid, None)
                running.discard(pid)
            elif pid not in running:
                pending.setdefault(pid, timestamp)
    return intervals, {
        "first_event_ns": first_ns, "last_event_ns": last_ns,
        "dispatches_without_observed_enqueue": unobserved_enqueue,
        "abandoned_enqueues": abandoned_enqueue,
        "unfinished_enqueues": len(pending),
        "clock_id": clock_id,
    }


def assess(samples, intervals, trace):
    if trace.get("clock_id") != "mono":
        raise ValueError("Trace header must confirm perf record --clockid mono")
    if (
        trace["first_event_ns"] is None
        or trace["first_event_ns"] > samples[0]["begin_ns"]
        or trace["last_event_ns"] < samples[-1]["end_ns"]
    ):
        raise ValueError("Scheduler trace must cover the complete activity sample window")
    starts = {pid: [begin for begin, _ in spans] for pid, spans in intervals.items()}
    queries = {}
    episodes = set()
    unmapped = 0
    for sample in samples:
        leaders = {
            backend["pid"]: backend for backend in sample["backends"]
            if backend["backend_type"] == "client backend"
        }
        workers = {}
        for backend in sample["backends"]:
            if backend["backend_type"] == "parallel worker":
                leader = backend["leader_pid"]
                if leader not in leaders:
                    unmapped += 1
                else:
                    workers.setdefault(leader, []).append(backend["pid"])
        for leader_pid, pids in workers.items():
            leader = leaders[leader_pid]
            if not leader["query_start"]:
                raise ValueError("Associated query leader has no visible query_start")
            key = (leader_pid, leader["query_start"])
            query = queries.setdefault(key, {
                "leader_pid": leader_pid, "query_start": leader["query_start"],
                "observations_with_workers": 0, "receive_wait_observations": 0,
                "receive_wait_with_confirmed_runnable_worker": 0,
            })
            query["observations_with_workers"] += 1
            if (
                leader["state"] != "active" or leader["wait_event_type"] != "IPC"
                or leader["wait_event"] not in RECEIVE_WAITS
            ):
                continue
            query["receive_wait_observations"] += 1
            matches = []
            for pid in pids:
                index = bisect.bisect_right(starts[pid], sample["begin_ns"]) - 1
                if index >= 0:
                    begin, end = intervals[pid][index]
                    # The worker must remain queued throughout the SQL observation window.
                    if end >= sample["end_ns"]:
                        matches.append((pid, begin, end))
            if matches:
                query["receive_wait_with_confirmed_runnable_worker"] += 1
                episodes.update(matches)
    waits = sum(query["receive_wait_observations"] for query in queries.values())
    candidates = sum(
        query["receive_wait_with_confirmed_runnable_worker"] for query in queries.values()
    )
    durations = sorted(end - begin for _, begin, end in episodes)
    return {
        "assessment": (
            "insufficient_observations" if not waits else
            "candidate_observed" if candidates else "no_candidate_observed"
        ),
        "queries_observed_with_parallel_workers": len(queries),
        "leader_receive_wait_observations": waits,
        "receive_wait_with_confirmed_runnable_worker": candidates,
        "candidate_fraction_of_receive_wait_observations": candidates / waits if waits else None,
        "unmapped_worker_observations": unmapped,
        "distinct_candidate_runqueue_episodes": len(durations),
        "candidate_episode_p99_ms": (
            durations[math.ceil(len(durations) * 0.99) - 1] / 1_000_000
            if durations else None
        ),
        "candidate_episode_max_ms": durations[-1] / 1_000_000 if durations else None,
        "queries": list(queries.values()),
        "interpretation": (
            "Sampled leader-wait/worker-runqueue co-occurrence, not causal critical-path "
            "delay or recoverable query latency. Short waits may be missed. Episode "
            "durations cover complete observed worker enqueues, not leader wait duration."
        ),
    }


def analyze(options):
    context, samples, summary = read_samples(options.samples)
    worker_pids = {
        backend["pid"] for sample in samples for backend in sample["backends"]
        if backend["backend_type"] == "parallel worker"
    }
    with options.trace.open(encoding="utf-8") as handle:
        intervals, trace = runqueue_intervals(handle, worker_pids)
    result = assess(samples, intervals, trace)
    result.update({
        "schema_version": 1, "experiment": context["experiment"],
        "activity_context": context, "collection_summary": summary,
        "trace_quality": trace,
    })
    with options.output.open("x", encoding="utf-8") as handle:
        emit(handle, result)
    print(
        f"{result['assessment']}: {result['receive_wait_with_confirmed_runnable_worker']}"
        f"/{result['leader_receive_wait_observations']} receive-wait observations "
        "had a confirmed runnable associated worker."
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    collector = commands.add_parser("collect", help="Read-only PostgreSQL activity sampling")
    collector.add_argument("--database", required=True, help="Local libpq database/connection string")
    collector.add_argument("--duration", type=positive_float, default=60)
    collector.add_argument("--interval", type=positive_float, default=0.1)
    collector.add_argument("--output", type=Path, required=True)
    analyzer = commands.add_parser("analyze", help="Correlate activity with an exported perf trace")
    analyzer.add_argument("--samples", type=Path, required=True)
    analyzer.add_argument("--trace", type=Path, required=True)
    analyzer.add_argument("--output", type=Path, required=True)
    options = parser.parse_args()
    try:
        (collect if options.command == "collect" else analyze)(options)
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        parser.exit(1, f"Error: {error}\n")


if __name__ == "__main__":
    main()
