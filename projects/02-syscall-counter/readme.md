# Project 2: Per-process syscall counter

## Goal

Count `execve` and `openat` attempts per process and display the ten busiest
processes once per second. Unlike Project 1, normal observations stay in BPF
maps instead of producing one diagnostic trace record per event.

Both successful and failed syscall attempts count. These are cumulative counts
since attachment, not rates or counts for the last second.

## Data flow

```text
execve attempt ----+
                   +--> syscall tracepoint program
openat attempt ----+              |
                                  v
                           obtain process TGID
                                  |
                                  v
                    per-CPU process_counts hash map
                    key: TGID
                    value on each CPU: {execve, openat}
                                  |
                                  v
                       userspace copies CPU values
                                  |
                                  v
                     sum CPUs, sort, print top ten
```

## What is new?

A **BPF map** holds data that kernel programs and userspace can access through
defined APIs. Maps are kernel objects with a specified type, key/value layout,
and capacity.

`process_counts` is a **per-CPU hash map**:

- The TGID key groups all threads of a process.
- Each key has a separate counter pair on every possible CPU.
- Kernel lookup selects the current CPU's value automatically.
- Userspace must allocate a complete CPU-value array and sum its elements.
- The map holds at most 4,096 TGIDs. Failed accounting increments a separate
  per-CPU array counter, displayed as `missed`.

If two CPUs see a new TGID simultaneously, one can insert it first.
`BPF_NOEXIST` prevents overwriting an existing entry; `EEXIST` is handled by
looking it up again.

### The same function name, two different interfaces

In `syscall_counts.bpf.c`, `bpf_map_lookup_elem()` is a **kernel helper** that
returns a pointer to a live map value.

In `syscall_counts.c`, `bpf_map_lookup_elem()` is a **userspace API** that asks
the kernel to copy values into a supplied buffer. For a per-CPU map, that buffer
must cover every possible CPU, not merely the currently online CPUs.

The shared header defines the value layout used by both sides.

## Files and reading order

| File | Responsibility |
| --- | --- |
| `syscall_counts.h` | Shared counter structure and capacity |
| `syscall_counts.bpf.c` | Map declarations, accounting helper, and two hooks |
| `syscall_counts.c` | Loader, map iteration, CPU aggregation, sorting, and cleanup |
| `Makefile` | Builds BPF, generates its skeleton, and links userspace |

Start with the shared header, then the BPF file's map declarations and
`count_attempt()`. Next read userspace `print_snapshot()` and finally `main()`.
All handwritten functions have XML-style documentation.

## Build and run

Use an isolated Linux VM with GNU Make, a C compiler, Clang's BPF target,
libbpf development files, `pkg-config`, native Linux UAPI headers, and `bpftool`.
Both syscall tracepoints must be available. Do not run on a shared production
kernel.

From this project directory on Linux:

```bash
make
sudo ./build/syscall_counts
```

No trace-pipe reader is needed for normal output.

In another Linux terminal, create repeated file-open attempts in one process:

```bash
python3 -c 'import os; print("TGID:", os.getpid(), flush=True); [open("/etc/hostname").close() for _ in range(1000)]'
```

That TGID should appear among the busiest processes. Python startup also opens
files, so its `OPENAT` total need not be exactly 1,000.

Exercise execution attempts:

```bash
/bin/true
python3 -c 'import os; os.execve("/definitely-missing-p2", ["missing-p2"], os.environ)'
```

The failed execution still counts. Since output is limited to ten TGIDs, a
process with few attempts may not be displayed.

Stop the loader with Ctrl+C. Both attachments and their unpinned maps are
released; restarting creates fresh counters.

```bash
make clean
```

## Deliberate limits

- Only `execve` and `openat` are counted, not `execveat`, `openat2`, or every
  syscall. A file-open API can use a different underlying syscall.
- Exited processes remain in the map until the tool stops. Reused TGIDs merge
  into their old entries; this is not yet a lifetime-aware process inventory.
- The fixed capacity bounds memory, but long-running sessions can fill it.
  Watch `missed`; restart to begin a fresh collection.
- Map iteration and cross-CPU reads are not an atomic snapshot. Processes can
  add attempts while userspace reads.
- No names or command lines are collected. TGIDs use the kernel's process-ID
  namespace and can differ from IDs displayed inside containers.
- This is an observer, not an enforcement mechanism or complete audit log.

The BPF source is dual MIT/GPL-2.0-only; a diagnostic error path uses the
GPL-restricted printing helper. Userspace retains the repository's MIT license.

## Completion checkpoint

Be able to explain:

- A map's type, key, value, and capacity.
- Why threads share a TGID key but CPUs have separate counter values.
- Kernel helper lookup versus userspace map lookup.
- Why aggregation uses possible CPUs and why snapshots are approximate.
- The insertion race, capacity exhaustion, and missed-attempt counter.
- What resets on detach and what these two syscall counts do not cover.
