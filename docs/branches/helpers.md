# Helper service handoff (track E, milestone 22)

Branch `helpers`, started from `main` (P0 and track A present). Owned files:
`include/concurrency/helpers/`, `src/concurrency/helpers/`,
`tests/concurrency/helpers/`. Target: `concurrency_helpers` (links
`concurrency`, `net`, `core`, `Threads::Threads`; no HTTP). Root CMake, root
`main` and the shared P0 contracts are unchanged.

## Interface

- `HelperPool::start(HelperConfig, adapter)` starts a fixed number of helper
  threads over one bounded job queue. `submit(action, port, completion)` never
  blocks and returns `accepted`, `saturated`, `stopping` or `invalid`.
- `CompletionPort::create(capacity, wake)` is the driver-owned destination. The
  owner calls `drain(budget)` on its own thread; completions never run on helpers.
- `HelperExecutor` is the P0 `WorkExecutor` port bound to one pool and one port,
  so L can hand the same helper interface to F and G. Admission failure returns an
  error; the driver delivers a matching `rejected` `WorkEvent` to its session.
- `make_reference_adapter(limits)` runs the P0 file/DNS code. Each job gets a
  fresh single-use reference executor, so helpers share no per-connection state.
  Tests inject a gated adapter around the same production code.

Only `ResolveWork` and `File*Work` are accepted. Upstream socket work is invalid:
helpers never touch driver sockets.

## Bounds and ownership

| Resource | Bound |
| --- | --- |
| Queued jobs | `queue_capacity` (default 64, max 65536) |
| Running jobs | `threads` (default 2, max 256); an abandoned resolver keeps its slot until it returns |
| Results per destination | port `capacity`; one slot is reserved at admission and released when the owner drains it or the port is closed |
| Result payload | one `WorkLimits` buffer per slot (default 64 KiB), at most 32 resolver candidates |

Ownership rule: `submit` takes the action and completion by value. Accepted means
exactly one completion reaches the port unless the port is closed first. Any other
result destroys the pool's copies and calls nothing; the caller keeps what it
retained. A job keeps its file token alive until the helper call returns.

Publication queues the result before calling `wake`, and calls `wake` under the
port lock. `close()` therefore cannot return while a wake is in progress, and the
owner may destroy its poller immediately after `close()`. Results that arrive for
a closed port are released on the helper thread. Wakes coalesce; drivers drain
under a budget and loop.

Stale results: the pool always stamps the submitted `OperationId` on the result.
The consumer compares connection generation, transaction and job sequence before
applying it. Tests cover a reused connection slot with a new generation.

## Deadlines, cancellation and stop

- Cancelled before execution: skipped, delivered as `cancelled`, file system not
  touched. Deadline passed while queued: delivered as `timed_out`, not executed.
- Cancelled while running: the reference code checks cancellation after the call
  and releases any payload or opened file; the slot frees only when the call returns.
- The driver enforces the client-visible deadline itself and never waits for the
  helper. The OS resolver call cannot be interrupted; its late result is stale.
- `stop()` is idempotent: it stops admission, completes queued jobs as `cancelled`
  (reserved slots make this non-blocking), lets running jobs finish and joins.
  **Limitation:** a resolver call already inside `getaddrinfo` can delay `stop()`
  until it returns. No thread is detached and no hard process-stop bound is claimed.
- Partial start: if creating helper N fails, helpers 0..N-1 are stopped and joined
  before `start` returns the error (`before_thread_start` is the test seam).
- Call `stop()` from a non-helper thread. Completions must not own the pool.

## Verification (actually run)

Linux, GCC 13.3.0, Ninja, `-DREQUIRE_ALL_MILESTONES=ON`:

```
cmake -S . -B build/agents -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DREQUIRE_ALL_MILESTONES=ON
cmake --build build/agents
ctest --test-dir build/agents -L '^(parallel_foundation|milestone0[1-9]|milestone22)$' --no-tests=error --output-on-failure
```

All selected tests passed, and the full strict suite passed. `helper_pool_test`
was also run repeatedly in Release, pinned to one CPU, and under
AddressSanitizer and ThreadSanitizer with no failures or reports.

Cases: exact bytes through a real open/read/EOF/close sequence; numeric and
localhost resolution; immediate saturation without growth and recovery; a full
result port that stays bounded, recovers after a budgeted drain and does not block
stop; a driver deadline that expires while a resolver is blocked, with unrelated
work completing meanwhile; cancel before and during execution; queued deadline
expiry; reused connection IDs; a destination destroyed while its job runs (no wake,
no callback); stop with queued and running jobs; invalid config/work; repeated and
partial start/stop.

**Pending:** Windows and macOS runs, and L's real-driver integration with slow
work. F's tests already run the static service through this pool (see
`thread-pool.md` on that branch).
