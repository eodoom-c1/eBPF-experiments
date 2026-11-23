# Experimental Methodology and Case Studies: Dynamic False-Sharing Mitigation

Status: revised draft research plan. No performance results, overhead bounds, or publication-novelty claims are established by this document.

This document defines an evaluation of benefit-aware thread placement, including the proposed eBPF and `sched_ext` implementation, provisionally named **CoLoc-SCX**. Read it alongside [System Architecture and Design](system-architecture.md) and the existing [requirements](requirements.md).

The requirements remain authoritative. Linux and `sched_ext` are a candidate implementation path, not a new first-iteration requirement. Known sharing groups and existing affinity controls are sufficient to begin testing the placement hypothesis.

## 1. Research questions and hypotheses

The primary question is whether reducing cross-core coherence costs through selective consolidation improves useful application work after accounting for lost parallelism, interference, migration, and controller overhead.

The evaluation must allow the answer to be negative for particular workloads or for the proposed policy as a whole.

| Research question | Hypothesis to test |
| --- | --- |
| When does consolidation help? | Some coherence-bound groups benefit from SMT-sibling placement or time-sharing one logical CPU; other groups lose more execution capacity than they save in coherence costs. |
| Do additional signals improve decisions? | Utilization, runnable demand, IPC, cache pressure, and TLB pressure can improve selection over a coherence-only policy. Their value must exceed their collection and decision costs. |
| Does page geometry affect the decision? | Actual huge-page backing can change translation costs and placement break-even points, but the effect is workload- and machine-dependent. |
| Does in-kernel actuation add value? | A `sched_ext` implementation may react more effectively than an equivalent userspace placement controller at some phase durations. This is a comparison, not an assumption. |
| Is the controller stable? | It can retain successful interventions despite reduced contention signals, reject harmful ones, and respond to phase changes without excessive placement churn. |

False sharing means independent data occupies one cache line and accesses from different cores include a writer. HITM observations establish neither independent data nor the identity of the previous writer. Distinguish confirmed false sharing, suspected contention, and true-sharing controls throughout the results.

## 2. System and hardware baseline

### 2.1 Pin and record the execution environment

Record the CPU model and stepping, microcode, cache-line size, logical and physical CPU topology, SMT configuration, cache-sharing domains, NUMA layout, memory configuration, and relevant firmware settings.

Pin the exact kernel release or commit, kernel configuration, scheduler implementation, compiler, libbpf, and perf versions. `sched_ext` interfaces are not stable across all kernels described as "6.12+".

For a Linux experiment, verify:

- The target's BPF, BTF, JIT, perf, and `sched_ext` capabilities and permissions. The three options `CONFIG_SCHED_CLASS_EXT`, `CONFIG_BPF_SYSCALL`, and `CONFIG_DEBUG_INFO_BTF` are not a complete environment specification.
- Availability and precise-sampling support of the selected PMU events, including address attribution, usable counter combinations, and sampling loss.
- Effective CPU eligibility, cgroup/cpuset restrictions, scheduler enrollment, and whether physical cores are shared with other workloads.
- CPU frequency policy, turbo behavior, interrupt placement, automatic NUMA balancing, and page-placement policy.

An Azure Linux VM is suitable only if the experiment's required PMU facilities and relevant physical-topology properties are exposed and validated. Guest CPU numbering alone does not establish physical SMT co-residency or exclusive physical-core access.

### 2.2 Comparison baselines

Use comparable workload inputs, eligible CPUs, physical-core budgets, memory policies, and load-generation arrangements.

| Baseline | Purpose |
| --- | --- |
| Default Linux fair scheduling, using EEVDF on the selected kernel | Establish the default OS result on the same machine and kernel configuration. |
| Utilization-only placement/load balancing | Test whether coherence evidence adds value beyond balancing runnable demand. |
| Tuned separate-core placement | Establish a strong non-consolidated placement rather than relying only on incidental default placement. |
| Static SMT-sibling placement | Measure the benefit and interference of simultaneous execution on one physical core. |
| Static single-logical-CPU placement | Measure serialization separately from SMT resource sharing. |
| Base SCX scheduler with coherence-aware placement disabled | Isolate the placement contribution from replacing the base scheduler. |
| Base SCX scheduler plus observation, without intervention | Isolate observation overhead from placement benefit. |
| Equivalent userspace placement policy | Test the incremental value of the in-kernel control path, using matched evidence and decision intervals where possible; report unavoidable differences. |
| Compact, selectively padded, and aggressively padded layouts | Compare coherence reduction against padding's memory-density and locality costs. |
| Best measured static placement in a small enumerated candidate set | Provide an offline reference, not a claim of globally optimal placement. |

Padding is a reference intervention, not an upper bound on runtime performance. Alignment at an array's base does not by itself separate its elements into different cache lines; verify the actual layout.

SHERIFF and Cheetah require separate treatment. SHERIFF uses thread-to-process conversion and memory isolation, including heap and globals; it is not merely a heap-padding allocator. Cheetah detects false sharing and estimates optimization potential; it is not an equivalent automatic mitigation runtime. Compare runnable tools according to their actual capabilities and supported application semantics. If a tool cannot run on the selected platform, explain the limitation rather than presenting it as an experimental result.

## 3. Application outcomes and supporting metrics

### 3.1 Primary outcome

Before each experiment, declare:

- The useful-work measure, such as completed requests, processed records, or completed benchmark iterations.
- The latency objective and offered load, including selected tail percentiles for service workloads.
- Regression limits for the affected group and any managed unrelated workload.
- The physical-core and memory budgets, evaluation horizon, and intervention acceptance criteria.

Report throughput and latency together. A throughput gain does not excuse a violated latency guardrail. Reduced HITM caused by serialization or reduced useful work is not sufficient evidence of improvement.

### 3.2 Hardware and controller measurements

| Category | Candidate measurements | Interpretation and limitations |
| --- | --- | --- |
| Coherence contention | Supported HITM or cache-to-cache samples, source classifications, sampled addresses and offsets | Use model-specific definitions. A sampled load receiving a HITM response does not count every invalidation, identify the previous writer, or prove false sharing. |
| Interconnect activity | Supported offcore, UPI, or fabric events | Establish event scope and units. Socket-link traffic does not represent every intra-socket coherence transfer. Do not equate unrelated vendor events. |
| Translation pressure | Supported load/store TLB misses, completed walks, and walk-active events | Page-walker activity is not directly exposed application stall time. Separate steady-state pressure from migration transients. |
| Execution demand | Instructions, cycles, IPC, utilization, runnable waiting, supported stall/resource events | IPC is a feature, not proof of SMT execution-port compatibility. Identify per-thread, logical-CPU, and whole-core scopes. |
| Cache interference | Supported L1/L2/LLC misses and, where available, occupancy measurements | Misses do not directly measure footprint or prove capacity interference. Occupancy requires its own supported measurement facility. |
| Placement behavior | Effective CPUs, simultaneous sibling execution, migrations, waiting, intervention durations, releases | Validate actual execution rather than assuming a CPU-selection hint or queue insertion achieved the intended placement. |
| Controller cost | BPF runtime statistics, observation/control CPU time, memory use, event loss, application slowdown | BPF runtime alone excludes parts of interrupt, perf, PEBS/IBS, and control-path overhead. Runtime-statistics collection itself can add cost. |

For example, Intel Skylake's `MEM_LOAD_L3_HIT_RETIRED.XSNP_HITM` describes retired loads receiving HITM responses from shared L3. It is not a portable "remote HITM" event. Select and document the correct events for the actual processor.

Record raw and normalized values, including relevant events per useful operation. Report sample periods or frequency settings, lost/throttled samples, event availability, enabled/running times, and multiplexing. Use compatible counter groups or document why ratios remain interpretable.

Do not multiply sample counts by a fixed HITM latency and label the result elapsed time saved. Sampling, overlap, access paths, and the recoverable fraction of each access cost must be addressed.

## 4. Controlled sensitivity experiments

Begin with a benchmark whose sharing relationships and useful progress are known. A custom benchmark is acceptable; any external benchmark must have an exact artifact/version and a verified access pattern.

### 4.1 Sharing and execution controls

Include independent variables sharing a line, padded independent variables, read-only sharing, true sharing, and no-sharing controls. A producer-consumer queue can contain both true and false sharing; separate these contributions rather than treating all adjacent indices as false sharing.

Vary the ratio of sharing-related operations to independent computation. Specify reproducible operations or instruction workloads, and measure the resulting rates rather than assuming a source-level operation occurs every fixed number of cycles.

Include compute-heavy, memory-stalled, and bandwidth-limited pairs. Sweep runnable demand and the number of participating threads, including groups larger than an SMT sibling set that a pair-only policy should reject or explicitly leave unsupported.

### 4.2 Working set and page geometry

Sweep active working sets, initially across approximately 64 KB to 64 MB and around relevant machine-specific cache/TLB boundaries. Control access pattern, initialization, NUMA first touch, and useful work independently of allocation size.

Compare verified base-page and huge-page configurations. Record backing for the relevant mappings, not merely a global THP setting or `MADV_HUGEPAGE` request. Distinguish PMD-sized THP, multi-size THP where relevant, and explicit huge pages.

Do not assume a single monotonic break-even point or a universal upward shift with THP. Huge pages change translation behavior, not L1/L2 data-cache capacity, and can introduce allocation, compaction, or footprint costs. Report first-touch behavior separately from warmed steady state.

### 4.3 Topology, competition, and phases

Compare eligible separate cores within a shared cache domain, cores in different cache domains, cross-NUMA placement, SMT siblings, and one logical CPU. Discover actual topology; an AMD CCX, LLC domain, and NUMA node are not interchangeable terms.

Vary managed unrelated work on the candidate and destination cores. Evaluate the joint outcome when it is relocated; do not assume that filling another core compensates for overloading the consolidated core. Never relocate unmanaged work.

Include phase changes, short bursts, long stable phases, partner sleep/wakeup, and continuously runnable threads. Test the specific feedback hazard where successful consolidation suppresses the signal that originally triggered it.

## 5. Policy ablation and overhead

### 5.1 Incremental policy ladder

| Variant | Inputs or guards |
| --- | --- |
| A: coherence-only | Candidate contention evidence and required identity/eligibility checks. No optional IPC, cache, or TLB admission guards. |
| B: coherence plus execution demand | Add utilization, runnable demand, and IPC where supported. |
| C: execution and cache pressure | Add calibrated cache-interference features. |
| D: full candidate model | Add calibrated translation-pressure and verified page-geometry features. |

Mandatory identity, authorization, eligibility, and failure handling remain enabled in every variant. Ablation is not permission to violate control requirements.

The ladder shows incremental changes in one feature order; it does not isolate all contributions or interactions. Add selected full-minus-one-feature comparisons, including removal of utilization/IPC inputs, and targeted interactions between execution demand, cache pressure, and translation pressure.

Treat actual page backing and policy awareness of page backing as separate factors. Do not enable THP only for the full model and credit the resulting difference entirely to admission logic.

No variant is assumed to dominate. Report false admissions, missed beneficial placements, uncertainty, reversals, and cases where extra measurements cost more than they help.

### 5.2 Complete overhead measurement

Compare the default/base scheduler, observer-only configuration, and full controller at representative loads. Sweep sampling intensity and decision interval; report both measurement quality and application impact.

An overhead budget such as 1-2% may be declared as an experiment-specific target, not a proven property. Include total CPU consumption and useful-work changes as well as BPF program runtime. Component costs need not add linearly because instrumentation can alter workload behavior.

## 6. Real-world case-study candidates

These are investigation candidates, not claims that a particular release has an established, placement-remediable bug.

| Candidate | Required evidence and experiment |
| --- | --- |
| Memcached | Identify an exact commit, affected fields, access sites, and a documented fix or controlled layout change. Use a pinned `memtier_benchmark` workload and report offered load, throughput, and tail latency. |
| RocksDB or LevelDB | Identify a specific structure and producer/consumer access pattern. Separate true communication from independent-field false sharing. Use pinned `db_bench` workloads and control storage, durability, and caching settings so another bottleneck does not dominate. |
| PARSEC, SPLASH-2x, or Phoenix | Pin benchmark versions and inputs. Verify allocation and sharing sites instead of assuming stack/global arrays. Compare original and selectively corrected layouts where available. |
| Linux kernel networking or worker activity | Identify exact kernel code and accessors. Establish whether the accessors are eligible, migratable tasks rather than IRQ/softirq execution or constrained per-CPU work. Keep this outside the first controlled prototype. |

For each retained case, preserve a reproduction recipe, source revision, relevant layout/access evidence, supported PMU observations, and a corrected/padded control where possible. If the suspected issue cannot be reproduced, report that rather than substituting an invented historical bug.

SHERIFF supplies useful historical leads: it reports insufficient 32-byte padding in its evaluated `streamcluster` and low-impact false sharing in its evaluated `canneal`. These are version- and environment-specific findings, not proof that current releases are ideal case studies or that their affected data is stack allocated.

## 7. Reproducibility and interpretation

Use repeated trials with randomized or counterbalanced condition ordering. Report trial counts, warm-up, run duration, variability, and uncertainty intervals. For tail latency, include request counts and the load-generation method; verify the generator is not the bottleneck and explain any coordinated-omission correction.

Record effective topology/placement, page backing, relevant background activity, frequency/thermal conditions, and all policy and measurement parameters. Separate estimator calibration workloads from evaluation workloads, or explicitly label same-workload calibration results.

Publish negative results and failed guardrails alongside wins. A controller can satisfy its control requirements without demonstrating a useful performance contribution.

Proceed through these evidence gates:

1. Reproduce known sharing and compare static placements using existing tools.
2. Verify the observation path, address/task attribution, and measurement overhead on the pinned target.
3. Evaluate the adaptive policy against the static, utilization-only, and base-scheduler controls.
4. Compare userspace and SCX actuation if the target supports both.
5. Add source-verified application cases and broader portability experiments.

Publication claims should identify the actual decision-policy contribution relative to prior work. Combining existing tools, collecting more counters, or moving policy into the kernel is not by itself a novelty result.

## 8. Static Linux benchmark (first implementation)

The first implementation is [static-placement.c](static-placement.c), built as the `static-placement` CMake target. It is a controlled benchmark, not an automatic analyzer or dynamic scheduler. The user has supplied WSL measurements from the basic and rate-controlled benchmark; the newly added PMU collection has not been compiled or executed by the assistant, and native PMU validation remains outstanding.

### Build and CPU selection

On the Linux target, from the repository root, configure and build when ready:

```bash
cmake -S cache-coherence-scheduler -B out/build/linux-release/cache-coherence-scheduler \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build out/build/linux-release/cache-coherence-scheduler \
  --target static-placement static-placement-worker-demand-options
ctest --test-dir out/build/linux-release/cache-coherence-scheduler --output-on-failure \
  -R 'static-placement-(invalid-arguments|worker-demand-options)'
lscpu -e=CPU,SOCKET,CORE,ONLINE
grep Cpus_allowed_list /proc/self/status
```

Configure the benchmark directory directly; this checkout does not contain a root CMake project or Linux preset. In Visual Studio, select a remote Linux or existing WSL target. A Windows-host configuration skips this executable with a status message; it does not build a native Windows version. The benchmark needs Linux pthread affinity, accessible sysfs CPU topology/cache information, and lock-free atomic counters. It does not need BPF, SCX, or PMU privileges.

The CTest check exercises malformed/overflowing CPU lists, zero work, invalid placement names, an empty output path, invalid or unrepresentable rate settings, and misuse of the no-argument `--pmu` flag, requiring nonzero exit and no measurement output. It also checks that enabling PMU collection does not bypass invalid-work rejection or modify an existing results file when arguments are rejected. It does not require specific CPU topology; valid-placement and performance checks still require the selected hardware. Tests have not been run by the assistant.

The worker-demand options test checks accepted scalar/list settings, zero/unpaced entries, bounds, independent rate/compute controls, placement-independent T1-T4 ordering, and uniform versus unequal run-level JSON. Argument-rejection tests also cover malformed workload lists, mixed scalar/list forms, and timing overflow in a later worker. These additions have not been built or run by the assistant.

Supply `--cpus A0,A1,B0,B1`, replacing the names with logical CPU numbers. A0 and A1 must belong to physical core A; B0 and B1 to a different physical core B. Use different siblings within each pair for an SMT experiment, or repeat a logical CPU within each pair for time-sharing. IDs must fit `CPU_SETSIZE` and the process's allowed mask. Validation uses Linux's reported topology; it cannot establish a VM's hidden physical placement.

| Placement | Thread 1 | Thread 2 | Thread 3 | Thread 4 |
| --- | --- | --- | --- | --- |
| `baseline` | A0 | A1 | B0 | B1 |
| `colocated` | B0 | A1 | A0 | B1 |

### Workload and matched controls

Every thread performs the same specified number of relaxed atomic increments to its own counter. Threads 2 and 3 use distinct counters on one reported cache line in `shared` layout; `padded` moves thread 3's counter to another line. Threads 1 and 4 have separate lines. Atomics keep the memory updates observable without introducing intentional same-counter sharing.

Both layouts reserve the same four-line allocation to isolate the intentional line-sharing change; this is not the larger-footprint padding study. Counter offsets, addresses, process ID, and OS thread IDs are reported for profiling cross-checks.

Run the four conditions with identical work settings. The CPU numbers below are examples only: replace them with two eligible, distinct physical cores discovered on the target.

```bash
BIN=out/build/linux-release/cache-coherence-scheduler/static-placement
CPUS=0,0,1,1
"$BIN" --cpus "$CPUS" --placement baseline --layout shared
"$BIN" --cpus "$CPUS" --placement colocated --layout shared
"$BIN" --cpus "$CPUS" --placement baseline --layout padded
"$BIN" --cpus "$CPUS" --placement colocated --layout padded
```

Each successful run writes one JSON record to stdout by default. With `--output PATH`, it appends that record and a newline to a text file instead, leaving stdout empty. The file is created if needed; its parent directory must already exist, and relative paths are resolved from the current working directory. Existing records are not overwritten. Failures are reported to stderr with a nonzero exit; retain only successful runs. `--help` prints usage instead.

To collect the four conditions in one portable file, use the same output path for each run:

```bash
"$BIN" --cpus "$CPUS" --placement baseline --layout shared --output results.txt
"$BIN" --cpus "$CPUS" --placement colocated --layout shared --output results.txt
"$BIN" --cpus "$CPUS" --placement baseline --layout padded --output results.txt
"$BIN" --cpus "$CPUS" --placement colocated --layout padded --output results.txt
```

Copy `results.txt` back to the analysis machine. It is newline-delimited JSON, readable as plain text, with the same settings and metrics as console output. A new file will contain four lines if all four runs succeed. Repeating the commands adds more records; use separate files for different devices or experiment sessions. New records have `schema_version: 4`, adding effective per-thread workload settings and nullable run-level settings for unequal workloads. Version 3 introduced the PMU profile and additional event configuration; version 2 introduced machine/invocation/topology context. Record relevant background activity and power conditions separately, since snapshots do not continuously monitor them.

File output happens after timing and result validation. Open, write, flush, and close failures return a nonzero exit status. A failed write can leave a partial final record, which must not be analyzed as a successful run. Run sequentially when sharing an output file; concurrent writers are not coordinated.

| Option | Meaning |
| --- | --- |
| `--iterations N` | Increments per worker; default 5,000,000. Increase for a meaningful measurement interval. |
| `--rate N` | Target increments per second per worker; omitted by default for unpaced execution. All requested work is completed even when the target cannot be sustained. |
| `--rates R1,R2,R3,R4` | Exactly four targets in T1-T4 order; each accepts 0 for unpaced execution. Cannot combine with `--rate`. |
| `--compute N` | Synthetic integer-compute steps per increment; default 0. This is not a promised instruction/cycle count. |
| `--computes C1,C2,C3,C4` | Exactly four compute-step counts in T1-T4 order, each from 0 to 1,000,000. Cannot combine with `--compute`. |
| `--batch N` | Increments per batch before optional pacing or idle waits; default 1,000. |
| `--idle-us N` | Requested sleep between batches, excluding the final batch; default 0, maximum 1,000,000. Actual sleeps can be longer. |
| `--output PATH` | Append one JSON result line to a text file instead of stdout; earlier runs are preserved. |
| `--pmu` | Request per-worker user-space hardware counters. Missing or failed events are explicitly unavailable; the workload still completes. |
| `--pmu-profile generic\|intel-n150` | Select the counter profile and enable PMU collection. The default is generic; the native profile requires an Intel N150. |

For example, append `--batch 1000 --idle-us 200` to every condition to explore lower runnable demand, or vary `--compute` to change the work/coherence ratio. Measure resulting utilization rather than assuming either knob produces a particular percentage.

### Requested per-worker rate and fixed work budget

The selected larger workload is **7.5 billion increments per worker**, targeting **500 million increments/s per worker**. Four workers therefore complete **30 billion increments total**, with an aggregate requested rate of 2 billion/s. The nominal duration is **15 seconds**, not one minute. Workers that cannot sustain the target continue until their full work budget is complete; nothing is dropped.

```bash
"$BIN" --cpus "$CPUS" --placement baseline --layout shared \
  --iterations 7500000000 --rate 500000000 --batch 1000000 --output high-load-results.txt
```

Use the same rate, iterations, and batch settings for the other three placement/layout combinations. A true one-minute work budget at this per-worker rate would instead require `--iterations 30000000000`: 120 billion increments total.

Pacing occurs after each batch against the common monotonic start. Ahead-of-schedule workers sleep to an absolute deadline; behind-schedule workers continue immediately. The final batch is paced as well. This targets a cumulative average rate with batch-sized bursts, not precisely spaced operations or an independent external arrival stream. Once behind, a worker may catch up without additional pacing sleeps.

The example's one-million-increment batches represent 2 ms of requested work at 500 million/s. Larger batches reduce clock-check and sleep overhead but permit larger bursts; very small batches can make sleep granularity dominate the workload. Rate-control overhead and pacing waits are included in elapsed/completion measurements; blocked sleep time is excluded from thread CPU time. Combining `--rate` with `--idle-us` can further reduce achieved throughput.

With uniform settings, JSON retains numeric `compute`, `target_increments_per_second_per_thread`, and `nominal_work_seconds`; the latter two are zero when all workers are unpaced. With unequal compute, the run-level `compute` is `null`. With unequal rates, the run-level rate and nominal duration are `null`. Each thread now reports its effective `compute`, `target_increments_per_second`, and `nominal_work_seconds` (zero for an unpaced worker), alongside `increments_per_second` using its own completion interval. Top-level throughput still uses the common run interval. Compare each achieved rate with its own target, rather than assuming a requested rate was delivered.

### Uneven worker demand

Workload lists follow **thread identity T1-T4, not CPU slots**; co-location changes CPUs but not which worker receives each setting. The controls are independent: a rate list can accompany uniform `--compute`, and a compute list can accompany uniform `--rate`. Existing uniform options and defaults retain their workload behavior.

After building, a short functional smoke command on the N150's previously used CPU0/1 pair is:

```bash
BIN=out/build/linux-release/cache-coherence-scheduler/static-placement
"$BIN" --cpus 0,0,1,1 --placement colocated --layout shared \
  --iterations 1000 --rates 0,1000,2000,4000 --computes 0,1,2,3 --batch 100 \
  --output uneven-demand-smoke.jsonl
```

Every worker must still complete 1,000 increments. T1 is unpaced; T2/T3/T4 have nominal completion times of 1/0.5/0.25 seconds and must not finish before those deadlines. Inspect each thread's effective settings, completion time, and checksum. This is a functional check, not performance evidence; compute checksums should agree with the corresponding uniform-compute case for the same thread seed and work budget.

For an illustrative unequal-demand comparison, hold this workload constant across all four conditions:

```bash
BIN=out/build/linux-release/cache-coherence-scheduler/static-placement
for layout in shared padded; do
  for placement in baseline colocated; do
    "$BIN" --cpus 0,0,1,1 --placement "$placement" --layout "$layout" \
      --iterations 600000000 --rates 40000000,10000000,10000000,40000000 \
      --computes 1,0,0,1 --batch 50000 --pmu-profile intel-n150 \
      --output uneven-demand.jsonl || exit 1
  done
done
```

These settings put higher requested demand on independent T1/T4 and lower demand on sharing T2/T3. They are not a validated rate boundary. Keep CPU/frequency policy and background conditions unchanged between comparisons; repeat matched conditions and reverse run order before drawing a placement conclusion. Do not restart hardware/PMU noise diagnostics.

Compare summed worker CPU cost **and each worker's achieved/requested rate**, including independent-worker regressions. Unpaced workers have no rate target. Pacing and start/finish overhead make exact target equality inappropriate; report the rate fractions and declare any admission tolerance rather than silently treating a shortfall as success.

`--iterations` remains the same fixed work budget for every worker. Consequently, unequal rates give unequal nominal durations: 15 seconds for T1/T4 and 60 seconds for T2/T3 in the example. Early finishers stop competing for CPU, so these finite-work averages do not establish common-duration steady-state admission, arrival backlog, or application tail latency. A realistic workload remains the next validation gate.

### Output, checks, and interpretation

The JSON record includes elapsed wall time, completed increments per second, each thread's completion time and CPU time, expected/start/end CPUs, and per-thread CPU percentage over the common wall interval. `scheduled_cpu_equivalents` is summed worker CPU time divided by wall time, not physical-core utilization; SMT siblings are not independent full-capacity cores.

Worker setup and affinity occur before the measured phase. Wall time includes start/finish synchronization; short runs can be dominated by that overhead. Worker CPU times exclude setup and final synchronization. Completion times are measured from the common start, allowing the sharing pair and displaced work to be examined separately.

The program checks singleton affinity and observed CPU at setup and completion, validates every final counter against the requested work, and rejects unavailable CPUs, inconsistent topology/cache-line sizes, non-lock-free counters, and failed affinity operations. It does not continuously monitor external changes or reserve CPUs from unrelated workloads.

First functional checks on Linux should cover all four placement/layout combinations, duplicated CPU slots for time-sharing, invalid same-core A/B groups, and CPUs outside the allowed mask. For file output, check repeated appends, valid JSON on each line, empty stdout, and a nonzero exit for an inaccessible output path. Failed runs must not be interpreted as measurements. User-run matched performance comparisons are recorded in [results](results.md); those runs do not establish that all argument-rejection or file-error checks passed.

For rate control, first try `--iterations 1000 --rate 1000 --batch 100`: every worker should complete exactly 1,000 increments and take at least approximately one second. Also test a rate the device cannot sustain, verifying that all work completes after the nominal duration and achieved rates are lower than the target. Without `--rate`, no pacing sleeps should occur. Pacing runtime checks have not been executed by the assistant.

### Counter collection and portable context

For the lower-load physical-Linux experiment, add `--pmu` to every condition:

```bash
"$BIN" --cpus 0,0,1,1 --placement baseline --layout shared \
  --iterations 600000000 --rate 10000000 --batch 50000 \
  --pmu --output physical-linux-results.txt
```

Use `0,0,1,1` only after verifying CPUs 0 and 1 are eligible and belong to distinct physical cores. Repeat with colocated/shared, baseline/padded, and colocated/padded using identical work and instrumentation settings. No separate `perf` executable or library is needed for built-in collection; Linux kernel headers provide the perf API.

With the default generic profile, each thread's `pmu.events` records these requested event pairs:

| Pair | Purpose |
| --- | --- |
| `cycles`, `instructions` | Retired instructions and user-space CPU cycles; paired IPC estimate. |
| `branches`, `branch_misses` | Branch activity and misprediction counts. |
| `cache_references`, `cache_misses` | Kernel-defined generalized cache counters; commonly LLC-related, but processor-dependent. |
| `l1d_load_accesses`, `l1d_load_misses` | Kernel-defined L1 data-cache read events. |
| `llc_load_accesses`, `llc_load_misses` | Kernel-defined last-level-cache read events. |
| `dtlb_load_accesses`, `dtlb_load_misses` | Data-TLB read events. |
| `dtlb_store_accesses`, `dtlb_store_misses` | Data-TLB write events. |
| `itlb_accesses`, `itlb_misses` | Instruction-TLB events. |

Events are opened disabled after affinity setup, started around the workload, stopped before final affinity checking, and read/closed after the finish barrier. Collection follows the worker thread, excludes kernel/hypervisor execution, and includes user-space pacing and measurement bookkeeping. Groups start/stop sequentially, so not all groups have identical boundaries; setup and final counter reads are outside the worker completion/CPU interval. Enabling groups contributes CPU/wall overhead, and stopping contributes finish-barrier overhead. Compare like instrumentation settings, particularly for short runs.

Small pairs avoid requiring every requested event to fit the PMU simultaneously. Linux may multiplex the groups. Each usable event reports `raw_count`, `time_enabled_ns`, `time_running_ns`, `running_fraction`, and `scaled_count = raw_count * time_enabled_ns / time_running_ns`. Scaled values estimate totals; low coverage and phase-sensitive sampling can bias them. Related events are grouped together for derived ratios and differences, but different pairs can observe different execution windows.

Denied/unsupported events, enable/disable failures, incomplete reads, zero running time, inconsistent timing, and close failures have `status: "unavailable"`, null count fields, and an operation/reason/errno where applicable. Other counters and the workload continue. Without `--pmu`, the per-thread `pmu` field is null. A successful workload is not proof that counters were available.

`ipc_estimate` uses paired instructions/cycles. `l1d_load_hits_estimate` and `llc_load_hits_estimate` use paired accesses minus misses with multiplexing scaling. They have null values with reasons when pairs are unavailable or misses exceed accesses. Their names deliberately identify estimates: generic cache events need model-specific validation before interpreting the subtraction as exact hits. Generic cache misses or TLB misses are not HITM or proof of a false-sharing pair. Atomic read-modify-write attribution can also be processor-dependent.

Always-collected `getrusage(RUSAGE_THREAD)` deltas report voluntary/involuntary context switches and minor/major page faults around the workload, including pacing and instrumentation bookkeeping. These are not runnable-wait measurements or per-thread active-memory-footprint estimates.

Each record's `metadata` includes UTC recording time, build timestamp, original arguments, kernel release/version, architecture, host, allowed/online CPUs, raw `/proc/cpuinfo`, `perf_event_paranoid`, SMT state, and selected CPU core/package/sibling information. Governor and `scaling_cur_freq` observations are captured before worker creation and after joining; min/max policy frequencies are recorded at output time. Frequencies are in kHz and are endpoint observations, not a frequency history or an average during execution. A build timestamp does not identify a Git commit; retain source revision alongside the results.

Optional metadata uses `{ "value": ..., "error": null }` or a null value with an explicit error/errno. Text is JSON-escaped; metadata files exceeding the 64-KiB buffer or containing embedded nulls are unavailable rather than silently truncated. Metadata is collected outside the measured workload.

If permissions deny counters, inspect the recorded `perf_event_paranoid` and error fields. Access depends on the kernel, security policy, and capabilities such as `CAP_PERFMON`; the program does not change those policies or elevate privileges. Any permission change must be an explicit host-administration decision.

On the Linux device, validate a short PMU-enabled run: confirm valid JSON, per-thread counts or explicit unavailable reasons, positive running time for usable events, and null unavailable counts. Cross-check cycles/instructions with supported external `perf` measurements, accounting for their broader whole-process scope. Validate a permission-restricted run as well, and ensure all worker counters still reach the expected total even when no PMU event is usable. These native PMU checks remain unexecuted by the assistant.

The generic profile does not collect HITM. Runnable waiting, service tail latency, and whole-machine utilization are not collected internally. Use supported external tools where needed and record their scope; whole-process perf measurements include setup/teardown outside the JSON timing interval. Repeat and randomize conditions, keep inputs and machine policy matched, and report regressions or null results rather than assuming co-location wins.

### Verified Intel N150 native profile

The user verified these three native selectors with `perf stat` on the Intel N150. Their encodings below come from that device's `perf list --details`, not from another Intel model. Select them explicitly with `--pmu-profile intel-n150`; this enables collection without also supplying `--pmu`. The program checks `/proc/cpuinfo` for the Intel vendor and N150 model before using raw encodings. `--pmu` alone and `--pmu-profile generic` retain the original generic counter requests.

| JSON event name | Meaning | Raw config | Config1 |
| --- | --- | --- | --- |
| `mem_load_uops_retired.l1_miss` | Retired load micro-operations missing L1 data cache | `0x08d1` | `0` |
| `itlb_misses.walk_completed` | Completed instruction-fetch page walks | `0x0e85` | `0` |
| `ocr.demand_rfo.l3_hit.snoop_hitm` | RFO/software exclusive-prefetch requests receiving modified data on the L3-hit snoop path | `0x01b7` | `0x10003c0002` |

The native L1 event replaces `l1d_load_misses`. The native walk and RFO/HITM events replace the generic `itlb_accesses`/`itlb_misses` pair, keeping the total at 16 counters. All remaining generic requests, pair scheduling, coverage reporting, and explicit failure handling are unchanged. The native event names identify their actual meanings: walks are not ITLB accesses, and retired-load misses are not silently relabeled as generic cache misses.

`l1d_load_hits_estimate` is null with an explicit reason in this profile, because generic accesses and native retired-load misses are not assumed to form a matched pair. IPC and the generic LLC estimate retain their existing definitions. `pmu.profile` and `metadata.pmu_profile` identify the chosen profile, and every event reports `type`, `config`, and `config1`, including unavailable events. The RFO filter is passed through `perf_event_attr.config1`.

From the directory containing `static-placement.c` on Linux:

```bash
cc -std=c11 -O3 -pthread static-placement.c -o static-placement
./static-placement --cpus 0,0,1,1 --placement baseline --layout shared --iterations 600000000 --rate 10000000 --batch 50000 --pmu-profile intel-n150 --output physical-n150-results.txt
```

Use the same profile for all four placement/layout conditions. First run a short check with `--iterations 5000000` and no `--rate`, confirming all three native fields report `status: "ok"` and positive running time. Successful standalone `perf stat` requests do not establish that the benchmark's grouped collection works. The user has since compiled and validated integrated collection through a smoke check and matched comparisons; see [results](results.md) for the measured evidence and remaining limitations.

The HITM event covers a specific L3-hit response path and does not identify a cache-line address or previous writer in this counting mode. Zero is a valid count and does not rule out false sharing on paths outside the filter. Do not equate this counter with all coherence traffic.

## 9. Sources and implementation references

- [Linux v6.12 sched_ext documentation](https://github.com/torvalds/linux/blob/v6.12/Documentation/scheduler/sched-ext.rst): configuration, enrollment, CPU-selection semantics, and interface stability.
- [Linux v6.12 sched_ext implementation](https://github.com/torvalds/linux/blob/v6.12/kernel/sched/ext.c): callback helper availability and dispatch restrictions.
- [Intel Skylake event definitions](https://github.com/intel/perfmon/blob/main/SKL/events/skylake_core.json): an example of model-specific PMU semantics; pin the event database revision used for experiments.
- [perf c2c manual](https://man7.org/linux/man-pages/man1/perf-c2c.1.html): supported observation facilities and cache-line analysis.
- [Linux v6.12 THP documentation](https://github.com/torvalds/linux/blob/v6.12/Documentation/admin-guide/mm/transhuge.rst): page coverage, translation effects, and allocation tradeoffs.
- [SHERIFF paper](https://people.umass.edu/tongping/pubs/sheriff-oopsla11.pdf): runtime mechanism and historical benchmark findings.
- [Cheetah publication](https://ieeexplore.ieee.org/document/7559527): false-sharing detection and optimization guidance.
- [Linux v6.12 workqueue documentation](https://github.com/torvalds/linux/blob/v6.12/Documentation/core-api/workqueue.rst): threaded, bound, unbound, and softirq execution contexts.
- [perf_event_open manual](https://man7.org/linux/man-pages/man2/perf_event_open.2.html): thread attachment, event encodings, group scheduling, permissions, and multiplexing read formats.
- [getrusage manual](https://man7.org/linux/man-pages/man2/getrusage.2.html): per-thread context switches and page-fault statistics.
