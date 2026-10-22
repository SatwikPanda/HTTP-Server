# Proxy branch handoff (`B — proxy`, milestones 13–16)

## Base identity and dependencies

- **Branch:** `codex/proxy`
- **Base commit (`PARALLEL_BASE` / `main`):** `4547b48687d70b4d2d54370f01b6dfa946b80181`
- **Direct prerequisites:** [P0 (`parallel_foundation`)](parallel-foundation.md)
- **Downstream consumers:**
  - `K` (`cache-proxy`) consumes `proxy::make_forward_transaction`, `proxy::ExchangeObserver`, `proxy::ForwardingConfig`, and `proxy::prepare_outbound` / `proxy::prepare_downstream_head` / `proxy::forwardable_trailers` without duplicating the upstream state machine or header policy. `src/proxy/cached` remains reserved for `K`.
  - `E` (`helpers`) provides asynchronous DNS/helper execution so DNS deadlines can interrupt in-flight OS resolution.
  - `F` (`thread-pool`), `G` (`event-loop`), and `L` (`application-assembly`) wire CLI mode selection (`--mode proxy`) and execute `proxy::make_forward_factory` across multi-connection drivers.

## Owned files and targets

- Public headers:
  - [`include/proxy/forwarding.hpp`](../../include/proxy/forwarding.hpp)
  - [`include/proxy/transaction.hpp`](../../include/proxy/transaction.hpp)
- Production implementation and CMake target (`proxy` static library, linking `http`, `concurrency`, `net`, `core`, `Threads::Threads`):
  - [`src/proxy/CMakeLists.txt`](../../src/proxy/CMakeLists.txt)
  - [`src/proxy/forwarding.cpp`](../../src/proxy/forwarding.cpp)
  - [`src/proxy/transaction.cpp`](../../src/proxy/transaction.cpp)
- Acceptance suites and test helpers (`milestone13_test`..`milestone16_test` with labels `milestone13`..`milestone16`):
  - [`tests/proxy/CMakeLists.txt`](../../tests/proxy/CMakeLists.txt)
  - [`tests/proxy/proxy_test_support.hpp`](../../tests/proxy/proxy_test_support.hpp)
  - [`tests/proxy/milestone13_test.cpp`](../../tests/proxy/milestone13_test.cpp)
  - [`tests/proxy/milestone14_test.cpp`](../../tests/proxy/milestone14_test.cpp)
  - [`tests/proxy/milestone15_test.cpp`](../../tests/proxy/milestone15_test.cpp)
  - [`tests/proxy/milestone16_test.cpp`](../../tests/proxy/milestone16_test.cpp)

## Forwarding, framing, limits, and deadline rules

### Target and header policy (`proxy::prepare_outbound`, `proxy::prepare_downstream_head`, `proxy::forwardable_trailers`)

- Requires an absolute cleartext `http://` request target in proxy mode; origin-form (`/...`), asterisk-form (`*`), non-HTTP schemes, userinfo, fragments, malformed bracketed IPv6, and invalid ports are rejected with `400 Bad Request` before any resolution or socket work.
- Separates resolution target (`OutboundRequest::resolve`, unbracketed lower-case host + effective port) from wire authority (`target.authority.wire()`, preserving IPv6 brackets and non-default port).
- Rewrites outbound request line to origin-form (`target.path_query`, defaulting empty path to `/` while preserving percent-encoding and query order) with `HTTP/1.1` and regenerates a single `Host` header from the selected authority regardless of client `Host` headers.
- Parses all `Connection` header instances into a case-insensitive token set (rejecting malformed tokens with `400`) and strips all nominated fields, the fixed hop-by-hop set (`Connection`, `Keep-Alive`, `Proxy-Connection`, `Proxy-Authenticate`, `Proxy-Authorization`, `TE`, `Trailer`, `Transfer-Encoding`, `Upgrade`), and `Content-Length` before regenerating framing.
- Preserves repeated end-to-end headers in insertion order (such as `Set-Cookie` and `Accept`) and end-to-end `Authorization` / conditional / `Range` fields while consuming `Proxy-Authorization` / `Proxy-Authenticate` locally.
- Enforces `ForwardingConfig::max_via_bytes` (default `1024`) and `via_pseudonym` (default `"httpserver"`, max 64 token bytes). Detects `Via` loops in requests (`403 Forbidden`) and origin responses (`502 Bad Gateway`), and appends `<protocol-version> <pseudonym>` in both directions.
- Filters trailers through `forwardable_trailers`, dropping hop-by-hop fields, `Via`, `Connection`-nominated fields, and all RFC 9112 prohibited trailer fields (`Content-Length`, `Transfer-Encoding`, routing/authentication/caching/content-header fields).

### Transaction state machine, streaming, and backpressure (`proxy::make_forward_transaction`, `proxy::make_forward_factory`)

- State machine phases:
  `prepared -> resolving -> connecting -> sending -> reading_heads -> streaming -> validating -> complete`, with any nonterminal state transitioning to `failed` (or destructor cancellation) and releasing upstream resources.
- Uses one fresh origin connection per transaction (`Connection: close` outbound and explicit `UpstreamCloseWork` before completion, including after close-delimited EOF or bodyless `HEAD`/`204`/`304` responses) while allowing downstream client keep-alive across transactions.
- Handles `1xx` informational responses (`100`, `103`) within cumulative header budgets before the final head and rejects `101 Switching Protocols` with `502 Bad Gateway`.
- Selects downstream framing independently of upstream framing:
  - Bodyless responses (`HEAD`, `204`, `304`): `BodyFraming::fixed_length` when a valid representation `Content-Length` is present on `HEAD`/`304`, otherwise `BodyFraming::none`.
  - Known safe `Content-Length`: `BodyFraming::fixed_length`.
  - Unknown length (upstream `Transfer-Encoding: chunked` or close-delimited): `BodyFraming::chunked` for `HTTP/1.1` clients, which `HttpSession` converts to close-delimited `HTTP/1.0` output with keep-alive disabled for `HTTP/1.0` clients.
  - Opaque `Content-Encoding` is preserved untouched along with transfer-decoded representation bytes.
- Backpressure and memory bounds:
  - Upstream read size is capped by `min(downstream_body_capacity, ProxyLimits::max_read_bytes)` (default `16 KiB`), and `pump()` only runs when downstream output has drained, so at most one decoded body segment is queued in `HttpSession`'s `ResponseWriter` window (`HttpSessionConfig::max_output_bytes`).
  - While downstream writes are draining, no upstream read work is requested. When `OutputDrained` arrives, `body_deadline_` is refreshed before resuming consumption/reads so slow clients do not cause false origin stall timeouts.
  - Per-factory active transaction gauge (`ProxyStats::try_admit` / `ProxyLimits::max_active_transactions`, default `256`) rejects excess concurrent creations with `503 Service Unavailable` and returns `active` to `0` on transaction destruction.
- Phase deadlines and error mapping:
  - `connect_timeout` (default `5000 ms`): shared across `ResolveWork` and all `ConnectWork` candidates (capped by `max_candidates`, default `8`). Note: with P0's synchronous `ReferenceWorkExecutor`, DNS deadline checks run before and after the OS `resolve()` call; asynchronous mid-lookup timeout enforcement requires `E` (`helpers`).
  - `response_timeout` (default `10000 ms`): bounds outbound request writes and reading interim/final response heads.
  - `body_timeout` (default `10000 ms`): bounds intervals between useful upstream body progress while actively reading from the origin.
  - Pre-commit failures map to client status (`400`/`403`), `502 Bad Gateway` (resolve/connect/protocol/framing failure), `503 Service Unavailable` (admission/work rejection), or `504 Gateway Timeout` (deadline expiry). Post-commit failures abort the `ResponseWriter` and close the client connection without emitting a second response head or a terminating `0\r\n\r\n` chunk.

### `K` (`cache-proxy`) integration seam

- `ExchangeObserver` callbacks (`prepare`, `on_final_head`, `on_body`, `on_complete`, `on_failure`) and `ExchangeTiming` (`cache::Timing`: `request_sent`, `final_head_received`, `final_head_wall`) execute on the session owner thread under the same budgets and commitment rules.
- For bodyless responses (`HEAD`, `204`, `304`), `UpstreamCloseWork` executes before `on_complete` and before the `ResponseHead` step is returned, allowing `K` to intercept a validation `304` and substitute a cached `200` representation cleanly.

## Executed verification

Configured and built in `build/proxy` with Ninja, Release mode, `BUILD_TESTING=ON`, and `REQUIRE_ALL_MILESTONES=ON` on Windows (GCC 16.2):

```powershell
cmake -S . -B build/proxy -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DREQUIRE_ALL_MILESTONES=ON
cmake --build build/proxy --config Release
ctest --test-dir build/proxy -C Release -L '^(milestone0[1-9]|parallel_foundation|milestone1[3-6])$' --no-tests=error --output-on-failure
```

Result: **24/24 tests passed** (0 failures, 0 skipped, 19.08 s total):
- Base + P0 gate: 20/20 passed (`milestone01`–`milestone09`, `parallel_foundation_test`, `parallel_work_test`).
- `milestone13_test` (`milestone13`, 0.03 s): table-driven target normalization/rejection, IPv6 authorities, escaped path/query preservation, `Host` replacement, repeated and `Connection`-nominated header stripping, proxy credential removal, `Via` loop/length checks, response header/trailer filtering.
- `milestone14_test` (`milestone14`, 0.39 s): fragmented fixed-length `GET` and `HEAD` forwarding, short writes, would-block transitions, persistent client keep-alive across fresh origin connections, real loopback client+origin sockets, IPv6 loopback origin forwarding.
- `milestone15_test` (`milestone15`, 0.30 s): chunked-to-chunked with trailers and opaque `Content-Encoding`, close-delimited-to-chunked (`HTTP/1.1`) and close-delimited (`HTTP/1.0`), `100`/`103` interim heads, `204`/`304`/`HEAD` over a 5-request persistent pipeline, `101` rejection, smuggled bytes after `204` ignored, split response delivery across windows 1..24 and cut offsets.
- `milestone16_test` (`milestone16`, 4.43 s): pre-commit (`400`/`403`/`502`/`503`/`504`) vs post-commit truncation close without final chunk, slow origin head/body timeouts, slow client backpressure pause without false origin timeout, candidate cap and refused-first-candidate fallback, admission cap (`503`), client disconnect cancellation returning active gauges to zero, and `ExchangeObserver` hook verification.
