# I — test-harness

**Status:** planned. **Branch:** `codex/test-harness`.
**Milestone:** shared fixture/matrix preparation for 30.
**Start:** [P0](P0-parallel-foundation.md). Real combined execution uses
[L](L-application-assembly.md); [M](M-correctness-gate.md) records platform acceptance.
Follow the [shared workflow](README.md).

## Outcome and ownership

Provide reusable controlled I/O fixtures and one system suite parameterized by
driver/application mode. Own `tests/fixtures/`, `tests/system/`, their local CMake
and launch scripts. P0 seeds usable fixture entry points; extend those compatibly.
Feature owners keep their tests in their own module paths. L owns root test
wiring/CI until its explicit handoff to M.

Read `tests/test_support.hpp`, existing milestone adapters, session/parser/response
tests and the CMake registration function. Use Release-active CHECK assertions.
Compile-only `tests/contracts/` objects remain separate from runtime acceptance.

## Implementation order

### 1. Define fixtures with explicit lifetimes

Use RAII owners for the temporary document root, origin listener, client sockets,
threads and stop controls. Bind loopback port zero and retrieve the actual bound
port rather than choosing fixed ports. Initialization returns explicit failure;
teardown requests stop, wakes waits and joins before destroying networking runtime.

Use per-test temporary directories and clean up only paths created by that
fixture. On Windows, permission/symlink setup may require capabilities unavailable
on the host; report unavailable coverage rather than weakening the assertion.
Do not require external DNS/origins.

### 2. Extend the controlled origin

Describe a response script as owned bytes/segments, framing choice, metadata,
barriers and terminal behavior. Support fixed/chunked/close-delimited, interim
heads, no-body responses, malformed/truncated wire data, slow or blocked delivery,
changed validators/representations and controlled connection loss.

Record accepted connections and exact request method/target/fields under bounded
storage. Tests must be able to wait until a request reaches a named barrier and
inspect origin counts without guessing elapsed time. Allow release/cancel from
the test thread; avoid a fixture that hangs its own teardown after an assertion.

Use production network I/O where integration is being established. For invalid
wire cases, send deliberate raw bytes rather than using the production serializer
to build an invalid response it correctly refuses to serialize.

### 3. Add byte-stream and clock controls

Reusable I/O adapters can force short writes, would-block, EOF, delayed reads,
specific failures and a blocked helper operation. They must call the production
session/driver; do not return a precomputed cache or HTTP outcome in place of it.

Use ManualSteadyClock/SystemClock for deterministic deadline/freshness cases.
Do not mutate non-thread-safe manual clocks concurrently without synchronization.
For real network scheduling, use barriers/condition predicates and bounded waits;
reserve elapsed-time assertions for the actual deadline property being tested.

Expected outcomes must not depend exclusively on reparsing output with the same
production parser under test. Pair parser round trips with raw golden bytes,
known lengths/body hashes, field assertions and an independent fixture oracle.
This catches a serializer/parser pair making the same mistake.

### 4. Parameterize the production system suite

Define startup/factory adapters for minimal reference execution where applicable,
thread-pool and event-loop. Scenario assertions take an adapter and express one
behavior. Do not copy an entire proxy/cache suite into driver-specific variants.

Required combinations are static server × both drivers and proxy × both drivers,
with proxy cache off/on. Run containment, GET/HEAD/conditions, persistent/pipelined
ordering, all response framing, forwarding fields, cache policy/revalidation,
slow peers, overload, cancellation and shutdown. Record unavailable IPv6 separately.

Use real production application factories. An unavailable feature is pending;
an always-success mock session cannot satisfy its acceptance. Driver-only session
tests are useful but labeled as that narrower coverage.

### 5. Make coverage and missing tests visible

Maintain an inventory mapping milestone/track to required runtime test targets,
labels, mode combinations and platforms. P0 defines registration conventions;
feature owners add implemented targets. In strict mode a required missing suite,
exit 77 or empty selection must fail the gate rather than silently pass.

Include CTest timeouts and diagnostic output identifying the scenario/driver.
On a hang, preserve bounded progress counters, queued job/connection counts and
last transition information. Avoid dumping unbounded body/log data.

## Nuances and verification

Validate the fixture with the production demo/reference path before relying on
it for new features. Cover fixture cancellation/teardown and port reuse, but do
not build tests that merely mirror every helper implementation detail.

Exercise one event script across both drivers to expose different scheduling
without changing expected protocol semantics. Concurrent cache tests need barriers
at lookup, origin response and commit to reproduce replacement/version races.
Resource assertions should observe handles/threads/reservations returning to
baseline after teardown, not just successful response status.

System cases use `milestone30` plus applicable feature labels. Before L exists,
record fixture verification and pending system cases separately. Full milestone
30 remains M's cross-platform acceptance work.

## Done and handoff

- [ ] P0's fixture APIs are preserved or centrally migrated with affected owners.
- [ ] Real system scenarios share assertions across drivers and have bounded
  setup, waits and teardown.
- [ ] Required-suite inventory exposes missing implementations and coverage gaps.
- [ ] `docs/branches/test-harness.md` gives fixture usage, test registration,
  production dependencies and actual checks for L/M.
