# Cache-Coherence Scheduler: Project Checkpoints

Latest checkpoint: **CP-011, 2026-11-22**.

**Current position:** [Results](results.md) covers 135 reviewed physical-Linux records: 72 primary comparisons, 50 follow-up diagnostics, 12 uneven-demand records, and one functional smoke. Three uneven-demand rounds confirm 64.16% lower mean shared CPU cost without a visible independent-worker rate penalty; the 26.47% padded saving also reproduces and limits attribution. Bounded synthetic confirmation is complete, not a universal admission boundary or steady-state result. CTest results remain unsupplied; hardware/PMU noise diagnostics stay closed. Next is realistic workload validation; no dynamic scheduler is implemented.

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

## CP-006: Repeated physical-Linux placement benefit with native coherence counters

Date: 2026-11-20. JSON recording times are UTC on 2026-11-21.

### Implementation and experiment verified

The user rebuilt [static-placement.c](static-placement.c), passed an integrated native-counter smoke run, and supplied three rounds of the four placement/layout conditions. All 12 result records parsed as version-3 JSON, used matching work and instrumentation settings, and reported available counters and expected CPU endpoints. These user-run measurements are not evidence that the separate CTest argument checks passed; the assistant did not run builds or tests.

The explicit `--pmu-profile intel-n150` profile collects the verified retired-load L1 miss, completed ITLB walk, and L3-hit RFO/snoop-HITM selectors, including the offcore `config1` filter. Generic requests remain the default. The incompatible generic-access/native-miss L1 hit estimate is explicitly unavailable. See section 8 of the [experimental methodology](experimental-methodology.md) for collection semantics and commands.

**Target and placement:** Intel N150, Ubuntu/Linux `7.0.0-43-generic`, SMT inactive, physical cores 0 and 1 in package 0. Slots `0,0,1,1` mean time-sharing, not SMT. Baseline places T1/T2 on CPU 0 and T3/T4 on CPU 1; co-location places T2/T3 on CPU 0 and T1/T4 on CPU 1.

**Matched inputs:** 600,000,000 increments per worker, target 10,000,000 increments/s per worker, batch 50,000, compute 0, idle 0, and the native PMU profile. Each run completes 2.4 billion increments at approximately 40 million increments/s aggregate in 60 seconds. Shared layout puts T2/T3 at offsets 0/8 within one 64-byte line; padded layout uses offsets 0/64.

Rounds 1 and 2 used baseline/shared, colocated/shared, baseline/padded, colocated/padded. Round 3 reversed that order.

### Raw evidence and provenance

The original JSONL files remain external to the repository in `C:\Users\ericodoom\Downloads`; they were read, not modified or copied into the repository. Each contains four records.

| Round | File | SHA-256 |
| --- | --- | --- |
| 1 | `n150-placement.txt` | `e4235a175be65836517ce87b90f809e81cdac28eda7a5ce5562e2d3530220819` |
| 2 | `n150-placement2.txt` | `1200e914c2a8e4c22c8a1bca923376b02c8a4c2177ebf04ab7f0d482b40a396c` |
| 3 | `n150-placement3.txt` | `a959eefde7f37ee01880061afe4d121eaf8aeec61efce6e7186937957464247c` |

The records span `2026-11-21T05:58:06Z` through `2026-11-21T06:27:31Z`. The first record in each file reports build timestamp `Nov 20 2026 21:50:27`. The source checkout at ledger entry is `50ee6f1dea23ee5205cee4570a8670c16e59523f`; JSON does not contain a binary digest or Git revision, so the tested binary's correspondence to that commit is not independently established.

### Measured CPU outcome

CPU time is the sum of the four workers' reported CPU seconds: time spent scheduled on a CPU, including hardware stalls, but excluding sleeping or waiting to be scheduled. Reduction is `(baseline - colocated) / baseline`. It is not fewer CPUs, lower clock frequency, shorter wall time, whole-machine utilization, or measured energy savings.

| Round | Layout | Baseline CPU seconds | Colocated CPU seconds | CPU reduction |
| --- | --- | ---: | ---: | ---: |
| 1 | Shared | 54.876899 | 39.811416 | 27.45% |
| 1 | Padded | 39.724250 | 39.786664 | -0.16% |
| 2 | Shared | 54.542746 | 37.498328 | 31.25% |
| 2 | Padded | 39.848505 | 38.412882 | 3.60% |
| 3, reversed | Shared | 54.247505 | 38.267694 | 29.46% |
| 3, reversed | Padded | 39.874844 | 38.285522 | 3.99% |

Using the ratio of three-round mean CPU times, shared layout improves from **54.555717 to 38.525813 CPU seconds: 29.38% lower**, saving 16.029904 CPU seconds per run. Padded improves from **39.815866 to 38.828356 CPU seconds: 2.48% lower**. Shared co-location increases the independent pair's CPU cost in every round; all totals above already include that regression, not just the sharing-pair improvement.

### Coherence-path evidence

Observed raw `ocr.demand_rfo.l3_hit.snoop_hitm` counts summed over T2/T3:

| Round | Baseline/shared | Colocated/shared | Baseline/padded | Colocated/padded |
| --- | ---: | ---: | ---: | ---: |
| 1 | 103,457,632 | 35 | 3 | 5 |
| 2 | 89,948,563 | 14 | 10 | 6 |
| 3, reversed | 91,795,584 | 1 | 3 | 12 |

The large shared-layout CPU benefit and collapse of this counted ownership-response path repeat after reversing run order. Retired-load L1 misses also fall sharply, and sharing-pair IPC rises from approximately 0.14-0.15 to 0.55. This supports avoiding cross-core coherence costs as the mechanism, rather than a benefit inferred only from fewer events.

### Conclusion, limits, and next evidence gate

**Supported hypothesis:** on this device, core pair, controlled workload, and requested rate, time-sharing the falsely sharing pair on one core reduces total worker CPU cost at the same delivered throughput. The substantially smaller padded difference is an important control; it is not assumed to be exactly zero.

Frequencies were not fixed; all runs used `powersave`, and endpoint frequency observations are not runtime averages. Counters were multiplexed with approximately 37.5% coverage, so scaled counts are estimates and raw counts cover only running windows. The RFO/HITM filter covers a particular L3-hit response path, including exclusive-prefetch requests; it neither measures all coherence traffic nor identifies a cache-line address. Affinity is checked at endpoints, not continuously, and cores are not reserved.

There is no measured energy result, application tail-latency result, adaptive policy, broader workload/topology validation, or publication-novelty claim. The next evidence gate is to test sensitivity to frequency policy and requested rate, then additional core pairs or workloads before generalizing or building dynamic control.

## CP-007: Frequency-bounded and higher-demand results consolidated

Date: 2026-11-21.

**Recorded:** [results.md](results.md) consolidates 36 matched native-profile records across three experiment blocks, each with three repeats per condition and a reversed-order third round. It includes CPU-time definitions, inputs/topology/frequency conditions, aggregate and per-round effects, coherence/cache/TLB evidence, separate exploratory findings, and artifact fingerprints. CP-006 is retained as the earlier evidence snapshot.

**Measured outcome:** shared-layout CPU reductions are 29.38% under adaptive limits, 56.27% under 1400-MHz bounds at 10M/s per worker, and 54.12% at 20M/s per worker. Padded reductions are 2.48%, 0.18%, and 0.29%. At higher demand, baseline/shared averages 61.143M/s and 39.327 seconds, versus 79.999M/s and 30.000 seconds colocated for the same 2.4-billion-increment budget. RFO/HITM repeatedly falls to background levels.

**Verified and qualified:** the assistant re-parsed all main records, recomputed the tables, and checked artifact SHA-256 fingerprints; all requested counters and CPU endpoint checks were available/consistent. User-run follow-up clock checks average 1404/1405 MHz baseline and 1411/1418 MHz colocated. They establish comparable clocks for those checks, not continuous proof for all earlier runs. No build, benchmark, or CTest was run by the assistant.

**Next evidence gate:** continue with 40M/s per worker, unpaced execution, and compute sensitivity, then uneven demand and realistic work before dynamic control. Exact tested-binary/source provenance, broader topology/workload evidence, energy, tail latency, and novelty remain unestablished.

## CP-008: Static results consolidated and diagnostic branch closed

Date: 2026-11-22.

**Recorded:** [results.md](results.md) now covers 72 primary records across A-F and 50 separately qualified follow-up records. It includes the 40M/s-per-worker point, unpaced execution, compute=1, noisy compute=4 matrices, frequency/HWP checks, core swaps, and PMU ablation. Earlier checkpoints remain historical snapshots.

### Latest primary outcomes

Three repetitions per condition, with the third round reversed; 600M increments per worker and the same two-core budget. Reductions use ratios of mean worker CPU times; throughput gains use mean per-run throughput.

| Experiment | Shared CPU reduction | Shared throughput gain | Padded CPU reduction |
| --- | ---: | ---: | ---: |
| D: 40M/s per worker, compute=0 | 42.70% | 74.43% | 2.54% |
| E: unpaced, compute=0 | 43.26% | 76.42% | 2.33% |
| F: unpaced, compute=1 | 44.35% | 79.45% | 0.76% |

D delivers 79.649M/s baseline/shared versus 138.929M/s colocated/shared; all conditions miss the 160M/s target. Both shared placements occupy approximately two CPU equivalents, so lower CPU cost means earlier completion, not lower occupancy while running. The repeated RFO/HITM collapse and primary padded controls support the static coherence explanation.

### Diagnostic disposition and limits

The compute=4 matrices and subsequent diagnostics do not establish a clean compute/benefit boundary. Busy-clock reports vary despite recorded bounds; captured HWP requests reach the hardware, but perfectly constant frequency is not established. Initial worker/core asymmetry does not reproduce consistently, and a slow run remains with benchmark PMUs disabled. Neither a permanent worker/core defect nor a sole PMU cause has been demonstrated.

**Closed at the user's request:** preserve these observations and their uncertainty rather than extending hardware/PMU/placement-noise diagnostics. The proposed one-worker-per-core test was withdrawn; no results from it are included. Padded co-location remains a negative control, not a recommended false-sharing intervention.

**Verification and provenance:** the results update checked 122 record totals, primary and diagnostic aggregates, 23 listed artifact SHA-256 fingerprints, links, and whitespace. Raw artifacts remain external to Git; exact tested-binary/source provenance is still unestablished. The assistant performed data/document checks only, not Linux builds, benchmarks, or CTest. This checkpoint update changes documentation only.

**Next substantive gate:** uneven useful-work demand and a realistic workload tied to the scheduling objective. A dynamic detector/policy/backend, energy and application tail-latency results, broader generalization, and novelty remain unimplemented or unestablished. Resolving the compute=4 anomaly is not a prerequisite for moving on.

## CP-009: Per-worker demand controls implemented and smoke verified

Date: 2026-11-22.

**Changed:** added `--rates R1,R2,R3,R4` and `--computes C1,C2,C3,C4` in thread identity order, preserving existing uniform controls and defaults. Zero list rates mean unpaced; scalar/list forms for the same control are rejected. Workers use their own settings for compute and pacing. Schema 4 reports per-thread settings and nullable unequal run-level fields. Focused parsing/JSON and rejection tests, standalone Linux CMake configuration, and exact commands are documented in [experimental methodology](experimental-methodology.md).

**Verified:** the user supplied one successful colocated/shared smoke with 1,000 increments per worker, rates `0,1000,2000,4000`, and compute `0,1,2,3`. The assistant independently checked effective settings, expected endpoint CPUs `1,0,0,1`, schema fields, nominal deadlines, achieved-rate arithmetic, and all four modulo-`2^64` compute checksums. Paced workers finish just after their deadlines. See [the smoke results](results.md#uneven-demand-functional-smoke-not-pooled) for the exact measurements.

**Evidence and limits:** `C:\Users\ericodoom\Downloads\uneven-demand-smoke.jsonl` remains unchanged and external to Git; its SHA-256 is among the report's 24 fingerprints. The smoke's build timestamp is `Nov 22 2026 20:40:50`, distinct from the earlier 122 records. Exact binary/source provenance remains unestablished. This confirms the supplied functional case, not CTest success, an admission threshold, or uneven-demand placement benefit. No assistant-run builds, benchmarks, or CTest; this handoff updates documentation only.

**Next evidence gate:** matched baseline/co-located comparisons at identical uneven worker settings and CPU budget, examining total CPU cost and each worker's rate, including independent-worker regressions. Equal fixed work with unequal rates produces unequal durations, so do not treat these runs as common-duration steady-state admission or application tail latency. Realistic workload validation remains ahead; the compute=4 diagnostic branch stays closed.

## CP-010: First uneven-demand placement screen reviewed

Date: 2026-11-22.

**Recorded:** the user supplied four conditions in `n150-uneven-demand.jsonl`, one run each. Every worker completes 600M increments; independent T1/T4 request 40M/s with compute=1, and sharing T2/T3 request 10M/s with compute=0. CPU slots, batch 50,000, idle 0, native PMUs, and recorded adaptive 700-3600-MHz limits are matched. See [the screen results](results.md#uneven-demand-placement-screen-one-round).

**Outcome:** shared total CPU falls 74.300618885 to 26.210598532 seconds (**64.72%**). Sharing-pair CPU falls **75.61%**; independent-pair CPU is nearly unchanged (**0.47% lower**) and all workers deliver within **0.013%** of their requested rates. Shared raw selected RFO/HITM falls 89,763,756 to 0. However, padded CPU also falls **26.26%**, so the entire shared saving cannot be assigned to false sharing.

**Verified and qualified:** settings, CPU endpoints, nominal deadlines, achieved-rate arithmetic, all 16 independently recomputed compute checksums, and all 256 PMU statuses check out. The artifact remains external to Git and has the report's 25th fingerprint. Earlier primary results and checkpoints are preserved. One run per condition is not repeatability or an admission boundary; 15/60-second unequal nominal durations allow independent workers to finish early and cease competing. Do not interpret approximately 40M/s whole-run throughput against the initial summed 100M/s requested rate. No assistant-run builds, workloads, or CTest; this update changes documentation only.

**Next evidence gate:** bounded matched confirmation of this uneven-demand point, followed by a realistic scheduling workload. Keep per-worker rate and total CPU cost as the objectives, with explicit limits on steady-state and tail-latency claims. The compute=4 hardware/PMU diagnostic branch remains closed; no dynamic scheduler or universal admission rule is established.

## CP-011: Uneven-demand confirmation complete

Date: 2026-11-22.

**Recorded:** the original uneven-demand file plus `n150-uneven-demand-round2-literal.jsonl` and `n150-uneven-demand-round3-literal.jsonl` provide 12 matched records, three per condition, with the final round reversed. Workload and recorded CPU/frequency settings match CP-010. The report now covers 135 physical-Linux records and 27 artifact fingerprints; earlier primary results and checkpoint history are preserved.

**Confirmed:** mean shared CPU falls 73.740079453 to 26.427554648 seconds (**64.16%**), with round savings 64.72%/64.01%/63.74%. Sharing-pair CPU falls **75.04%**; independent-pair mean CPU stays nearly unchanged (**0.40% lower**) without a visible delivered-rate penalty. Maximum rate shortfall is 0.012857%. Shared raw selected RFO/HITM falls from 83-90 million to 0/0/2 across rounds. Padded CPU savings also reproduce (**26.47%** mean), limiting attribution of the full shared saving to false sharing. See [the three-round confirmation](results.md#uneven-demand-confirmation-three-rounds).

**Verified and limited:** all 12 records, run order, 48 independently recomputed compute checksums/settings/CPU endpoints/pacing deadlines, rate arithmetic, and 768 requested PMU statuses check out. PMU running fractions span approximately 29.21-51.00%; phase/scaling limitations remain. Unequal 15/60-second nominal durations and early-finish interference prevent steady-state admission or application tail-latency claims. Exact binary/source provenance and separate CTest success remain unestablished. The assistant performed data/document checks, not builds, workloads, or CTest.

**Disposition and next gate:** bounded synthetic confirmation is complete; no further repetitions for this point or hardware/PMU diagnostics. Select and validate a realistic workload with explicit useful-throughput/latency objectives and independent-work regression checks before claiming application benefit or a dynamic admission rule. No dynamic scheduler is implemented.

## Updating checkpoints

Append a short dated checkpoint at a meaningful milestone or handoff, retaining earlier entries. Update the latest-checkpoint line and current-position paragraph.

Each checkpoint should state what changed, what was actually verified and where the evidence lives, remaining blockers/open choices, and the next concrete evidence gate. Distinguish files written, code implemented, checks performed, and measured results. Link decisions rather than repeating their rationale.
