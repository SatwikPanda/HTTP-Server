# Parallel foundation contracts

P0 implements these contracts on the milestone 09 base. The declarations linked
below are authoritative. A–J extend their owned modules without changing these
shared types. Integration and full application policy remain K/L; the fixed
resource examples do not complete A or B.

## Identity, delivery and ownership

[`core/id.hpp`](../include/core/id.hpp) defines `ConnectionId {value,generation}`,
`TransactionId {connection,sequence}`, and
`OperationId {transaction,endpoint,sequence}`. Transactions start at 1. Endpoint
roles are `downstream`, `upstream`, `file`, and `resolver`.

[`concurrency/session.hpp`](../include/concurrency/session.hpp) retains
`ConnectionSession::initial_action()` and `on_event(const Event&) -> Action`.
Read/write/wait actions and their events carry operation IDs. `HttpSession`
advances the downstream operation sequence after accepting a completion. Every
new work request gets its own nonzero job sequence. IDs never wrap into reuse;
exhaustion closes/rejects work. A stale connection generation, transaction, or
operation is ignored without applying its bytes or error to the active request.

There is **one outstanding action per connection**, including jobs. No concurrent
read/write/read-ahead jobs on the same connection or file token are needed by
this contract. Client and upstream actions alternate under backpressure. F/G may
run many connections concurrently, but must not submit the same outstanding
action twice when a stale event returns it again. A matching completion makes
the session runnable on its owner thread. A would-block result must transition
to a readiness wait, not an immediate retry loop. Close releases registrations
and resources; an upstream-close work request closes only upstream.

For compatibility, a downstream event with transaction sequence zero is an
untagged synchronous event. Existing test adapters use this path. **New async
drivers must always echo the complete action ID.** Stop events apply to the whole
connection and do not require an ID. A timer from a prior phase cannot close a
newly progressing request; the session also checks its current monotonic deadline.

`ReadDataEvent::data` lives only for `on_event`. `WriteAction::data` borrows
session-owned storage until the next matching event or session destruction. Do
not retain it across either boundary. A short write advances only the acknowledged
bytes. Zero progress is an error. `WorkEvent` owns its payload through shared
immutable bytes/file tokens, so it can cross a bounded completion queue safely.
The driver owns sockets, their readiness registrations and all socket operations.
Only `Poller::wake()` is concurrent. Validate registration generations, unwatch
before close, and keep `NetworkRuntime` alive until all drivers have joined.

## HTTP application boundary

[`http/application.hpp`](../include/http/application.hpp) defines:

```cpp
virtual core::Result<std::unique_ptr<Transaction>>
TransactionFactory::create(const Request&, const TransactionContext&) const = 0;
virtual TransactionStep Transaction::on_event(const TransactionEvent&) = 0;
```

Set `HttpSessionConfig::application` to a shared immutable factory. It may create
independent transactions concurrently and must synchronize any mutable shared
service state. `TransactionContext` supplies the ID, response window and borrowed
steady/wall clocks. The immutable request and clocks outlive its transaction.
Transactions must not open sockets, access files, resolve DNS or block in these
callbacks. They return one of the following steps:

| Step | Fields and meaning |
| --- | --- |
| `ResponseHead` | `Response response`, `BodyFraming framing`, optional `uint64_t length`; metadata only, body/trailers empty |
| `ResponseBody` | `SharedBytes bytes`; nonempty, at most the latest `OutputDrained::body_capacity` |
| `ResponseFinished` | Validated `HeaderMap trailers`; successful message completion |
| `BufferedResponse` | A complete bounded `Response`, for the existing demo/small generated errors |
| `TransactionWork` | `WorkRequest request`, absolute steady `deadline` |
| `TransactionFailure` | `core::Error error`, HTTP error `StatusCode status` |

The first event is `StartTransaction`. After each head/body segment is completely
written, the session sends `OutputDrained {body_capacity}`. Work completions arrive
as `WorkEvent`. A buffered response completes automatically. HEAD/204/304 complete
after their head drains; no body jobs are requested, and transaction destruction
releases any prepared resource. Return `ResponseFinished` after the last body;
never infer success from EOF of a truncated fixed-length message.

The session owns request parsing/restrictions, ordered pipelining, persistence,
request counts, Date/Connection generation and the common writer. Later requests
remain in bounded connection input. Applications can demand close but cannot
override client/max-request/error close decisions. For HTTP/1.0 clients, a streamed
chunked response becomes close-delimited HTTP/1.0 output. Keep-alive is disabled
for that response.

**Commit point:** the first positive downstream write acknowledgment. Before
commit, a transaction failure can replace the uncommitted response with one bounded
error response and close. After commit, failure aborts the writer and closes;
it never emits a second head or a successful terminating chunk. A partial response
does not increment completed responses. Stop cancels jobs immediately when waiting;
an active write may drain its response under the supplied bounded drain deadline.
Job deadlines are capped by session progress/drain deadlines.

The no-factory compatibility path creates a real demo transaction. The executable
enables keep-alive. [`http/reference_application.hpp`](../include/http/reference_application.hpp)
also provides `make_file_reference_factory(path)` and
`make_origin_reference_factory(HostPort, target="/")`. They exercise actual file
streaming and origin response streaming through this boundary. Their resource is
fixed by the caller; they provide no client URL routing, static path policy, cache,
or general forwarding semantics. The origin example copies only representation
type/encoding into its response; B implements general hop-by-hop header policy.

## Streaming parser and writer

[`ResponseParser`](../include/http/response_parser.hpp) exposes
`consume_stream(span<const byte>, size_t body_capacity) -> ResponseEvent`.
Each call returns `consumed`, one `kind`, and an optional borrowed `body` span.
Kinds are `need_more`, `paused`, `informational_head`, `final_head`, `body`,
`trailers`, `complete`, and `error`.

Advance input by exactly `consumed`. Retain every remaining byte. Each call emits
at most one event; calling again acknowledges that event. Head/trailer metadata
is read from `response()` or the latest `informational_responses()` entry before
the next consume/reset. A body span points into the supplied input, not parser
storage: handle/copy it before releasing that input. Copies require a bounded
owned segment; no implicit unlimited collecting callback exists.

Zero body capacity returns `paused` without consuming payload. Bounded framing
metadata may still be consumed and delivered. Empty input can deliver a pending
completion/trailer event, otherwise returns `need_more`; it is not EOF. Use
`finish_on_eof()` for transport EOF, then consume empty input to observe completion.
Only close-delimited framing accepts EOF as completion. Errors stay terminal until
`reset()`. Completion leaves subsequent response bytes unconsumed.

The streaming path never reserves Content-Length or stores decoded body bytes in
`Response::body`. `body_bytes()` counts the total transfer-decoded payload.
`RequestLimits::max_body_bytes` bounds that total, independently of the receiving
window and cache object limit. The default is 10 MiB; pass a larger explicit total
policy for larger permitted responses. Header bytes are cumulative across interim
and final heads; extension/trailer/count limits remain enforced. `consume()` is a
bounded collector over the same framing machine. Mixing collector/streaming calls
between resets is an error. All milestone 08 buffered tests remain in the gate.

[`ResponseWriter`](../include/http/response_writer.hpp) exposes `begin`, `buffered`,
`body`, `finish`, `pending`, `acknowledge`, and `abort`. A writer owns one wire
segment, bounded by its window including framing overhead. Drain `pending()` before
adding another segment. `body_capacity()` subtracts worst-case chunk overhead.
Fixed length counts representation bytes independently of partial wire writes;
`finish` rejects underflow/overflow. Metadata-only HEAD may advertise a large known
length. No-body status rules override body emission. Only successful `finish`
generates a final chunk; `abort` discards pending bytes and cannot later succeed.
The direct writer requires the caller to close a close-delimited transport.

## Socket and helper work ports

[`concurrency/work.hpp`](../include/concurrency/work.hpp) defines `WorkAction`
with full ID, absolute deadline, typed request and shared `Cancellation`.
`WorkExecutor::submit(WorkAction, Completion) -> Result<void>` is the admission
port. Successful admission promises exactly one owning callback, possibly inline;
failed admission promises none. Map an admission failure to a matching rejected
event on the session owner. Async implementations deliver callbacks to that owner,
never directly to sessions on helper threads. Each action's cancellation flag can
be set concurrently; a retired transaction's results must still be discarded by ID.

| Request | Successful result / other normal outcomes |
| --- | --- |
| `ResolveWork {target}` | Owning ordered `candidates` |
| `ConnectWork {candidates}` | Connected upstream; one shared deadline across fresh candidate sockets |
| `UpstreamReadWork {max_bytes}` | Owned `bytes`, positive `transferred`; would-block or EOF |
| `UpstreamWriteWork {bytes,offset,count}` | Positive partial `transferred`; would-block |
| `UpstreamWaitWork {interest}` | Readiness completed; reattempt I/O to discover EOF/error |
| `UpstreamCloseWork {}` | Upstream closed, downstream unaffected |
| `FileInfoWork {path}` | `FileInfo`: size, modified wall time, regular/directory/link-component flags, canonical path |
| `FileOpenWork {path}` | Opaque shared `file` token and metadata; only regular files without link components |
| `FileReadWork {file,offset,max_bytes}` | Owned bounded bytes; EOF distinct from a short positive read |
| `FileCloseWork {file}` | Idempotent close; subsequent reads through any alias fail |

`WorkStatus` distinguishes `complete`, `progress`, `would_block`, `eof`,
`cancelled`, `timed_out`, `rejected`, and `failed`. Check status before reading
payload. File metadata/type failures additionally classify missing/forbidden/not-regular/I/O
for portable application decisions. Metadata checks links before canonicalization;
Windows checks all reparse points. A detected link returns the flag without
following it. A supplies containment/decoding policy and a canonical root; generic
file jobs are not a document-root security boundary. The tree is assumed controlled:
inspection and open are not atomic against concurrent path replacement. Opened
stream length is authoritative; timestamps are path metadata under this assumption.

`ReferenceWorkExecutor` is single-owner and has one upstream for one connection.
Older/replayed job IDs are cancelled without closing a replacement socket. A newer
transaction retires the previous upstream. Connect waits validate registration
tokens and call `finish_connect`; every failed candidate gets a fresh socket.
The adapter performs nonblocking upstream read/write once per action. Waits check
stop/cancellation at most every 25 ms. Closing the driver destroys its executor.

DNS and file calls are synchronous in this reference and cannot be interrupted
inside the OS/library call. Deadlines/cancellation are checked before and after;
late payloads are released. E/G must use bounded async helper execution for file/DNS
work; G executes socket work with its own poller. Never run this synchronous
adapter on G's event-loop thread. `WorkLimits` defaults: 64 KiB payload, 32
candidates, 8192 path units; validated ceilings are 16 MiB, 256, and 32768.

File tokens share an opaque stream. A read pins it for the operation; a mutex
serializes access/close and close never destroys an active stream. Schedule one
operation at a time per token; the mutex is a safety guard, not permission to
reorder reads. On cancellation, an in-flight helper owns its token until completion.
Last token release closes the stream. Reference completion payloads and copied
writer segments are each bounded, but are separate allocations.

## Shared values and accounting

[`target.hpp`](../include/http/target.hpp) provides `parse_authority(text)` and
`parse_target(text, origin_authority={})`, both returning `core::Result`.
HTTP absolute-form and origin-form are supported. Scheme/host ASCII case and port
80 normalize; IPv6 brackets are reconstructed by `Authority::wire()`. IPv6 literal
compression, trailing DNS dots, escaped octet case and query order are not folded.
Path/query bytes are exact, with `/` supplied for an empty absolute path. Reject
userinfo, fragments, malformed escapes, invalid/zero ports, non-HTTP schemes and
raw whitespace/control/non-ASCII bytes. Hostnames accept ASCII letters/digits,
hyphens, dots and underscores. Filesystem percent decoding belongs to A.

[`date.hpp`](../include/http/date.hpp) adds `parse_http_date(value, now)` for
IMF-fixdate, RFC 850 and asctime. It validates weekday/calendar/time and is locale
independent. RFC 850 selects the most recent matching two-digit year no more than
50 years after injected `now`. Invalid/unrepresentable dates return an error;
leap-second `:60` is rejected. Formatting retains the existing contract.
[`validators.hpp`](../include/http/validators.hpp) parses wildcard or bounded tag
lists preserving quotes and opaque commas/backslashes; `weak_match` compares
quoted values while ignoring weakness. Empty members, malformed quotes and mixed
wildcard/tag lists are rejected conservatively.

[`cache/contracts.hpp`](../include/cache/contracts.hpp) freezes `Key`/`KeyHash`,
`EntryVersion`, immutable `Snapshot`, `Timing`, `Storage`, and `Policy` ports.
An entry contains end-to-end metadata and immutable transfer-decoded body bytes;
content encoding stays opaque. C assigns globally non-reused observation versions,
including misses/tombstones, and rejects overflow. `replace`/`erase` require the
observed version; a stale miss or late 304 cannot overwrite a newer generation.
`StorageOutcome` distinguishes hit/miss/stored/oversized/exhausted/conflict/failure.
P0 does not implement LRU or freshness policy; the old cache headers remain unused
legacy scaffolding and must not be used by new tracks.

`Timing::request_sent` is captured when the outbound request has fully drained;
`final_head_received` and `final_head_wall` are captured at parser final-head
delivery. Insertion/body completion does not reset these times. `PolicyDecision`
separates hit/fetch/revalidate/bypass/cache-only failure and lookup/store/network
permissions. `Policy::revalidate` returns merged metadata, timing and the exact
snapshot version; K executes the storage compare-and-replace.

[`core/budget.hpp`](../include/core/budget.hpp) implements `MemoryBudget::reserve`
and move-only `Reservation`. Reserve before allocation/growth. Failure returns
would-block with accounting unchanged. Resize uses checked subtraction/addition;
move transfers the charge, destruction releases it, and reclassification changes
category without releasing capacity. The budget state outlives its public handle
while reservations exist. Budget operations are synchronized; a mutable reservation
has one owner. Published body charges never resize.

The enforced total counts each live allocation once. Categories candidate,
resident, pinned, temporary and metadata are disjoint accounting buckets. Resident
entries retained by readers stay resident until evicted; then storage reclassifies
the unique allocation as pinned. Two readers of one body do not double-charge it.
`Body::create` rejects a reservation smaller than vector capacity. New metadata,
container-node estimates, keys, header/tag string capacities and temporary copies
must receive separate reservations before allocation. C also bounds entry and
reader counts. This is logical allocation accounting, not an exact process RSS
measurement. Publish only const snapshots and relinquish mutable aliases. Storage
alone reclassifies shared reservations. Failure/rollback and last-reader release
are exercised by P0 tests.
The C/D follow-up orders `Body` members so payload destruction precedes releasing
its reservation; public signatures are unchanged. C's concrete reader admission,
miss epochs, logical costs and lock order are documented in
[the storage handoff](branches/cache-storage.md). D's bounded directive and
validation decisions are documented in [the policy handoff](branches/cache-policy.md).

## Configuration, metrics and lifecycle

[`core/config.hpp`](../include/core/config.hpp) supplies `RuntimeConfig` and
`validate_config`; it does not start services or parse CLI arguments. H implements
CLI over these fields; L maps buffer/deadline/request fields into session config
and composes services. Default bind is loopback. Cache is disabled by default and
only enabled for proxy mode. Irrelevant mode-specific settings retain defaults;
H rejects explicitly supplied conflicting options. Fixed spellings for H:

| Fields | CLI spelling |
| --- | --- |
| application, driver | `--mode demo|server|proxy`, `--concurrency single|thread-pool|event-loop` |
| bind_address, port, document_root | `--bind`, `--port`, `--root` |
| workers, helpers, sessions, upstreams | `--workers`, `--helpers`, `--max-sessions`, `--max-upstreams` |
| accept_queue, job_queue, completion_queue | `--accept-queue`, `--job-queue`, `--completion-queue` |
| input, output, low_water, high_water | `--input-bytes`, `--output-bytes`, `--low-water-bytes`, `--high-water-bytes` |
| max_requests | `--max-requests-per-connection` |
| idle/progress/connect/work/drain_timeout | `--idle-timeout-ms`, `--progress-timeout-ms`, `--connect-timeout-ms`, `--work-timeout-ms`, `--drain-timeout-ms` |
| cache enabled, entries, object/resident/total bytes | `--cache on|off`, `--cache-entries`, `--cache-object-bytes`, `--cache-resident-bytes`, `--cache-total-bytes` |

These future spellings do not change the existing executable CLI in P0. Preserve
its current `--header-limit`/connection options through H's migration tests.
Validate useful windows, low < high <= output, bounded queues/counts/durations,
and object <= resident <= total. Root validation runs through file/startup work.

[`core/metrics.hpp`](../include/core/metrics.hpp) defines bounded fixed `Metric`
dimensions, `MetricsSink::record(Metric,uint64_t) noexcept`, and `NullMetrics`.
`HttpSessionConfig::metrics` borrows a non-null sink that outlives sessions.
Requests start at parsed/rejected request dispatch; completed responses count only
after the whole response drains. Wire bytes count acknowledged writes; body bytes
count representation bytes of completed responses, excluding HEAD. Request errors
count transaction/transport failures; work rejection/cancellation count accepted
matching completions only. H implements aggregation and additional bounded hooks;
no unbounded URL/header labels or synchronous logging in callbacks.

[`concurrency/lifecycle.hpp`](../include/concurrency/lifecycle.hpp) defines
`SessionFactory(ConnectionId) -> Result<unique_ptr<ConnectionSession>>` and the
`DriverLifecycle` start/request_stop/join port. Stop is a normal-thread idempotent
request; drivers wake owners, stop admission, bound drain and join before service
destruction. H supplies platform stop notification; F/G implement lifecycle; L
owns startup/shutdown order and factory dependencies. The existing blocking
`SingleSessionDriver::drive`/`drive_channel` entry points remain usable.

## Build and acceptance seams

Root targets remain `core`, `net`, `concurrency`, `http`, `httpserver`. Dependencies
flow http → concurrency → net → core; drivers must not link back to HTTP. Optional
module paths and their dependency order are fixed in
[`cmake/ParallelModules.cmake`](../cmake/ParallelModules.cmake). Create a local
`CMakeLists.txt` only when its module has implementation. Root discovery adds it
automatically. B must not also add `src/proxy/cached`; root reserves that for K.
Production modules must not add their test subdirectories a second time.

[`add_project_test`](../cmake/ProjectTests.cmake) accepts NAME (positional),
`SOURCES`, `LINKS`, `LABELS` and optional `TIMEOUT`. It preserves Release CHECKs
and strict skip-code behavior. Root configuration checks the explicit 20-test
inventory. Use `--no-tests=error` with selected-label runs. Optional tests add
their labels without replacing the base inventory; I/M extend the required list
when their acceptance targets are implemented.

[`tests/fixtures/parallel_io.hpp`](../tests/fixtures/parallel_io.hpp) provides an
owning temporary directory and a raw controlled origin with segment boundaries,
write window, premature EOF/malformed bytes, request count and a deterministic
release gate. `NetworkRuntime` must outlive it; join before reading captured
requests. Destructor stop/join and waits are bounded. I owns future fixture
extensions. P0's two runtime suites are `parallel_foundation_test` (parser/writer,
utilities, budget) and `parallel_work_test` (real work, sessions, sockets, errors,
stale events). Neither is a contract-only compile target.

See [the implementation handoff](branches/parallel-foundation.md) for actual
commands, results, limitations and base revision handling.
