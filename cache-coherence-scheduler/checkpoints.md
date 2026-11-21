# Cache-Coherence Scheduler: Project Checkpoints

Latest checkpoint: **CP-005, 2026-11-20**.

**Current position:** the user has supplied basic and rate-controlled WSL measurements and selected a physical Linux device for subsequent runs. Optional PMU collection and portable machine context are now implemented in source but not compiled or executed by the assistant. Native counter availability, measurement quality, and repeatable placement effects remain unverified. No dynamic scheduler is implemented.

See the [decision ledger](decisions.md) for why this direction was chosen. The [requirements](requirements.md) remain authoritative.

## CP-001: Static-placement milestone selected

### Completed and verified

| Artifact or state | Evidence / qualification |
| --- | --- |
| Requirements and non-requirements drafted | [requirements.md](requirements.md), including CPU utilization/runnable demand, outcome-based evaluation, and the rule against releasing placement solely because HITM disappears. |
| Evaluation plan drafted | [experimental-methodology.md](experimental-methodology.md), with matched baselines, controls, metrics, and evidence gates. It contains no measured results. |
| Candidate Linux/SCX design drafted | [system-architecture.md](system-architecture.md), including PMU attribution limits, kernel-helper feasibility, and intervention feedback. It is not implemented. |
| Capability flow documented | [capability-flow.md](capability-flow.md). Numbered section labels were removed after the preview reported unsupported Markdown lists; rendering has not been independently verified locally. |
| Development scaffolding present | Root CMake configuration/presets and repository setup files exist. Their presence does not establish a working measurement environment. |
| First milestone direction recorded | Start with static placement to isolate placement benefit before prediction and dynamic control; see D-01 in [decisions.md](decisions.md). |

At this checkpoint, the local branch is `cache-line-analysis`. The research documents and setup changes remain uncommitted; no changes were staged or pushed during this documentation work. This is a snapshot, not an instruction to commit them.

### First experiment outline

Use four controlled threads on two physical cores, with known false sharing between threads 2 and 3. This is an illustrative starting comparison, not an implemented benchmark or a measured finding.

| Configuration | Physical core A | Physical core B |
| --- | --- | --- |
| Baseline | Threads 1 and 2 | Threads 3 and 4 |
| Co-located | Threads 2 and 3 | Threads 1 and 4 |

Keep worker logic, inputs, useful work, and CPU budget matched. Specify the logical-CPU assignment and SMT mode rather than assuming that two threads on a core means simultaneous execution.

The preferred candidate is per-thread affinity or a small startup mechanism, established before the timed work. Verify actual placement. Do not modify application logic merely to encode a placement if an existing control is sufficient.

Measure useful throughput, latency where meaningful, utilization/runnable demand, and supported coherence events per useful operation. Include repeated trials and an appropriate negative control; add a selectively padded control where practical. Lower HITM or low average utilization alone is not success.

### Next milestone: reproducible static comparison

| Step | Evidence needed before calling it complete |
| --- | --- |
| Choose and validate a target | Recorded OS/CPU/topology, effective CPU eligibility, working placement controls, and explicit PMU availability/limitations. Known sharing permits initial application-level comparisons without a new detector. |
| Select a controlled workload | Exact source/version and inputs, identified sharing/access pattern, useful-work metric, and defined baseline. |
| Apply the two static placements | Effective per-thread placement verified before and during measured work; SMT and time-sharing modes distinguished. |
| Run matched controls and repeats | Reproduction commands, run settings, raw outcomes, variability, and any missing evidence or failures recorded. |
| Interpret the result | Explain useful-work/latency changes and capacity/coherence tradeoffs, including regressions and null results. |

Completion means a repeatable, fairly controlled comparison, not a guaranteed speedup. If no useful benefit is found, record that and revisit the workload or hypothesis before investing in dynamic scheduling.

### Not started or not established

- No execution host, first workload, affinity mechanism, or numeric acceptance criteria have been selected for this experiment.
- No static-placement implementation, benchmark runs, or measured overhead/speedup results exist.
- No automatic false-sharing detector, adaptive policy, SCX program, or dynamic backend is implemented.
- No evidence establishes publication novelty, a universal threshold, or a universally beneficial placement.

## CP-002: Linux static benchmark source written

### Changes

| Artifact or choice | Current state |
| --- | --- |
| First backend/workload | Linux and the controlled four-thread/two-core benchmark were selected explicitly; see D-02 in [decisions.md](decisions.md). |
| Benchmark | [static-placement.c](static-placement.c) implements fixed baseline/co-located placements, shared/padded layouts, affinity/topology checks, compute/idle controls, counter validation, and JSON run metrics. |
| Build integration | [CMakeLists.txt](CMakeLists.txt) defines `static-placement` and links pthreads through CMake's Threads package. The repository root includes this subdirectory. |
| Focused argument checks | `tests` contains a CTest script for malformed/overflowing CPU lists, zero work, and invalid placement. These checks are written, not executed. |
| Windows-host configuration issue | A reported fatal Linux-only error was replaced with an explicit target-skipped status and early return. The revised configuration has not been rerun by the assistant. |
| Experiment instructions | [experimental-methodology.md](experimental-methodology.md), section 8, describes building on Linux, selecting CPUs, running the four conditions, and interpreting metrics. |
| Development decisions | D-03 through D-06 in [decisions.md](decisions.md) record the actual implementation choices and why. |

### Verification and limitations

Source review and non-build document/whitespace checks are the only assistant validation at this checkpoint. No configure, build, test, or benchmark commands were run by the assistant; compilation and runtime behavior are not established.

The benchmark does not collect PMU events, make a placement recommendation, prove false sharing in arbitrary programs, reserve CPUs, or implement an adaptive scheduler. Endpoint affinity checks do not continuously monitor external placement changes. No speedup, overhead bound, or novelty result is claimed.

### Next evidence gate

Select an accessible Linux host and verify eligible physical cores, SMT topology, sysfs access, and available profiling tools. Build on that target, perform the functional checks in methodology section 8, then run repeated matched baseline/co-located and shared/padded comparisons.

Numeric performance guardrails, the actual host, and measured results remain open. Record failures and null results as well as improvements before deciding whether to expand the workload or add a placement decision model.

## CP-003: Portable results-file output added

Date: 2026-11-19.

**Changed:** `--output PATH` appends one existing-format JSON record per completed run to a text file instead of stdout. File operations occur after measurement and validation, with explicit open/write/close error handling. D-07 records the choice; methodology section 8 includes collection and transfer instructions.

**Verification:** Source changes were inspected. Focused CTest cases were added for an empty output path and preserving an existing file when arguments are rejected. No configuration, build, test, or benchmark was run by the assistant.

**Next evidence gate:** On the Linux target, build and run the argument checks, then verify successful repeated appends and file-error handling alongside the existing placement/layout functional checks. Hardware performance validation remains outstanding.

## CP-004: Initial WSL records reviewed and per-worker rate control added

Date: 2026-11-19.

**Evidence reviewed:** The user supplied `results.txt` containing four matched 5-million-increment-per-worker WSL runs. Records parsed successfully and recorded CPU endpoints matched assignments. Shared co-location throughput increased 34.95%, but padded throughput decreased 38.97%; single short trials and hidden physical placement prevent a reliable performance conclusion. Baseline/shared thread CPU times exceeded their completion wall times, leaving CPU-utilization interpretation unresolved. These records are not evidence that CTest passed.

**Changed:** Optional `--rate N` paces each worker in batches while retaining complete fixed-work execution. Requested rate, nominal duration, and achieved per-worker rates are reported. D-08 records the confirmed 7.5-billion-increment-per-worker budget at a target of 500 million/s per worker: 30 billion total, nominally 15 seconds.

**Verification:** Source changes were inspected and argument-rejection cases added for invalid rates and timing-range overflow. No configuration, build, test, or benchmark was run by the assistant. Rate-control runtime behavior remains unverified.

**Next evidence gate:** On Linux, validate low-rate pacing and behind-schedule full completion, then collect longer repeated matched runs. Investigate the WSL clock discrepancy before relying on CPU-utilization values; establish physical placement before making hardware-level claims.

## CP-005: Physical-Linux measurement context and optional PMU counters added

Date: 2026-11-20.

**Host context:** The user reports four online/allowed CPUs, one thread per core, no detected virtualization, and a 700-MHz to 2.6-GHz frequency range on the physical Linux target. Time-sharing slots such as `0,0,1,1` were selected; native measurements from this host have not yet been supplied.

**Changed:** `--pmu` requests paired per-thread user-space counters with raw/scaled counts, coverage, explicit unavailability, and derived IPC/cache-hit estimates. Context-switch/page-fault deltas and kernel/build/invocation/CPU/topology/frequency context are included in version-2 JSON records. D-09/D-10 record the host and measurement decisions; methodology section 8 describes collection and interpretation.

**Verification:** Implementation and perf API semantics were reviewed; focused argument-rejection checks were added. No configure, build, CTest, or benchmark was run by the assistant. Hardware-event support, permission handling, multiplexing behavior, and runtime JSON output still require native Linux validation.

**Next evidence gate:** Build on the physical target, validate PMU availability or explicit failures and cross-check counts, then collect repeated matched placement/layout runs with identical instrumentation. Preserve source revision and ambient conditions alongside the self-contained records.

## Updating checkpoints

Append a short dated checkpoint at a meaningful milestone or handoff, retaining earlier entries. Update the latest-checkpoint line and current-position paragraph.

Each checkpoint should state what changed, what was actually verified and where the evidence lives, remaining blockers/open choices, and the next concrete evidence gate. Distinguish files written, code implemented, checks performed, and measured results. Link decisions rather than repeating their rationale.
