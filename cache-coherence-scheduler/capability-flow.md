# Cache-Coherence Scheduler: Capability Flow

This diagram explains the proposed capability, not an implemented system. The goal is improved useful throughput and latency, not minimum HITM or maximum CPU utilization in isolation.

```mermaid
flowchart TD
    Setup["Configure managed tasks, allowed CPUs,<br/>application objectives and regression limits"]
    Setup --> Platform["Verify observation capabilities,<br/>physical topology and actuation support"]

    subgraph Observe["Observe"]
        Samples["Supported perf/eBPF observations:<br/>contention, execution demand, cache/TLB pressure"]
        Known["Known benchmark or source-verified<br/>sharing relationships"]
        App["Application throughput and latency,<br/>effective placement and runnable demand"]
        Evidence["Aggregate windowed evidence:<br/>scoped line/task identities, quality and uncertainty"]
        Samples --> Evidence
        Known --> Evidence
        App --> Evidence
    end

    Platform --> Samples
    Platform --> Known
    Platform --> App

    subgraph Decide["Compare eligible placements"]
        Ready{"Required evidence available<br/>and candidate authorized/eligible?"}
        Compare["Compare current placement, SMT siblings,<br/>one logical CPU and spreading across cores"]
        Benefit{"Expected useful-work benefit exceeds<br/>migration, lost parallelism, interference<br/>and uncertainty costs?"}
        NoChange["Make no new intervention;<br/>record reason and continue observing"]
        Live{"Live actuation enabled?"}
        Advisory["Record proposed placement and rationale;<br/>do not change placement"]
        Evidence --> Ready
        Ready -->|No| NoChange
        Ready -->|Yes| Compare
        Compare --> Benefit
        Benefit -->|No| NoChange
        Benefit -->|Yes| Live
        Live -->|No| Advisory
    end

    subgraph Evaluate["Trial, verify and reassess"]
        Apply["Revalidate and request supported placement:<br/>SCX or OS affinity, managed tasks only"]
        Applied{"Effective placement achieved?"}
        Failure["Report failed or partial actuation"]
        Outcome["Measure useful throughput, latency and overhead<br/>for affected and displaced managed work"]
        Accept{"Meets declared acceptance criteria<br/>and application guardrails?"}
        Retain["Retain placement and its rationale:<br/>falling HITM alone is not a release signal"]
        Restore["Restore/revise controller-owned placement<br/>within current restrictions; report outcome"]
        Reassess["Continue outcome monitoring;<br/>reassess on changes or scheduled review/probe"]
        Live -->|Yes| Apply
        Apply --> Applied
        Applied -->|No| Failure
        Failure --> Restore
        Applied -->|Yes| Outcome
        Outcome --> Accept
        Accept -->|No| Restore
        Accept -->|Yes| Retain
        Retain --> Reassess
        Reassess -->|Outcome checks| Outcome
        Reassess -->|Placement reassessment| Evidence
    end

    NoChange --> Evidence
    Advisory --> Evidence
    Restore --> Evidence
```

**Observation is evidence, not proof:** HITM samples can nominate contention relationships but do not alone identify the previous writer or establish false sharing. Known sharing groups are valid initial inputs. Required fields and counter paths must be verified on the selected CPU/kernel; missing data is not zero pressure.

**Placement is a measured intervention:** SMT siblings share a physical core, while one logical CPU serializes execution. Any relocation of unrelated work must be authorized and included in the benefit/guardrail evaluation. SCX CPU selection and queue insertion do not guarantee immediate co-execution.

**Feedback must account for success:** consolidation can remove the HITM signal that triggered it. Retention and release depend on application outcomes, the recorded rationale, and explicit reassessment rules, not signal disappearance alone. Restoration cannot reverse latency already incurred, and failed/partial changes must remain visible.

Linux/`sched_ext` is a candidate backend; initial experiments may use existing affinity controls. This diagram does not require the full multidimensional cost model or arbitrary-program detection before controlled placement experiments.

Related documents: [requirements](requirements.md), [system architecture](system-architecture.md), and [experimental methodology](experimental-methodology.md).
