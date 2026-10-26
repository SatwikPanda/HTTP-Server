# Event-loop driver handoff (`G — event-loop`, milestones 25–28)

## Base identity and dependencies

- **Branch:** `event-loop`
- **Base commit (`main`):** `c86509b88496c59f3590c2ab55c82874c048ce99` (includes `P0` / `parallel_foundation` and merged Track `A` `static-service`)
- **Direct prerequisites:** [`P0` (`parallel_foundation`)](parallel-foundation.md)
- **Downstream consumers:**
  - `L` (`application-assembly`) wires `concurrency::event_loop::make_event_loop_driver`, `concurrency::event_loop::EventLoopConfig`, and `concurrency::event_loop::EventLoopDriver` into `--driver event-loop` alongside the concrete helper service (`E`) and application factories (`A`/`B`/`K`).
  - `I` (`test-harness`) and `M` (`verification-parity`) drive assembled server/proxy/cache matrix suites and deterministic single-step tests via `EventLoopDriver::step_once()`.
  - `J` (`benchmark-harness`) and `N` (`benchmarks-report`) measure event-loop vs. thread-pool throughput, latency, fairness, and memory usage under matched configurations.

## Owned files and targets

- Public headers:
  - [`include/concurrency/event_loop/event_loop_driver.hpp`](../../include/concurrency/event_loop/event_loop_driver.hpp)
- Production implementation and CMake target (`event_loop` static library, linking `concurrency`, `net`, `core`, `Threads::Threads`):
  - [`src/concurrency/event_loop/CMakeLists.txt`](../../src/concurrency/event_loop/CMakeLists.txt)
  - [`src/concurrency/event_loop/event_loop_driver.cpp`](../../src/concurrency/event_loop/event_loop_driver.cpp)
- Acceptance suites and test harness (`milestone25_test`..`milestone28_test` with labels `event_loop` and `milestone25`..`milestone28`):
  - [`tests/concurrency/event_loop/CMakeLists.txt`](../../tests/concurrency/event_loop/CMakeLists.txt)
  - [`tests/concurrency/event_loop/event_loop_harness.hpp`](../../tests/concurrency/event_loop/event_loop_harness.hpp)
  - [`tests/concurrency/event_loop/milestone25_test.cpp`](../../tests/concurrency/event_loop/milestone25_test.cpp)
  - [`tests/concurrency/event_loop/milestone26_test.cpp`](../../tests/concurrency/event_loop/milestone26_test.cpp)
  - [`tests/concurrency/event_loop/milestone27_test.cpp`](../../tests/concurrency/event_loop/milestone27_test.cpp)
  - [`tests/concurrency/event_loop/milestone28_test.cpp`](../../tests/concurrency/event_loop/milestone28_test.cpp)

Both `src/concurrency/event_loop` and `tests/concurrency/event_loop` are automatically discovered by [`cmake/ParallelModules.cmake`](../../cmake/ParallelModules.cmake). Neither root `CMakeLists.txt` nor `src/main.cpp` was modified.

## Architecture and contracts

### 1. Ownership tables and stale-event/completion protection

- **Single-thread socket and poller ownership:** One loop thread (or caller thread when stepping via `step_once()`) owns the listening socket, all accepted downstream sockets, all outbound upstream sockets, all `net::Poller` registrations, and all live `concurrency::ConnectionSession` instances. Only `EventLoopDriver::request_stop()` and the helper completion callback (`CompletionPort::publish()`) touch shared state from outside the loop thread, waking the poller via `net::Poller::wake()`.
- **Durable identity and registration table:** Raw OS socket descriptors/handles are never used as identity keys. Every listener, downstream connection, and upstream socket registration is assigned a monotonic `core::ConnectionId{id, generation}` and recorded in `registrations_` (`RegistrationToken -> RegistrationRecord{token, connection, role, transaction, work_sequence, candidate_index}`).
- **Two-stage readiness validation:** Before dispatching any `net::ReadyEvent` returned from `poller_->wait()`, the loop checks:
  1. `poller_->is_current(ev)` to verify the poller slot and registration generation match.
  2. `registrations_.find(ev.token)` and `reg.connection == ev.owner`, plus verification that the target `SessionEntry` is still live, not closing, and still holds that exact `RegistrationToken` (`client_token` or `upstream_token`).
  Any mismatch increments `EventLoopStats::stale_events_dropped` and discards the event without touching a closed or newly accepted/reused socket in the same poll batch.
- **Unwatch-before-close invariant:** Every socket teardown path (`close_upstream_socket`, `close_session`, `unwatch_listener`, `cleanup_all_on_owner`) removes the entry from `registrations_` and calls `poller_->unwatch(token)` **before** calling `socket.close()`.

### 2. Bounded scheduling loop and fairness budgets

Each `execute_iteration()` executes a fixed, bounded sequence:

1. **Check stop and deadlines:** Transitions into draining mode on `request_stop()` (unwatching the listener so no new connections are accepted) and scans bounded active sessions for expired session, connect, upstream I/O, or helper-job deadlines (`check_deadlines`).
2. **Deliver bounded helper completions (`process_completions`):** Pops at most `budgets.max_completions_per_turn` items from the helper completion queue. Each completion is matched against `(connection, transaction, sequence, kind)`; stale completions for closed sessions, superseded transactions, or cancelled jobs increment `stale_completions_dropped` and are disposed (`close_file_handle` if a `FileOpenWork` arrived late). Matching completions mark the session runnable.
3. **Accept bounded connections (`process_accepts`):** While `listener_ready_` is set and fewer than `budgets.max_accepts_per_turn` connections have been accepted in this turn, accepts nonblocking sockets. If `active_sessions >= limits.max_active_sessions` or `ready_queue_.size() >= limits.max_ready_queue`, attempts a bounded nonblocking `503 Service Unavailable` (`Connection: close`) write and closes immediately without creating a session (`overload_rejections`).
4. **Advance bounded runnable sessions (`process_runnable_sessions`):** Pops up to `ready_queue_.size()` (capped by `budgets.max_ready_sessions_per_turn`) sessions from the FIFO ready queue. Each session turn is bounded by `budgets.max_events_per_session_turn` and `budgets.max_bytes_per_session_turn`. When a session hits its per-turn event or byte budget while still able to make immediate progress (`can_session_progress_immediately`), it is re-enqueued at the tail of `ready_queue_` (`budget_requeues`) so small requests and timers cannot be starved by large transfers.
5. **Update desired poller interests (`update_listener_interest`, `update_session_interests`):** Translates each session's current `SessionAction` into `net::Interest` flags and updates the poller only when the desired interest changes:
   - `ReadAction`: watches downstream `Interest::read` only when `action.read.max_bytes > 0` (input window has capacity).
   - `WriteAction`: watches downstream `Interest::write` only while unsent bytes remain; removes writable interest immediately upon completion to prevent busy-spins on writable sockets.
   - `ConnectWork` (in-progress nonblocking connect): watches upstream `Interest::read_write`.
   - `UpstreamIoWork`: watches upstream `Interest::read`, `Interest::write`, or as specified by `UpstreamWaitWork` while within admission/backpressure limits.
   - `SleepAction` / `HelperWork` / `BlockedAction`: watches downstream `Interest::none` (or waits for timer/completion wake).
6. **Compute nearest poll deadline (`compute_nearest_deadline`):** If runnable work, unread listener readiness, or unconsumed helper completions remain (`immediate_work`), polls with `after_work` (`0 ms` timeout, incrementing `immediate_wakes`). Otherwise, computes the minimum of `stop_deadline_`, each session's `session.deadline()`, active connect/upstream/helper job `deadline`, and `after_work + budgets.idle_Wait_cap` (capped to `10 ms` during `step_once()`).
7. **Poll and enqueue ready sessions:** Calls `poller_->wait(poll_deadline)` and enqueues validated ready sessions into `ready_queue_` without duplicates (`entry.queued` flag).

### 3. Nonblocking upstream state machine and readable+hangup handling

- **Nonblocking candidate connect (`advance_connect_work`):** Never calls blocking `net::connect_candidates()` on the loop thread. Evaluates `ConnectWork::candidates` sequentially under the shared `ConnectWork::deadline`, creating a fresh nonblocking TCP socket for each attempt. If `begin_connect` returns `ConnectState::in_progress`, registers the socket with `poller_` and waits for readiness before calling `finish_connect()` (`SO_ERROR`) to confirm success or advance to the next candidate.
- **Readable bytes alongside hangup:** When a downstream or upstream readiness event reports both `readable` and `hangup` (or `error`), `client_readable` / `upstream_readable` remained latched so `read_some()` drains all kernel-buffered bytes before delivering `ReadResult::Status::eof` on the subsequent read. This ensures complete responses arriving with `FIN` and post-commit truncated origin responses are framed accurately.
- **Independent downstream/upstream lifetimes:** Closing an upstream socket via `UpstreamCloseWork` or failed candidate fallback unwatches and closes only the upstream registration without disturbing the downstream client registration.

### 4. Helper integration, backpressure, and shutdown guarantees

- **Zero blocking file/DNS work on the loop thread:** `FileInfoWork`, `FileOpenWork`, `FileReadWork`, and `ResolveWork` are never executed synchronously on the event-loop thread. `FileCloseWork` closes the `FileHandle` immediately; all other helper actions are submitted via `WorkExecutor::submit()`.
- **Admission and saturation:** Global limits (`max_active_sessions`, `max_upstreams`, `max_connecting`, `max_pending_jobs`, `max_ready_queue`, `max_completion_queue`) are checked before allocating sockets or submitting helper jobs. If `active_jobs >= limits.max_pending_jobs` or `WorkExecutor::submit()` returns `SubmitState::queue_full` / `rejected` / `stopped`, the loop delivers `WorkResult{WorkStatus::rejected, would_block}` to the session (`helper_queue_rejections`), which maps to a coherent `503 Service Unavailable` prior to response commit.
- **Safe completion lifetime and shutdown sequence:** `CompletionPort` is held via `std::shared_ptr<CompletionPort>` shared between the driver and submitted helper callbacks. On `request_stop(drain_timeout)`:
  1. The listener is unwatched so no further connections are accepted.
  2. Active sessions receive `StopEvent` when not mid-action or drain until `stop_deadline_`.
  3. Once drained or `stop_deadline_` expires, `cleanup_all_on_owner()` marks `completion_port_->closed = true`, drains and closes any open `FileHandle`s in unread completions, cancels pending jobs via `helper_executor_->cancel(connection)`, unwatches and closes all upstream/downstream/listener sockets, and resets `poller_`.
  4. Late helper completions arriving after teardown see `completion_port_->closed == true`, close any newly opened `FileHandle` immediately, and return without dereferencing `poller_`.
- **Inherited resolver join limitation (from Track E):** Cancelling a transaction or stopping the event-loop driver invalidates the session/transaction generation so late `ResolveWork` completions are safely discarded, but the OS `getaddrinfo` call inside a helper worker thread cannot be preemptively interrupted by the loop. Joining the helper executor (`E`) during process teardown may therefore wait for an in-flight OS resolver call to return even after the event-loop driver itself has stopped and joined within its drain deadline.

## Executed verification

All builds and tests were executed on **Windows (GCC 16.2 / MinGW-w64 Ninja, Release, `BUILD_TESTING=ON`, `REQUIRE_ALL_MILESTONES=ON`)**:

```powershell
cmake -S . -B build/agents -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DREQUIRE_ALL_MILESTONES=ON
cmake --build build/agents --config Release
ctest --test-dir build/agents -C Release -L '^(milestone0[1-9]|parallel_foundation|event_loop|milestone2[5678])$' --no-tests=error --output-on-failure
```

### Results summary

- **24 / 24 tests passed** (`0` failures, `0` skips, total runtime `20.31 s`):
  - Base milestones `01`–`09` + `parallel_foundation`: 20/20 passed.
  - `milestone25_test` (`0.36 s`): Static file serving over `EventLoopDriver` + `AsyncTestWorkExecutor` (`server::make_static_factory` + `http::HttpSession`) covering GET, HEAD, 304 `If-None-Match` / `If-Modified-Since`, 404/403/400/405 responses, multi-chunk streaming of a 256 KiB file with a 16 KiB chunk cap, 8 concurrent keep-alive clients (24 requests), and idle clients alongside active file transfers.
  - `milestone26_test` (`6.34 s`): Nonblocking upstream proxy/origin flows over `EventLoopDriver` covering 100-Continue interim + chunked body + trailers, multi-candidate connect fallback (`refused_endpoint -> live_origin`) and all-candidates-refused `502 Bad Gateway`, post-commit truncated origin body closing downstream without forging a terminating chunk, slow gated origin isolated from fast sessions, `max_upstreams` admission enforcement and recovery, and deterministic `step_once()` execution across reused registration slots.
  - `milestone27_test` (`0.32 s`): Fairness, backpressure, and overload bounds covering a 512 KiB slow-reader transfer running concurrently with 5 fast requests (`budget_requeues > 0`, fast requests complete in `< 250 ms`, no writable-interest spin), helper queue saturation (`max_pending_jobs = 1` returning `503` and recovering after helper release), and connection admission overload (`max_active_sessions = 1` rejecting excess connections and recovering).
  - `milestone28_test` (`0.14 s`): Deadlines, cancellation, stale completions, and lifecycle teardown covering idle connection timeout under continuous traffic, client disconnect mid-file-read cancelling/dropping late helper completions without leaking file handles, graceful drain within `request_stop` deadline vs. forced deadline expiry for stalled origins, and 3 repeated `start()` / `request_stop()` / `join()` cycles on the same `EventLoopDriver` instance.

### Known notes for integration (`L` / `M`)

- Linux and macOS verification for `event-loop` (`milestone25`–`milestone28`) is pending CI / cross-platform runs.
- Note on `main`: `tests/server/static_response_test.cpp` (from Track A `static-service`, outside Track G's owned paths) currently omits `net::initialize_network()` in `main()`, causing Winsock `10093` (`WSANOTINITIALISED`) when run standalone on Windows. Track A / L should add `auto runtime = test::take(net::initialize_network());` to `tests/server/static_response_test.cpp`.
