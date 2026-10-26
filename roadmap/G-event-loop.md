# G — event-loop

**Status:** implemented and integrated; final LMN candidate qualification is separate. **Branch:** `event-loop`.
**Milestones:** 25–26 and driver portions of 27–28.
**Start:** [P0](P0-parallel-foundation.md). Full acceptance requires
[E](E-helpers.md), A/B/K and [L](L-application-assembly.md).
Follow the [shared workflow](README.md).

## Outcome and ownership

One loop thread advances many sessions through the same protocol state machines
as F. Own `include/concurrency/event_loop/`, `src/concurrency/event_loop/`,
`tests/concurrency/event_loop/` and local CMake. L injects application/session
factories and E. Link against P0 helper interfaces so the module builds independently;
never use synchronous file/DNS reference adapters on the production loop.

Inspect `include/net/poller.hpp`, `src/net/poller.cpp`, `socket.hpp`,
`io_types.hpp`, `include/core/id.hpp` and P0's action/continuation contract.
The existing backend is level-triggered poll/WSAPoll; this track does not add
epoll, kqueue or IOCP.

## Implementation order

### 1. Make ownership tables and stale-event checks explicit

The loop owns listener, downstream/upstream sockets, registrations and live
sessions. Keep a table mapping registration token to connection generation,
endpoint role and transaction/attempt identity. The raw native socket number is
never a durable identity; the OS can reuse it immediately after close.

Before using a saved readiness event, call `is_current()` and verify its owning
session/transaction. Before close, unwatch its registration. Remove table entries
in a consistent order so an event from the current batch cannot target a new
socket created in the same iteration. Do not permit helper threads to mutate
these maps or operate on their sockets.

### 2. Build a bounded scheduling loop

A conceptual iteration is:

```text
check stop/deadlines
deliver bounded helper completions, dropping stale results
advance bounded runnable sessions and accepts
update each endpoint's desired interest
compute nearest deadline (or immediate wake if runnable work remains)
poll for readiness
validate and enqueue ready sessions without duplicates
```

Maintain a ready queue with a per-session queued flag. Cap accepts, events,
bytes and completions per turn. When a budget expires while work can progress,
requeue the continuation at the tail rather than losing it or running it forever.
Timers must be checked under sustained readiness as well as after a blocking wait.

Bound the timer mechanism itself. A heap with endlessly accumulated stale timeout
records is not bounded merely because connections are bounded. Update/remove
entries or compact under a documented limit. A simple scan of bounded sessions
may be an acceptable initial implementation; choose readability before speculative
optimization and measure later.

### 3. Translate actions into interests and progress

Read only when the session has input capacity. Write only while an owned segment
has unsent bytes. Connecting sockets watch the relevant readiness until
`finish_connect()` confirms success/failure; readiness alone is insufficient.
Remove writable interest once output/connect work finishes, otherwise a writable
socket can make the loop spin.

Preserve offsets through short writes. Distinguish would-block from zero-progress
success and failure. If readiness includes hangup, consume readable bytes before
interpreting EOF; a complete final response can arrive with hangup. Route client
and origin events independently so closing one does not accidentally close both.

Blocking convenience calls such as `connect_candidates()` must not be invoked
as one operation on the loop. Execute P0's individual candidate actions with a
shared deadline and a fresh socket after each failed attempt.

### 4. Integrate helpers and backpressure

Submit bounded file/DNS jobs to E; return to scheduling immediately. A completion
makes its matching session runnable after identity checks. Publish/wake ordering
must allow a completion to arrive between the queue check and poll without being
lost. If a completion budget leaves items queued, arrange immediate future work.

Pause upstream reading/file production when downstream buffered output reaches
high-water, and resume below low-water. Capacity calculations include already
received parser bytes, owned segments, queued completions and cache candidates.
Avoid a deadlock where every producer waits for a consumer that was never made
runnable after output drained.

No file stat/open/read, synchronous DNS, sleep, long log write or potentially
blocking metric export runs on this thread. Ordinary bounded in-memory cache
decisions can run here, with short locks and bounded work. A saturated helper
queue produces an explicit admission result, not a busy submission loop.

### 5. Enforce admission, deadlines and lifecycle

Bound active sessions, upstreams, connecting sockets, pending jobs and runnable
entries globally. Admission must reserve capacity before creating resources.
An overload response is bounded and attempted only when coherent; otherwise
close promptly. Freeing capacity makes waiting permitted work eligible again.

Derive the poll deadline from the earliest session/job/connect/drain deadline.
Apply P0's distinction between an intentionally paused producer and an actually
stalled peer. A slow client still has an output-progress deadline. Avoid extending
time limits merely because the loop handles unrelated readiness.

On stop, disable listener acceptance, request session drain and keep processing
timers/completions until done or deadline. Then invalidate work and unwatch/close
owned sockets, dispose queued results and coordinate helper quiescence. Keep
completion destination lifetime safe until E can no longer wake it. Runtime is
destroyed last. Document the resolver join limitation inherited from E.

## Verification

Start with production demo/echo to prove scheduling, then run A/B/K unchanged
through L. Required adversarial cases: many idle clients with an active request;
large transfer beside small requests; slow reader/origin; immediate connect failure;
readable+hangup; helper delay/full queue; writable-interest removal; deadlines
under continuous traffic; cancellation and reused registrations within one batch.

Expose bounded counters for iterations, readiness waits, per-turn work and buffer
peaks so tests can detect spins/starvation without relying on arbitrary sleeps.
Use controlled clocks/I/O for deterministic scheduling plus real loopback checks.
Stop at each transaction stage and repeat start/stop to check owned resources.

Run `milestone25`, `milestone26`, relevant `milestone27`/`milestone28` and base/P0
checks. Full milestone 25/26 acceptance requires real static/proxy/cache services
and E, not only a fabricated session that always completes.

## Done and handoff

- [ ] Shared production sessions make progress with bounded scheduling and memory.
- [ ] No blocking file/DNS/log work occurs on the loop; all interests and wakeups
  have explicit enable/disable/resume rules.
- [ ] `docs/branches/event-loop.md` records budgets, timer structure, ownership,
  shutdown guarantees and actual tests for L/M.
