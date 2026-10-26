# P0 — parallel-foundation

**Status:** implemented; Windows GCC/Clang/MSVC strict 01–09/P0 gate verified. **Branch:** `parallel-foundation`.
**Start:** verified milestone 09. **Deliver:** `PARALLEL_BASE` for A–J.
Read the [shared workflow](README.md) and the P0 section of the
[parallel roadmap](../implementation_roadmap2.md).

Exact APIs: [parallel contracts](../docs/parallel_contracts.md).
Verification and uncommitted base identity: [handoff](../docs/branches/parallel-foundation.md).
The steps below describe the delivered implementation; A–N remain planned.

## Outcome and ownership

Make the integration points executable before independent branches depend on
them. Own the initial shared HTTP/session changes, common contract types and
utilities, reference I/O execution, root CMake extension points and test helpers.
Publish exact signatures and ownership rules in `docs/parallel_contracts.md`.
Avoid speculative frameworks: one working extension point per needed capability
is enough. Later modules implement these contracts, not alternative protocols.

Inspect `include/concurrency/session.hpp`, `single_session_driver.hpp`,
`include/http/http_session.hpp`, `response_parser.hpp`, `serializer.hpp`,
`framing.hpp`, `date.hpp`, `include/core/clock.hpp`, `id.hpp`, and their sources.
Read existing HTTP/session/response tests before modifying compatibility behavior.

At the planning snapshot, Action/Event address one stream; they cannot identify
upstream sockets or file/DNS jobs. ResponseParser accumulates decoded bytes in
`Response::body`. Date formatting exists; shared parsing/validator utilities
still need implementation. Cache headers are unimplemented scaffolding. Recheck
these facts after milestone 09 before deciding which changes remain necessary.

## Implementation order

### 1. Freeze the baseline and contract vocabulary

Run the strict 01–09 gate first. List current public types and tested behavior;
extend them with compatible defaults/adapters where possible. Keep the demo and
echo usable through the same single-session entry points.

Specify the following in the contract document before branch split:

| Boundary | Decisions that must be explicit |
| --- | --- |
| Identity | Connection generation, per-connection transaction sequence, endpoint role and job/attempt ID; how stale events are rejected |
| Action delivery | Whether several interests may remain outstanding; how completion makes a session runnable; how read/write/job actions are bounded |
| Ownership | Who owns socket, file token, output segment and completion payload; exactly when borrowed spans expire |
| Completion | Distinguish partial progress, would-block, EOF, cancellation, deadline, operation failure and message completion |
| Close | Closing only upstream versus closing the client/session; how terminal actions release pending work |
| Limits | Total message bytes versus buffered bytes versus cache object bytes; high/low-water marks and zero-capacity behavior |

Use separate identities for a connection and its requests: a late result from
request 1 must not be applied to request 2 on the same keep-alive connection.

### 2. Introduce the shared application boundary

Have the HTTP connection session retain request parsing, body restrictions,
persistence, pipeline ordering and output framing. Inject a transaction factory
for demo/static/proxy behavior. The transaction consumes parsed request metadata
and produces metadata/body work through typed actions, not direct socket calls.

Process one request at a time; later bytes stay in the bounded connection input.
Define the point where a response is committed to downstream output and how
failure before/after it changes error handling. Keep this fact shared by B/K and
both drivers. Ensure HEAD/204/304 rules are applied by the common writer.

### 3. Make response parsing genuinely streamable

Extend the production parser to expose informational head, final head, decoded
body fragment, trailer and completion/error events. Specify how much input was
consumed when the receiver has no capacity. Body counters track total bytes
independently of an accumulated string; never reserve Content-Length bytes in
streaming mode. Continue strict framing validation from milestone 08.

A useful flow is consume → deliver bounded event → acknowledge consumption →
resume. Borrowed body bytes may live only until the call returns unless copied
into a budgeted owned segment. Backpressure must pause even when the parser has
already received several chunks in one input buffer.

Retain the old bounded whole-message convenience API as a collector over shared
parsing logic, or another compatible path that shares framing/state machinery.
Do not let old buffered limits accidentally cap a permitted streamed file at the
window size. Keep total-size policy limits explicit and overflow-safe.

The writer serializes a validated head, bounded chunks/segments and successful
completion. Emit a final chunk only on actual success. Never rewrite a failed
partial response into a second response on the same connection.

### 4. Execute socket and work actions in the reference driver

Add client/upstream routing, resolve results, candidate connect attempts and
opaque file work. Use existing Socket/Poller operations. A candidate failure
requires unwatch/close and a fresh socket, with one shared connect deadline.
Writable readiness requires `finish_connect()` before declaring success.

The reference file/DNS adapter may execute synchronously initially. Document
that its blocking lookup cannot enforce a client-visible DNS deadline while
inside the OS call; E supplies asynchronous execution later. Its interface must
already support an eventual result, cancellation and admission failure, without
forcing G to use synchronous work.

Freeze file tokens and per-handle operation ordering. Closing a session marks
outstanding work stale; an in-flight read keeps its file resource alive until
the operation finishes. Cleanup must not concurrently destroy an active stream.

### 5. Implement shared utilities and value types

Provide one authority/absolute-target parser for A/B/C. Normalize only established
equivalences such as scheme/host case and default port; retain exact escaped
path/query, including query order. Filesystem percent-decoding belongs to A.

Implement HTTP-date parsing with validated calendar arithmetic and the documented
supported HTTP-date forms; keep it locale independent and thread safe. Define
invalid-date results and injected time for any relative-year interpretation.
Add validator list parsing/weak comparison used by A/D, preserving quoted tags.

Define cache key, immutable entry, timestamps, entry version and policy result
types independently of LRU implementation. Specify checked reservation transfer
for candidates, resident and pinned allocations, plus the accounting for metadata
and temporary copies. Implement a small real RAII reservation primitive and test
its failure/rollback behavior; C implements the storage policy around it.

Freeze typed configuration, no-op metrics hooks and lifecycle/factory contracts.
H owns CLI/metric implementations. L owns the final dependency composition.

### 6. Establish build and test seams

Preserve current target names. Since `http` links to `concurrency`, new drivers
consume session interfaces/factories without linking back to application code.
Introduce explicit optional module-local CMake includes, in dependency order,
and a test helper accepting arbitrary source paths/links/labels/timeouts.

Seed a usable controlled origin and temporary-file fixture for P0 and B; I later
owns extensions. Include real examples of multiple endpoint actions and file
results, not only compiling interfaces. Check that contract/handoff/benchmark
documentation is trackable and add ignore exceptions if needed before branches
split.

## Nuances and verification

- Buffer owners must survive a short write; an action containing a span cannot
  outlive the backing storage. Cancellation also releases that owner once safe.
- A paused parser must preserve all unconsumed bytes and remain bounded; an event
  callback cannot accept unlimited data by silently copying it elsewhere.
- A readiness/error event for an old registration must not close a replacement
  socket. Check both poller generation and transaction identity.
- Informational heads and trailers count toward explicit limits even when their
  application payload is discarded.
- A zero-progress action is not permission to spin. Every nonterminal state must
  identify pending I/O, timer, completion or an explicit runnable continuation.

Run labels `parallel_foundation` and the strict 01–09 gate. Cover parser splits,
all framing/no-body modes, payloads much larger than the window, pause/resume,
partial writes, EOF/error, real client/upstream loopback, real file work, stale
events and reservation rollback. Preserve buffered parser regression tests.

## Done and handoff

- [x] Production reference paths execute every new action/result category.
- [x] Exact signatures, supported states, lifetimes, limits and test fixtures are
  documented with no unresolved interface decisions.
- [x] Demo/echo and strict 01–09/P0 runtime checks pass; platform evidence is named.
- [x] Record base identity and `docs/branches/parallel-foundation.md`; the
  `PARALLEL_BASE` commit SHA is assigned when the user commits these changes.
- [x] A–J can build against these contracts without editing each other's shared types.
