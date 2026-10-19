# H — config-observability

**Status:** planned. **Branch:** `codex/config-observability`.
**Milestones:** 29 and portable stop hooks for 28.
**Start:** [P0](P0-parallel-foundation.md). Full startup/metrics acceptance occurs
in [L](L-application-assembly.md). Follow the [shared workflow](README.md).

## Outcome and ownership

Provide validated configuration, bounded observability and portable stop requests.
Own `include/core/config.hpp`, `src/core/config.cpp`, `include/observability/`,
`src/observability/`, related tests and `docs/configuration.md`. P0 reserves exact
shared option types and implementation paths. Private core platform adapters for
console/signal hooks also belong to H. L owns `src/main.cpp` and process composition.

Read current `src/main.cpp`, `tests/cli_validation_test.cmake`, `core/error.hpp`,
`core/clock.hpp`, and P0's configuration/metric contracts. Preserve existing CLI
behavior unless a documented migration is part of the agreed final interface.
The option spellings below describe required concepts; use P0's frozen spellings.

## Implementation order

### 1. Parse configuration without starting services

Separate argument parsing, validation/default application and resource startup.
Return a typed config or `core::Error`; `--help` must not open a listener or launch
workers. Validate numeric values with checked conversions and full-string parsing,
rejecting signs/overflow/trailing junk where disallowed.

Cover application mode, concurrency mode, bind address/port, document root,
worker/helper counts, cache enablement and object/resident/in-flight budgets,
accept/job/completion/session/upstream limits, HTTP buffer limits, deadlines and
maximum requests per keep-alive connection. Keep loopback as the default binding;
remote exposure requires an explicit bind choice.

Validate relationships as well as individual ranges: high-water above low-water,
nonzero useful windows/work budgets, consistent object/storage limits, supported
mode-specific fields and duration arithmetic. Define whether irrelevant options
are rejected or ignored and make help match that decision. Validate root existence
through the file/startup path, outside an active event-loop iteration.

Do not invent a config-file format or environment override layer unless needed
by the agreed scope. Defaults live in one shared typed source, not duplicated in
CLI, service factories and help text.

### 2. Define metric meaning before recording counters

Use bounded atomic/per-owner aggregation or another small synchronized design.
Record each outcome at an agreed lifecycle boundary to avoid double counting
retries or counting a response complete when only its head was sent.

| Metric group | Semantics to document |
| --- | --- |
| Requests | Accepted/started/completed/error; client cancellation; final status |
| Transfer | Useful representation bytes versus wire bytes; attempted versus completed writes |
| Time | Monotonic request/queue/origin latency; units and histogram boundaries |
| Capacity | Active/queued/connecting counts, paused producers, rejections and timeouts |
| Cache | Hits, misses, bypasses, insertions, evictions, duplicate misses and validation outcomes |
| Memory | Resident, retained/pinned, candidates and enforced total; overlapping reporting categories |
| Process | CPU time/utilization definition, sampled/peak memory and sampling limitations |

No arbitrary URL/key/header labels or unbounded request histories. Use fixed
dimensions and bounded histograms. A metrics snapshot must not hold cache locks
or block the loop while writing output. Keep a no-op sink valid for regression
tests and apply consistent enabled/disabled settings during benchmarks.

### 3. Implement bounded logging and process sampling

Enqueue bounded structured records to a separate writer, with a documented drop
or aggregation policy at capacity. Count dropped records and avoid allocating
unbounded strings before checking admission. Slow output destinations must not
stall the loop. Bound shutdown flushing and document incomplete logs if dropped.

Avoid logging credentials, cookies, raw authorization fields or unnecessary query
contents; normal diagnostic identifiers and portable error/native codes suffice.
Retain enough context to identify operation/connection/transaction failures.
Process CPU/memory sampling uses private OS adapters and a bounded sampling rate;
unsupported measurements are unavailable, not zero.

### 4. Provide safe portable stop hooks

Expose a normal-thread stop request that L connects to the drivers' wake/drain
protocol. POSIX signal handlers and Windows console callbacks have different
execution restrictions. Use a minimal platform-safe notification to normal
control code; do not assume `Poller::wake()`, logging, mutexes or C++ allocation
are safe inside an asynchronous signal handler.

L owns registration lifetime and destroys hooks only when callbacks cannot touch
dead state. Repeated signals/requests must be idempotent. Handle partial startup,
already-stopped state and a stop arriving during teardown without deadlock.

### 5. Audit the application status contract

Coordinate with A/B/K/L rather than putting HTTP decisions inside logging. Check
200/304, 400/403/404/405/408, 413/414/417/431 and 500/501/502/503/504/505 outcomes.
No-body status/HEAD behavior and close-on-partial-response failures remain shared
writer rules. Preserve portable operation names/native codes for diagnostics
without leaking local internals into generated HTTP bodies.

## Verification and handoff

Test parser/defaults and every invalid/boundary option combination without socket
startup. With L, execute each documented startup example and verify actual mode,
driver, bind and cache selection. Use a small known workload to reconcile request,
byte, cache and error totals; test repeated stop and slow/full logging queues.

Run `milestone29`, hook cases under `milestone28`, the existing CLI regression
and base/P0 gate. Platform hooks require real Windows/POSIX execution in M;
portable parser tests alone do not establish signal behavior.

- [ ] Shared factories receive validated typed config with one set of defaults.
- [ ] L receives CLI APIs, metric sinks, hook lifetime rules and examples.
- [ ] `docs/branches/config-observability.md` records actual checks, counter units,
  logging overflow policy and unsupported sampling/hook coverage.
