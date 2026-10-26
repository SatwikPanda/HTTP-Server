# L — application-assembly

**Status:** implemented and integrated; LMN resource qualification added, final M platform gate pending. **Branch:** `application-assembly`.
**Milestones:** 24, 27–28 and combined acceptance of 23, 25–26, 29.
**Start:** incrementally after P0. **Finish:** A–I and K integrated.
Use [J](J-benchmark-harness.md) for smoke workloads, then hand off to
[M](M-correctness-gate.md)/[N](N-results.md). Follow the [shared workflow](README.md).

Current evidence: [integrated application handoff](../docs/branches/application-assembly.md).
The LMN follow-up retains every production factory/mode and adds repeated
lifetime qualification; see [the current validation record](../docs/validation/lmn.md).

## Outcome and ownership

Deliver the final executable composition and integration fixes so the merger
receives working modes rather than missing factory wiring. Own `src/main.cpp`,
root CMake composition, combined application lifetime code, CI wiring,
`tests/README.md`, shared run documentation and the validation record until the
explicit M/N handoffs. Module implementation fixes go through their owners.

Read current CMake/main, all factory/handoff contracts, NetworkRuntime/HttpServer
lifecycle and the track dependency table. Existing tests/minimal driver remain
regression tools. New production mode selection must not rely on a fixed-response
demo implementation pretending to serve files or proxy requests.

## Integration order

1. Record P0 and prerequisite SHAs in the integration manifest. Merge each ready
   A–J module and run its tests plus earlier gates before adding unrelated fixes.
2. Instantiate H's validated configuration, metric sink and stop controls with
   the networking runtime. Validate configuration before starting active drivers.
3. Create E's helper service and shared file/resolver adapters. Create optional
   cache storage/policy and the application factory: A for server, B for cache-off
   proxy, K for cache-enabled proxy.
4. Select F or G, injecting the same session/application factory and helper ports.
   Keep runtime and borrowed services alive until drivers and helpers terminate.
5. Connect CLI startup/help/errors, listening endpoint reporting, stop/wake/drain
   and metrics output. Test actual executable selection, not only factory unit tests.
6. Complete cross-feature acceptance and hand the stable assembled commit to M.

This describes ownership order, not a requirement to build a large service
container. Prefer ordinary RAII owners with explicit construction/destruction
order and narrow constructor inputs.

## Build and executable wiring

Maintain the current public targets and P0 module registration. Link every new
production target and acceptance executable explicitly. Check the graph remains
acyclic: drivers consume session interfaces, application factories sit above them,
and `http -> concurrency` must not gain a reverse dependency.

Verify one selected native networking backend, C++20/no extensions, warning flags,
Threads linkage and Windows ws2_32. Make a clean configure fail clearly if an
included feature lacks a prerequisite. Do not silently ship a binary omitting a
requested mode because a target was absent.

Update root usage docs from executed commands, including single/multi-config
executable paths and MSVC developer-shell requirements. Track needed docs in Git.
`PROJECT_ARCHITECTURE.md` is tracked as the original design reference; the build
does not depend on local-only documentation.

## Complete the product matrix

| Mode | Driver | Required behavior |
| --- | --- | --- |
| Static | Thread pool | A's paths, GET/HEAD, streaming, validators and persistence |
| Static | Event loop | The same assertions plus delayed-file nonblocking behavior |
| Proxy, cache off | Thread pool / event loop | B's target/header/framing/deadline and failure behavior |
| Proxy, cache on | Thread pool / event loop | K/C/D hits, cache-only, Age, validation and bounded memory |

Use I's parameterized suite. Compare status, metadata, representation bytes,
ordering and close semantics across drivers. Fewer workers can limit throughput;
it cannot justify different HTTP/cache correctness.

## Concurrent memory, fairness and overload

Exercise parallel hits/misses/evictions/revalidation with origin-change barriers.
Hold slow consumers while replacing entries; verify immutable snapshots,
version checks, candidate abort and accounting under real concurrency. Duplicate
misses are allowed/countable; mixed body/metadata is a failure.

Saturate accepted queues, active client/upstream limits, pending connects,
helper/completion queues and output windows separately and in combination. Confirm
rejection is bounded, no producer spins, timers run, unrelated small requests
progress and capacity recovers after pressure is removed.

Record measured peak counts/bytes and the configured limit that bounds each.
Check both ordinary cleanup and failed admission midway through a transaction.
Metrics must explain overload without introducing blocking output on G's loop.

## Shutdown and partial startup

Define one process lifecycle state machine: constructed, starting, running,
draining, stopping, stopped. Stop requests are idempotent in every state.

Stop listener admission and queued work, notify sessions of the drain deadline,
wake all owners and continue completion/timer processing while draining. At the
deadline, invalidate transactions and close registered sockets on their owners
after unwatch. Dispose queued results safely, quiesce/join helper and client
threads in a dependency-safe order, release shared services, then destroy runtime.

Keep completion destinations alive until workers can no longer use them; do not
join a producer blocked on a queue whose consumer has already vanished. Account
for file operations still holding tokens. Preserve E's explicit limitation for
uncancellable resolver joins rather than claiming the drain deadline bounds all
process teardown.

Inject failure at configuration, root open, listener bind, poller creation,
helper startup and driver startup. Already-created resources must roll back in
safe order. Repeated start/stop should return handles/threads/reservations to
baseline. H's OS callbacks must not retain dead application pointers.

## Verification and handoff

Run the base/P0 gate and all present feature suites, including `milestone24`,
`milestone27`, `milestone28` and `milestone29`. Complete F/G's actual service
acceptance. Inspect I's inventory so absent tests are not mistaken for passes.
Execute J's small scenario-family smoke runs when the harness is available.

- [x] Real executable selects every required mode and uses production modules.
- [x] Combined local strict tests pass with no required pending substitutions.
- [x] `docs/branches/application-assembly.md` records included SHAs, exact
  factories/targets, lifecycle guarantees and validation commands/results.
- [x] M receives CI/validation ownership after assembly freeze; N receives final
  run/results documentation after J/L freeze. M still must establish portability.
- [x] No final feature wiring is left as an exercise for the person merging branches.
