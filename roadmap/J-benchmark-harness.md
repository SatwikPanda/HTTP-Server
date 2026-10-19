# J — benchmark-harness

**Status:** planned. **Branch:** `codex/benchmark-harness`.
**Milestone:** workload/tooling preparation for 31.
**Start:** [P0](P0-parallel-foundation.md). Smoke runs need
[L](L-application-assembly.md); accepted measurements need
[M](M-correctness-gate.md) and are produced by [N](N-results.md).
Follow the [shared workflow](README.md).

## Outcome and ownership

Deliver reproducible workload definitions, a controlled origin/client setup,
automation scripts, machine-readable output and instructions under `benchmarks/`.
Benchmark tooling may use external test-only tools; production runtime dependencies
remain unchanged. Reuse [I](I-test-harness.md)'s controlled origin where suitable,
but do not couple benchmark execution to assertions that distort the workload.

Read the architecture's benchmark plan, H's metric definitions and L's actual
CLI. Do not hard-code speculative option names. A script must reject an unsupported
configuration instead of silently running defaults that invalidate the comparison.

## Implementation order

### 1. Define scenario manifests

Use a versioned, documented machine-readable format. Each scenario identifies
application/driver, object set, request distribution, connection reuse, cache
state, origin behavior, concurrency/arrival pattern and measurement phases.
Track random seeds where workload ordering is randomized.

| Dimension | Required coverage |
| --- | --- |
| Clients | Concurrency 1/10/50/100/500/1000 subject to documented host limits |
| Pool workers | 1/2/4/8/16, plus relevant available CPU counts when useful |
| Object bytes | 1 KiB/64 KiB/1 MiB/16 MiB |
| Connections | Reused connections and one request per connection |
| Files | Repeated hot file and varied files with filesystem-cache state recorded |
| Proxy | Cache disabled, cold, warm, revalidation and mixed popularity |
| Slow work | Delayed origin, throttled response stream and slow receiving client |

Define a practical matrix of scenario families rather than an unexplained
explosion of every Cartesian combination. Record which cases are required,
optional or excluded for measured host limits. Both drivers must see comparable
files, origin behavior, budgets, timeouts, logging and release optimizations.

### 2. Automate isolated startup and cleanup

Build/run the selected executable, temporary file tree and local origin with
explicit readiness checks and bounded startup time. Record actual listening
endpoints and process identities. On failure, stop only processes/resources
created by the run and preserve bounded diagnostics. Never kill unrelated processes
or delete a guessed working directory.

Use explicit cache reset or restart for cold cases; prefill and verify expected
origin/hit counts for warm cases. Document whether warmup populates a cache that
the measurement calls cold. Reset it again before measurement when necessary.
Treat OS filesystem-cache state independently from the application's HTTP cache.

### 3. Implement measurement phases and correctness checks

Define warmup and measurement intervals (for example 10 s and 60 s), then at least
five measured repeats for final runs. A short smoke mode can use smaller intervals
to verify setup and result shape; label it clearly as smoke evidence.

Record client-observed completion latency from a consistent start point to full
response completion. Validate status, representation length/content and expected
HEAD/no-body semantics. Incorrect, truncated, dropped or timed-out responses are
errors, never successful throughput. Distinguish request attempts from completions
and useful representation bytes from wire framing bytes.

Closed-loop fixed-concurrency clients issue new work after completion. An open-loop
arrival-rate workload uses scheduled issue times and must record queued/delayed
issuance and overload; do not present one model as the other. Document how latency
measurement includes or excludes generator-side waiting to avoid hiding overload.

Monitor generator CPU, connection limits and achieved arrival rate. A saturated
generator or shared-host origin may cap throughput before the server does. Use
separate generator hardware when available, otherwise record contention explicitly.

### 4. Save raw results before summarizing

Each run stores unique run ID, scenario/version/seed, commit/tree state, full
command/config, compiler/build type, OS, CPU/RAM, socket limits/backend, worker/
helper counts, origin placement and filesystem/cache preparation. Save attempted,
successful, erroneous and timed-out request counts and measurement duration.

Persist latency samples or an aggregatable bounded histogram with its precision,
useful bytes, process CPU/memory observations, queue/connection peaks and cache/
validation counters. Save raw repeats separately; never overwrite earlier results
with a summary row. Incomplete runs have explicit failure state and diagnostics.

Do not average p99 values as though they were a combined distribution. N can
report per-run variation or merge compatible histograms to compute a defined
aggregate. Record unavailable metrics as unavailable rather than zeros.

## Verification and handoff

Smoke every scenario family with both drivers after L. Compare known small bodies
and expected origin counts; intentionally inject a wrong/truncated response and
confirm it is counted as failure. Verify reset/prefill reproducibility, deterministic
seed behavior, cleanup on interruption and output schema validation.

Use `milestone31` for the small harness acceptance check, with a bounded CTest
timeout appropriate to smoke mode. Full performance runs are explicit scripts,
not an hours-long default correctness test. Before L, script/schema preparation
does not establish runnable benchmark acceptance.

- [ ] Document one command for smoke, one for a selected scenario/repeats, and the
  exact environment/setup prerequisites in `benchmarks/`.
- [ ] `docs/branches/benchmark-harness.md` records actual smoke coverage and
  limitations; N takes output/report ownership after the harness freezes.
- [ ] No performance comparison is claimed from generated sample or smoke data.
