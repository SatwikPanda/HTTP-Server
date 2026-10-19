# F — thread-pool

**Status:** planned. **Branch:** `codex/thread-pool`.
**Milestones:** 23 and driver portions of 27–28.
**Start:** [P0](P0-parallel-foundation.md). Full acceptance requires
[E](E-helpers.md), real A/B/K services and [L](L-application-assembly.md).
Follow the [shared workflow](README.md).

## Outcome and ownership

Run one acceptor with a bounded accepted-socket queue and fixed session workers.
Own `include/concurrency/thread_pool/`, `src/concurrency/thread_pool/`,
`tests/concurrency/thread_pool/` and local CMake. Consume a session factory and
helper ports; do not link the driver back to HTTP/application implementations.
L supplies E and the selected application factory.

Inspect `include/concurrency/session.hpp`, `single_session_driver.hpp`,
`src/concurrency/single_session_driver.cpp`, Socket/Poller contracts and existing
echo/session loopback tests. Reuse P0's action executor when appropriate, keeping
worker scheduling separate from shared protocol decisions.

## Implementation order

### 1. Establish ownership before adding concurrency

The acceptor thread owns the listener and its poller. A worker owns its poller,
current client and that client's upstream sockets. Transfer an accepted socket
while it is unregistered, through a move-only queue item. Only its eventual owner
registers and operates on it. Configure nonblocking mode explicitly on acceptance
or before worker use; do not rely on inheritance.

Create worker-local pollers on their owning thread or transfer them before any
concurrent use. Keep runtime/factories/helpers alive until all workers join.
Use an explicit worker startup outcome so a poller/thread creation failure can
stop and join already-started workers before returning an error.

### 2. Bound admission and accepted queue residence

Queue items contain socket ownership, accepted time and connection identity.
Bound queued and active connections separately and define a maximum queue wait
or an absolute deadline carried from acceptance. A queued client must not evade
all timeouts until a worker eventually dequeues it.

At capacity, promptly close or send a small bounded 503 when it can be done
without blocking the acceptor. Generate any rejection through the shared output
contract; do not create a separate HTTP implementation in the queue code. Never
retry a full queue by spinning or add an unbounded emergency queue.

Use a condition variable to wake idle workers for queued work or stop. Maintain
clear states for accepting, draining and stopped so no socket becomes orphaned
between accept and queue insertion during shutdown.

### 3. Drive one session per worker

Create the same ConnectionSession used by G. Execute reads/writes/connect actions
with nonblocking sockets, preserve output offsets and wait only on relevant
readiness or the nearest deadline. Feed partial progress, would-block, EOF,
errors and helper completions through the shared event contract.

The worker may wait for socket readiness, but file/DNS work is submitted to the
separate helper service. Process returned completions on the owning worker and
check generation/transaction/job identity. A worker's helper completion port
must wake that worker's wait safely, including when it has no socket interest.

Keep idle, request, origin and output deadlines distinct. Advance progress clocks
only on real progress. Keep-alive occupies a worker, so enforce request-count and
idle limits; do not promise unlimited idle clients with a fixed worker count.

### 4. Maintain fairness and bounded shared use

Although workers give separate clients execution slots, global origin sockets,
jobs, cache candidates and accepted queues still need admission limits. A worker
must not monopolize those resources indefinitely through retries or prefetch.
Use the same high/low-water and stream limits as G. A slow client may consume a
worker; tests should establish the declared capacity behavior and recovery.

No cache lock or queue mutex may be held during readiness wait, helper submission
that can block, serialization or response I/O. Treat metric hooks as nonblocking
contracts; do not call a blocking logger on a worker hot path accidentally.

### 5. Drain and stop by owner

Stop accepting, wake the acceptor and every worker, stop queue admission and
decide which queued sessions can begin within the drain policy. Close abandoned
unregistered queue sockets under their queue-owner path. Active workers receive
the shared stop/drain event and close their own registered sockets.

The coordinator may request stop and call wake, but must not concurrently close
a socket a worker is polling. At deadline, invalidate pending work, unwatch then
close on the owner, release session resources and exit. Join client workers and
coordinate helper quiescence before destroying shared services/runtime. Stop is
idempotent and works before start, during partial startup and after completion.

## Verification

Run production echo/demo sessions first, then unchanged A/B/K tests with E in L.
Force queue-full behavior, fewer workers than clients, slow reader/origin,
idle keep-alive, short writes, queued timeouts, helper saturation and failed
connects. Assert bounded active/queued counts and recovery after pressure ends.

Use barriers to keep one worker busy while another finishes a request. Verify
real progress rather than only counting accepted sockets. Stop during queueing,
read/write wait, DNS, origin connect and cache validation. Check stale completions,
repeated start/stop and partial-start cleanup for handle/thread leaks.

Run `milestone23`, relevant `milestone27`/`milestone28` checks and base/P0 gates.
The worker module can merge before services exist, but full milestone 23 stays
pending until L runs static, proxy and cache acceptance through it.

## Done and handoff

- [ ] Socket/poller ownership and queue timeout policy are explicit and tested.
- [ ] Every queue, resource acquisition and wait has a bounded stop/error path.
- [ ] Publish factory/config inputs, metrics and checks in
  `docs/branches/thread-pool.md`; L owns final CLI selection.
