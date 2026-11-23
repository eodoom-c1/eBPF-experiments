# Static-placement experiment results

Updated: 2026-11-22. Raw recording timestamps use UTC.

**Finding:** on the tested Intel N150, co-locating the known false-sharing pair reduces total worker CPU cost. At higher demand it also restores a requested throughput that baseline placement cannot sustain. The primary padded controls show substantially smaller placement effects. Three uneven-demand rounds confirm **64.16% lower mean shared CPU cost** without a visible independent-worker rate penalty, but their **26.47% padded CPU saving** also reproduces and limits attribution. Later compute=4 trials and controls were noisy; their cause remains unresolved. These are static-workload results, not an implemented dynamic scheduler or a publication-novelty claim.

**Stopping point:** the hardware/PMU/placement-noise diagnostic branch is closed at the user's request. The bounded three-round uneven-demand confirmation is also complete; no further repetitions are needed for this point. Preserve the supported static results and their qualifications, and move to realistic workload validation rather than more hardware diagnostics or synthetic repetitions.

## What we tested and under what conditions

Four workers repeatedly update separate lock-free relaxed atomic counters. Shared layout places T2/T3 at offsets 0/8 on one 64-byte cache line; padded layout separates them at offsets 0/64. Placement changes without changing the useful-work budget or worker logic.

| Placement | Physical core / CPU 0 | Physical core / CPU 1 |
| --- | --- | --- |
| Baseline | T1, T2 | T3, T4 |
| Colocated | T2, T3 | T1, T4 |

The target is physical Ubuntu/Linux on an Intel N150, kernel `7.0.0-43-generic`, with SMT inactive. `--cpus 0,0,1,1` uses two distinct cores in package 0 and **time-sharing, not SMT**. All primary runs use 600,000,000 increments per worker: **2.4 billion useful increments per run**, `--batch 50000`, `--idle-us 0`, and `--pmu-profile intel-n150`. Compute is 0 in A-E and 1 in F; placement comparisons keep compute unchanged.

| Experiment | Frequency configuration on CPUs 0/1 | Requested rate per worker | Aggregate target | Nominal duration | Repetitions |
| --- | --- | ---: | ---: | ---: | --- |
| A: adaptive frequency | `intel_pstate`, `powersave`, 700-3600 MHz limits | 10M/s | 40M/s | 60 s | Three per condition |
| B: frequency bounded | Same driver/governor, minimum = maximum = 1400 MHz | 10M/s | 40M/s | 60 s | Three per condition |
| C: higher demand | Same 1400-MHz limits | 20M/s | 80M/s | 30 s | Three per condition |
| D: highest tested demand | Same 1400-MHz limits | 40M/s | 160M/s | 15 s | Three per condition |
| E: unpaced execution | Same 1400-MHz limits | Uncapped (`--rate` omitted; recorded rate = 0) | None | None; fixed work | Three per condition |
| F: first compute increment | Same 1400-MHz limits; `--compute 1` | Uncapped | None | None; fixed work | Three per condition |

Each primary experiment has 12 records: baseline/shared, colocated/shared, baseline/padded, and colocated/padded across three rounds. The first two rounds use that order; the third reverses it. All **72 primary records** parsed successfully, had available requested PMU events, and reported expected CPU endpoints. Another **50 follow-up records** are summarized separately below: **122 original primary/follow-up benchmark records**, plus **12 uneven-demand placement records** and **one uneven-demand functional smoke**, giving **135 reviewed physical-Linux records** here. The uneven-demand results and smoke are not pooled with the original results. The earlier generic pilot and other smoke/probe artifacts remain excluded from this count. Frequency limits are output-time observations, not proof of continuously fixed clocks.

Padded co-location is a **negative control**, not a recommended scheduler action: it tests whether placement changes affect performance without the intended same-cache-line counter sharing. Padding does not prove that every other source of interference is absent.

## Meaning of the measurements

**Worker CPU time** sums the four threads' time scheduled on CPUs, including hardware stalls but excluding sleeping or waiting to be scheduled. **CPU cost** divides that sum by completed increments. It is not CPU count, clock frequency, whole-machine utilization, or energy.

Tables report arithmetic means of three runs per condition. CPU reduction is `100 * (1 - mean_colocated_CPU_time / mean_baseline_CPU_time)`, not an average of the three percentages. Throughput is the arithmetic mean of per-run completed increments divided by wall time; it need not equal the work budget divided by mean wall time.

Pacing retains the complete work budget. If a requested rate cannot be sustained, workers finish later; no work is dropped. These completion intervals are not application request-tail-latency measurements.

## Main outcomes

| Experiment | Condition | Worker CPU seconds | CPU ns/increment | Wall seconds | Throughput, M increments/s |
| --- | --- | ---: | ---: | ---: | ---: |
| A | Baseline/shared | 54.556 | 22.732 | 60.000 | 40.000 |
| A | Colocated/shared | 38.526 | 16.052 | 60.000 | 40.000 |
| A | Baseline/padded | 39.816 | 16.590 | 60.001 | 40.000 |
| A | Colocated/padded | 38.828 | 16.178 | 60.001 | 40.000 |
| B | Baseline/shared | 85.072 | 35.447 | 60.000 | 40.000 |
| B | Colocated/shared | 37.200 | 15.500 | 60.000 | 40.000 |
| B | Baseline/padded | 37.333 | 15.555 | 60.000 | 40.000 |
| B | Colocated/padded | 37.266 | 15.527 | 60.000 | 40.000 |
| C | Baseline/shared | 78.331 | 32.638 | 39.327 | 61.143 |
| C | Colocated/shared | 35.939 | 14.975 | 30.000 | 79.999 |
| C | Baseline/padded | 36.024 | 15.010 | 30.000 | 79.999 |
| C | Colocated/padded | 35.921 | 14.967 | 30.000 | 79.999 |
| D | Baseline/shared | 59.912 | 24.963 | 30.157 | 79.649 |
| D | Colocated/shared | 34.331 | 14.305 | 17.276 | 138.929 |
| D | Baseline/padded | 34.217 | 14.257 | 17.204 | 139.513 |
| D | Colocated/padded | 33.346 | 13.894 | 16.828 | 142.726 |
| E | Baseline/shared | 59.835 | 24.931 | 30.105 | 79.761 |
| E | Colocated/shared | 33.952 | 14.147 | 17.063 | 140.712 |
| E | Baseline/padded | 34.169 | 14.237 | 17.181 | 139.693 |
| E | Colocated/padded | 33.374 | 13.906 | 16.802 | 142.965 |
| F | Baseline/shared | 64.267 | 26.778 | 32.337 | 74.413 |
| F | Colocated/shared | 35.767 | 14.903 | 17.975 | 133.534 |
| F | Baseline/padded | 35.946 | 14.978 | 18.073 | 132.799 |
| F | Colocated/padded | 35.672 | 14.863 | 17.916 | 134.011 |

| Experiment | Shared CPU reduction in rounds 1 / 2 / 3 | Shared reduction from mean CPU times | Padded reduction from mean CPU times |
| --- | --- | ---: | ---: |
| A | 27.45% / 31.25% / 29.46% | **29.38%** | 2.48% |
| B | 57.89% / 57.83% / 52.74% | **56.27%** | 0.18% |
| C | 56.01% / 54.86% / 51.21% | **54.12%** | 0.29% |
| D | 43.92% / 39.90% / 44.11% | **42.70%** | 2.54% |
| E | 42.10% / 41.06% / 46.46% | **43.26%** | 2.33% |
| F | 45.91% / 46.49% / 40.22% | **44.35%** | 0.76% |

**A: CPU efficiency under normal frequency policy.** Both placements sustain 40M/s, but shared co-location saves 16.03 worker CPU seconds per run. The independent pair's CPU cost increases; these totals already include that regression. This is not a throughput speedup.

**B: benefit persists with common frequency limits.** Shared co-location saves 47.87 worker CPU seconds per run and closely matches padded CPU cost. The much smaller padded effect is a negative control against treating every placement difference as a coherence benefit. Frequency policy changes the measured effect size; A and B are separate regimes, not pooled.

**C: capacity and delivered-throughput benefit.** Baseline/shared achieves only 58.5-64.9M/s across rounds, versus approximately 80M/s with co-location: **30.84% higher mean per-run throughput** and **54.12% lower CPU cost** for identical total work. Baseline's sharing threads achieve 14.6-16.3M/s each and finish in 37-41 seconds; its independent threads meet 20M/s and finish in 30 seconds. All colocated workers meet the target. Baseline uses approximately 1.99 scheduled CPU equivalents, versus 1.20 colocated, on the same two-core budget.

**D: benefit persists when all conditions miss the target.** At 40M/s per worker, baseline/shared delivers 77.5-82.9M/s across rounds, versus 138.2-140.3M/s with co-location: **74.43% higher mean per-run throughput**, **42.70% lower CPU cost**, and **42.71% shorter mean wall time** for identical work. Co-location again approaches padded performance, but neither placement nor padding sustains the requested 160M/s aggregate.

Both shared placements consume approximately **1.99 scheduled CPU equivalents**. Lower CPU cost here means fewer CPU seconds because co-location finishes sooner, not lower CPU occupancy while running. Padded conditions use approximately 1.98-1.99 equivalents and also miss the target, indicating an execution-capacity constraint beyond false sharing under these settings. Baseline's independent workers finish around 17 seconds while its sharing workers take around 30 seconds; colocated workers finish around 17 seconds. These staggered fixed-work completions and changing pacing behavior mean the table is **not a steady-state capacity measurement**, and the higher baseline throughput than in C does not by itself establish a smaller coherence penalty at higher demand.

**E: benefit persists without an explicit rate cap.** Unpaced shared co-location delivers **76.42% higher mean per-run throughput**, **43.26% lower CPU cost**, and **43.32% shorter mean wall time** for identical work. It again approaches padded performance; padded CPU savings are only **2.33%**. Both shared placements use approximately 1.99 scheduled CPU equivalents, so the CPU saving reflects earlier completion rather than lower occupancy while running.

Relative to D, mean throughput changes by **+0.14% for baseline/shared**, **+1.28% for colocated/shared**, **+0.13% for baseline/padded**, and **+0.17% for colocated/padded**. These close results support execution/coherence constraints, rather than the explicit rate cap, as the main limitation at the 40M/s-per-worker demand point; they do not establish statistical equivalence. Baseline's independent workers still finish around 17 seconds while its sharing workers take around 30 seconds. Removing pacing does not remove the staggered-completion limitation: these remain finite-work averages, not steady-state capacity measurements.

**F: benefit survives a small amount of computation.** One checksum recurrence step per increment leaves shared co-location with **79.45% higher mean throughput** and **44.35% lower CPU cost**, versus **0.76% padded CPU savings**. Throughput falls approximately 5-7% across condition means relative to E because the workload includes additional computation. The slightly higher placement savings than in E do not establish that computation strengthens the benefit. The larger compute=4 points below do not establish a clean compute/benefit curve.

## Coherence, cache, and TLB evidence

Raw `ocr.demand_rfo.l3_hit.snoop_hitm` counts below sum **T2/T3 only**. Ranges span the three repeats; padded ranges include both placements. They are observed counts during PMU running windows, not scaled totals or cache-line address samples.

| Experiment | Baseline/shared RFO/HITM range | Colocated/shared range | Padded control range |
| --- | ---: | ---: | ---: |
| A | 89,948,563-103,457,632 | 1-35 | 3-12 |
| B | 73,774,229-85,206,739 | 2-4 | 3-12 |
| C | 64,842,451-87,304,039 | 3-6 | 6-10 |
| D | 39,361,316-47,346,128 | 9-29 | 10-154 |
| E | 43,685,754-45,477,146 | 0-5 | 4-26 |
| F | 48,526,977-56,718,320 | 0-6 | 1-34 |

The collapse to background-level counts repeats with shared co-location and with padding. Alongside lower CPU cost and higher sharing-pair IPC, this supports avoiding cross-core ownership-response costs as the mechanism. The filter covers a particular L3-hit snoop path, including exclusive-prefetch requests, not all coherence traffic.

The following are **mean scaled estimates per million useful increments**, summed over **all four workers**, for shared layout. Each cell is **baseline / colocated**. Generic counters retain kernel-defined, processor-dependent meanings.

| Counter / estimate | A | B | C | D | E | F |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Native retired-load L1 misses | 219,055.43 / 771.05 | 208,764.14 / 803.86 | 175,094.18 / 800.26 | 102,131.04 / 281.53 | 129,942.96 / 129.44 | 132,082.22 / 106.78 |
| Generic LLC load misses | 0.000369 / 0 | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 |
| Generic LLC load-hit estimate | 248.90 / 254.12 | 35.36 / 255.14 | 8.65 / 12.99 | 6.88 / 5.28 | 0.36 / 0.47 | 0.80 / 0.58 |
| Generic data-TLB load misses | 146.98 / 143.55 | 92.47 / 148.08 | 0.0482 / 32.58 | 0.0026 / 0.0033 | 0.0067 / 0.0092 | 0.0119 / 0.0074 |
| Generic data-TLB store misses | 0.0100 / 0.0150 | 0.0167 / 0.0022 | 0.0059 / 0.0011 | 0.0011 / 0.0007 | 0.0022 / 0.0048 | 0.0022 / 0.0030 |
| Native completed instruction-TLB walks | 79.70 / 78.33 | 51.91 / 79.18 | 0.0200 / 17.35 | 0.0111 / 0.0048 | 0.0122 / 0.0015 | 0.0030 / 0.0037 |

In E, mean sharing-pair IPC rises from approximately **0.22 to 0.55** with co-location; in F it rises from approximately **0.36 to 0.95**. These are within-workload comparisons. F executes additional instructions, so its higher IPC than E is not a useful-throughput improvement. The native L1-miss and RFO/HITM observations again support the coherence explanation.

Retired-load L1 misses fall sharply while generic LLC misses are rare in this collector: ownership-transfer penalties need not appear as large generic LLC-miss counts. This does not establish DRAM traffic or validate every generic selector's model-specific semantics. LLC hits are an access-minus-miss **estimate**; L1 hits are explicitly unavailable because generic accesses and native retired-load misses are not a matched pair.

TLB differences are secondary observations, not a demonstrated translation optimization. Pacing, sleeping, and context-switch patterns change when a target is unsustainable; this experiment does not isolate their contribution.

## Under-load frequency verification

Follow-up `turbostat` checks wrapped one approximately 30-second baseline/shared run and one colocated/shared run, ensuring overlap with the workload.

| Placement | CPU 0 busy MHz | CPU 1 busy MHz | CPU 0 / CPU 1 busy percentage |
| --- | ---: | ---: | --- |
| Baseline | 1404 | 1405 | 73.05% / 71.59% |
| Colocated | 1411 | 1418 | 39.16% / 33.14% |

Busy-clock averages match within approximately 1% across placements **for these two checks**. They do **not** continuously verify clocks throughout B-F or prove exactly constant frequency. Later compute=4 monitoring found substantially different clock reports despite the same recorded limits, as summarized below. Do not generalize the two checks into a blanket fixed-clock claim. `Busy%` includes all activity on the CPUs, not just benchmark workers; it is not used to calculate the CPU reductions above. An earlier idle-only trace and two command-error logs did not establish under-load frequency.

## Later compute and diagnostics (closed)

These **50 follow-up records** retain 600M increments per worker, batch 50K, compute=4, no pacing, and idle=0. They are diagnostic observations, **not pooled with A-F**. The two full compute=4 matrices have three repetitions per condition; their shared-layout means are baseline / colocated:

| Batch | Shared worker CPU seconds | Shared wall seconds | Shared throughput, M/s | Shared CPU reduction | Padded CPU reduction |
| --- | ---: | ---: | ---: | ---: | ---: |
| G: compute=4, recorded 1400/1400-MHz limits, 12 runs | 77.842 / 49.873 | 40.016 / 27.024 | 60.031 / 92.807 | 35.93% | 0.61% |
| H: monitored repeat, same recorded limits, 12 runs | 72.587 / 49.437 | 37.940 / 25.930 | 63.449 / 92.698 | 31.89% | -11.11% |

G's shared CPU savings vary **49.45% / 19.95% / 37.16%** by round; one colocated/shared run takes 35.08 s versus 21.20 and 24.79 s in the others. H's workload-average busy clocks span **1422-1780 MHz** despite recorded 1400/1400-MHz limits, and padded co-location consumes **11.11% more CPU**. Shared raw RFO/HITM still collapses (G: 46.3-63.4M to 5-71; H: 31.7-50.5M to 2-23), preserving qualitative coherence evidence, but these batches do **not** establish a causal reduction in benefit as compute increases.

All remaining diagnostics use padded layout. Normal mapping means CPU slots `0,0,1,1`; swapped means `1,1,0,0`. Mixed placement pairs T2/T3 and T1/T4. PMUs use the native profile except the explicitly disabled ablation condition.

| Diagnostic | Conditions / records | Result | Meaning and limitation |
| --- | --- | --- | --- |
| Live frequency-policy check | Baseline, recorded 1400/1400-MHz limits; 1 run and 26 policy snapshots | All sampled limits/governors remain unchanged; busy clocks 1466/1471 MHz. Sampled current-frequency peaks 3234/3089 MHz. Run: 26.140 s, 91.813M/s. | No observed policy change explains the discrepancy. Samples are not continuous proof, and peaks do not prove sustained clocks. |
| HWP/register check | Same workload and limits; 1 run | Captured CPU 0/1 HWP minimum = maximum = 14, package control disabled; TSC approximately 806 MHz agrees with CPUID. Run: 36.442 s, 65.857M/s. | Requests reach hardware at captured points; no obvious gross TSC-calibration error. Close busy-clock averages coexist with approximately 30 versus 42.8 cycles/increment across cores. Cause unidentified. |
| Initial baseline core swaps | Recorded 1400/1400-MHz limits; 6 runs | Normal/swapped mean CPU: 54.900 / 49.568 s; throughput: 76.791 / 92.199M/s. T3/T4 are slower on either core in this batch. | Worker identity, counter addresses and CPU interaction remain confounded; this is not proof of padded false sharing or a permanent slow worker. |
| Mixed-pair core swaps | Adaptive 700-3600-MHz limits; 6 runs | Normal/swapped mean CPU: 29.819 / 29.811 s; throughput: 152.928 / 148.624M/s. Busy clocks 2395-2397 MHz; each worker closely matches its same-core partner. | Frequency regime changed, preventing attribution of differences from the earlier batch to pairing alone. |
| Matching adaptive baseline | Same adaptive limits; 6 runs | Normal/swapped mean CPU: 29.103 / 31.990 s; throughput: 160.631 / 141.508M/s. Busy clocks 2393-2397 MHz. T3/T4 are not consistently slower. | Mixed placement's descriptive CPU change is +2.46% normally and -6.81% swapped. Separate batches were not interleaved; no isolated causal placement effect or fixed worker/core defect established. |
| Benchmark PMU ablation | Adaptive limits, swapped baseline; 3 runs per mode, interleaved | PMU on/off mean CPU: 29.776 / 28.762 s; wall: 15.891 / 15.255 s; throughput: 151.993 / 160.434M/s. | Disabling PMUs gives 3.41% lower mean CPU cost, but no-PMU wall times still vary 18.41 / 13.65 / 13.71 s. Counter collection is not necessary for the anomaly and cannot be its sole cause; overhead estimate is noisy. |

**Disposition:** intermittent compute=4 execution-cost variation remains unresolved. It is not evidence that all earlier co-location results disappear, nor that compute=4 has a reliably quantified benefit boundary. Further hardware/PMU/core-pair diagnostics are **stopped**, not a prerequisite for moving on. The proposed one-worker-per-core test was withdrawn; no results from it are included.

## Uneven-demand functional smoke (not pooled)

Per-worker `--rates` and `--computes` controls are implemented in T1-T4 identity order, independent of CPU placement. Existing scalar controls retain uniform behavior; scalar/list forms for the same control cannot be combined. Schema 4 reports effective per-thread settings and uses `null` for unequal run-level compute, rate, or nominal duration as appropriate. Build, CTest, and runnable smoke/comparison commands are in [experimental methodology](experimental-methodology.md#uneven-worker-demand).

The user supplied `uneven-demand-smoke.jsonl`: one colocated/shared N150 run recorded at `2026-11-23T04:41:32Z` (November 22 locally), with 1,000 increments per worker, batch 100, idle 0, and no benchmark PMUs. Recorded frequency limits are 700-3600 MHz; no fixed-clock claim is made. Total wall time is 1.000377981 seconds. This is a short functional check, not a placement comparison.

| Worker | CPU | Compute steps/increment | Requested increments/s | Achieved increments/s | Completion seconds |
| --- | ---: | ---: | ---: | ---: | ---: |
| T1 | 1 | 0 | Unpaced (0) | 10,458,063.167 | 0.000095620 |
| T2 | 0 | 1 | 1,000 | 999.724 | 1.000275803 |
| T3 | 0 | 2 | 2,000 | 1,998.683 | 0.500329368 |
| T4 | 1 | 3 | 4,000 | 3,995.395 | 0.250288113 |

**Verified:** valid schema-4 JSON, effective workload settings, expected start/end CPU assignments, nullable unequal run-level fields, nominal durations, achieved-rate arithmetic, and all four compute checksums independently recomputed from thread seeds using `1000 * compute` unsigned LCG steps modulo `2^64`. The successful record reflects the benchmark's fixed-work validation. T2/T3/T4 finish just after their 1/0.5/0.25-second nominal deadlines, achieving 99.9724%/99.9341%/99.8849% of their requested rates; no paced worker finishes early.

**Limit:** this smoke case passes its functional checks, not a declared scheduling-admission threshold. It establishes neither co-location benefit under uneven demand nor common-duration steady-state admission or application tail latency. Separate CTest output has not been supplied. The 122 earlier measurements and their conclusions are unchanged; the hardware/PMU diagnostic branch remains closed.

## Uneven-demand placement screen (one round)

`n150-uneven-demand.jsonl` contains one run per condition, in baseline/shared, colocated/shared, baseline/padded, colocated/padded order. Every worker completes 600M increments; T1/T4 request 40M/s with compute=1, and sharing T2/T3 request 10M/s with compute=0. CPU slots remain `0,0,1,1`, batch 50,000, idle 0, native Intel N150 PMUs enabled. Recorded CPU0/1 limits remain 700-3600 MHz with `powersave`; these are not continuously measured clocks. This screen is separate from A-F and the compute=4 diagnostics.

| Condition | Total worker CPU seconds | Sharing T2/T3 CPU seconds | Independent T1/T4 CPU seconds | Minimum T1/T4 rate attainment | Minimum T2/T3 rate attainment |
| --- | ---: | ---: | ---: | ---: | ---: |
| Baseline/shared | 74.301 | 63.534 | 10.767 | 99.9875% | 99.9995% |
| Colocated/shared | 26.211 | 15.494 | 10.716 | 99.9988% | 99.9996% |
| Baseline/padded | 35.753 | 19.253 | 16.500 | 99.9948% | 99.9995% |
| Colocated/padded | 26.366 | 15.653 | 10.713 | 99.9988% | 99.9997% |

Rate attainment is `100 * achieved / requested`, with the minimum taken over each worker pair. All workers deliver within 0.013% of their requested rate; this is an observed shortfall, not a silently declared admission tolerance.

**Shared result:** total worker CPU time falls **64.72%**, from 74.300618885 to 26.210598532 seconds. Sharing T2/T3 CPU falls **75.61%**, from 63.533552788 to 15.494247957 seconds. Independent T1/T4 CPU is nearly unchanged: 10.767066097 to 10.716350575 seconds, **0.47% lower**, with no visible rate penalty in this screen.

**Control and counter evidence:** padded total CPU also falls **26.26%**, from 35.752591144 to 26.365563428 seconds; its independent-pair CPU falls 35.07% and sharing-pair CPU falls 18.70%. Consequently, the entire shared-layout saving cannot be attributed to false sharing. Summed raw selected RFO/HITM counts fall from **89,763,756 to 0** in shared layout and **66 to 1** in padded layout, supporting a coherence component without isolating every placement effect. All 256 requested per-worker event statuses are available; multiplex running fractions span approximately 36.87-38.29%. The previously stated counter scope/scaling limitations apply.

**Verified and limited:** effective settings, CPU endpoints, nominal deadlines, achieved-rate arithmetic, and all 16 worker checksums were checked; checksums were independently recomputed using logarithmic affine LCG arithmetic modulo `2^64`. One run per condition establishes neither repeatability nor a universal scheduling/admission boundary. Nominal durations are 15 seconds for T1/T4 and 60 seconds for T2/T3; observed independent completion is approximately 15.0001-15.0019 seconds and sharing completion approximately 60.0001-60.0003 seconds. Independent workers stop competing long before the sharing pair finishes. Whole-run wall time remains approximately 60 seconds and aggregate throughput approximately 40M/s; neither should be compared directly with the initial summed 100M/s requested rate. This is finite-work evidence, not common-duration steady-state admission or application tail latency. Hardware/PMU noise diagnostics remain closed.

## Uneven-demand confirmation (three rounds)

The original screen plus `n150-uneven-demand-round2-literal.jsonl` and `n150-uneven-demand-round3-literal.jsonl` provide **12 records, three per condition**. Rounds 1/2 use baseline/shared, colocated/shared, baseline/padded, colocated/padded order; round 3 reverses it. The workload, CPU budget, recorded frequency policy, and native PMU profile match the first-round settings above. Only these three four-record files are included, not other retry outputs.

| Condition | Mean total worker CPU seconds | Mean sharing T2/T3 CPU seconds | Mean independent T1/T4 CPU seconds | Minimum T1/T4 rate attainment | Minimum T2/T3 rate attainment |
| --- | ---: | ---: | ---: | ---: | ---: |
| Baseline/shared | 73.740 | 62.988 | 10.752 | 99.9871% | 99.9995% |
| Colocated/shared | 26.428 | 15.719 | 10.708 | 99.9988% | 99.9994% |
| Baseline/padded | 36.008 | 19.192 | 16.816 | 99.9948% | 99.9995% |
| Colocated/padded | 26.477 | 15.777 | 10.701 | 99.9987% | 99.9994% |

CPU values are condition means; rate attainment is the worst worker value within each pair across all three runs. Reductions use ratios of condition mean CPU times, not averages of round percentages.

| Round | Shared CPU reduction | Padded CPU reduction | Shared baseline raw RFO/HITM | Shared colocated raw RFO/HITM |
| --- | ---: | ---: | ---: | ---: |
| 1 | 64.72% | 26.26% | 89,763,756 | 0 |
| 2 | 64.01% | 27.15% | 83,239,402 | 0 |
| 3 (reversed) | 63.74% | 26.00% | 89,857,945 | 2 |

**Confirmed at this demand point:** mean shared total CPU falls 73.740079453 to 26.427554648 seconds (**64.16%**). Mean sharing-pair CPU falls **75.04%**, from 62.988426272 to 15.719282832 seconds. Mean independent-pair CPU remains nearly unchanged, 10.751653181 to 10.708271816 seconds (**0.40% lower**); independent CPU does not decrease in every individual round, but no delivered-rate penalty is visible. The maximum per-worker rate shortfall across all conditions is 0.012857%, within the previously reported 0.013%.

**Replicated control effect:** mean padded total CPU falls 36.007749932 to 26.477288493 seconds (**26.47%**). Padded independent-pair CPU falls 36.37% and sharing-pair CPU falls 17.80%. This is a persistent non-sharing placement effect in these comparisons, not a reason to assign the full shared saving to false sharing. Padded raw selected RFO/HITM counts remain low: baseline 66/17/7 versus colocated 1/2/3.

**Verification and limits:** all 12 records, intended run order, 48 independently recomputed checksums/settings/CPU endpoints/pacing deadlines, achieved-rate arithmetic, and 768 requested PMU statuses check out. Multiplex running fractions span approximately **29.21-51.00%**, so event scaling and phase-coverage limitations remain important. The 15/60-second unequal-duration and early-finish limitations from the screen still apply. These repeated finite-work results establish neither a universal admission boundary nor common-duration steady-state capacity, application tail latency, energy savings, or exact fixed-clock operation.

**Disposition:** the bounded synthetic confirmation is complete. No more repetitions for this point or hardware/PMU noise diagnostics are requested. Next is selection and validation of a realistic workload tied to the scheduling objective.

## Earlier exploratory results, not pooled

| Evidence | Result | Qualification |
| --- | --- | --- |
| Initial WSL pilot, four 5M-increment-per-worker runs | Shared throughput +34.95%; padded throughput -38.97% | Historical summary in [CP-004](checkpoints.md#cp-004-initial-wsl-records-reviewed-and-per-worker-rate-control-added); single short trials, physical placement unverified, and a CPU-time/completion-time inconsistency. Valid only for the reported environment, not physical coherence attribution. |
| Later rate-controlled WSL exploration | Mixed outcomes, including shared-layout CPU-cost regressions | Earlier session findings; raw artifacts were not reanalysed for this report. Not pooled with physical-Linux results. |
| Physical shared-only pilot, `placement.txt`, generic PMU profile | CPU 53.327339 to 35.314475 s: 33.78% lower at approximately 40M/s | No padded controls or repeats; L1 miss/ITLB access requests unavailable. One stray `a` in JSON was removed only in memory for numerical parsing; the original file is unchanged. |

Native-event smoke checks subsequently established grouped counter availability. Frequency probes were diagnostics, not matched placement studies: a file named `3600` initially held a 700-MHz-limited record, then was replaced by a normal adaptive 700-3600-MHz record, not a fixed-3.6-GHz result. Those probes are excluded from the main tables.

## Evidence and reproducibility

Original artifacts remain outside Git in `C:\Users\ericodoom\Downloads`; no result files were modified or copied into the repository. Eight primary files contain 72 records and nine follow-up benchmark files contain 50 records. These 122 report build timestamp `Nov 20 2026 21:50:27`; the 12 uneven-demand records across three files and separate one-record smoke report `Nov 22 2026 20:40:50`. The earlier documentation-time checkout was `50ee6f1dea23ee5205cee4570a8670c16e59523f`; the records do not embed a Git revision or binary digest, so exact binary-to-commit provenance is not independently established. Paths below are relative to Downloads; repeated folder names reflect the supplied archive extraction. Paired clock/policy logs remain alongside the benchmark files. The table has 27 fingerprints covering benchmark artifacts, the functional smoke, and selected diagnostic logs, not every auxiliary clock log.

| Artifact | Role | SHA-256 |
| --- | --- | --- |
| `n150-placement.txt` | A, round 1 | `e4235a175be65836517ce87b90f809e81cdac28eda7a5ce5562e2d3530220819` |
| `n150-placement2.txt` | A, round 2 | `1200e914c2a8e4c22c8a1bca923376b02c8a4c2177ebf04ab7f0d482b40a396c` |
| `n150-placement3.txt` | A, round 3 | `a959eefde7f37ee01880061afe4d121eaf8aeec61efce6e7186937957464247c` |
| `n150-fixed1400.txt` | B, 12 records | `dbb5e6f19c0d13bc15d879037ee950c760762fa3ca7e1b7e10040fce102215e3` |
| `n150-rate20m.txt` | C, 12 records | `92b7bc3657873bd9c3e534b695b87d4ab5f4e31b2d9f07148e3653ccb1408f47` |
| `n150-rate40m.txt` | D, 12 records | `31796fd442be5238e24fb7fc7721a5ff0c2c6b388a7fc478f3a43ee0eecc1059` |
| `n150-unpaced.txt` | E, 12 records | `36f4902d39c064572c92448cac63f3911abc812ef11399af5ad7e3e7c012d6f9` |
| `n150-unpaced-compute1.txt` | F, 12 records | `060982bba0c4f46475f14e6ba958600834a1942850085d217cad79c2aa56f981` |
| `n150-unpaced-compute4.txt` | G, 12 records | `df4094b510e9d03a52bb80a8cd84ed17cc097cc9922c2bb08eba12e7dd59dcc1` |
| `n150-compute4-monitored\n150-compute4-monitored\n150-unpaced-compute4-monitored.txt` | H, 12 records | `b747785a0ce31a96f26e1d799e5c6558888b0731fcc3cbc262d8ced14ef6a9f4` |
| `n150-frequency-diagnostic\n150-frequency-diagnostic\benchmark.txt` | Live-policy diagnostic, 1 record | `a31752b389c74a653276e36d5a28380df66fefe7518f28a2e906606ba48580a7` |
| `n150-frequency-diagnostic\n150-frequency-diagnostic\policy-during.txt` | 26 policy snapshots | `4404164455c7bc5e26df308db3d7ec557c587ec6143d0722d0d624ea87086335` |
| `n150-frequency-diagnostic\n150-frequency-diagnostic\clock.txt` | Live-policy workload clock log | `3c827ccad951d5d0c02f7f715b4e1735ec015d67c329e9498ec2418159fc00ab` |
| `n150-hwp-diagnostic\n150-hwp-diagnostic\benchmark.txt` | HWP diagnostic, 1 record | `3cc44ab477ddb269ecf9a9eaad71b8ee366ba2fa221fe090370e21d0f76fb9b1` |
| `n150-hwp-diagnostic\n150-hwp-diagnostic\turbostat.txt` | HWP/configuration and clock log | `f4c153664b256293f8a18c9ad28bf7abb00d602847473562d0bf13f888d0c9fc` |
| `n150-core-swap-compute4\n150-core-swap-compute4\benchmark.txt` | Initial baseline swaps, 6 records | `aa30c8cb62e7180bc1fe874a4d405d3293460351712702b3f5e0ef93c7a11171` |
| `n150-core-swap-compute4-mixed\n150-core-swap-compute4-mixed\benchmark.txt` | Adaptive mixed swaps, 6 records | `6b942280674f2748baf784b9da6216593134e073c0be4368f7348d7dc1c42678` |
| `n150-core-swap-compute4-baseline-adaptive\n150-core-swap-compute4-baseline-adaptive\benchmark.txt` | Adaptive baseline swaps, 6 records | `bf2d984dc153d77f8e2a21bfb22166674e6d13bb2e6bebff5abe542574f37ad9` |
| `n150-compute4-pmu-ablation\n150-compute4-pmu-ablation\benchmark-pmu.txt` | PMU-on ablation, 3 records | `d43fa05010a6248377ca3ba4564b4dc14a1d3dd18defdc34e177b1b28c00f0de` |
| `n150-compute4-pmu-ablation\n150-compute4-pmu-ablation\benchmark-no-pmu.txt` | PMU-off ablation, 3 records | `bc1d839047898a63fe8803d6be8f35d84531dcf06370604c018dc036a09e4f88` |
| `n150-clock-baseline.txt` | Follow-up busy-clock check | `ec86432700941e4f8324a374e3ed8984655c283b1e652a09c6c8e25d07331a87` |
| `n150-clock-colocated.txt` | Follow-up busy-clock check | `2fd91ddfbac148bfe9c8abfd592e02af9fbeacea4b41a0d9447e12a84a425652` |
| `placement.txt` | Earlier generic-profile pilot | `0f104c9459b32986caa5d5ba285fe50d597b59eb08988ce554b47627873bb619` |
| `uneven-demand-smoke.jsonl` | Schema-4 uneven-demand functional smoke, 1 record; not pooled | `edb35d6c552a5f56928f9d99ed99697ccc728bdede50adb77bc14e2bf941b100` |
| `n150-uneven-demand.jsonl` | Uneven-demand placement screen, 4 records; one per condition, not pooled | `5db6e9f8567565cb8d6f98c22fda0af9ab0a88ffec841f17859c972126b0e66e` |
| `n150-uneven-demand-round2-literal.jsonl` | Uneven-demand confirmation round 2, 4 records | `d67938cf13351a21302bf851631a4a2113628e31d43f0746763ea2b5f138b228` |
| `n150-uneven-demand-round3-literal.jsonl` | Uneven-demand confirmation round 3, reversed, 4 records | `3cca2dd96e0ca1033a591504bddfc806eca43f72ae5af12e55bc946c048b8a72` |

For commands, counter encodings, scope, and error handling, see [experimental methodology](experimental-methodology.md), section 8. The user compiled and executed the workloads; the assistant parsed artifacts and recomputed summaries. Benchmark results do not establish that separate argument-rejection/file-error tests passed.

## Significance, limits, and next gate

**Supported:** static co-location can reduce CPU cost at matched delivered throughput and recover throughput when baseline's sharing pair falls behind. At the highest tested paced demand, it improves throughput even when both placements occupy nearly two CPUs, but does not restore the requested rate. The benefit persists unpaced and at compute=1. Three unequal-demand rounds also reproduce CPU savings without a visible independent-worker rate penalty at the tested point. The primary small padded effects and repeated RFO/HITM collapse support a layout-specific coherence explanation; the unequal-demand padded effect and later noisy controls limit attribution and broader generalization.

**Dynamic-policy implications:** high observed CPU occupancy can include avoidable coherence stalls, so occupancy alone cannot reject intervention. Conversely, HITM disappearing after successful co-location is not evidence that the underlying sharing relationship disappeared or that separation is now safe.

**Limits:** one CPU model, core pair, synthetic counter/checksum workload, and three repetitions per primary and uneven-demand condition; no broad statistical significance claim. The unequal-demand material padded effect limits attribution of its full CPU saving to false sharing. PMU coverage is typically approximately 37.5% in primary results, but spans approximately 29.21-51.00% in the uneven-demand confirmation, with possible phase-sensitive scaling bias. PMUs measure user-space execution, including pacing/bookkeeping, with boundaries near but not identical to CPU/wall intervals. Affinity is checked at endpoints, not continuously, and cores are not reserved. Exact constant-frequency operation and the cause of compute=4 variability are not established. Background activity, thermal/power conditions, energy, application tail latency, general uneven-demand admission, and other topologies are not established by these results.

**Next substantive gate:** select and validate a realistic workload tied to the scheduling objective; the bounded uneven-demand confirmation is complete. Evaluate useful throughput, total worker CPU cost, and independent-work regressions with explicit service objectives. The synthetic unequal-duration averages are not common-duration steady-state or application tail-latency evidence. No more repetitions for this synthetic point or hardware/PMU/placement-noise diagnostics. Dynamic detection, admission, migration, feedback, and research novelty remain unimplemented or unestablished.
