# Thread-pool driver handoff (track F, milestone 23)

## CI portability correction — 2026-10-26

The macOS job in [integration run 40370186351](https://github.com/SatwikPanda/HTTP-Server/actions/runs/40370186351)
built successfully but timed out in the shutdown fixture's `send_all`. Sending
2 MiB before reading an echo can fill both socket directions: the driver waits
for the client to read while the client waits for the driver to read.

Shutdown checks now send a small HTTP request that produces a bounded 8 MiB
response. A transparent session probe observes real would-block writes before
requesting stop. The drain case waits for stop delivery, reads the entire response
and verifies its body/framing and clean close; the stalled case verifies the drain
deadline with unread output. The existing large echo/offset test remains intact.
No driver or session behavior is mocked or changed. Local strict affected suites
pass with Windows GCC, Clang and MSVC; Clang also passes five consecutive repeats.

## Original branch handoff

Branch `thread-pool`, started from branch `helpers` (track E), because the driver
submits file/DNS work through E's `HelperExecutor` and its tests run E for real.
Owned files: `include/concurrency/thread_pool/`, `src/concurrency/thread_pool/`,
`tests/concurrency/thread_pool/`. Target: `concurrency_thread_pool` (links
`concurrency_helpers`, `concurrency`, `net`, `core`; no HTTP). Root CMake, root
`main`, CLI and shared P0 contracts are unchanged; L owns `--concurrency
thread-pool` and `--workers` wiring.

## Interface

`ThreadPoolDriver::create(ThreadPoolConfig, ThreadPoolDependencies)` then the P0
`DriverLifecycle` calls `start()`, `request_stop(drain)`, `join()`.
Dependencies: a bound listening socket (ownership moves to the driver), a
`SessionFactory`, an optional `HelperPool`, and optional bounded overload bytes.
L should build those bytes as a 503 with the shared writer; the driver contains
no HTTP code and only writes them once, nonblocking, before closing.

| Config | Default | Meaning |
| --- | --- | --- |
| `workers` | 4 | fixed session workers (max 1024) |
| `accept_queue` | 64 | accepted sockets waiting for a worker |
| `queue_timeout` | 2000 ms | longest wait in that queue, from acceptance |
| `accept_batch` | 64 | accepts per readiness event |
| `completion_capacity` / `completion_budget` | 4 / 16 | per-worker helper result slots / completions per drain |

`stats()` reports accepted, rejected (full/stopping), queue timeouts, sessions
started/finished/failed, helper work rejected/timed out, forced closes, and
current/peak queued and active counts.

## Ownership and scheduling

- The acceptor thread owns the listener, its poller and every queued socket. It
  sets each accepted socket nonblocking explicitly, then queues it unregistered
  with its acceptance time and a fresh `ConnectionId`.
- Each worker creates its poller on its own thread, takes one socket from the
  queue, registers it, and drives one session with the same action/event loop as
  the single-session driver. The client socket has no readiness interest except
  during a `WaitAction`, so pipelined input cannot spin a worker that is waiting
  on a helper.
- File and resolver work goes to the helper pool through a per-worker
  `CompletionPort` that wakes the worker's poller. The worker waits for the result,
  its deadline, or stop, whichever comes first. On deadline it cancels the job and
  gives the session `timed_out` without waiting for the helper; the late result is
  dropped because it no longer matches the worker's single outstanding job.
- Upstream socket work runs on the worker with a per-connection reference
  executor, so the upstream socket is owned by the same thread as its client.
- Only `Poller::wake()` crosses threads, through a handle cleared before the
  owning poller is destroyed.

## Capacity and timeouts

- Full queue or stopping: the acceptor closes the new socket at once (after the
  optional overload bytes). No fallback queue, no retry loop.
- A queued socket cannot outlive `queue_timeout`: the acceptor wakes at the oldest
  expiry and closes it even while every worker is busy.
- Session idle/request/output limits and keep-alive request limits are enforced by
  the session itself; the worker also applies the session's `idle_timeout()`. A
  keep-alive client occupies a worker until those limits end it, so the declared
  capacity is `workers` concurrent connections plus `accept_queue` waiting ones.

## Stop

`request_stop(drain)` is idempotent and callable before start, during start and
after completion. It stops admission, wakes the acceptor and every worker, and the
acceptor closes the listener and all queued sockets. Each active session gets
`StopEvent{drain}`; a pending helper job is cancelled immediately. A session still
open at the drain deadline is closed by its worker (`forced_closes`). `join()`
joins the acceptor and workers. Workers close their completion ports before exit,
so after `join()` no helper wakes a driver poller; L then stops the helper pool and
finally drops `NetworkRuntime`. `start()` after stop fails; start twice fails. If a
thread fails to start, the threads already created are stopped and joined before
`start()` returns the error (`before_thread_start` is the test seam).

## Verification (actually run)

Linux, GCC 13.3.0, Ninja, `-DREQUIRE_ALL_MILESTONES=ON`:

```
cmake -S . -B build/agents -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DREQUIRE_ALL_MILESTONES=ON
cmake --build build/agents
ctest --test-dir build/agents -L '^(parallel_foundation|milestone0[1-9]|milestone22|milestone23)$' --no-tests=error --output-on-failure
```

All selected tests passed, and the full strict suite passed. `thread_pool_test`
was also run repeatedly in Release and under AddressSanitizer and
ThreadSanitizer with no failures or reports.

Cases: 8 echo clients through 3 workers (peak active never above 3); a busy
worker does not block others; full queue gives an immediate overload close and
recovers; a queued client is swept at its timeout while the only worker is busy;
3 MiB echo to a client that delays reading, so the worker meets would-block and
resumes at the right offsets; keep-alive idle limit frees the worker;
request-count limit closes a persistent connection; the static service (track A)
serves through helpers while another request's file job is blocked, the blocked
request gets 503 at its job deadline, and the late result is discarded; stop
during a helper wait returns without waiting for the helper; stop closes queued
and active sockets and the listener; draining honours the session's budget and a
session that ignores stop is force-closed at the deadline; stop before start,
double start, repeated start/stop, partial start; failing session factory.

**Pending:** Windows and macOS runs; full milestone 23 acceptance needs L to run
the static, proxy and cache suites through this driver (B/K are not yet
available here). Milestone 27/28 driver portions beyond stop/drain are not claimed.
