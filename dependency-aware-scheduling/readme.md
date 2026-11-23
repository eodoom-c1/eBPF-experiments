# PostgreSQL dependency-scheduling baseline

**Measurement only.** Start with representative parallel queries in an existing
PostgreSQL workload. Determine whether leaders wait for associated workers that
are runnable but not scheduled. No scheduler, affinity changes, artificial CPU
load, or PMU counters are involved.

This is a first screening tool, not a novel scheduling contribution. It reuses
Linux `perf` tracepoints and read-only `pg_stat_activity` observations.

## Requirements

- Linux, Python 3.9+, `perf`, and `psql`; no Python packages or build step.
- PostgreSQL 13+ with representative queries that actually use parallel workers.
- Run the observer on the database VM, in the same PID namespace as PostgreSQL.
  Remote databases and isolated container PID namespaces are not supported.
- A monitoring role with `pg_read_all_stats`, provisioned by the database owner.
- Permission to record system-wide scheduler tracepoints, normally via `sudo`.

Keep the existing database, queries, concurrency, and resource limits unchanged
for the baseline. Do not increase contention just to create a positive result.
Use a disposable/test deployment or an explicitly authorized production trace.
`pg_stat_activity` is sampled every 100 ms by default over one persistent psql
connection. Sampling overhead and missed polling deadlines are recorded.
Observer and psql CPU accounting excludes the monitoring backend's server-side
CPU cost; an overhead comparison must also examine application outcomes.

## User-run commands

From the Linux repository root, first run the focused tests:

```bash
python3 -m unittest discover -s dependency-aware-scheduling/tests -p 'test_*.py'
mkdir -p out/research/dependency-baseline
```

Keep your representative PostgreSQL workload running. In **terminal 1**, begin
the scheduler trace:

```bash
sudo perf record -a --clockid mono \
  -e sched:sched_switch -e sched:sched_wakeup \
  -e sched:sched_wakeup_new -e sched:sched_process_exit \
  -o out/research/dependency-baseline/sched.data -- sleep 90
```

While that trace is recording, in **terminal 2** collect database observations.
Use a configured libpq service named `dependency-baseline` for your local
database and monitoring role. Do not put a password in the command line.

```bash
python3 dependency-aware-scheduling/profile-postgres.py collect \
  --database 'service=dependency-baseline' \
  --duration 60 --interval 0.1 \
  --output out/research/dependency-baseline/activity.jsonl
```

The libpq service can be configured in `~/.pg_service.conf`; use your existing
credential mechanism, such as `.pgpass` with restricted permissions. Start
terminal 2 promptly: the scheduler trace must cover every activity observation.
Collection does not execute or create application queries.

After `perf record` finishes, export and analyze:

```bash
sudo perf script --header --show-lost-events --ns -F trace:cpu,time,event,trace \
  -i out/research/dependency-baseline/sched.data \
  > out/research/dependency-baseline/sched.txt

python3 dependency-aware-scheduling/profile-postgres.py analyze \
  --samples out/research/dependency-baseline/activity.jsonl \
  --trace out/research/dependency-baseline/sched.txt \
  --output out/research/dependency-baseline/assessment.json
```

Check both perf commands' exit status and stderr before interpreting a recording.
The analyzer verifies the recorded `--clockid mono` command in the trace header
and rejects reported lost events, malformed/out-of-order events, and traces
that do not cover the sample window. Use fresh output paths for another
run: activity and assessment files are created exclusively, not overwritten.
The existing `out/` ignore rule keeps measurement artifacts outside Git.
Both raw `key=value` scheduler fields and libtraceevent's compact
`command:pid [priority]` formatting are supported. The `trace:` field selector
avoids irrelevant warnings about fields for non-tracepoint event types.
Metadata errors such as `Invalid HEADER_EVENT_DESC` are separate from scheduler
formatting: do not ignore export errors or interpret a partial export as a result.

## Interpretation and research gate

The observer records backend PID, leader PID, query-start identity, state, and
wait event, **not query text or connection credentials**. An observation is a
candidate only when a leader reports `IPC/MessageQueueReceive` or
`IPC/ExecuteGather` and an associated parallel worker is confirmed runnable
throughout the SQL observation's monotonic before/after window.

Worker runqueue episodes start at a wakeup or runnable preemption and end at
dispatch. Unobserved beginnings and unfinished episodes are reported separately.
Multiple queued workers do not multiply a leader's observation count. Reported
episode durations are complete worker queue waits, not recovered query latency.

`candidate_observed` means sampled co-occurrence, **not proven critical-path
causality**. Another worker may be producing useful results; a queued worker may
not be the one delaying completion. Short queries and waits between polls can be
missed. `insufficient_observations` and `no_candidate_observed` are different
outcomes; neither establishes that a scheduling opportunity cannot exist.

Before designing an intervention, require application completion/latency data,
representative repeated observations, an observer-overhead comparison, and
evidence that this dependency-related delay is a substantial part of the real
bottleneck. Compare against existing scheduling policies before claiming novelty.
On Azure, recorded guest steal accounting is supporting context, not complete
visibility into host scheduling. No physical-core or cache-sharing inference is
made from guest vCPU numbers.
