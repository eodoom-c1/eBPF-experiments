# System Architecture and Design: Dynamic False-Sharing Mitigation

Status: revised candidate architecture, not an implemented or validated system. **CoLoc-SCX** is the provisional name for the Linux eBPF and `sched_ext` path.

Read this document alongside the [experimental methodology](experimental-methodology.md) and [requirements](requirements.md). The requirements remain authoritative: the first placement experiments may use known sharing groups and existing OS affinity controls. This proposal does not make `sched_ext`, arbitrary-binary diagnosis, or kernel-workload mitigation mandatory for the first iteration.

## 1. Objective and architectural boundaries

The controller selects placements that improve useful application work under declared latency, resource, and workload-isolation constraints. It may retain the existing placement, consolidate a group, spread a previously consolidated group, or abstain.

The central hypothesis is that some sharing-related coherence costs exceed the lost parallelism and interference introduced by consolidation. High HITM counts alone are not sufficient grounds for intervention.

Separate three responsibilities:

| Component | Responsibility |
| --- | --- |
| Observation | Collect supported contention, execution-demand, topology, and interference evidence with explicit attribution and quality. |
| Decision | Compare eligible placement candidates over a stated horizon, including transition costs, uncertainty, and effects on other managed work. |
| Actuation and evaluation | Apply a permitted placement, verify its effective execution, evaluate application outcomes, and retain or restore it. |

For the SCX candidate, observation programs and placement callbacks can share BPF maps. A userspace loader configures events, topology, enrollment, and policy parameters. Userspace can also perform slower aggregation and evaluation where justified.

Describe this as an **in-kernel placement fast path with userspace configuration**, not a system operating entirely inside the kernel. The policy's location is an experimental choice, not the research contribution by definition.

## 2. Platform feasibility contract

Pin one CPU model and exact kernel version before implementation. Linux v6.12 is a reference for the restrictions below; "6.12+" is not an interface-compatibility guarantee.

Before promising live support, verify:

- Kernel BPF/BTF/JIT/perf/SCX configuration, privileges, event access, and verifier acceptance.
- Precise-event support, usable sample addresses, event-specific data interpretation, sampling loss, and supported counter combinations.
- Actual logical-CPU, physical-core, SMT, cache-sharing, and NUMA topology.
- Effective task affinity, cpuset/cgroup constraints, scheduler enrollment, and competing scheduler classes.
- Whether a virtualized target exposes the required PMU facilities and trustworthy physical-topology relationships.

An unsupported event or placement candidate is an explicit capability limitation. Missing measurements must not be stored as zero pressure.

### 2.1 Confirmed v6.12 PMU-helper restriction

The original design proposed `bpf_perf_event_read_value()` directly inside `ops.running` and `ops.stopping`. That helper is not exposed by the v6.12 SCX helper resolver, which delegates ordinary helpers to `bpf_base_func_proto()`.

Do not assume the callback path is usable. Validate a supported observation program and local-counter accounting path, then publish its summaries to maps for the placement engine. A newer kernel or another supported facility may provide a different path, but it requires its own capability and verifier check.

### 2.2 Sample-record and BPF-context distinction

The v6.12 public `bpf_perf_event_data` context exposes registers, sample period, and address. Configuring `PERF_SAMPLE_DATA_SRC`, `PERF_SAMPLE_TID`, or physical-address fields in perf records does not automatically expose those fields through this BPF context.

Specify the actual acquisition path for each required field. Possible designs include an event-filtered BPF observer with limited fields or userspace decoding of perf records. Do not claim a data-source classifier or physical-line identity until its path has been demonstrated.

## 3. Evidence model and identity

### 3.1 What a coherence sample means

A supported sampled load can identify the requesting access and its reported location. It does not generally identify the previous modifying thread. Intel's `MEM_LOAD_L3_HIT_RETIRED.XSNP_HITM` is an example of a model-specific retired-load event, not a universal invalidation or remote-socket event.

Alternating sampled requesters at the same line are therefore candidate participants, not a proven producer-consumer pair. Load-only sampling can miss important write activity.

Use evidence levels such as:

| Evidence | Permitted interpretation |
| --- | --- |
| Known benchmark access pattern or source-verified independent fields | Established false-sharing relationship within the verified scope. |
| Shared-line observations with incomplete access information | Suspected contention; writer identity and false-versus-true sharing may be unknown. |
| Same-word communication | True-sharing control or a broader coherence-optimization candidate, not confirmed false sharing. |
| Missing, stale, or ambiguous attribution | Insufficient evidence for decisions requiring that attribution. |

If arbitrary-program false-sharing classification is not available, describe the broader policy as coherence-contention-aware placement. Reusing an existing detector is acceptable.

### 3.2 Address and task identity

A global key containing only a virtual cache-line address is invalid across processes. Equal virtual addresses can map unrelated memory, while a shared physical line can have different virtual addresses.

For the initial same-process scope, identify a line by an address-space lifetime identifier and normalized virtual line address. Derive line size from the supported target, rather than silently treating every architecture as 64-byte.

Cross-process support requires validated shared-backing or physical identity, with handling for mapping/page lifetime and reuse. Do not assume that adding a physical-address request flag solves the BPF access path or lifetime problem.

Identify a placement unit as a thread using a task lifetime, not a reusable numeric PID alone. Remove or invalidate observations and placements when tasks exit or their relevant identity changes.

## 4. Logical data structures

These are logical records, not final C layouts or promises that every BPF map type is usable in every callback. Select concrete map types after validating access, concurrency, memory bounds, and the chosen aggregation context.

| Record | Candidate representation | Required content and invariant |
| --- | --- | --- |
| `cacheline_observations` | Bounded hash/LRU storage or per-CPU observations merged outside the sampling handler | Scoped line identity, participant/access evidence, timestamps, explicit observation-window state, weighted sample information, and quality. Preserve uncertainty about writers. |
| `task_metrics` | Task storage or lifetime-keyed bounded map, where supported | Windowed execution-demand and PMU summaries with scope, timestamps, validity, enabled/running times, and attribution quality. Process RSS is not a per-thread working set. |
| `cpu_topology` | CPU-indexed arrays and sibling/domain sets | Physical-core identity, all eligible SMT siblings, cache domains, NUMA node, online state, and generation. Do not assume exactly two threads per core. |
| `counter_snapshots` | Per-CPU storage for a verified observer path | Event identifiers, local counter snapshots, enabled/running times, attribution interval, and error/loss information. |
| `pair_registry` | Lifetime-keyed lookup referencing a consistent placement decision | Partner identity, selected core/eligible CPUs, decision generation, evaluation horizon, originating evidence, and validity. Readers must reject incomplete or inconsistent entries. |
| `decision_records` | Bounded records and/or event output | Inputs, uncertainty, candidate comparison, rejection reason, effective actuation, application outcome, and restoration result. |

Bound storage and document eviction consequences. Evicted evidence is unavailable evidence, not proof that contention ended.

Define how concurrent samples, aggregation, task exits, and scheduler readers interact. PMU handlers may execute in NMI context; do not assume arbitrary locking, allocation, map operations, or timer work is safe there. Publish complete decisions through a consistency mechanism verified for the selected program contexts.

The first policy may support only pairs. Larger sharing groups must be reported as unsupported or handled by an explicitly evaluated policy, not silently reduced to whichever participant was sampled last.

## 5. Observation and aggregation pipeline

### 5.1 Setup

The loader discovers capabilities and topology, selects the managed tasks and allowed CPUs, and opens supported events with explicit privilege, user/kernel scope, precision, and sampling settings.

The exact event list depends on the processor. Intel PEBS and AMD IBS are separate backends with different capabilities; do not treat their event names or sample formats as interchangeable.

### 5.2 Sampling fast path

Keep the sampling handler bounded. Capture only fields available through the validated context and update or emit observations through operations proven safe in that context.

Normalize addresses using the validated line size and identity scope. Associate observations with explicit windows and event sampling configuration. A cumulative count with only a last-seen timestamp cannot establish "64 samples within 10 ms".

Repeated cross-core observations can nominate a relationship for evaluation. They must not directly establish false sharing or authorize movement before eligibility, evidence quality, and admission checks.

### 5.3 Execution-demand and auxiliary counters

Use a supported collection path for cycles, instructions, translation pressure, and cache pressure. If using per-CPU deltas, define how the attribution interval handles task switches, migration, interrupts, kernel execution, and counter scheduling.

Read compatible events over comparable intervals. Preserve enabled/running times, multiplexing and loss indicators, and unavailable states. Do not subtract snapshots taken on different CPUs as if they were one task counter.

IPC, utilization, page-walk activity, and cache misses are measured features. They are not direct estimates of execution-port occupancy, per-thread working set, exposed TLB stalls, or cache capacity.

### 5.4 Aggregation and model evaluation

Aggregate observations in a context demonstrated to support the required operations. A timer or scheduler callback is not assumed to provide every helper available to a tracing program.

The initial implementation should use known sharing relationships if automatic attribution would otherwise block placement evaluation. More general diagnosis can be added after the placement benefit is established.

## 6. Benefit estimation and admission

### 6.1 Candidate actions and constraints

Consider retaining placement, moving a pair to SMT siblings, time-sharing one logical CPU, and spreading a consolidated pair. Evaluate moving managed unrelated work only when its eligibility, destination capacity, and performance guardrails are included.

Reject candidates that violate authorization, CPU eligibility, topology, evidence-quality, or application constraints. Affinity is not exclusive core reservation.

### 6.2 Common-horizon comparison

Choose a decision horizon `H` and consistent units. An illustrative total-cycle accounting model is:

```text
estimated recoverable coherence cost over H
    > estimated one-time transition cost
      + estimated parallelism/execution interference over H
      + estimated ongoing translation interference over H
      + estimated ongoing cache interference over H
      + uncertainty margin
```

This is a bookkeeping framework, not a validated additive counter model. Overlapping stalls and correlated features must not be double counted. Estimate terms jointly or calibrate their recoverable contributions, and validate predictions against useful work and latency.

The transition term includes migration and cold cache/TLB effects. Ongoing interference is distinct from initial warmup. Evaluate the outcome for all affected managed work, not only the task being moved.

### 6.3 Estimator limitations

- HITM sample rate times full access latency does not directly measure recoverable application cycles. Account for sampling and the difference between current and candidate access paths.
- Use machine-specific measurements; do not hardcode a universal 40 ns or 120 ns transfer cost.
- Threads sharing one process normally share `mm` and process RSS. "Always migrate the smaller-RSS thread" does not distinguish their active footprints.
- `mm->rss_stat` does not provide the proposed per-thread working set or THP coverage ratio.
- A 2 MB mapping covers 512 base pages of 4 KB, but this is not a 512-fold reduction in TLB refill cost. Verify actual backing and calibrate translation effects.
- Moving a thread does not automatically move its memory pages; account for resulting NUMA access costs.

Use verifier-safe bounded integer/fixed-point arithmetic if evaluating the model in BPF. Specify units, scaling, valid ranges, overflow handling, and invalid-data behavior. Integer arithmetic does not make an uncalibrated model accurate.

Select the simplest estimator supported by the initial measurements. A rule-based admission test is acceptable; no machine-learning model or four-dimensional implementation is required before simpler controls have been evaluated.

## 7. SCX placement path

### 7.1 Enrollment and base scheduling

Choose full-switch or partial enrollment explicitly. In v6.12, full-switch mode makes the SCX scheduler responsible for the ordinary task population; it cannot claim that unrelated tasks automatically retain fair-class load balancing. In partial mode, non-enrolled fair-class tasks remain outside SCX control.

Provide a documented base scheduling policy and a non-coherence configuration for fair comparison. Account for competing scheduler classes and preserve the distinction between controller-managed and unmanaged tasks.

### 7.2 CPU selection and dispatch

In `ops.select_cpu`, check a complete, current placement decision, task eligibility, sibling availability, and topology generation. Revalidate the partner's relevant state rather than relying indefinitely on a previously sampled CPU.

Returning a CPU is a hint, not a guarantee of placement or immediate simultaneous execution. A partner can stop or migrate before the selected task runs.

Use dispatch operations according to the pinned kernel's restrictions. In v6.12, direct dispatch from `select_cpu`/`enqueue` cannot use `SCX_DSQ_LOCAL_ON` to target an arbitrary other CPU's local queue; CPU selection must establish the appropriate enqueueing CPU.

Queue insertion is not an execution guarantee. Measure effective CPU placement, overlap on SMT siblings, waiting, and interference. Define behavior for busy destinations and failed placement; do not claim a lower-priority occupant is automatically preemptible.

Continuously runnable tasks may not revisit `select_cpu` at the desired time. Define a supported reevaluation/actuation path for them rather than assuming all migrations happen on wakeup.

## 8. Intervention lifecycle and stability

Use a small explicit control sequence:

1. **Observe:** collect sufficient evidence and an application baseline; remain advisory unless live actuation is enabled.
2. **Trial:** apply an eligible candidate for a bounded evaluation interval and verify effective execution.
3. **Retain or restore:** compare useful-work and latency outcomes with declared acceptance and regression criteria.
4. **Reassess:** respond to phase, demand, topology, or eligibility changes; use controlled release/probing when needed.

Successful consolidation may suppress cross-core HITM. Therefore, disappearance of the triggering samples is not, by itself, a reason to expire the placement. Preserve the reason for intervention and distinguish evidence aging from evidence that the intervention is no longer beneficial.

A lease bounds how long a decision can remain unreviewed; it does not prove that release is beneficial. If evidence becomes insufficient for continued actuation, record a deliberate release reason rather than labeling it recovered performance.

Use measured dwell times, hysteresis, and migration budgets to limit churn. Neither a fixed cooldown nor a particular lease duration guarantees amortization. Specify interactions between trial duration, reassessment, and cooldown, including urgent restoration for a guardrail violation.

Restoration covers controller-owned configuration only and must respect newer external restrictions. It cannot reverse latency already incurred. Report failed and partial actuation/restoration explicitly.

## 9. Scope and failure handling

The first live experiment should use controlled user-space work on a verified target. Kernel networking is a later candidate: kernel memory does not imply that every accessor is a freely migratable task. IRQ/softirq paths, bound workqueues, per-CPU constraints, and task eligibility require separate analysis.

Keep errors distinguishable:

| Condition | Required response |
| --- | --- |
| Missing event, unsupported helper, or verifier rejection | Report the capability limitation; do not claim the observation path is available. |
| Lost, stale, or ambiguously attributed data | Mark evidence quality and abstain from decisions requiring it. |
| Task exit, changed CPU eligibility, or topology change | Invalidate affected decisions and revalidate before further actuation. |
| Inconsistent pairing record or failed CPU selection | Use the documented base policy and record why the requested intervention was not applied. |
| Application regression | Restore or revise controller-owned placement according to declared guardrails; report the outcome. |
| SCX abort/fallback | Record loss of controller operation. Scheduler fallback supports liveness, not application latency or throughput guarantees. |

No policy promises universally beneficial SMT pairing, zero coherence traffic, immediate co-execution, a fixed overhead bound, or optimal placement.

## 10. Implementation and evidence gates

The [experimental methodology](experimental-methodology.md) defines comparisons and metrics. Before advancing this candidate architecture:

1. Demonstrate that static placements can help at least one known-sharing workload and hurt an appropriate negative control.
2. Validate every required observation field and counter path on the pinned CPU/kernel, including helper and NMI-context restrictions.
3. Validate attribution, task/map lifetimes, effective placement, and intervention failure reporting.
4. Evaluate the simplest adaptive policy with observation-only, base-scheduler, utilization-only, and static controls.
5. Add extra model dimensions or in-kernel integration only when their measured decision value justifies their cost.

The actual contribution must be established through comparison with prior placement/control work. Existing eBPF, perf, detection tools, and SCX facilities are reusable infrastructure, not a novelty problem.

## 11. Sources and implementation references

- [Linux v6.12 sched_ext documentation](https://github.com/torvalds/linux/blob/v6.12/Documentation/scheduler/sched-ext.rst).
- [Linux v6.12 SCX helper resolver](https://github.com/torvalds/linux/blob/v6.12/kernel/sched/ext.c#L5412-L5423) and [base helper resolver](https://github.com/torvalds/linux/blob/v6.12/kernel/bpf/helpers.c#L1891-L2036).
- [Linux v6.12 direct-dispatch restrictions](https://github.com/torvalds/linux/blob/v6.12/kernel/sched/ext.c#L5993-L6020).
- [Linux v6.12 BPF perf-event context](https://github.com/torvalds/linux/blob/v6.12/include/uapi/linux/bpf_perf_event.h).
- [perf_event_open manual](https://man7.org/linux/man-pages/man2/perf_event_open.2.html) and [perf c2c manual](https://man7.org/linux/man-pages/man1/perf-c2c.1.html).
- [Intel Skylake event definitions](https://github.com/intel/perfmon/blob/main/SKL/events/skylake_core.json); pin the event-database revision used in an implementation.
- [Linux v6.12 THP documentation](https://github.com/torvalds/linux/blob/v6.12/Documentation/admin-guide/mm/transhuge.rst) and [RSS counter categories](https://github.com/torvalds/linux/blob/v6.12/include/linux/mm_types_task.h).
- [Linux v6.12 workqueue documentation](https://github.com/torvalds/linux/blob/v6.12/Documentation/core-api/workqueue.rst).
