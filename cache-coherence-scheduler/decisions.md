# Cache-Coherence Scheduler: Development Decisions

Record concrete design and implementation choices as we develop the capability: what we chose, why, and which alternative we set aside. This is not a summary of requirements, research principles, or possible future approaches.

Keep entries short. Add an entry when a choice is actually made, not merely suggested. If a decision changes, add a superseding entry and reference it from the old one.

Required behavior belongs in [requirements](requirements.md); progress, results, blockers, and open choices belong in [checkpoints](checkpoints.md).

## D-01: Start with static placement before dynamic scheduling

Date: 2026-11-19. Status: accepted.

**Decision:** Build the first version around fixed thread placement for each experiment run, rather than a controller that changes placement while the workload executes.

**Why:** This provides a cheap, verifiable way to test whether colocating falsely sharing threads helps when CPU capacity permits. It lets us find basic placement and measurement bugs without also debugging an online scheduling loop.

**Alternative set aside:** Implementing dynamic detection, admission, migration, and feedback before validating static placement effects.

**Boundary:** This decision does not select an OS, affinity API, launcher, benchmark, or source-rewriting approach. Those choices will be recorded when made.

## D-02: Use Linux and a controlled four-thread benchmark first

Date: 2026-11-19. Status: accepted.

**Decision:** Implement the first static version for Linux with a controlled four-thread/two-core benchmark.

**Why:** This was explicitly selected before implementation. Known sharing and useful-work counts let us isolate placement effects without first diagnosing an arbitrary application.

**Alternative set aside:** A native Windows backend or an existing application as the first workload. The actual Linux execution host remains to be selected and validated.

## D-03: Set per-thread affinity before timing

Date: 2026-11-19. Status: accepted.

**Decision:** Use `pthread_setaffinity_np` inside the benchmark, explicit logical-CPU slots, and synchronized worker startup. Swap threads 1 and 3 between baseline and co-located configurations.

**Why:** This avoids rewriting application logic or building a general launcher, preserves two workers per physical core, and supports both SMT siblings and single-logical-CPU time-sharing without conflating them.

**Alternative set aside:** Automatic CPU selection, exclusive core reservation, and live migration.

## D-04: Use known counter sharing and a matched padded control

Date: 2026-11-19. Status: accepted.

**Decision:** Give each worker its own lock-free relaxed atomic counter; place counters 2 and 3 on one cache line or separate lines while keeping the allocation budget fixed.

**Why:** Repeated updates remain observable, the intended false-sharing relationship is known, and placement/layout can vary without changing the amount of requested work.

**Alternative set aside:** Automatic detection, a true-sharing counter, and a memory-density study in this first benchmark.

## D-05: Report raw run metrics without an automatic placement verdict

Date: 2026-11-19. Status: accepted.

**Decision:** Emit JSON containing useful-work throughput, per-thread completion/CPU times, placement checks, and profiling identities. Add compute and optional idle knobs; leave PMU collection external.

**Why:** This supports repeatable comparisons and utilization sweeps without pretending CPU time is physical-core capacity or that unsupported hardware evidence exists.

**Alternative set aside:** A hardcoded admission threshold, online policy, or success claim based solely on lower coherence counts.

**Update:** D-10 supersedes the external-only PMU choice. Reporting raw measurements without an automatic placement verdict remains unchanged.

## D-06: Skip the Linux target during Windows-host configuration

Date: 2026-11-19. Status: accepted.

**Decision:** CMake reports that the benchmark is skipped on non-Linux targets instead of failing configuration.

**Why:** Visual Studio can configure the Windows host while the folder is open. The reported fatal error obstructed that workflow without improving Linux validation.

**Alternative superseded:** Failing the entire configuration on a non-Linux target. This does not add a Windows implementation or make the target available there.

## D-07: Support portable text results with an explicit output file

Date: 2026-11-19. Status: accepted.

**Decision:** Add `--output PATH` to append the existing JSON result as one line in a text file after timing and result validation. Keep stdout as the default when the option is omitted, and report file open, write, and close failures as run failures.

**Why:** Experiments will run on another device, and a single file containing repeated runs can be copied back for analysis without overwriting earlier measurements. JSON is already text and preserves the existing analysis fields.

**Alternative set aside:** A second human-readable report format, automatic filenames, and silently replacing an existing results file.

## D-08: Pace each worker toward a requested rate without dropping work

Date: 2026-11-19. Status: accepted.

**Decision:** Add optional `--rate N`, measured in increments per second per worker. Pace completed batches against absolute monotonic deadlines from the common start. Behind-schedule workers continue without a pacing sleep; every worker still completes its full `--iterations` budget.

**Why:** The user selected 500 million increments/s per worker and confirmed 7.5 billion increments per worker: 30 billion total, nominally 15 seconds, with longer execution permitted when the target cannot be sustained. Report the requested rate, nominal work duration, and achieved per-worker rates rather than claiming hardware can meet the request.

**Boundary:** This is batch-level pacing of the existing counter workload, not an independent arrival generator or a queue of external requests. Rate-control overhead is included in timing. Without `--rate`, the workload remains unpaced.

**Alternative set aside:** Stopping at a fixed deadline, discarding unfinished work, or interpreting the target as an aggregate rate.

## D-09: Use physical Linux for the next placement measurements

Date: 2026-11-19. Status: accepted.

**Decision:** Run the next experiments on Linux booted directly on physical hardware, with explicit topology and allowed-CPU checks. The user reports four online/allowed CPUs with one thread per core; select time-sharing slots such as `0,0,1,1` rather than assuming SMT siblings exist.

**Why:** WSL observations remain evidence for that environment, but did not establish the intended physical-core relationship. Direct Linux makes that placement control easier to verify.

**Boundary:** Physical Linux does not eliminate frequency, background-load, pacing, or statistical concerns.

## D-10: Log optional per-thread PMU measurements and self-contained context

Date: 2026-11-20. Status: accepted.

**Decision:** Add `--pmu` for user-space counters attached to each worker. Request cycles/instructions, branches/misses, generalized cache references/misses, L1D/LLC load accesses/misses, data-TLB load/store accesses/misses, and instruction-TLB accesses/misses. Schedule related counters in pairs and report raw counts, enabled/running times, and multiplexing estimates.

**Why:** Results will be collected on another physical device and transferred for analysis. The user explicitly chose to continue with usable counters while logging unavailable reasons. Include kernel/build/invocation/CPU/topology context, before/after governor/frequency observations, and unprivileged context-switch/page-fault deltas in each record.

**Boundary:** Missing, denied, unscheduled, or failed counters are unavailable with null counts, not zeros. Derived cache hits are labeled estimates, not model-validated hits or HITM. PMU collection is opt-in; enabling counters adds measurement overhead.

**Alternative set aside:** Requiring every requested event, silently changing host permissions, or collecting whole-process counters that include thread setup and teardown.
