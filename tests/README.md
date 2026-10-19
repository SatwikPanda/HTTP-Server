# Milestone acceptance tests

The milestone 01–06 suites were written before milestone 03 implementation. `CHECK` remains
active in Release builds; the original regression tests are compiled with
`NDEBUG` undefined. Every CTest executable has a timeout so a deadlock fails.

```powershell
cmake -S . -B build/milestones -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/milestones
ctest --test-dir build/milestones -L 'milestone0[12]' --output-on-failure
ctest --test-dir build/milestones -L milestone03 --output-on-failure
ctest --test-dir build/milestones --output-on-failure
cmake -S . -B build/milestones -DREQUIRE_ALL_MILESTONES=ON
ctest --test-dir build/milestones -L 'milestone0[123456789]' --output-on-failure
```

Milestones 04–06 have production-backed adapters and run their acceptance checks.
Milestones 07–09 call production HTTP code directly and need no adapter.
A suite without its adapter returns CTest's skip code 77, which is pending,
never passing. To enforce completion (turn a pending suite into a failure),
configure with `-DREQUIRE_ALL_MILESTONES=ON`. Use that gate when completing each
milestone. Do not add a mock implementation solely to make acceptance assertions
pass. Each adapter must call production code; only clocks and I/O may be faked.

Every build also compiles the milestone 04–06 assertion bodies as object targets
against the declaration-only headers in `tests/contracts/`. These objects check
that the contracts still compile and are never counted as passing CTests.

- **01:** independent accepted socket lifetime, runtime moves, occupied ports,
  self/move assignment, invalid operations, handle reuse, move-only results.
- **02:** binary/fragmented/large echo, bounded buffer, forced partial writes and
  would-block, idle/write timeouts, zero-progress writes, reset/error outcomes.
- **03:** level-triggered read/write readiness, generation and owner validation,
  move/close lifetime checks, foreign/stale tokens, deadline expiry, concurrent
  coalesced wakeups and draining, IPv4/IPv6, pending errors on refused connects,
  fresh-socket candidate fallback, resolver validation, stop/restart during waits.
- **04 adapter:** `tests::SessionHarness(input_limit, output_limit, idle_timeout)`
  injects I/O and a steady clock into the real session. It provides
  `force_write_chunk(size)`, `block_next_writes(count)`, `receive(bytes)` (feed
  fragments under backpressure), `peer_eof()`, `run_until_idle()`, `advance(ms)`,
  `stop(drain_deadline)`, `output()`, `closed()`, `peak_input_bytes()`,
  `peak_output_bytes()`, and `open_resources()`. Also provide
  `echo_session_real_socket_roundtrip(bytes)` and
  `driver_accepts_two_sequential_clients()` returning bool.
  The suite also checks manual steady and system clocks: initialization, setting,
  positive/negative advancement, zero increments, and native-tick truncation.
- **05 adapter:** `fixed_response()` returns a response under a fixed wall clock;
  `serialize(response)` returns `Result<string>`; `content_length`, `header`,
  `headers`, and `add_header` expose production message fields.
  `fixed_http_roundtrip(request, max_write_chunk)` runs the real demo with forced
  short writes under the same clock and returns raw bytes.
  `oversized_demo_header_is_rejected()` checks the configured header bound.
- **06 adapter:** `ParserHarness::consume(string_view)` returns `{consumed,error}`;
  expose `complete()`, `target()`, `headers(name)`, and `reset()`. Provide
  `rejects_target_limit(limit,target)`, `rejects_header_byte_limit(limit)`,
  `rejects_header_count_limit(limit)`, `rejects_connection_input_limit(limit)`,
  and `maps_{syntax,version,header_limit}_to_status(status)`. Each helper feeds
  actual malformed/oversized input to the production parser or session.
  `tests/milestone06_adapter.hpp` wraps `http::RequestParser` and
  `http::HttpSession`; each limit helper also checks that input exactly at the
  limit is accepted, so the rejection is caused by that limit.

Only local loopback networking is used. IPv6 is exercised when supported. DNS
tests use numeric literals and localhost; no external origin is required.

## Request parsing (milestone 06)

`http::RequestParser` consumes requests incrementally and leaves later bytes
unconsumed. Its head parsing requires CRLF line endings and exactly
one space between request-line parts, accepts HTTP/1.0 and HTTP/1.1 (other
well-formed versions get 505), validates field names and values, keeps repeated
fields, rejects obsolete line folding, and requires exactly one valid `Host` for
HTTP/1.1. Host values are limited to IP literals, IPv4 addresses, and DNS-style
names with an optional port, so `a,b` is rejected. Unknown method tokens are
kept for the application to reject. Empty lines before the
request line are ignored but count toward the header limit.

Limits are checked as each byte arrives: request line (8192), target (8000,
414 URI Too Long), whole head (8192, 431), field count (100, 431), and the
session's connection input buffer (16 KiB of retained head/body/extensions/trailers
plus unconsumed input, 431). Other head rejections are 400. The session answers a
rejected head and closes, because the stream can no longer be framed.

`http_parser_test` adds every three-way split of a request, pipelined
boundaries at every read size, each malformed case mapped to its specific
error and status, early rejection of garbage and overlong lines, exact limit
boundaries, and session-level preservation of bytes after the head.

## Request framing (milestone 07)

The parser accepts unframed, fixed-length, and chunked request bodies, stores
decoded bytes separately from trailers, and exposes `finish_on_eof()` to reject
truncated messages. Identical repeated/list Content-Length values are accepted;
conflicting, empty, malformed, or overflowing values are rejected. TE plus CL,
repeated chunked codings, unsupported transfer codings, and HTTP/1.0 TE fail.
The shared field parser rejects folding and invalid names/values in both heads
and trailers. Routing, framing, authentication, response-control, and
Connection-nominated fields are prohibited in trailers.

Defaults bound decoded bodies to 10 MiB, each complete chunk-size line to 8192
bytes, extensions (including separators/whitespace) to 1024 bytes, and trailers
to 4096 bytes/32 fields. Extensions validate token/quoted-string syntax. Chunk
sizes and cumulative body sizes are checked without overflowing. EOF preserves
an earlier error until reset.

`HttpSession` rejects nonempty GET/HEAD bodies and closes. Unsupported Expect
receives 417 as soon as the head is known, including when the client is waiting
to send a body. Disabled methods receive 405 with `Allow: GET, HEAD`; unknown
methods receive 501; CONNECT and Upgrade are rejected. `milestone07_test` covers
fragments, boundaries, limits, early policy rejection, and bounded trailer input.

## Response parsing and output (milestone 08)

`http::ResponseParser(method, limits)` parses HTTP/1.0 or HTTP/1.1 origin bytes
without opening an upstream connection. It requires a three-digit 100–599 status
and a space after the status even for an empty reason phrase. Repeated fields
retain their order. It accepts fixed-length, chunked, and close-delimited bodies;
only close-delimited bodies complete at EOF. HEAD/204/304 stop at the end of the
head, preserving later bytes. Informational responses are stored separately
until a final response arrives; the header-byte budget covers their combined
heads and bounds their accumulation. 101 and successful CONNECT responses fail.
Framing validation is shared with requests. This implementation also rejects
framing fields on 1xx/204, ambiguous HEAD/304 metadata, and unsupported transfer
codings. Content-Encoding bytes, including compressed NUL-containing data, stay
opaque. The shared request-line limit applies to response status lines.

`serialize_response(response)` chooses fixed length or the response's explicit
chunked TE. The overload taking `Method::HEAD` emits representation metadata
without a body. The overload taking `BodyFraming` explicitly selects fixed,
chunked, close-delimited, or valid bodyless output; close-delimited output sets
`Connection: close` and requires the caller to close the transport. CL and TE
are mutually exclusive. Bodyless statuses emit no chunks/trailers; 304 may retain
length/TE metadata. A populated body on a bodyless status is rejected.

`encode_chunk` and `encode_final_chunk` return `core::Result<string>`, bound their
output, and validate generated trailer fields. An empty `encode_chunk` emits a
terminator, so use it or `encode_final_chunk`, never both for the same terminator.
`SerializationLimits::max_body_bytes` bounds emitted data; `max_head_bytes`
separately bounds the head, chunk framing, and final chunk with trailers.

`milestone08_test` and `http_response_test` cover every two-way split of valid
and malformed fixtures, bytewise input, representative three-way splits, bytes
after completion, interim responses, no-body boundaries, EOF at every truncated
position, numeric overflow, exact limits, trailer injection, and serializer/parser
round trips for all body modes. These are production-backed fake-origin checks;
real upstream proxy transactions remain milestones 13–16.

## Keep-alive and pipelined requests (milestone 09)

`http::HttpSession` supports HTTP/1.1 persistent connections and pipelining when
keep-alive is enabled (`HttpSessionConfig::keep_alive`). The session maintains its
connection-wide input buffer (`input_`) across requests, bounded by `max_input_bytes`.
Only completed HTTP transactions are reset: `parser_.reset()`, response buffer
clearing, and progress updates occur between transactions while unconsumed bytes remain
buffered for subsequent requests.

Requests are processed sequentially one at a time, preserving strict response order.
HTTP/1.1 persistence defaults to open unless `Connection: close` is requested by client
or server. HTTP/1.0 connections close by default unless `Connection: keep-alive` is
present. Responses advertise `Connection: keep-alive` when continuing and
`Connection: close` when terminating.

Configurable timeouts distinguish connection phases:
- `idle_timeout`: deadline while waiting between transactions on an idle persistent
  connection; upon expiry, the connection closes cleanly without an error response.
- `request_progress_timeout`: deadline during partial incoming request receipt.
- `output_progress_timeout`: deadline during response write progress.
- `max_requests_per_connection`: bounds the number of requests per connection; the final
  allowed request emits `Connection: close` and closes transport afterwards.

Framing errors (syntax, unsupported versions, oversized headers) and application rejections
(nonempty GET/HEAD bodies, disabled methods, unsupported expectations) set close-demanded,
emit the appropriate error status with `Connection: close`, and immediately close transport,
ensuring invalid or unread body bytes cannot desynchronize subsequent pipelined requests.

`milestone09_test` covers sequential requests, pipelining within single and multi-chunk
reads, client close headers, HTTP/1.0 persistence rules, max request caps, phase-specific
deadlines, framing/rejection isolation, and loopback TCP integration with `HttpServer`.

## Fixed-response HTTP demo

The executable serves `Hello, World!\r\n` for bodyless GET, and the corresponding
metadata without body bytes for HEAD, then closes. Request framing and method
restrictions are enforced; persistent keep-alive connections are supported in
`HttpSession`. `--buffer-size` limits the incoming
header block (4–16777216 bytes; default 4096); the library defaults to 8192 header
bytes and 1 MiB total response bytes. Both default I/O progress timeouts are 5 s.

After building, run these commands in separate terminals:

```powershell
.\build\agents\httpserver.exe --host 127.0.0.1 --port 8080
curl.exe --include http://127.0.0.1:8080/
```

On Linux/macOS, use `./build/agents/httpserver` and `curl`. Ctrl+C stops the
listener and wakes active I/O; join any library driver before destroying its
server or network runtime.

The milestone 05 gate includes `milestone05_test`, `http_serialization_test`,
`http_session_test`, and `http_integration_test`. They check production message
creation, repeated fields, generated-field injection, request-line injection,
fixed/binary lengths, output bounds, every read chunk size of a representative
request, exact header bounds, premature EOF, bare-LF rejection, short writes,
would-block, zero/oversized progress, native errors, read/write deadlines,
manual-clock drain bounds, and wall-clock jumps. Real nonblocking loopback tests
exercise IPv4/IPv6, half-close, sequential clients, idle clients, occupied ports,
stop during accept/read waits, and restart. Unsupported IPv6 is reported.

## Verified status (2026-10-20)

All targets build on Windows with GCC 16.2 Release (`build/agents`).
The build passes all 18 tests in the strict milestone 01–09 gate, with zero failures
and zero skips. IPv4 loopback ran. The sandbox still blocks local
socket connections with native error 10013; full gates ran with approved local
loopback access. Existing MSVC `/DNDEBUG` versus `/UNDEBUG` warnings are expected.
CI now gates milestone 09; Linux/macOS acceptance of milestones 07–09 and remote
results for these changes remain pending. No new Linux/macOS run is claimed.

## Verified status (2026-10-12)

Milestone 06 is implemented. On Linux (GCC 13.3 Release, strict mode), all 14
tests passed in the strict milestone 01–06 gate, with zero failures and zero
skips. The same suites passed under Clang 18 Debug and under GCC
AddressSanitizer/UndefinedBehaviorSanitizer. A MinGW-w64 GCC 13 Windows build
compiled every target, and its HTTP and milestone 05–06 tests passed under
Wine, which is not real Windows. Six deliberate parser faults (Host not
required, bytes after the head dropped, bare LF accepted, an off-by-one head
limit, folding accepted, parsing past a complete head) each failed at least
one test. IPv6 was unavailable on the Linux host, so the IPv6 integration
cases did not exercise IPv6 there. Windows and macOS acceptance of milestone 06
is pending a CI run; the CI gate now includes milestone 06.

Known limitation, not new in milestone 06: when a session rejects a request
before reading all of it, it closes with unread input, and the peer's TCP stack
may then discard the error response on reset. A bounded lingering close belongs
with the keep-alive and shutdown milestones.

## Verified status (2026-10-11)

Every target builds on Windows with GCC 16.2 (Release), Clang 22.1.8 (Debug), and
MSVC 19.29 (Release). Each passes all 12 tests in the strict milestone 01–05 gate,
with zero failures and zero skips. IPv4 and IPv6 HTTP integration ran on this host.
The separate non-strict all-suite run passed the implemented tests and skipped
milestone 06. The sandbox blocks loopback with native error 10013, so network
tests ran with approved loopback access. MSBuild also needed execution outside
the sandbox. Existing MSVC NDEBUG override warnings are intentional for the
earlier assert-based tests; HTTP checks now use Release-active CHECK.

The Windows/Linux/macOS GitHub Actions matrix passed for commit `f7755a4` in
[run 38137762406](https://github.com/SatwikPanda/HTTP-Server/actions/runs/38137762406).
All three jobs successfully configured, built every target in Release, and ran
the strict milestone 01–05 gate. Linux/macOS were verified in CI rather than
locally. Milestone 06 remains pending. The older-MSVC milestone 03 future compile error
described below is now resolved by checking Result in the worker before returning
the vector; production Result semantics are preserved.

### Earlier verification (2026-10-10)

After milestone 04 implementation, the manual clocks needed explicit
`duration_cast` conversion from nanoseconds to each native clock duration.
Implicit conversion fails for the coarser system-clock ticks on MSVC and macOS.
Sub-tick increments truncate toward zero on each call; they are not accumulated.
Windows GCC 16.2 Release passed all eight tests for milestones 01–04 in strict
mode. MSVC 19.29 Release built `httpserver` and `milestone04_test`, and passed
the milestone 04 suite. Its full build encountered a separate older-STL
`std::future<Result<...>>` default-construction error in `milestone03_test`.
The updated Windows/Linux/macOS CI gate includes milestone 04; remote results
for this fix remain pending.

The milestone 01–02 gate passed before milestone 03 implementation. It found and
prevented an infinite loop on a successful zero-byte write. Following milestone
03 implementation, both Windows GCC 16.2 Release and Clang 22.1 Debug passed all
seven registered tests for milestones 01–03 in strict mode (zero skips). The
three future assertion bodies also compile against declaration-only contracts.
The strict milestone 04–06 gate was checked separately and correctly failed
with NOT IMPLEMENTED for each pending milestone.

The first Linux CI run exposed a retry spin caused by truncating the last
fraction of a millisecond to zero. The retry adapter now rounds sleeps up,
rechecks early wakeups, and reports timeout before issuing another I/O attempt
after the deadline. Milestone 02 checks blocked reads and writes with a 1 ms
deadline and a longer retry delay, as well as immediate error/zero-progress exits.

The first macOS CI run exposed an invalid refusal fixture: a bound socket that
never listened can silently drop connection attempts on Darwin (see the
`TCPS_CLOSED` branch in [Apple's TCP input source](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/netinet/tcp_input.c)).
The fixture now closes a temporary listener before connecting to its port, and
allocates the fallback listener first to keep the two candidates distinct. It
also verifies that an all-refused candidate list reports connection refusal.

After these corrections, both Windows builds again passed the seven-test strict
gate. The deadline regression passed 20 consecutive runs per build. Linux and
macOS results after the corrections remain pending a CI rerun.

The Windows test sandbox prevents loopback connections, so networking tests
were executed with approved loopback access. Linux and macOS have not run
locally; the CI matrix now checks all three operating systems. These local
results do not claim final cross-platform acceptance.

The poller has one owner for registration/wait/socket operations; only wake()
is thread-safe across callers. Unwatch before closing and validate saved events
with is_current() before acting on them. Sockets may move while watched; their
registration tracks the underlying lifetime rather than the wrapper's address.
Destroy NetworkRuntime after all sockets, pollers, and driver threads. Stop and
join the server's driver before destroying EchoServer. A stopped server resumes
only after an explicit start().

Name lookup is synchronous and should run outside an event loop. The outbound
adapter takes already resolved candidates and shares one deadline across fresh
socket attempts. IPv6 candidates with scope IDs are currently unsupported by
Endpoint and are filtered; ordinary IPv4/IPv6 loopback is tested.
