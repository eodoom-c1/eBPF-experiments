# Project 1: Exec-attempt tracer

## Goal

Build a small eBPF tool that reports which processes attempt to execute a
program. This is our first hands-on project: learn the complete eBPF lifecycle
before introducing aggregation, structured event streams, or enforcement.

Each record contains:

- **TGID:** the thread-group ID, usually called the process PID in userspace.
- **TID:** the individual task/thread ID.
- **comm:** the current task name, limited to 15 bytes plus a null terminator.

Example output:

```text
p1_exec tgid=4123 tid=4123 comm=bash
```

## Execution flow

```text
Userspace process calls execve()
                 |
                 v
Kernel reaches sys_enter_execve tracepoint
                 |
                 v
Our attached eBPF program runs
                 |
                 +-- obtains process/thread IDs
                 +-- obtains the current task name
                 +-- writes a diagnostic trace record
                 |
                 v
Kernel continues handling execve()
```

We attach to `syscalls:sys_enter_execve`. The program observes the syscall
without approving, rejecting, or modifying it. Returning `0` from this tracing
program does not control the syscall's outcome.

## What the event means

`execve()` replaces a process's current program; it does not itself create a
process. A shell commonly creates a child first, and that child calls `execve()`.

The tracepoint fires at syscall entry, so we observe an **attempt**, including
attempts that later fail. We do not yet monitor successful execution or the
separate `execveat()` syscall.

The recorded `comm` is the name before execution. A shell child attempting to
execute `ls` might still report `bash`. It is not the executable path or full
command line. Shell built-ins need not call `execve()`.

The tracer observes matching syscalls on the Linux kernel where it is attached.
Inside a VM, that means the guest kernel, not the outer host. Containers sharing
that kernel can also generate events.

## Components and lifecycle

| Component | Responsibility |
| --- | --- |
| `exec_trace.bpf.c` | Kernel-side tracepoint program |
| `exec_trace.c` | Userspace loader and attachment lifecycle |
| `Makefile` | Builds the BPF object, generates the skeleton, and links the loader |
| Generated `build/exec_trace.skel.h` | Glue between the loader and compiled BPF object |

We use C and libbpf. The kernel-side program obtains IDs with
`bpf_get_current_pid_tgid()`, reads the task name with
`bpf_get_current_comm()`, and emits diagnostic records with `bpf_printk()`.

The stages are distinct:

1. **Compile:** Clang translates C into eBPF instructions.
2. **Load:** The kernel verifier checks memory safety, permitted helpers, and
   bounded execution.
3. **Attach:** The loader connects the accepted program to the tracepoint.
4. **Observe:** Matching syscalls trigger the program.
5. **Detach:** The loader releases the attachment and other resources.

The ELF section name identifies the intended hook; it does not attach the
program by itself. We do not pin resources, so stopping the loader and closing
the last attachment reference stops new events. Previously buffered records may
remain readable.

Later projects will use CO-RE kernel-field relocations. This first program can
treat the tracepoint context as opaque and obtain its data through helpers.

## Deliberate limitations

`bpf_printk()` writes to the kernel tracing infrastructure, not directly to the
loader's terminal. A trace-stream reader displays records, commonly through
`/sys/kernel/tracing/trace_pipe`.

This is a debugging transport: the stream is shared, buffering is finite,
readers consume records, and printing has overhead. It is not a durable audit
trail, and this project does not yet account for lost records.

We do not define application aggregation maps, although supporting maps may be
used internally by libbpf and the printing machinery.

## Build and run on Linux

Use an isolated Linux VM, not a shared production machine. This program traces
the whole kernel, and reading `trace_pipe` consumes shared diagnostic records.
The source can be edited on Windows, but the Makefile builds Linux programs.

Required tools and development files:

- A C compiler and GNU Make.
- Clang with the BPF target enabled.
- `bpftool` with skeleton generation support.
- `pkg-config` and libbpf development headers/libraries.
- Native Linux UAPI headers, including `asm/types.h`.

The kernel must expose the `syscalls:sys_enter_execve` tracepoint and support BPF
tracepoint programs. Loading/attaching and reading tracefs require appropriate
privileges; the commands below use `sudo` inside the lab VM.

From this project directory on Linux, build without root privileges:

```bash
make
```

The generated files are kept under the ignored `build/` directory. Compilation
and skeleton generation do not attach anything to the kernel.

In terminal 1, start the loader:

```bash
sudo ./build/exec_trace
```

In terminal 2, read only the displayed records bearing our prefix:

```bash
sudo cat /sys/kernel/tracing/trace_pipe |
  grep --line-buffered 'p1_exec'
```

The reader still consumes the shared trace stream before filtering it. Other
trace readers can compete for records.

If the tracefs mount is absent, configure it only inside the isolated VM:

```bash
sudo mkdir -p /sys/kernel/tracing
sudo mount -t tracefs tracefs /sys/kernel/tracing
```

### Exercise the tracer

In a third Linux terminal, invoke an external executable:

```bash
/bin/true
```

You should see an execution-attempt record. Its `comm` can still be the invoking
shell's name.

Make an actual `execve()` syscall that fails:

```bash
python3 -c 'import os; os.execve("/definitely-missing-p1", ["missing-p1"], os.environ)'
```

Python should report `FileNotFoundError`, while the tracer still records the
attempt. Launching Python itself can produce another record.

Contrast that with a Bash built-in:

```bash
type printf
printf '%s\n' 'A Bash built-in need not execute another program.'
```

Stop the loader with Ctrl+C in terminal 1. Subsequent executions should produce
no new `p1_exec` records from this instance; already buffered records can remain.
Stop the trace reader separately with Ctrl+C.

To remove only generated build files:

```bash
make clean
```

### Failures and licensing

The loader reports opening, loading, and attachment failures separately.
libbpf also writes its diagnostics, including verifier failures, to stderr.
A missing tracepoint/tracefs mount or insufficient privileges can prevent
attachment even when compilation succeeds.

The BPF source is dual MIT/GPL-2.0-only and declares the kernel-recognized
`Dual MIT/GPL` license string because the printing helper requires GPL
compatibility. The userspace source retains the repository's MIT license.

## Completion checkpoint

Before moving on, be able to:

- Explain an execution attempt versus process creation or successful execution.
- Explain TGID, TID, and why `comm` can name the old program.
- Distinguish compilation, verifier acceptance, and attachment.
- Observe successful and deliberately failed syscall attempts.
- Stop the loader and verify that no new project events are generated.
- Add a small change, such as a TGID filter, and explain its behavior.
