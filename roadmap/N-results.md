# N — results

**Status:** campaign/report tooling implemented on `codex/lmn-completion`; final qualified comparison pending. **Branch:** `results`.
**Milestones:** finish 31, then 32.
**Start:** [L](L-application-assembly.md) + [J](J-benchmark-harness.md) for smoke
work; accepted measurements require [M](M-correctness-gate.md)'s qualified commit.
Follow the [shared workflow](README.md).

Implementation and candidate evidence: [N handoff](../docs/branches/results.md)
and [validation summary](../docs/validation/lmn.md).

## Outcome and ownership

Deliver repeated raw measurements, an evidence-based comparison and complete
project instructions. After J/L freeze, own benchmark outputs/report and final
run/results documentation. Coordinate harness fixes with J and implementation
fixes with module owners; a behavior change returns to M for qualification.

Read J's schema/scenarios, H's metric definitions, L's actual CLI and M's validation
record. Benchmark coverage can be narrower than correctness-platform coverage,
but the report must identify the measured hosts and avoid extrapolating results
to unmeasured operating systems or workloads.

## Execution plan

### 1. Verify the measurement candidate and setup

Check the binary matches M's qualified commit and Release configuration. Record
compiler/options, OS, CPU/RAM, power/environment constraints, socket limits/backend,
worker/helper counts, cache/buffer budgets, deadlines, logging and origin/generator
placement. Use equivalent settings between driver comparisons.

Run J's smoke scenario families in both modes and inspect status/body checks,
cache preparation, counters and output schema. Incorrect responses invalidate
performance claims even when reported throughput is high. Fix setup/schema issues
before expensive repeats; do not replace missing real runs with sample data.

### 2. Execute and retain repeated workloads

Run the documented scenario matrix within host limits, covering the required
concurrency, worker and object-size dimensions and the server/proxy/cache families.
Define warmup, measurement interval and at least five repeats per reported
configuration. Save random seeds and consider alternating run order so thermal
or background-load drift does not always favor the same driver.

Reset application cache for cold measurements, prefill warm hits and label OS
filesystem-cache state independently. Verify origin/validation counts against
the intended scenario. State which matrix points could not run because of host
limits and preserve failure reasons rather than reporting zero performance.

Retain raw per-repeat records, latency data/histograms, configuration and bounded
diagnostics in a documented results directory. Use unique run IDs; derived tables
and plots must be regenerable from those files. A repeated failed trial remains
in the raw record with its failure status.

### 3. Analyze useful work and variation

Report successful requests/s, useful representation bytes/s, p50/p95/p99 latency,
errors/timeouts, CPU and peak/sampled memory, active connections/queue peaks,
cache hit/miss/bypass/eviction counts and validation traffic. State denominators,
measurement windows and units. Do not count truncated bodies or error responses
as successful resource delivery.

Present per-run variation using a documented statistic. Do not average percentile
values and call the result a global percentile; either report per-run percentiles
or recompute from compatible underlying samples/histograms. Keep closed-loop
concurrency and open-loop arrival-rate results distinct and explain generator
queueing/saturation and shared-host contention.

Create throughput-versus-concurrency and p99-versus-concurrency plots, plus useful
CPU/memory/cache observations. Use standard plotting tools and save source scripts,
units, legends and standalone outputs. Mark excluded/unavailable points rather
than interpolating fabricated measurements.

### 4. Explain the observed tradeoffs

Connect measured behavior to the architecture: fixed workers occupied by slow
clients; one loop's CPU budget; poll/WSAPoll scan costs; helper saturation;
file-cache state; cache locking/pinning; origin or load-generator limits. Treat
these as hypotheses until counters/profiling/results support them.

Compare like-for-like resource settings and explicitly distinguish one event-loop
thread from a multiworker CPU budget. Do not declare one model universally faster.
If measurements reveal a correctness defect, stop using the affected results,
fix through the owner, requalify in M and rerun affected configurations.

### 5. Complete the developer handoff

Finish build requirements, exact executable paths, configuration reference,
server/proxy examples, driver/cache selection, test commands, benchmark smoke/full
commands, result reproduction and known limitations. Explain deferred protocol
features and uncancellable resolver shutdown behavior using actual implementation
guarantees. Preserve and reconcile local architecture documentation if present;
essential instructions must live in tracked files.

Have a clean-checkout reproduction follow one server example, one cache proxy
example, both driver choices, the correctness gate and one saved workload. Record
what actually ran. Instructions mentioning planned APIs/options must be replaced
with the implemented ones before final delivery.

## Exit and final evidence

- [ ] Milestone 31 smoke coverage and milestone 32 measured repeats are recorded
  separately; raw measurements support every performance claim.
- [ ] Results include errors, variation, machine/configuration details, plots and
  scripts to regenerate them.
- [x] M's platform gate remains valid for the delivered implementation commit.
- [ ] `docs/branches/results.md` links final run/config/test/result documentation
  and records excluded host-limited workloads explicitly.
- [ ] All A–N implementation and integration work is included in the delivered
  history; merging it requires no additional feature wiring or invented evidence.
