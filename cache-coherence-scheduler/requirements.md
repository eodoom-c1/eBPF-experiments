# Cache-Coherence Scheduler: Requirements

Status: draft specification. This document defines the first subproject's intended behavior and boundaries; it does not claim that the scheduler is implemented or novel.

## 1. Objective

Build a workload-aware placement controller that uses cache-coherence evidence to decide whether to colocate affected threads on one physical core and rebalance unrelated work onto other cores.

The objective is improved useful throughput and application latency under stated constraints, not maximum CPU utilization, maximum IPC, or minimum coherence traffic in isolation. A valid decision may be to retain the current placement or undo a previous change.

The first research focus is harmful false sharing. General performance diagnosis supplies evidence to this scheduler; it is not the entirety of this subproject.

## 2. Scope and terminology

- **Scheduler:** the placement decision and control system described here. The name does not require replacing the operating system's scheduler.
- **Placement unit:** a thread. Related threads may belong to one process or to different processes sharing memory.
- **False sharing:** independent data shares a cache line, and accesses from different cores include at least one writer, causing unnecessary coherence activity. Shared-line activity alone is not proof of false sharing.
- **Physical core and logical CPU:** distinct topology entities. Time-sharing one logical CPU and using SMT siblings on one physical core are different placement choices.
- **Managed workload:** a workload explicitly enrolled and authorized for observation and placement changes. Unrelated work may be relocated only if it is also managed.
- **Decision interval:** the observation and evaluation period for a placement decision. Its duration must be specified for each experiment rather than assumed to be universally appropriate.

Source, tests, benchmarks, and subproject-specific documentation belong under `cache-coherence-scheduler`. Shared repository configuration may remain at the repository root.

This specification defines the required behavior and boundaries. The [experimental methodology](experimental-methodology.md) and [system architecture](system-architecture.md) provide a supporting evaluation plan and candidate design; they do not expand these requirements. Linux/`sched_ext` and the full multidimensional cost model remain candidate implementation choices, subject to explicit selection and capability validation.

## 3. Functional requirements

| ID | Requirement |
| --- | --- |
| FR-01 | Accept observations identifying participating threads, cache lines or shared-memory locations, access patterns where available, observation times, and evidence quality. Preserve the distinction between suspected and established false sharing; do not infer it from LLC misses or shared-line counts alone. |
| FR-02 | Obtain the target machine's logical-CPU, physical-core, SMT, and relevant cache/NUMA topology, together with the CPUs each managed thread is permitted to use. Reject placement candidates that violate those restrictions. |
| FR-03 | Use CPU utilization, runnable demand, and IPC where supported alongside coherence evidence. Record the measurement scope, interval, and availability so that per-thread, logical-CPU, and whole-core values are not treated as interchangeable. |
| FR-04 | Consider retaining placement, consolidating an affected group on one physical core, and spreading a previously consolidated group across cores. When supported, evaluate single-logical-CPU placement separately from placement across SMT siblings. |
| FR-05 | Jointly consider relocation of managed unrelated work to other eligible cores. Account for destination capacity and interference; do not assume that filling other cores compensates for overloading the consolidated core. |
| FR-06 | Assess expected benefit against lost parallelism, runnable waiting, intra-core competition, migration, cache warmup, and collection/decision overhead. Do not prescribe consolidation solely because coherence traffic is high. |
| FR-07 | Produce a decision record containing the observation window, relevant evidence and uncertainty, affected threads, proposed CPU eligibility, expected tradeoff, objective, constraints, and reason for acting or abstaining. A rejected or unsupported action must have an explicit reason. |
| FR-08 | Provide an observation/advisory mode that makes no placement changes. Live actuation must be explicitly enabled for a defined set of managed workloads and permitted CPUs. |
| FR-09 | Use supported OS controls for actuation, revalidate target identity and eligibility before applying a change, and read back effective placement. Report failures and partial application; do not present a proposed or partially applied placement as successful. |
| FR-10 | Evaluate each applied intervention against its recorded baseline and application-level success/regression criteria. Retain, revise, or restore placement according to those criteria. Record restoration failures; configuration rollback cannot reverse latency already incurred. |
| FR-11 | Restore only controller-owned changes without silently overwriting newer external placement restrictions. Do not modify unmanaged workloads, security settings, or unrelated system configuration. Affinity must not be described as exclusive CPU reservation. |
| FR-12 | Reassess decisions when workload phases, sharing groups, or resource demand change. Reduced or absent HITM after consolidation is not, by itself, grounds for undoing the placement: successful mitigation can suppress its triggering signal. Retain the intervention's rationale and reassess using application outcomes and the documented stability rules. Limit placement churn so that repeated migrations do not consume the expected benefit. Document and test the chosen reassessment and stability rules. |
| FR-13 | Surface missing or unsupported hardware events, stale observations, event loss, attribution limitations, and counter multiplexing where applicable. Abstain from decisions that require unavailable evidence rather than treating missing values as zero. |

Confirmed false-sharing groups supplied by controlled benchmarks are valid initial inputs. FR-01 does not require inventing a complete automatic detector for arbitrary binaries before the placement policy can be evaluated.

## 4. Measurement and evaluation requirements

| ID | Requirement |
| --- | --- |
| ER-01 | Define the experiment's primary application objective and guardrails before a run: useful throughput, a selected latency statistic, acceptable regressions for affected and unrelated workloads, and any resource constraints. Numeric targets remain experiment-specific. |
| ER-02 | Report completed useful work and application latency alongside coherence events, supported access-latency measurements, CPU utilization, IPC, runnable waiting, placement transitions, and controller overhead. Identify unavailable metrics explicitly. |
| ER-03 | Normalize relevant coherence measurements by useful operations or completed requests where meaningful. Fewer transfers caused by doing less work must not count as an improvement. |
| ER-04 | Establish machine-specific reference conditions for unshared, shared-read, and modified-on-another-core accesses where measurable. Record the serving cache/access path; consolidation may change that path rather than merely reduce LLC-hit latency. |
| ER-05 | Compare against default OS scheduling, utilization-only load balancing, tuned separate-core placement, and static same-core placement. Use comparable workload and resource budgets. |
| ER-06 | Compare compact, selectively padded, and aggressively padded layouts where source is available. Vary entity count and working-set size, reporting memory footprint and supported cache, bandwidth, and TLB measurements to expose padding's density costs. |
| ER-07 | Include positive false-sharing cases, true-sharing controls, cases without harmful sharing, capacity-limited cases where consolidation should be rejected, competing managed work, and changing workload phases. Include incomplete-evidence cases and restoration/failure paths. |
| ER-08 | Separate collection overhead from policy benefit, and ablate utilization and IPC inputs to determine whether they improve decisions. Use repeated runs and report variability, warmup, load generation, topology, SMT configuration, CPU frequency settings, and relevant OS/tool versions. |

### Historical motivation, not a hardcoded target

The supplied excerpt from Brendan Gregg's *Systems Performance*, citing [Levinthal 09], gives approximate historical LLC-hit costs of 40 cycles for an unshared line, 65 cycles for a line shared in another core, and 75 cycles for a line modified in another core.

These are not measurements from our machines. They motivate measuring sharing-related penalties, not a universal 40-cycle acceptance threshold. Event counts multiplied by those historical differences do not establish elapsed time saved.

### Why padding is a comparator rather than an assumed solution

Padding can isolate independent data, but applying it broadly increases footprint and lowers useful data density. Depending on access patterns, that can weaken spatial locality and increase cache-capacity, bandwidth, and TLB costs. Placement must be evaluated against sensible selective padding, not only an artificially poor unpadded baseline. The controller need not change data layout itself.

## 5. Development and implementation requirements

| ID | Requirement |
| --- | --- |
| DR-01 | Keep observation, placement decisions, and OS actuation separately testable. Policy tests must be able to exercise recorded or synthetic observations without changing live thread placement. This does not require a plugin framework or multiple live backends. |
| DR-02 | Reuse existing facilities where practical: `perf`/`perf c2c` for supported hardware evidence, bpftrace and BCC for investigation and cross-checks, and native OS placement controls. Using these tools is not a competing contribution or a novelty problem. |
| DR-03 | Select custom libbpf/C collection only where the required integration or overhead justifies it. Treat `pmc-cloud-tools` as a candidate pending verification of its compatibility, event coverage, attribution, and overhead. |
| DR-04 | Keep development accessible from Visual Studio. The existing Linux CMake setup can target a remote Azure Linux VM; WSL is not mandatory. Add subproject build targets when implementation begins, not as empty placeholders in this specification step. |
| DR-05 | Verify capabilities on the selected execution platform before promising a live feature. Azure PMU exposure is not assumed. Native Windows experiments require Windows-specific collection and actuation; eBPF for Windows does not establish Linux-hook compatibility. |

## 6. Non-requirements for the first iteration

| ID | Explicitly out of scope or not required |
| --- | --- |
| NR-01 | Replacing the OS scheduler, writing a kernel scheduler, or requiring `sched_ext` before existing placement controls have been evaluated. |
| NR-02 | A new eBPF runtime, general-purpose profiler, PMU collector, or reimplementation of bpftrace, BCC, `perf`, or `perf c2c`. |
| NR-03 | Complete automatic false-sharing diagnosis for arbitrary unmodified programs, or perfect recovery of all user-space dependencies from sampled hardware events. |
| NR-04 | Automatically padding structures, rewriting application code, changing critical-section granularity, or switching locking protocols. |
| NR-05 | Solving all true sharing, lock contention, memory-bandwidth pressure, or other performance problems through consolidation. Such evidence may instead justify abstention. |
| NR-06 | Cluster-wide placement, cross-node process migration, capacity throttling, admission control, or power/energy optimization. These remain related project directions, not first-iteration scheduler deliverables. |
| NR-07 | Simultaneous Linux and Windows implementations. The first live backend must be selected explicitly; a second backend is not necessary to validate the first. |
| NR-08 | Production deployment, live trading integration, hard real-time guarantees, or a claim that rollback prevents all performance harm. |
| NR-09 | Guaranteed improvement for every workload, universal thresholds for IPC/utilization/coherence costs, or a fixed microsecond decision-loop target without measurements. |
| NR-10 | An LLM, machine-learning model, or particular optimization algorithm. Choose the simplest decision method justified by the requirements and experiments. |
| NR-11 | A claim of publication novelty merely from combining tools, collecting more counters, or adding feedback. Novelty requires a comparison of the actual decision method with prior work. |

## 7. Acceptance scenarios

These scenarios define what must be demonstrated; they do not assert that the proposed policy will succeed.

| Scenario | Required observable outcome | Requirements covered |
| --- | --- | --- |
| Coherence-bound group with spare execution capacity | Compare same-core and separate-core candidates; record coherence and useful-work changes. Keep consolidation only if the declared success criteria are met. | FR-01-FR-07, ER-01-ER-05 |
| Group requiring more capacity than one core supplies | Reject or reverse harmful consolidation rather than crediting lower transfer counts as success. | FR-03-FR-06, FR-10, ER-03, ER-07 |
| Managed unrelated work on a candidate core | Evaluate a joint relocation within allowed CPUs and report both workloads' outcomes. An unmanaged workload is never relocated. | FR-02, FR-05, FR-08-FR-11 |
| SMT enabled versus disabled | Identify actual topology and distinguish time-sharing from SMT placement; report unsupported candidates explicitly. | FR-02-FR-04, FR-13 |
| True sharing, no harmful sharing, or insufficient evidence | Do not label contention as confirmed false sharing or act on unavailable evidence. Record the diagnosis limit or abstention. | FR-01, FR-07, FR-13, ER-07 |
| Sharing pattern or workload demand changes | Reevaluate the previous placement and demonstrate the documented stability rules and regression response. | FR-10-FR-12, ER-07 |
| Successful consolidation suppresses HITM | Do not undo placement solely because the triggering samples cease. Reassess using the retained intervention rationale, application outcomes, and documented stability rules. | FR-10, FR-12 |
| Thread exits, an affinity change fails, or external restrictions change | Avoid acting on a reused identity or invalid CPU set. Report partial failure and restoration outcome without silently overriding external restrictions. | FR-02, FR-09-FR-11 |
| Padding changes the coherence/density tradeoff | Compare layouts at multiple working-set sizes and report useful-work and memory-system effects, not only coherence counters. | ER-02, ER-06, ER-08 |

Policy correctness and measured performance benefit are separate results. A controller can satisfy its control requirements while the research hypothesis fails. A positive performance claim additionally requires repeatable application-level gains over the relevant baselines without violating declared guardrails.

## 8. Decisions to resolve before live implementation

1. Select the first live backend: Linux on an available target, or native Windows on the physical test machine. Verify the required observation capabilities before committing to either path.
2. Select the first controlled workload, how its sharing groups and useful progress are identified, and which unrelated workload participates in the load-balancing experiment.
3. Define the first experiment's latency/throughput objective, regression limits, measurement budget, and decision/evaluation intervals.
4. Choose the initial evidence source and simplest benefit-estimation rule, including behavior when IPC or coherence events are unavailable.

No particular algorithm, unsupported platform capability, or numeric performance improvement is implied until these decisions are made and tested.
