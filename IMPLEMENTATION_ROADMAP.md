# Incremental Implementation Roadmap

This roadmap breaks [PROJECT_ARCHITECTURE.md](PROJECT_ARCHITECTURE.md) into small, ordered implementation goals for the C++20 HTTP/1.1 static file server and forward caching proxy. The architecture remains the source of truth for interfaces, protocol rules, and scope.

## Starting point

At the creation of this roadmap, the repository had a C++20 CMake executable, a placeholder `src/main.cpp`, and declarations for a move-only `net::Socket` in `include/net/socket.hpp`. Every milestone initially started unchecked. See the verification record below for current completion status.

The first useful result is a TCP server that accepts a connection. Build a TCP echo server next, then a minimal HTTP response, and gradually replace the temporary behavior with the complete application.

## How to use this plan

- Work in milestone order. Each milestone assumes the earlier ones are complete unless an additional dependency is called out.
- Treat each functionality checkbox as a small implementation task. Mark a milestone complete only when its completion checks pass.
- Keep the project buildable and preserve earlier checks after every milestone. Add tests with the behavior they verify, rather than postponing all verification until the end.
- Introduce folders, source files, and CMake targets as they become necessary. Module paths below describe ownership, not a requirement to create empty files in advance.
- Use a minimal driver that handles one client session at a time until the thread pool is added. Keep protocol and application decisions outside that driver from the beginning.
- Run local checks continuously. Run available Windows, Linux, and macOS checks at phase boundaries; final portability acceptance requires all three platforms. Record any platform not yet tested.
- Treat the command-line examples in the architecture as future functionality. Add options as their features become available and consolidate them in milestone 29.

## Verification record — 2026-10-10

- Acceptance suites for milestones 01–06 were written before implementing milestone 03. Milestones 04–06 have declaration-only adapter contracts: their full assertion bodies compile, but their runnable tests remain pending until production-backed adapters exist. They are not complete.
- The milestone 01–02 gate passed all five test executables before milestone 03 implementation began. A zero-progress echo write regression was found and fixed first. Existing assertions now remain active in Release builds.
- Milestones 01–03 pass the strict completion gate on Windows with GCC 16.2 (Release) and Clang 22.1 (Debug). Each build passes seven registered tests, with zero failures and zero skips: ownership, controlled echo I/O, real echo integration, milestone 01, milestone 02, milestone 03, and CLI validation.
- Milestone 03 covers level-triggered readiness, read/write interest changes, generation/lifetime checks, concurrent coalesced wakeups, deadlines, IPv4 and IPv6 loopback, resolution, refused-connect completion, fresh-socket candidate fallback, idle readiness, and shutdown/restart races. Real echo I/O uses readiness waits instead of retry sleeps.
- `REQUIRE_ALL_MILESTONES=ON` makes a pending suite fail. The milestone 04–06 gate was deliberately run with that option and correctly rejected all three pending suites. Without strict mode, they report CTest skips rather than passes.
- Linux and macOS have not been executed locally. The GitHub Actions workflow now builds and runs the strict milestone 01–03 gate on Windows, Linux, and macOS; those remote results are still required for portability acceptance.
- The first remote run found a Linux milestone 02 retry spin and a macOS milestone 03 refusal-fixture timeout. The retry adapter now rounds up sleeps and rechecks its deadline; the refusal fixture releases a temporary listener before connecting. Regression assertions cover submillisecond waits, timeout errors, distinct fallback endpoints, and all-refused candidates. Both Windows compiler gates pass after the corrections; updated Linux/macOS CI results are pending.
- See [tests/README.md](tests/README.md) for commands, coverage, future adapter contracts, and networking lifetime rules. NetworkRuntime must outlive sockets, pollers, and driver threads. Call `stop()` and join the driver before destroying the server. Resolution is synchronous and belongs outside an event loop; socket deadlines do not bound DNS lookup time.

## Build order at a glance

| Phase | Milestones | Working result |
| --- | --- | --- |
| A. TCP foundation | 01-04 | Portable echo server with partial I/O, readiness, and deadlines |
| B. HTTP core | 05-09 | Incremental HTTP parsing and ordered persistent connections |
| C. Static server | 10-12 | Safe GET/HEAD file serving and conditional requests |
| D. Forward proxy | 13-16 | Streaming HTTP proxy without caching |
| E. Cache | 17-21 | Bounded shared cache with freshness, LRU, and revalidation |
| F. Concurrency | 22-28 | Thread-pool and event-loop modes with shared behavior |
| G. Completion and evaluation | 29-32 | Validated CLI, cross-platform checks, benchmarks, and report |

## Phase A: TCP foundation

### 01. Create a TCP server that accepts a connection

**Goal:** Run an executable that listens on loopback and accepts one client.

**Implement:**

- [x] Add the initial `core::Error`, C++20-compatible `Result<T>`/`Result<void>`, address-family, and endpoint types needed by the socket interface.
- [x] Implement `NetworkRuntime` and move-only socket ownership. Keep native handles in an opaque implementation; define destruction and move operations out of line.
- [x] Implement create, bind, listen, accept, and idempotent close. Accepted sockets must own their handles independently of the listener.
- [x] Select Windows or POSIX source files in CMake. Initialize Winsock and link `ws2_32` on Windows; implement the corresponding POSIX operations.
- [x] Replace the placeholder entry point with a listener on a configurable port, defaulting to loopback. Report startup and accept failures clearly.
- [x] Introduce a small test executable registered with CTest for ownership and networking checks.

**Completion checks:** A local client connects successfully; the server accepts and closes it. Binding an occupied port reports an error. Moving or closing a socket twice never closes another socket. Networking cleanup runs after all sockets are destroyed.

**Main areas:** `include/core/`, `include/net/`, `src/net/windows/`, `src/net/posix/`, `src/main.cpp`, `CMakeLists.txt`, `tests/`.

### 02. Turn the TCP listener into a reliable echo server

**Goal:** Receive bytes and return exactly those bytes to the client.

**Implement:**

- [x] Add nonblocking configuration and `read_some()`/`write_some()` with distinct byte-count, EOF, would-block, and failure outcomes.
- [x] Configure every accepted socket explicitly. Preserve read buffers and an offset into unsent output across short writes.
- [x] Retry interrupted native operations appropriately, capture native errors immediately, cap native buffer lengths, and suppress POSIX `SIGPIPE`.
- [x] Bound the echo buffer. Keep this as a temporary networking exercise, with a simple bounded wait/retry mechanism until milestone 03 supplies the poller.

**Completion checks:** Empty disconnects, binary bytes, fragmented sends, and payloads larger than the buffer behave correctly. A slow receiver causes partial output without lost bytes, unbounded growth, or a process crash. Use a controlled I/O test double to force short writes and would-block outcomes.

**Main areas:** Socket adapters, temporary echo driver, socket tests.

### 03. Add readiness, outbound connections, and wakeup

**Goal:** Wait for socket progress efficiently and establish outbound TCP connections.

**Implement:**

- [x] Add `Poller::watch/modify/unwatch/wait/wake`, backed by level-triggered `WSAPoll` or POSIX `poll`.
- [x] Implement a socket-based wakeup channel and deadline-bounded waits. Unregister sockets before closing them.
- [x] Add connection IDs and registration generations so stale events cannot operate on a reused handle.
- [x] Add `HostPort`, IPv4/IPv6 resolution, and `begin_connect()`/`finish_connect()`. Try candidate addresses with a fresh socket after each failure.
- [x] Check the pending socket error to determine connect success; writable readiness alone is insufficient.
- [x] Drive the echo example through readiness. Subscribe to writes only while data or a connection attempt is pending; process buffered reads even when hangup is reported.

**Completion checks:** Echo still works without busy polling. A wake interrupts a wait. Refused connections, an expired deadline, IPv4/IPv6 loopback where available, and stale registration events are handled correctly.

**Main areas:** `net::Poller`, resolver and connection adapters, readiness tests.

### 04. Introduce the shared session and minimal driver

**Goal:** Separate connection decisions from socket operations before adding HTTP.

**Implement:**

- [ ] Create `ConnectionSession::on_event()` with explicit state and actions for reading, writing, waiting, and closing; extend the action vocabulary as later features arrive.
- [ ] Move the temporary echo behavior into a session. The single-session driver owns sockets and executes actions.
- [ ] Add input/output limits, steady-clock deadlines, idle/progress tracking, and a basic stop/drain path.
- [ ] Provide deterministic fake I/O events and a fake clock for session tests. Retain a separate wall-clock interface for future HTTP dates.
- [ ] Ensure sessions never depend on Winsock/POSIX headers or close another owner's socket.

**Completion checks:** The same echo session runs under fake I/O and real sockets. Timeout, disconnect, pending output, and stop events close resources predictably. The driver can accept another client after the previous session ends.

**Main areas:** `include/concurrency/`, `src/concurrency/`, core clocks and IDs, session tests.

## Phase B: HTTP core

### 05. Send the first valid HTTP response

**Goal:** Replace echo behavior with a small fixed HTTP response.

**Implement:**

- [ ] Define request/response, header-field, method, status, and HTTP-version types independent of networking.
- [ ] Store repeated fields without indiscriminately joining them. Add case-insensitive field-name lookup.
- [ ] Implement validated status-line/header serialization, CRLF termination, and fixed-length output.
- [ ] As a temporary demonstration, accumulate a bounded header block and reply with a constant text body, `Date`, `Content-Type`, `Content-Length`, and `Connection: close`.
- [ ] Reject invalid header names/values in generated output. Replace the temporary header detection with the real parser in milestone 06.

**Completion checks:** A local HTTP client displays the fixed response, its length matches the actual bytes, and forced partial writes still produce the same message. This milestone is a demonstration, not the finished request parser.

**Main areas:** `include/http/`, `src/http/`, minimal HTTP session, serialization tests.

### 06. Parse request lines and headers incrementally

**Goal:** Understand a request even when TCP splits it across arbitrary reads.

**Implement:**

- [ ] Add `consume(bytes)`, consumed-byte counts, parsing states, completion/error events, and `reset()`.
- [ ] Validate request-line syntax, HTTP version, field names/values, repeated fields, and the HTTP/1.1 `Host` requirement. Reject obsolete folded headers.
- [ ] Bound request-line/target length, total header bytes, header count, and connection input buffers.
- [ ] Preserve bytes following the parsed message rather than dropping the beginning of the next request.
- [ ] Map malformed syntax, unsupported versions, and size limits to the appropriate errors; close when parsing is unsafe to continue.

**Completion checks:** Representative bodyless requests parse at every byte split position. Combined requests preserve their boundaries. Malformed Host fields, header syntax, overlong targets, and header limits produce deterministic failures.

**Main areas:** Request parser, shared HTTP limits, parser tests.

### 07. Implement request framing, chunk decoding, and application restrictions

**Goal:** Determine exactly where a request ends, including requests the application rejects.

**Implement:**

- [ ] Add no-body, fixed-length, and chunked request framing. A request without a framing header has no body.
- [ ] Reject conflicting/ambiguous lengths, `Transfer-Encoding` plus `Content-Length`, malformed numbers, and numeric overflow before allocation.
- [ ] Parse chunk sizes, bounded extensions, body fragments, zero chunks, and separately bounded trailers. Prevent trailers from redefining routing or framing.
- [ ] Reject unsupported transfer codings; distinguish an invalid sequence not ending in chunked from an unsupported coding as specified in the architecture.
- [ ] Add premature-EOF detection and decoded-body limits.
- [ ] Enforce bodyless GET/HEAD at the application boundary: reject nonempty bodies and close. Handle unsupported `Expect` with 417 and close; reject disabled/unknown methods, CONNECT, and Upgrade explicitly.

**Completion checks:** Fixed-length and chunked fixtures parse across all important boundaries. Truncated bodies, invalid chunks, prohibited trailers, oversize bodies, and ambiguous framing fail safely. Rejected body bytes never become a second request.

**Main areas:** HTTP framing, chunk/trailer parser, method policy, parser tests.

### 08. Parse upstream responses and serialize each framing mode

**Goal:** Prepare the HTTP core for proxying before opening an HTTP upstream connection.

**Implement:**

- [ ] Add response status-line parsing using the shared header/framing machinery and the originating request method as context.
- [ ] Support fixed-length, chunked, and close-delimited upstream bodies, with `finish_on_eof()` distinguishing valid completion from truncation.
- [ ] Handle informational responses before the final response and reject protocol switching. HEAD, informational, 204, and 304 responses produce no body events.
- [ ] Implement chunked output encoding and correct no-body serialization. Never generate both `Content-Length` and `Transfer-Encoding`.
- [ ] Keep transfer decoding separate from content encoding: compressed representation bytes remain opaque.

**Completion checks:** Fake origin byte streams cover all framing modes, interim responses, invalid status lines, premature EOF, and HEAD/204/304 boundaries. Serializer checks distinguish body length metadata from whether bytes are actually sent.

**Main areas:** Response parser, framing selection, serializer, response fixtures.

### 09. Add keep-alive and ordered pipelined requests

**Goal:** Serve multiple requests over one TCP connection with correct boundaries.

**Implement:**

- [ ] Keep per-session input and output state across requests. Reset only the completed HTTP transaction.
- [ ] Process one request at a time, retaining later pipelined bytes within the input limit.
- [ ] Honor HTTP/1.1 persistence and `Connection: close`; consume an accepted request fully or close before another request is processed.
- [ ] Add an idle timeout, request-progress deadline, output-progress deadline, and maximum requests per keep-alive connection.
- [ ] Preserve response order and close after framing errors or unsafe output failures.

**Completion checks:** Sequential and pipelined requests return ordered responses. Split headers and leftover bytes are preserved. Close semantics, idle clients, and rejected bodies cannot desynchronize the next request.

**Main areas:** `ConnectionSession`, minimal driver, persistent-connection integration tests.

## Phase C: Static file server

### 10. Resolve request paths safely under a document root

**Goal:** Map a request target to an allowed local file.

**Implement:**

- [ ] Accept the server's supported origin-form and absolute-form targets, separate the query, and decode path escapes exactly once.
- [ ] Reject invalid escapes, NULs, backslashes, traversal outside the root, Windows drive/UNC/device forms, and alternate data stream syntax.
- [ ] Canonicalize the document root and check containment by path components, not a string prefix.
- [ ] Define and implement the initial rejection policy for symlinks/reparse points. Use a controlled document tree as required by the architecture.
- [ ] Map `/` to `index.html`, disable directory listings, and distinguish missing, forbidden, and invalid targets.

**Completion checks:** Valid nested paths resolve correctly. Encoded traversal, sibling paths sharing a prefix, double-encoding cases, query strings, platform-specific path forms, and symlink/reparse escapes are covered.

**Main areas:** `include/server/`, `src/server/`, path-policy tests, test document root.

### 11. Stream files for GET and implement HEAD

**Goal:** Serve real files with bounded memory use.

**Implement:**

- [ ] Add `StaticFileService::prepare_response()` and a file-service interface for metadata, opening, bounded binary reads, and cleanup.
- [ ] Extend session actions for file operations. The minimal driver may execute them synchronously; the event loop will use helpers later.
- [ ] Stream fixed-size chunks only when output capacity is available. Do not load entire files into memory.
- [ ] Generate `Date`, `Content-Type`, and known-length framing. Add the explicit MIME table and `application/octet-stream` fallback.
- [ ] Implement HEAD using GET metadata without sending file bytes. Add coherent 403/404/405/500 responses and `Allow: GET, HEAD` where required.
- [ ] Add example files under `www/` and configuration for the document root. Close after a file failure if the response has already started.

**Completion checks:** GET reproduces empty, text, binary, and large files byte-for-byte. HEAD returns corresponding metadata with no body. A slow client keeps output and file-read buffers bounded; missing and inaccessible files return the expected status.

**Main areas:** Static service, file adapter, MIME lookup, session file actions, `www/`, server tests.

### 12. Add static-file validators and conditional responses

**Goal:** Avoid sending a representation when the client's copy is still valid.

**Implement:**

- [ ] Add HTTP-date parsing/formatting, `Last-Modified`, and ETags. Mark a size/mtime-derived ETag weak.
- [ ] Evaluate `If-None-Match` before `If-Modified-Since`, including wildcard and weak matching for GET/HEAD.
- [ ] Return 304 with appropriate metadata and no body when a condition matches.
- [ ] Keep validator decisions in the file service/HTTP layer rather than the socket driver.

**Completion checks:** Initial GET, matching condition, changed file, weak tag, HEAD, and conflicting conditional headers behave correctly under a controlled clock. The existing keep-alive suite also passes with file responses.

**Phase checkpoint:** The static server is usable through the minimal single-session driver. Repeat portable networking and server checks on available target operating systems before adding proxy behavior.

## Phase D: Forward proxy without a cache

### 13. Parse proxy targets and prepare outbound requests

**Goal:** Convert a client proxy request into a correctly addressed origin request.

**Implement:**

- [ ] Add explicit server/proxy mode selection. In proxy mode require an absolute HTTP target.
- [ ] Parse scheme, normalized host, effective port, exact path/query, and bracketed IPv6 authorities. Reject unsupported schemes, userinfo, fragments, and malformed authorities.
- [ ] Rewrite the outbound target to origin-form and regenerate `Host` from the selected authority.
- [ ] Remove `Connection`, every field it names, and other hop-specific fields; keep framing generation under the serializer's control.
- [ ] Consume proxy credentials locally rather than forwarding them. Append `Via`, detect loops, and retain the explicit CONNECT/Upgrade rejection.

**Completion checks:** Table-driven cases verify default and explicit ports, IPv6 authorities, exact escaped paths/query order, conflicting Host values, connection-nominated fields, proxy credentials, and loops. These checks need no live origin.

**Main areas:** `include/proxy/`, `src/proxy/`, target parsing, forwarding policy tests.

### 14. Forward the first complete origin response

**Goal:** A client can fetch a fixed-length resource through the proxy.

**Implement:**

- [ ] Extend session actions for resolution, connection attempts, upstream request writes, and upstream response reads.
- [ ] Keep client and upstream state separate. Use one fresh upstream connection per transaction while preserving client keep-alive.
- [ ] Stream a valid fixed-length GET response using the response parser and downstream serializer.
- [ ] Forward HEAD correctly and preserve end-to-end response metadata. Apply hop-specific field removal in the response direction as well.
- [ ] Add a controlled test origin with selectable bodies, headers, status codes, and connection behavior.

**Completion checks:** Proxy GET and HEAD match the controlled origin. Multiple client requests can use separate origin connections. Fragmented origin headers and partial upstream/downstream writes remain correct.

**Main areas:** Proxy transaction state, session upstream actions, test origin, proxy integration tests.

### 15. Support streaming and all planned upstream response forms

**Goal:** Relay a response whose size or framing is not known in advance.

**Implement:**

- [ ] Relay chunked and close-delimited upstream responses with independently selected downstream framing.
- [ ] Process informational responses before the final response. Enforce no-body response rules and reject switching protocols.
- [ ] Validate/bound trailers; discard optional unsupported trailers without allowing them to change framing or routing.
- [ ] Preserve opaque content encodings while removing upstream transfer framing.
- [ ] Complete close-delimited bodies only on EOF and never reuse those origin connections.

**Completion checks:** The origin fixture sends fixed-length, chunked, close-delimited, interim, HEAD, 204, and 304 responses. Downstream status, metadata, and representation bytes remain correct, including over persistent client connections.

**Main areas:** Proxy streaming, framing conversion, origin fixtures.

### 16. Add proxy backpressure, limits, and failure handling

**Goal:** Remain bounded and predictable with slow or failing peers.

**Implement:**

- [ ] Pause upstream reads at the downstream output high-water mark and resume below the low-water mark.
- [ ] Bound active upstream sockets, pending connects, DNS work, and per-session buffers. Introduce the admission checks now; exercise concurrent saturation when drivers are added.
- [ ] Enforce connect/response/progress deadlines. Map resolution/connect/invalid-response failures to 502 and upstream timeouts to 504 before downstream output starts. Configure the DNS deadline now, but enforce it through asynchronous helper dispatch in milestone 22; the minimal driver still blocks during resolution at this stage.
- [ ] On upstream failure after response output starts, close the client connection without appending a second response or a successful final chunk.
- [ ] Cancel/clean up the upstream transaction when the client disconnects. Record basic transaction outcomes for later metrics.

**Completion checks:** Slow readers, slow origins, truncated bodies, malformed origin framing, failed resolution/connect, timeouts, and client disconnects release resources. Backpressure bounds output memory and still allows progress when the client resumes reading.

**Phase checkpoint:** The proxy correctly forwards supported HTTP traffic with caching disabled. Preserve this mode as the comparison baseline for later benchmarks.

## Phase E: HTTP cache and LRU storage

### 17. Implement bounded LRU storage independently of HTTP policy

**Goal:** Store immutable entries and evict them predictably.

**Implement:**

- [ ] Define the cache key as scheme, normalized host, effective port, and exact path/query. Preserve query order and escaped-path distinctions.
- [ ] Define `CacheEntry` with status, end-to-end metadata, transfer-decoded body, content encoding, validators, timing, and byte cost.
- [ ] Implement map-plus-list lookup, promotion, insertion/replacement, removal, and least-recently-used eviction.
- [ ] Enforce resident byte, entry-count, and per-object limits, including metadata in the chosen accounting policy.
- [ ] Protect map/list changes with a mutex and return `shared_ptr<const CacheEntry>` snapshots. Never hold the lock during serialization or I/O.

**Completion checks:** Known access sequences evict the expected entries. Replacement updates costs correctly. Oversized objects are rejected; an evicted entry remains readable by an existing response reference.

**Main areas:** `include/cache/`, `src/cache/`, LRU and accounting tests.

### 18. Decide which transactions may use or populate the cache

**Goal:** Encode the architecture's conservative shared-cache rules as pure decisions.

**Implement:**

- [ ] Initially store only complete 200 responses to unconditional GET requests.
- [ ] Bypass requests with Authorization, Cookie, Range, or client conditionals. Forward HEAD without using it to replace a cached GET body.
- [ ] Reject storage for Set-Cookie, any Vary, no-store/private, partial, malformed, interrupted, or oversized responses. Initially bypass storage for close-delimited responses.
- [ ] Parse Cache-Control case-insensitively, including quoted values. Handle conflicting/malformed values conservatively and apply field-qualified restrictions to the entire response.
- [ ] Separate request lookup eligibility, insertion eligibility, and validation requirements. Request no-store bypasses lookup and insertion.
- [ ] Evaluate `only-if-cached` before any bypass branch can initiate network access.

**Completion checks:** A policy matrix covers every bypass/storage restriction, combinations of directives, HEAD, and cache-only requests. No ineligible transaction is accidentally reused or inserted.

**Main areas:** `CachePolicy`, directive parser, policy tests.

### 19. Calculate freshness and current Age with a fake clock

**Goal:** Determine whether a stored response is reusable now.

**Implement:**

- [ ] Select explicit lifetime in priority order: `s-maxage`, `max-age`, then `Expires`. With no explicit lifetime, treat the entry as immediately stale and retain only useful validation candidates.
- [ ] Implement the architecture's apparent-age, response-delay, corrected-initial-age, and resident-time calculations using paired wall/monotonic timestamps.
- [ ] Apply request max-age/min-fresh and request/response no-cache. Require validation when stale; never serve stale data, including when max-stale permits it.
- [ ] Honor must-revalidate/proxy-revalidate and return decisions such as hit, fetch, revalidate, bypass, or cache-only failure.
- [ ] Clamp arithmetic, handle invalid/absent dates conservatively, and generate an updated Age for cached responses.

**Completion checks:** Fake-clock cases cover already-aged origin responses, transit delay, exact expiry, quoted directives, wall-clock movement, malformed values, no-cache, and cache-only requests requiring validation.

**Main areas:** Cache freshness evaluator, HTTP dates, clock-based tests.

### 20. Integrate cache misses and fresh hits into the proxy

**Goal:** Repeated eligible requests can be served without contacting the origin.

**Implement:**

- [ ] Consult policy/storage before resolving an origin. Serve a usable fresh hit with newly serialized framing and current Age.
- [ ] On a miss, forward normally and build a bounded cache candidate alongside downstream streaming. Commit only after full valid receipt.
- [ ] Stop retaining candidate bytes when an object exceeds its limit, while continuing to forward it.
- [ ] Track resident entries, pinned response references, and candidate buffers separately. Enforce a global in-flight memory budget as well as resident limits.
- [ ] Return 504 for unsatisfied only-if-cached requests without DNS/connect activity. Until milestone 21, fetch stale entries unconditionally; never serve them as fresh.
- [ ] Add cache-enabled/disabled configuration and hit/miss/bypass/insertion/eviction counters.

**Completion checks:** A second fresh GET causes no origin request. Expiry causes a fetch. Interrupted or oversized responses never become hits. Cache-only requests produce zero origin traffic even on bypass paths, and all memory categories stay bounded.

**Main areas:** Proxy/cache integration, memory accounting, controlled-origin request counters.

### 21. Revalidate stale entries and handle replacement

**Goal:** Reuse a stored body only after successful origin validation.

**Implement:**

- [ ] Prefer a stored ETag in If-None-Match; otherwise use Last-Modified in If-Modified-Since. Preserve weak validator syntax.
- [ ] On a matching 304, merge permitted metadata, recompute freshness/age, retain the body, and serve cached 200 to the unconditional client GET.
- [ ] Handle unexpected or mismatching 304 with an unconditional retry or upstream error; never attach an unrelated cached body.
- [ ] Replace with a new 200 only after complete eligible receipt. Remove entries when revalidation changes eligibility or an authoritative changed response cannot replace them.
- [ ] Return an error on upstream failure rather than stale content. Keep client conditional requests on the bypass path.
- [ ] Record validation requests/results separately from ordinary hits and misses.

**Completion checks:** Test unchanged 304, changed 200, weak validators, Last-Modified fallback, metadata changes, changed cache eligibility, mismatched 304, interrupted replacement, and failed revalidation.

**Phase checkpoint:** With the single-session driver, the server and proxy implement the planned HTTP and cache behavior. Enforced DNS deadlines still depend on the helper service in milestone 22. Adding concurrency must preserve the existing HTTP and cache decisions.

## Phase F: Concurrency and lifecycle

### 22. Add a bounded helper service for file and DNS work

**Goal:** Make blocking work available through completion events before introducing the event loop.

**Implement:**

- [ ] Add a bounded job queue, helper threads, and explicit admission failure behavior for file metadata/reads and name resolution.
- [ ] Deliver completion events carrying connection generations. Wake the owning driver and discard stale completions safely.
- [ ] Enforce client-visible DNS deadlines without waiting for an in-progress resolver call to finish. Abandon the timed-out transaction, return 504 when appropriate, and discard its eventual result while continuing other work.
- [ ] Keep file chunk jobs bounded by downstream demand; helpers do not own or close driver sockets.
- [ ] Distinguish helper workers from client-session workers so a session worker cannot deadlock waiting on its own saturated pool.
- [ ] Add stop/cancel behavior for queued jobs and document the limitation of in-progress blocking resolver calls.

**Completion checks:** Delayed file/DNS work completes through events. DNS deadlines expire even while a resolver job remains blocked. Queue saturation remains bounded, closed sessions ignore late results, and cancellation releases results without touching reused connections.

**Main areas:** Helper queue/service, file/resolver actions, completion events, lifecycle tests.

### 23. Implement the thread-pool driver

**Goal:** Handle several client sessions concurrently with fixed worker capacity.

**Implement:**

- [ ] Add one acceptor, a bounded accepted-connection queue, and a configurable fixed number of workers.
- [ ] Give each worker ownership of one client session and its current upstream transaction. Drive the existing state machine with nonblocking I/O plus readiness waits.
- [ ] Enforce request/idle/progress deadlines and keep-alive request limits so slow clients cannot occupy workers indefinitely.
- [ ] On saturation, send a bounded 503 when feasible or close promptly. Do not create an unbounded fallback queue.
- [ ] Expose `--concurrency thread-pool` and `--workers`; keep networking runtime alive until all workers finish.

**Completion checks:** Multiple clients progress independently up to configured capacity. Excess load is bounded. Run the complete server, proxy, and cache integration suite through this driver.

**Main areas:** Thread-pool driver, accept queue, worker lifecycle, concurrency tests.

### 24. Verify shared cache and memory behavior under concurrency

**Goal:** Concurrent requests preserve cache correctness and resource limits.

**Implement:**

- [ ] Audit synchronization for lookup/promotion, replacement, eviction, statistics, and global memory reservations.
- [ ] Keep immutable snapshots valid while another worker evicts or replaces an entry.
- [ ] Make reservation/release paths cover cancellation, errors, and oversized candidates. Include temporary buffers and pinned entries in the memory model.
- [ ] Allow duplicate concurrent misses initially and count them. Ensure competing fetch/validation completions cannot merge metadata with a different stored body.
- [ ] Exercise concurrent origin changes, revalidation, and slow consumers using deterministic synchronization in tests.

**Completion checks:** Parallel hits, misses, evictions, replacement, and disconnects cause no corruption, deadlocks, mixed body/metadata, or budget overshoot. Use race-detection tooling where supported and record platform coverage.

**Main areas:** Cache synchronization/accounting, concurrent integration tests, counters.

### 25. Build the event-loop driver for static serving

**Goal:** One loop thread can advance many static-server sessions.

**Implement:**

- [ ] Watch the listener, client sockets, and wakeup channel in one poller. Keep socket operations and session ownership on the loop thread.
- [ ] Advance only ready sessions using the same session logic, parser, serializer, and static service.
- [ ] Dispatch file work to the helper service and process generation-checked completions.
- [ ] Set the poll timeout from the nearest deadline and check timers every iteration.
- [ ] Expose `--concurrency event-loop` for server mode; keep file access and blocking logging off the loop thread.

**Completion checks:** Many idle clients coexist with active requests. Static-server and persistent-connection tests pass unchanged through the event-loop driver. Delayed file jobs do not stop unrelated cached/buffered work or timer processing.

**Main areas:** Event-loop driver, registration/session ownership, helper dispatch.

### 26. Run proxy and cache sessions through the event loop

**Goal:** The second driver supports the full application feature set.

**Implement:**

- [ ] Register upstream sockets alongside client sockets and route events to the correct transaction.
- [ ] Execute DNS through helpers and progress nonblocking connects using pending-error checks and connection deadlines.
- [ ] Support upstream request writes, response reads, cache lookup/insertion/revalidation, and downstream output with the existing shared logic.
- [ ] Pause/resume upstream readiness interest according to downstream capacity. Remove writable interest when no output remains.
- [ ] Bound active sessions, upstream sockets, pending jobs, and connecting sockets across the loop.

**Completion checks:** The same proxy/cache suite passes in both concurrency modes. Mixed cache hits, slow misses, and DNS/connect failures do not stall unrelated sessions or exceed resource limits.

**Main areas:** Event-loop upstream scheduling, cache integration, driver-parameterized tests.

### 27. Add fairness and verify overload recovery

**Goal:** A busy connection or exhausted queue cannot monopolize the application.

**Implement:**

- [ ] Limit bytes/events handled per session per event-loop iteration. Apply budgets to accepts, reads, writes, and completion processing.
- [ ] Coordinate high/low-water marks across upstream reads, file jobs, client writes, and cache candidates.
- [ ] Verify every bounded queue has explicit rejection or retry behavior that does not busy-spin.
- [ ] Collect active-connection counts, queue depths, paused producers, timeouts, and overload rejections for diagnosis.

**Completion checks:** Run a large transfer beside small requests, a slow reader beside a fast reader, and overloaded helper/accept queues. Timers and unrelated requests continue progressing, memory stays bounded, and capacity recovers after load subsides in both drivers.

**Main areas:** Driver scheduling, backpressure, admission control, overload tests.

### 28. Complete graceful shutdown and resource cleanup

**Goal:** Stop predictably while requests or background jobs are active.

**Implement:**

- [ ] Add portable lifecycle control with isolated OS signal/console hooks that safely request stop and wake driver waits.
- [ ] Stop accepting, drain active transactions up to a configured deadline, cancel queued work, unregister/close sockets, join threads, and finally destroy the networking runtime.
- [ ] Release file handles, cache candidates, pinned references, and memory reservations on every termination path.
- [ ] Reject late events using generations and prevent workers/helpers from closing another driver's sockets.
- [ ] Decide and document the resolver strategy: without cancellable native resolution, do not claim a strict shutdown bound for an in-progress blocking DNS call.

**Completion checks:** Stop during idle keep-alive, queued clients, file reads, DNS, connect, body streaming, and revalidation. Both drivers terminate cleanly within the documented guarantees; repeated start/stop runs reveal no handle/thread leaks.

## Phase G: Completion and evaluation

### 29. Finalize configuration, error handling, and observability

**Goal:** Expose a consistent, understandable application interface.

**Implement:**

- [ ] Validate server/proxy mode, thread-pool/event-loop mode, bind address, port, document root, worker/helper counts, cache enablement/budgets, queue/connection limits, timeouts, and keep-alive limits.
- [ ] Retain loopback binding by default and require an explicit bind-address option for remote load tests. Supply `--help`, sensible defaults, and clear invalid-configuration errors.
- [ ] Audit the architecture's status map: 200/304, 400/403/404/405/408, 413/414/417/431, and 500/501/502/503/504/505. Emit errors only when a coherent response can still be sent.
- [ ] Aggregate completed/error requests, useful bytes, active connections, queue depths, timing, cache hits/misses/bypasses/evictions/validations, and resident/in-flight memory.
- [ ] Keep logging bounded and outside the loop's blocking path. Isolate OS-specific process CPU/memory sampling.
- [ ] Document configure/build/test commands and executable paths, including the Windows developer-shell requirement for MSVC.

**Completion checks:** Every documented startup example works in its appropriate mode. Invalid inputs fail clearly. Counters match a small known workload and errors retain the expected status/close behavior in both drivers.

**Main areas:** Configuration, `src/main.cpp`, `observability/`, CMake, usage documentation.

### 30. Complete the cross-platform correctness gate

**Goal:** Establish a verified baseline before measuring performance.

**Implement:**

- [ ] Parameterize the end-to-end suite across server/proxy and thread-pool/event-loop combinations instead of maintaining separate behavior tests.
- [ ] Run parser split-position, malformed-message, EOF, persistent-connection, file containment, proxy, cache, overload, cancellation, and shutdown cases.
- [ ] Add a repeatable Windows/Linux/macOS build-and-test matrix using CMake, Ninja, and CTest, in CI where available.
- [ ] Run supported memory/undefined-behavior/race diagnostics and a bounded resource-soak test; document unsupported tooling or unresolved failures.
- [ ] Confirm that runtime HTTP behavior uses project code, the C++ standard library, and OS APIs. External clients/test tools remain test dependencies only.

**Completion checks:** All required platform/driver combinations pass, with reproducible commands and recorded results. A platform that has not been run remains an open acceptance item, not an assumed pass.

**Main areas:** `tests/`, test fixtures, build matrix, validation record.

### 31. Create reproducible benchmark workloads

**Goal:** Compare both drivers under controlled, identical conditions.

**Implement:**

- [ ] Add workload definitions and repeatable commands under `benchmarks/`, including a controlled origin and configurable slow clients/origins.
- [ ] Cover concurrency 1/10/50/100/500/1000 as host limits allow, workers 1/2/4/8/16, and objects 1 KiB/64 KiB/1 MiB/16 MiB.
- [ ] Cover reused versus new connections, repeated versus varied files, and proxy cache-disabled/cold/warm/revalidation/mixed-popularity workloads.
- [ ] Use Release builds with the same files, cache budget, deadlines, logging, and origin setup. Record compiler, OS, CPU, memory, socket limits, backend, worker/helper counts, and filesystem-cache state.
- [ ] Define warmup, measurement interval, at least five repeats, cold-cache reset, warm-cache prefill, response correctness checks, and load-generator saturation checks.
- [ ] Distinguish fixed-concurrency from fixed-arrival-rate tests. Document shared-host contention or use a separate load generator where available.

**Completion checks:** A small smoke benchmark runs each scenario family in both modes and writes machine-readable raw results with configuration metadata. Incorrect/dropped responses are recorded as errors rather than successful throughput.

**Main areas:** `benchmarks/`, fixtures, scripts, benchmark instructions.

### 32. Run the comparison and document the completed project

**Goal:** Finish with working software and evidence explaining its tradeoffs.

**Implement:**

- [ ] Run the planned benchmark matrix within documented host limits and retain raw data from every repeat.
- [ ] Report requests/second, useful bytes/second, p50/p95/p99 latency, errors/timeouts, CPU, peak memory, connection/queue counts, cache statistics, and validation traffic.
- [ ] Produce throughput-versus-concurrency and p99-latency-versus-concurrency plots, with memory/CPU/cache observations and repeated-run variation.
- [ ] Explain bottlenecks and distinguish the CPU execution budget of one loop thread from a multiworker pool. Make conclusions specific to measured workloads and operating systems.
- [ ] Finish the README/run guide, configuration reference, testing instructions, results report, and known limitations. Reconcile architecture documentation with the actual implementation.

**Completion checks:** Another developer can build, run both modes, execute the correctness suite, and reproduce a benchmark from the saved instructions. The report supports its conclusions with saved measurements.

## Final definition of done

- [ ] Static server: contained file lookup, GET/HEAD, MIME types, streaming, validators, error responses, and persistent connections.
- [ ] Forward proxy: target/authority parsing, correct headers, all planned response framing, deadlines, bounded streaming, and failure handling.
- [ ] Cache: conservative eligibility, explicit freshness/Age, cache-only semantics, revalidation, immutable entries, LRU, and resident/in-flight limits.
- [ ] Concurrency: the same application behavior passes under thread-pool and event-loop drivers, including slow peers and overload.
- [ ] Portability: Winsock and POSIX implementations pass recorded Windows, Linux, and macOS checks.
- [ ] Lifecycle: resource ownership, generation checks, cancellation, and shutdown match documented guarantees.
- [ ] Evaluation: reproducible workload commands, raw repeated measurements, plots, and a measured comparison are included.

## Deferred extensions

The first project version is complete without the following. Add them only after the final definition of done, as separate follow-up plans:

- HTTPS/TLS, CONNECT tunnels, protocol upgrades, HTTP/2, or HTTP/3.
- Dynamic handlers, generated compression, or persistent on-disk caching.
- Vary-based variants, authenticated cache rules, range assembly, or cache-side client conditional evaluation.
- Upstream connection pooling or coalescing duplicate concurrent cache misses.
- Native `epoll`/`kqueue` backends or a separate completion-oriented IOCP driver.
- Stronger filesystem containment against concurrent path replacement using native file-opening APIs.
