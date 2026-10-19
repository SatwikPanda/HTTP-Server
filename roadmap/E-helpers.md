# E — helpers

**Status:** planned. **Branch:** `codex/helpers`. **Milestone:** 22.
**Start:** [P0](P0-parallel-foundation.md). Production consumers are
[F](F-thread-pool.md), [G](G-event-loop.md) and the reference driver assembled by
[L](L-application-assembly.md). Follow the [shared workflow](README.md).

## Outcome and ownership

Execute blocking file and DNS operations without blocking the event-loop owner.
Own `include/concurrency/helpers/`, `src/concurrency/helpers/`,
`tests/concurrency/helpers/` and module CMake. Use P0's work/result interfaces,
opaque file resources and completion destination. E schedules effects; it does
not make HTTP, path-access or cache decisions and never operates on driver sockets.

Inspect `include/net/resolver.hpp`, `src/net/resolver.cpp`,
`include/net/poller.hpp`, `include/core/id.hpp`, P0's file adapter and lifecycle
examples. The existing resolver blocks inside the OS call and has no hard
cancellation contract. Preserve that fact in the API and documentation.

## Implementation order

### 1. Define bounded ownership at submission

A job carries connection generation, transaction ID, job ID, operation kind,
deadline/cancellation state, immutable inputs and destination completion port.
Copy/own strings and paths needed after submission; never enqueue string_views
into a request object that the session can reset.

Bound queued jobs, running jobs, per-owner outstanding work and completion payload
bytes. Reserve enough completion capacity at admission, or use a bounded result
slot associated with each admitted job. A full completion queue must not make
shutdown wait forever for a stopped consumer. Keep metadata-only DNS results
bounded too, including candidate count and owned error information.

Submission returns accepted, saturated, stopping or invalid-work result without
blocking the loop. On rejection, the caller retains or releases resource ownership
according to a single documented rule. Do not allocate an unbounded fallback queue.

### 2. Implement execution and completion delivery

Use a fixed set of helper threads, a bounded synchronized job queue and a
condition variable for idle workers. Pop work under a short lock; perform actual
file/DNS work outside it. Publish an owned result, then notify the destination.
Queue publication must happen before wake so the consumer cannot miss the result.

Use `Poller::wake()` only through a destination whose lifetime is protected.
A weak/invalidatable completion port or equivalent ownership protocol prevents
a worker from dereferencing a destroyed poller. Wake coalescing is allowed:
drivers drain available results under their work budget and reschedule remaining
work instead of assuming one wake equals one result.

Completion delivery on the driver thread checks connection generation,
transaction ID and job ID. A stale result is disposed, including file handles and
reservations, without invoking a new session. E must be safe when cancellation
and result publication race in either order.

### 3. Preserve file operation order and lifetime

File metadata/open/read/close use P0's adapter. A handle belongs to a controlled
work sequence, with at most the allowed number of ordered operations outstanding.
Do not issue two unsynchronized reads on one seekable stream or close it while a
worker still uses it. A job retains the resource until completion even if its
session disappears. Cleanup can mark a handle retired and release it after the
last operation finishes.

Read only the requested bounded chunk and report actual byte count/EOF/error.
Do not prefetch all file contents, perform MIME/conditional logic, or create an
unbounded result merely because file I/O runs off-thread. Reserve output capacity
before work admission; account for a result waiting to be delivered.

### 4. Separate transaction deadlines from OS cancellation

The owning driver observes a DNS/file deadline without waiting for the helper.
On timeout it invalidates the job's consumer context and emits the appropriate
application outcome. The OS resolver may still finish later; its result is stale.
An abandoned running job continues to consume a helper slot and bounded memory
until it actually returns. Do not replace it with unlimited new helper threads.

Queued cancellation removes or marks a job so execution is skipped and its
reservations are released. Running cancellation is best effort according to the
adapter; the completion protocol must work even if cancellation is impossible.
Distinguish timeout/cancel from resolver failure in diagnostics.

### 5. Stop without deadlocking result delivery

Stop admission, cancel queued jobs, invalidate destinations that will no longer
consume, and let active operations release their own resources. Publish or safely
discard terminal results without waiting indefinitely for a full queue. Join
helpers before destroying their adapters, destination state or NetworkRuntime.

The helper pool is distinct from the client worker pool. A client worker waiting
for DNS must never occupy the same bounded pool needed to execute that DNS job.
Make partial startup failure safe: stop/join already-created threads and release
queued resources if creating a later worker fails.

Without cancellable native resolution, an in-progress resolver may delay join
beyond the client drain deadline. State the guarantee precisely. Do not detach
threads borrowing destroyed process state or claim a hard process-stop bound.

## Verification

| Case | Assertion |
| --- | --- |
| Real temporary-file sequence | Open/read/EOF/close returns exact bytes with bounded chunks |
| Localhost/numeric resolution | Production resolver executes and returns bounded owned candidates |
| Admission saturation | Immediate explicit rejection; no hidden queue/thread growth |
| Full results queue | Memory remains bounded; owner wake/resume and shutdown can progress |
| Paused file/DNS adapter | Unrelated driver work and deadlines progress; only I/O is controlled |
| Cancel before execution | Operation is skipped and reservations return |
| Cancel during execution | Late result safely disposed; active slot released only on actual finish |
| Reused connection/request IDs | Generation/transaction mismatch cannot deliver to new work |
| Destination destruction race | No wake of destroyed poller or callback into dead session |
| Repeated/partial start-stop | All owned threads/resources terminate within documented guarantees |

Use barriers/latches in controlled I/O adapters rather than timing sleeps. Never
test an uncancellable real resolver by deliberately hanging external DNS; use
local resolution plus a controlled blocking I/O boundary. Run `milestone22` and
base/P0 tests, then L's real driver integration with slow work.

## Done and handoff

- [ ] Every admitted job has a bounded resource/result slot and one terminal
  ownership path, including timeout and stale completion.
- [ ] L can inject the same helper interface into F/G without changing protocols.
- [ ] `docs/branches/helpers.md` records capacities, shutdown limits, adapter
  semantics and actual test results.
