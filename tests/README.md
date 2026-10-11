# Milestone acceptance tests

All six suites were written before milestone 03 implementation. `CHECK` remains
active in Release builds; the original regression tests are compiled with
`NDEBUG` undefined. Every CTest executable has a timeout so a deadlock fails.

```powershell
cmake -S . -B build/milestones -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/milestones
ctest --test-dir build/milestones -L 'milestone0[12]' --output-on-failure
ctest --test-dir build/milestones -L milestone03 --output-on-failure
ctest --test-dir build/milestones --output-on-failure
cmake -S . -B build/milestones -DREQUIRE_ALL_MILESTONES=ON
ctest --test-dir build/milestones -L 'milestone0[12345]' --output-on-failure
```

Milestones 04–05 have production-backed adapters and run their acceptance checks.
Milestone 06 is **pending**, never complete or passing. It returns CTest's
skip code 77 until a production-backed adapter exists. To enforce completion
(turn a pending suite into a failure), configure with
`-DREQUIRE_ALL_MILESTONES=ON`. Use that gate when completing each milestone.
Do not add a mock implementation solely to make acceptance assertions pass.

The remaining future milestones have no stable production interfaces yet. Their
acceptance test bodies are fully specified against small adapters, to avoid
prematurely implementing those milestones. Add `tests/milestone06_adapter.hpp`
with the following contract when implementing
the corresponding feature; the test body
automatically becomes active at the next build. Link any new production targets
to the corresponding CMake test executable. Each adapter must call production
code; only clocks and I/O may be faked.

Every build compiles the milestone 04–06 assertion bodies as object targets.
Milestones 04–05 use their production adapters; milestone 06 uses the
declaration-only headers in `tests/contracts/`. These objects are never counted
as passing CTests. The future adapters provide production-backed
definitions for those contracts (or equivalent wrappers) when available.

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

Only local loopback networking is used. IPv6 is exercised when supported. DNS
tests use numeric literals and localhost; no external origin is required.

## Fixed-response HTTP demo

The executable now serves `Hello, World!\r\n` as a fixed HTTP/1.1 response and
closes after one header block. Request parsing, HEAD semantics, body framing,
and keep-alive remain future milestones. `--buffer-size` now limits the incoming
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
