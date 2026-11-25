# eBPF research ideas

These are research hypotheses, not verified novel solutions. eBPF must be
central to the mechanism, not incidental monitoring. Before implementation,
measure a substantial bottleneck in a representative real workload, check
the closest prior art, and establish why existing solutions are insufficient.
XRP is a benchmark for originality and impact, not an architecture to imitate.

## 1. Resource isolation for shared-file writeback

**Status:** Documented kernel limitation; novelty, workload impact, and
implementation feasibility remain unverified.

### Problem

Linux cgroup writeback attributes an inode's writeback I/O to one owning
cgroup. The kernel documentation explicitly warns that simultaneous writes
to one inode from multiple cgroups can cause significant misattribution.
First-use page ownership also fails to capture responsibility when writers
modify overlapping regions.

### Proposed eBPF contribution

Explore a programmable mechanism that carries responsibility from application
writes through deferred writeback and enforces resource budgets against the
responsible tenants. This is an attribution-and-enforcement problem, not
simply another disk-priority policy.

The ownership model must define how to account for overwritten data, shared
pages, and merged I/O. Tracking must have bounded overhead, and enforcement
must preserve writeback progress and filesystem correctness. New kernel
hooks may be necessary; tracing alone cannot provide enforcement.

### Research gates

- Identify a real application with shared-file writers and measure substantial
  unfairness or application latency under existing cgroup controls.
- Check prior work on shared-resource accounting and writeback isolation;
  determine whether separating files or using existing controls solves it.
- Establish a trustworthy attribution model and safe enforcement points
  before designing an eBPF interface.
- Compare application outcomes, accounting accuracy, and overhead against
  existing isolation mechanisms at the same resource budget.

**Source:** [Linux cgroup v2 writeback documentation](https://docs.kernel.org/admin-guide/cgroup-v2.html#writeback).

## 2. Application-informed writeback for performance

**Status:** Substantial bottleneck measured in published database experiments;
the proposed eBPF optimization's novelty, benefit, and feasibility remain
unverified. No corresponding bottleneck has been established on our VM.

### Problem and evidence

Buffered writeback can fall behind on cloud storage, accumulating dirty pages
until Linux throttles foreground writers. Flushing earlier may prevent these
stalls, but can also write short-lived data that would otherwise be deleted
without reaching storage.

A September 2026 PostgreSQL 16.4 TPC-C study compared one-volume and four-volume
layouts at identical aggregate provisioned IOPS and bandwidth. The four-volume
layout delivered 23% higher throughput, 59% lower maximum New-Order transaction
latency, and 70% fewer dirty-throttling episodes. These are storage-layout
results, not predicted eBPF gains.

The 2026 kernel writeback discussion also reports a package-installation
performance regression after lowering dirty limits. Unnecessary flushing of
temporary files was suggested as an explanation, not established as the cause.

### Proposed eBPF contribution

Investigate whether application information can guide which eligible dirty
data to flush now, which to defer, and how much to submit per batch. The goal
is to reduce foreground stalls without increasing unnecessary writes.

eBPF must control writeback decisions, not merely report them. The kernel must
retain durability, filesystem-ordering, memory-safety, and forward-progress
guarantees. New writable kernel hooks may be necessary; ordinary tracing
hooks are insufficient.

This concerns writeback timing and work selection for performance, distinct
from the shared-file tenant attribution and enforcement problem in idea 1.

### Prior art and exclusions

- Parallel writeback already has Linux patches with multiple contexts,
  adjustable concurrency, and XFS allocation-group affinity. Adding flusher
  threads or adjusting their count alone is not the proposed contribution.
- Earlier writeback, automatic heuristics, and file-lifetime prediction have
  existing proposals or precedents; using eBPF does not establish novelty.
- cache_ext already supports programmable page-cache eviction. Any proposed
  writeback mechanism must establish a consequential gap beyond that work.

### Research gates

- Measure a substantial residual writeback bottleneck in a representative
  application after ordinary tuning and applicable existing optimizations.
- Identify application information that improves decisions beyond existing
  heuristics, and check the closest writeback and lifetime-prediction work.
- Establish safe control points and bounded policy overhead before designing
  an eBPF interface.
- Compare end-to-end throughput and latency against tuned defaults and
  applicable parallel-writeback implementations at the same resource budget.
  Include bytes written and memory pressure; do not trade away durability.

### Sources

- [Invisible Yet Dominant: Big Stalls of Kernel I/O Mechanisms in Cloud OLTP Databases](https://arxiv.org/html/2609.12597v1).
- [Parallelizing filesystem writeback: October 2025 v2 patch series](https://marc.info/?l=linux-fsdevel&m=176044369514739).
- [Initiating writeback earlier: 2026 kernel discussion](https://lwn.net/Articles/1078767/).
- [cache_ext: programmable page-cache eviction](https://github.com/cache-ext/cache_ext).

