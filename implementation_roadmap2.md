# Parallel Implementation Roadmap After Milestone 09

This plan keeps milestones 01–09 as the shared foundation and organizes the
remaining work into feature branches that can be developed concurrently. Each
branch delivers its production code, build integration, tests, and documentation.
An explicit assembly branch delivers the final executable wiring. The intended
handoff is a set of tested branches to merge in dependency order, with no missing
implementation left for the person doing the merges.

The milestone numbers **10–32 retain their meanings** from
[IMPLEMENTATION_ROADMAP.md](IMPLEMENTATION_ROADMAP.md). This document replaces
their strictly sequential execution order with the dependencies below; it does
not remove their requirements or acceptance checks. **P0** is an additional
shared integration step between milestone 09 and parallel feature development.
P0, C and D are implemented and locally verified on Windows; other tracks remain
planned. C/D are delivered together on `cache-storage-policy`; see the
[storage](docs/branches/cache-storage.md) and [policy](docs/branches/cache-policy.md)
handoffs for evidence and remaining integration/platform acceptance. See the
[P0 handoff](docs/branches/parallel-foundation.md) for current evidence.

Detailed implementation guides for P0 and every A–N track are in
[roadmap/README.md](roadmap/README.md). Use this document for dependency order and
the individual guides for implementation steps, edge cases and acceptance work.

## 1. Preserve the base and define the finish line

### Current evidence and entry gate

Milestone 09 is implemented. Its base commit is
`d644e68de34f36e9d4628060de8efbe2ab4026cc`; the original strict 01–09 gate passed
18/18 on Windows before P0. P0 and additional milestone 09 corrections now pass
all 20 strict 01–09/P0 tests on Windows GCC 16.2, Clang 22.1.8 and MSVC 19.29.
Linux/macOS execution of these changes and remote CI verification remain pending.
The [handoff](docs/branches/parallel-foundation.md) records exact commands and
results. C/D use `4547b48687d70b4d2d54370f01b6dfa946b80181` as `PARALLEL_BASE`,
including committed P0 and its subsequent macOS fixture corrections.

- [x] Preserve unconsumed bytes and ordered responses across sequential/pipelined requests.
- [x] Respect persistence/close semantics and close on framing or unsafe output failures.
- [x] Enforce bounds, phase deadlines and maximum requests per connection.
- [x] Run the strict base/P0 gates and record actual platform results and base identity.

`BASE_09` and `PARALLEL_BASE` below are names for recorded commit SHAs, not
existing Git branches or tags. Recheck the code when starting P0 because
milestone 09 may already have introduced some of the required seams.

Preserve the public behavior, tests, portable socket contracts, error categories,
and ownership rules already built. Extend maintained interfaces compatibly
where practical; do not replace the HTTP implementation or discard the existing
minimal driver. The fixed-response application remains a regression fixture.

### Finished project

The final application must provide:

- A static file server with safe paths, GET/HEAD, bounded binary streaming, MIME
  types, validators, conditional responses, and persistent connections.
- A forward HTTP proxy with correct targets/headers, all planned response
  framing, bounded streaming, deadlines, and failure handling.
- A shared in-memory cache with conservative eligibility, explicit freshness,
  Age, cache-only semantics, revalidation, immutable entries, LRU eviction, and
  resident/in-flight memory limits.
- Selectable thread-pool and event-loop drivers running the same application
  decisions, including overload recovery and documented shutdown guarantees.
- Recorded Windows, Linux, and macOS correctness results and reproducible
  benchmark data comparing the drivers.

Retain C++20, CMake, the standard library and OS APIs as the runtime dependencies.
Keep the cleartext GET/HEAD scope. TLS, CONNECT, upgrades, HTTP/2/3, dynamic
handlers, generated compression, persistent caches, Vary variants, authenticated
cache rules, range assembly, cache-side client conditional evaluation, upstream
pooling, miss coalescing, native epoll/kqueue/IOCP drivers, and stronger containment
against concurrent filesystem replacement remain deferred.

## 2. Dependency and branch plan

There are three kinds of work: one shared preparation step, independent feature
branches, and integration/evidence branches. A branch may start against agreed
contracts before all of its acceptance dependencies exist. That does **not** make
the corresponding milestone complete.

| Track / canonical name | Milestones owned | May start after | Required to finish acceptance |
| --- | --- | --- | --- |
| P0: `parallel-foundation` | Shared contracts and reference execution | `BASE_09` | Strict base gate and P0 production checks |
| A: `static` | 10–12 | P0 | Static service through the minimal driver |
| B: `proxy` | 13–16 | P0 | Uncached proxy through the minimal driver; DNS deadline enforcement also needs E |
| C: `cache-storage` | 17; storage/accounting part of 24 | P0 | Real storage, immutable snapshots, concurrent accounting tests |
| D: `cache-policy` | 18–19; pure validation decisions for 21 | P0 | Real policy with controlled clocks and metadata |
| E: `helpers` | 22 | P0 | Real file/DNS execution, bounded admission, late-completion and shutdown tests |
| F: `thread-pool` | 23; driver portions of 27–28 | P0 | E and real A/B/K application suites in L |
| G: `event-loop` | 25–26; driver portions of 27–28 | P0 | E and real A/B/K application suites in L |
| H: `config-observability` | 29; portable stop hooks for 28 | P0 | Real startup, counters, logging and stop behavior in L |
| I: `test-harness` | Shared fixtures/matrix preparation for 30 | P0 | Full correctness results belong to M |
| J: `benchmark-harness` | Workload/tooling preparation for 31 | P0 | Both-mode smoke runs after L; measurements after M |
| K: `cache-proxy` | 20–21; integration part of 24 | B + C + D | Actual proxy/cache transactions, including concurrency in L |
| L: `application-assembly` | 24, 27–28; combined acceptance of 23, 25–26, 29 | Incrementally after P0 | A–I and K integrated; every application/driver combination works |
| M: `correctness-gate` | 30 | L + I | Full recorded Windows/Linux/macOS gate |
| N: `results` | Finish 31, then 32 | L + J for smoke work; M for measurements | Raw repeats, plots, run guide, final acceptance |

Canonical names are stable track slugs. Suggested Git branches use `codex/<name>`;
for example, A uses `codex/static` and K uses `codex/cache-proxy`. P0 is the shared
preparation step; the feature and integration tracks are labeled A–N without gaps.

The letters identify ownership tracks, not extra product features. A–J
can be developed simultaneously from `PARALLEL_BASE`. Their source branches may
merge in any order because their implementation files are disjoint. F/G/H can be
merge-ready modules while their **full milestone acceptance remains pending**
until the real services are assembled. K must include its B/C/D prerequisites.
L can assemble features as they land; it must finish after every dependency.

```text
milestones 01–09 -> P0 parallel-foundation -> PARALLEL_BASE

PARALLEL_BASE
  |
  +-> A static --------------------------------------+
  +-> B proxy -----------+                           |
  +-> C cache-storage --+-> K cache-proxy ------------+-> L application-assembly
  +-> D cache-policy ---+                           |     |
  +-> E helpers -------------------------------------+     +-> M correctness-gate
  +-> F thread-pool ---------------------------------+     |     |
  +-> G event-loop ----------------------------------+     |     v
  +-> H config-observability ------------------------+     +---> N results
  +-> I test-harness --------------------------------+           ^
  +-> J benchmark-harness ---------------------------------------+
```

Branches with a shared ancestor and separate files reduce textual conflicts.
Correct interfaces, dependency order, and tests establish semantic compatibility.
Successful Git merges alone are not the final acceptance gate.

## 3. P0 — `parallel-foundation`: make the common integration points executable

**Implemented and locally verified.** Exact declarations and behavior are frozen
in [docs/parallel_contracts.md](docs/parallel_contracts.md). The working tree must
be committed before recording its SHA as `PARALLEL_BASE` for branch development.

**Owner:** the foundation/integration maintainer. **Merge first.**

Before P0, `concurrency::Action`/`Event` addressed one stream without upstream
or job identities, and `ResponseParser` collected the entire decoded body.
P0 now supplies typed work, identities, bounded streaming and shared value ports.
The old cache headers remain scaffolding; C/D implement storage and policy.

Exact P0 signatures, invariants, target names and examples are published in
`docs/parallel_contracts.md`. Proposed private names for later tracks below remain
design assignments; they must use those shared contracts.

### Shared contracts and their single owners

| Contract | P0 must settle and exercise | Later implementation owner |
| --- | --- | --- |
| Session/application boundary | Inject an application/transaction factory into shared HTTP session logic; retain parsing, ordered requests, persistence and response framing in one place. Keep the demo factory working. | P0 owns the seam; A/B/K supply services; L composes them |
| Actions/events | Identify downstream/upstream endpoint, connection generation, transaction/job ID, completion/error, deadlines, cancellation and close scope. Define multiple pending interests and how a session becomes runnable without spinning. | P0 owns the shared vocabulary; F/G execute it |
| Streaming | Deliver informational/final heads, bounded body fragments, trailers, completion and errors; stop consumption when capacity is exhausted and preserve unconsumed bytes. Define byte ownership across callbacks, partial writes and helper completion. | P0 owns parser/writer extensions; A/B/K consume them |
| File and resolver ports | Typed bounded work requests/results, file-handle lifetime, serialized per-handle reads, cancellation and admission failure. No native handles in public headers. Helpers never own driver sockets. | P0 owns ports/reference adapters; E owns asynchronous scheduling; A owns path policy |
| Common HTTP utilities | One authority/absolute-target parser preserving escaped path/query; HTTP-date parsing alongside existing formatting; validator parsing/matching. Keep filesystem decoding in A. | P0 implements shared utilities; A/B/D reuse them |
| Cache data and decisions | Normalized key, immutable entry/body, validators, paired wall/steady timestamps, byte cost, snapshot/version identity, lookup/store/conditional-replace results and policy decisions. | P0 owns shared types; C owns storage; D owns policy; K coordinates transactions |
| Resource accounting | Units, limits and RAII reservation semantics for resident entries, pinned allocations, candidates and bounded work. Define how unique allocations and references are counted without undercounting or charging the same allocation accidentally. | P0 owns the contract/basic reservation primitive; C/E/F/G own enforcement |
| Configuration, metrics and lifecycle | Shared typed options/defaults, clock injection, request outcomes, nonblocking metric hooks, stop/drain contract and factory inputs. A no-op metric sink preserves current operation. | P0 owns types; H implements CLI/sinks/hooks; L owns process lifetime |

### Required implementation and checks

- [x] Extend existing contracts with defaults/adapters that preserve earlier
  callers. Run the unchanged demo/echo and milestone 01–09 checks. Any necessary
  internal refactor must preserve their externally tested behavior.
- [x] Implement bounded streaming in the production response parser and writer.
  Retain the bounded whole-message convenience API. Test a response larger than
  the buffer window, every framing mode, no-body rules, split boundaries,
  partial consumption, EOF, trailer limits, and producer pause/resume.
- [x] Distinguish a total message size limit from the in-memory window and cache
  object limit; a large permitted representation must not require a large
  allocation. Keep configurable total-size limits enforced during streaming.
- [x] Adapt the minimal driver to execute the shared socket/job actions using
  production networking and file/resolver adapters. Exercise downstream and
  upstream loopback I/O, real temporary-file reads, completion identity,
  cancellation, deadlines and cleanup. A synchronous reference adapter may block
  on DNS/file work; record that limitation until E supplies asynchronous work.
- [x] Test the shared HTTP utilities, cache value contracts and reservation
  primitive. Do not introduce a second parser, driver-specific HTTP decisions,
  or a placeholder cache that pretends to provide acceptance behavior.
- [x] Publish small runnable integration examples using the real session and
  minimal driver. Compile-only contract checks supplement runtime checks.
- [x] Seed a usable controlled loopback-origin fixture with the framing, status,
  body and request-count controls B needs. Freeze its entry points before I
  extends it; B must not depend on an unmerged future fixture implementation.
- [x] Freeze module factories and test fixture entry points. Each feature must
  be testable without editing `src/main.cpp` or waiting for every other feature.

### Build seams that prevent shared-file conflicts

P0 is the only branch that initially changes root `CMakeLists.txt`, common
headers, the base HTTP/session implementation, and common test support. Keep the
current public target names and compatible linkage. Since `http` already links
to `concurrency`, drivers must accept session factories and depend on the shared
session contracts; they must not create a `concurrency -> http -> concurrency`
target cycle. Compose application services above the drivers.

P0 must check that the planned contract, branch-handoff and benchmark documentation
directories are trackable, adding ignore-rule exceptions if needed before the
branches split. Feature authors can then deliver those files without competing
edits to `.gitignore`.

Add explicit, optional module-local CMake registration points for the planned
feature paths. Check that a module's CMake file exists before including it;
require targets/dependencies explicitly inside it. Every feature branch creates
its own implementation, local CMake file and tests together. Do not glob source
files, create empty future targets, or silently skip a dependency for a module
that is present. Use clear configuration errors for missing required targets.

Provide a shared test-registration helper that accepts source paths, target
dependencies, labels and timeouts; the present helper assumes a single file
directly under `tests/`. Keep C++20, warning options, `Threads::Threads`, the
single native networking backend, `ws2_32`, and Release-active assertions.

**Exit:** P0 builds and its production checks plus the strict 01–09 gate pass.
Record its commit as `PARALLEL_BASE`. No shared interface is still marked TBD.
Public type changes after this point go through one foundation change, tested
and merged into every affected branch before dependent work continues.

## 4. Parallel feature deliverables

All original milestone acceptance checks remain required. The following sections
assign their implementation and highlight integration obligations. Each track
ships its own build definitions, tests, and a handoff file under
`docs/branches/<name>.md` containing dependencies and checks actually run. Use the
canonical name from the table; for example, K uses `docs/branches/cache-proxy.md`.

### A — `static`: static service, milestones 10–12

**Own:** `include/server/`, `src/server/`, `tests/server/`, `www/` and its module
CMake file. Use P0 file ports, date/validator utilities and session factory.

- [ ] **10:** Support origin-form and the server's absolute-form targets; split
  query before lookup and decode escapes once. Reject malformed escapes, NUL,
  backslashes, escaping traversal, Windows drive/UNC/device/ADS forms. Compare
  canonical path components, reject symlinks/reparse points, use a controlled
  document tree, map `/` to `index.html`, and serve regular files only without
  directory listings. Distinguish invalid, forbidden and missing targets.
- [ ] **11:** Prepare metadata/open/read/close through the shared file port;
  stream bounded binary chunks on demand. Supply Date, MIME with binary fallback,
  known-length framing and HEAD metadata. Implement coherent 403/404/405/500 and
  Allow responses. A file failure after output starts closes the response.
- [ ] **12:** Generate Last-Modified and a weak size/mtime ETag. Evaluate
  If-None-Match before If-Modified-Since, including wildcard/weak matching, and
  produce correct bodyless 304 metadata for GET/HEAD.

**Acceptance:** traversal/encoding/platform path matrix; empty/text/binary/large
files byte-for-byte; HEAD and conditional precedence; slow readers with bounded
buffers; changed/missing/inaccessible files and midstream failure. Run real file
responses over persistent minimal-driver connections. Repeat unchanged tests
with E/F/G in L. Stronger protection against concurrent tree mutation is deferred.

### B — `proxy`: uncached proxy, milestones 13–16

**Own:** `include/proxy/` and `src/proxy/` forwarding/transaction files,
`tests/proxy/`, module CMake and an uncached service factory. Reserve separate
cache-integration files for K. Keep cache-disabled forwarding independently usable.

- [ ] **13:** Require absolute `http` targets; use normalized authority/effective
  port, bracketed IPv6, and exact escaped path/query. Reject unsupported schemes,
  userinfo, fragments and malformed authorities. Generate origin-form and Host.
  Strip hop-specific and Connection-nominated fields in both directions, consume
  proxy credentials locally, append Via, detect loops, and reject CONNECT/Upgrade.
- [ ] **14:** Maintain distinct client/upstream state and use one fresh origin
  connection per transaction while retaining client keep-alive. Handle partial
  origin writes, fragmented heads, GET/HEAD and fixed-length response streaming.
- [ ] **15:** Relay chunked and close-delimited bodies with independent downstream
  framing; handle interim and HEAD/204/304 boundaries; reject switching protocols.
  Bound/validate trailers, discard optional unsupported ones safely, retain opaque
  content encodings, and complete close-delimited input only on EOF.
- [ ] **16:** Pause upstream input at output high-water and resume below low-water.
  Enforce connection, pending-connect, job and buffer budgets. Candidate connects
  share a deadline and use fresh sockets with pending-error checks. Before output,
  map resolution/connect/invalid-response failures to 502 and timeouts to 504;
  afterward close without a second response or successful final chunk. Cancel
  upstream work on client loss. Client-visible DNS deadlines require E.

**Acceptance:** controlled loopback origin covers exact headers/bodies and all
framing/status forms, origin counts, persistent clients, slow peers, pause/resume,
malformed/truncated input, timeouts and disconnects. Use the P0-seeded common
origin fixture maintained by I. Later fixture additions go through I's owner and
become explicit commit prerequisites if B's tests need them. Expose P0's agreed transaction
boundary so K can wrap forwarding without copying its state machine.

### C — `cache-storage`: bounded cache storage, milestone 17 and part of 24

**Own:** `include/cache/storage/`, `src/cache/storage/`, `tests/cache/storage/`
and a storage-specific CMake file. P0 decides how the old unused cache headers
are retired or adapted before C and D split; they are not a production baseline.

- [x] **17:** Implement map/list LRU with immutable shared snapshots. Use the
  agreed key including exact escaped path/query; store end-to-end metadata and
  transfer-decoded, content-encoded body bytes. Enforce entry-count, resident-byte
  and per-object limits with documented metadata costs.
- [x] Keep mutation locks short; never hold them during serialization or I/O.
  Return snapshots that remain valid through replacement/eviction.
- [x] Implement reservations for candidates and retained/pinned allocations;
  eviction cannot pretend to free an allocation still referenced by a response.
  Bound temporary copies and transfer reservations correctly on insertion.
- [x] Provide version-checked replacement/metadata update and explicit failure
  results so a late validation cannot update a different body.

**Acceptance:** deterministic eviction/replacement, exact budget boundaries,
oversized rejection, pinned lifetime, concurrent lookup/evict/replace, allocation
and cancellation failure paths, and no global budget overshoot. These establish
storage behavior; real concurrent proxy/cache acceptance remains in K/L.

### D — `cache-policy`: cache policy, milestones 18–19 and validation decisions for 21

**Own:** `include/cache/policy/`, `src/cache/policy/`, `tests/cache/policy/` and
a policy-specific CMake file. Depend on shared cache/HTTP value types, not C's
storage implementation or either driver.

- [x] **18:** Store only complete eligible 200 responses to unconditional GET.
  Bypass Authorization, Cookie, Range and client conditionals; forward HEAD
  without replacing a cached GET. Reject Set-Cookie, any Vary, no-store/private,
  partial/malformed/interrupted/oversized and initially close-delimited responses.
- [x] Parse Cache-Control case-insensitively with quoted values and conservative
  malformed/conflicting handling. Field-qualified restrictions apply to the whole
  response. Separate lookup, storage and validation decisions; request no-store
  bypasses lookup and insertion. Check only-if-cached before any network path.
- [x] **19:** Prefer s-maxage, then max-age, then Expires. No explicit lifetime
  means immediately stale; retain only useful validation candidates. Apply
  request max-age/min-fresh and request/response no-cache. Require validation when
  stale; never use max-stale to serve stale content. Honor must/proxy-revalidate.
- [x] Use paired wall/steady times: apparent age is max(0, response time minus
  Date); corrected initial age is max(apparent age, received Age plus response
  delay); current age adds monotonic resident time. Clamp arithmetic, handle bad
  dates conservatively, use strict age < lifetime freshness, and generate Age.
- [x] Define validator selection and permitted 304 metadata merging as pure
  decisions, with ETag preferred over Last-Modified and body/version identity
  required. K owns their execution against an origin and store.

**Acceptance:** a directive/eligibility matrix and fake-clock cases cover expiry,
already-aged responses, transit delay, wall-clock jumps, invalid values, missing
lifetime, no-cache and cache-only failure. Test production policy, not a test-only
policy substitute. Storage and networking are unnecessary for these pure tests.

### E — `helpers`: file/DNS helpers, milestone 22

**Own:** `include/concurrency/helpers/`, `src/concurrency/helpers/`,
`tests/concurrency/helpers/` and helper-module CMake.

- [ ] Implement bounded worker/job/completion queues for real file operations and
  name resolution using P0 ports. Bound completion bytes as well as job counts;
  file chunks are requested only when downstream capacity exists.
- [ ] Carry connection generation, transaction and job identity. Wake the owning
  driver, deliver on its thread, and release stale results/file resources safely.
  Serialize operations on a file handle; helpers never operate on driver sockets.
- [ ] Support admission rejection, queued cancellation, timed-out transactions
  and shutdown. A client-visible DNS deadline must fire without waiting for a
  blocked resolver; eventual results are discarded.
- [ ] Keep helper workers distinct from session workers. Document that cancelling
  a transaction cannot necessarily cancel an in-progress OS resolver call; joining
  that thread can outlast the drain deadline. Never detach workers that borrow
  destroyed state or claim a strict process shutdown bound without a solution.

**Acceptance:** real temporary files and localhost/numeric resolution, controlled
delayed I/O, saturation, bounded result queues, wakeups, stale generations,
timeout-before-completion, and repeated stop/start with resources released.

### F — `thread-pool`: thread-pool driver, milestone 23

**Own:** `include/concurrency/thread_pool/`, `src/concurrency/thread_pool/`,
`tests/concurrency/thread_pool/` and its module CMake file.

- [ ] **F / 23:** One acceptor, bounded accepted queue, fixed workers, explicit
  queue rejection (bounded 503 when feasible, otherwise close). Transfer sockets
  before registering them with their owning worker. Each worker owns the client
  and current upstream; nonblocking I/O progresses through readiness waits.
  Enforce keep-alive/request/progress limits and keep runtime lifetime correct.

### G — `event-loop`: event-loop driver, milestones 25–26

**Own:** `include/concurrency/event_loop/`, `src/concurrency/event_loop/`,
`tests/concurrency/event_loop/` and its module CMake file.

- [ ] **G / 25–26:** One loop owns listener/client/upstream registrations and all
  socket operations. Schedule file/DNS through E, check connection errors before
  treating connect as successful, and route completions to the right generation.
  Process readable bytes alongside hangup. Unwatch before close. Watch writes only
  while connecting or output is pending, and derive waits from nearest deadlines.

#### Shared requirements for F and G: milestones 27–28

Both consume the same session and helper ports. Neither owns HTTP, static-file,
proxy or cache decisions. F/G link to P0's helper interfaces, not E's concrete
service. Their development tests inject controlled I/O; L injects E for production
and full acceptance. This keeps independent builds possible without putting
blocking work on the loop.

- [ ] **Both / 27:** Enforce active-client/upstream/connect/job limits and coordinated
  backpressure. G budgets accepts, events, bytes and completions per iteration;
  both avoid busy-spin and bound retries/rejection work. Timers and small requests
  must progress beside large transfers and slow peers.
- [ ] **Both / 28:** Stop accepting, wake waits, drain up to the configured deadline,
  cancel queued work, reject stale events, unwatch/close owned sockets and join
  owned threads. Release file resources, candidates, snapshots and reservations.

**Development checks:** run real echo/demo sessions and production action handling
with controlled clocks/I/O. G must never substitute synchronous reference file/DNS
execution on its loop. **Milestone acceptance:** after E/A/B/K are available, L
runs their unchanged suites on each driver, including overload and stop during
idle, queueing, file/DNS work, connect, streaming and revalidation.

### H — `config-observability`: configuration, metrics and stop hooks, milestone 29

**Own:** `include/core/config.hpp`, `src/core/config.cpp`, `include/observability/`,
`src/observability/`, associated tests and `docs/configuration.md`. P0 must reserve
these implementation paths and place shared option types in its contract header.
Portable stop-hook implementation paths under `src/core/` also belong to H;
native console/signal and process sampling details stay in private adapters.

- [ ] Validate server/proxy, thread-pool/event-loop, bind/port/root, worker/helper
  counts, cache enablement and budgets, queue/connection limits, deadlines and
  keep-alive limits. Define defaults/help and retain loopback binding by default.
- [ ] Implement bounded logging outside the loop's blocking path, metric snapshots
  and CPU/memory adapters. Collect request/error counts, useful bytes, latency,
  connections/queues, cache outcomes/validation, evictions, and memory categories.
- [ ] Provide safe OS stop requests that wake drivers and run normal teardown on
  an owning thread. Keep handlers minimal; L controls process lifetime.
- [ ] Supply parser/configuration and metrics APIs plus startup examples. L wires
  them into `src/main.cpp`; features expose factories instead of editing main.
- [ ] Audit the shared status policy with A/B/L: 200/304; 400/403/404/405/408;
  413/414/417/431; 500/501/502/503/504/505. Emit a status only while a coherent
  response can still be sent.

**Acceptance:** invalid/boundary configuration cases, known-workload counter
totals, bounded logging saturation, and real CLI/stop behavior in L. Document
actual executable paths and the MSVC developer-shell requirement.

### I — `test-harness`: prepare correctness validation, milestone 30

**I owns:** shared fixtures under `tests/fixtures/`, matrix tests under
`tests/system/`, their local CMake and test launch scripts. P0 seeds only the
minimal executable fixtures required for its own checks; I extends them without
changing agreed entry points. Feature authors own their module-specific cases.

- [ ] Provide a controllable loopback origin (framing/status/header/body choices,
  request counters, slow/truncated delivery), temporary document tree, controlled
  clocks/I/O, synchronization barriers and a driver-parameterized runner.
- [ ] Prepare server/proxy × thread-pool/event-loop cases with cache enabled and
  disabled where applicable. Test common assertions against the actual session,
  application services and drivers. Missing implementations stay pending.
- [ ] Cover split positions, EOF, persistent requests, containment, proxy/cache,
  overload, late completions, cancellation and resource lifetime. Avoid timing
  guesses that conceal spins or races; use bounded CTest timeouts.

**Acceptance:** I's system tests run against assembled production modules in L.
Prepared fixtures alone do not establish milestone 30 completion; M owns the
recorded cross-platform correctness gate.

### J — `benchmark-harness`: prepare measurement tooling, milestone 31

**J owns:** `benchmarks/` tooling, workload definitions, scripts and instructions.
The harness may be developed early; final performance results require M.

- [ ] Prepare concurrency 1/10/50/100/500/1000 within host limits, workers
  1/2/4/8/16, and objects 1 KiB/64 KiB/1 MiB/16 MiB. Cover reused/new connections,
  repeated/varied files, slow clients/origins, and proxy cache off/cold/warm/
  revalidation/mixed popularity.
- [ ] Use identical Release configurations and fixtures across drivers. Record
  compiler, OS, CPU/RAM, socket/backend limits, worker/helper counts, budgets,
  deadlines, logging, origin placement and filesystem-cache state.
- [ ] Define warmup, measurement duration, at least five repeats, cache reset/
  prefill, byte/status correctness and load-generator saturation checks. Distinguish
  fixed concurrency from arrival-rate tests and record shared-host contention.
- [ ] Emit machine-readable raw runs with successful/error/timeout counts and
  configuration metadata. External load/analysis tools stay test dependencies.

**Acceptance:** J's smoke benchmark exercises every scenario family on both
drivers after L. Generated sample data does not establish milestone 32 completion;
N owns measured results after M's correctness gate.

## 5. Dependent integration deliverables

### K — `cache-proxy`: proxy/cache integration, milestones 20–21 and 24

**Start from:** P0 + merged B/C/D. **Own:** new
`include/proxy/cache_transaction.hpp`, `src/proxy/cache_transaction.cpp`,
`tests/proxy_cache/` and a separate integration-module CMake file. If the agreed
proxy seam needs changing, resolve it with B before implementation continues.

- [ ] **20:** Evaluate cache-only semantics and policy before DNS. Serve usable
  hits with fresh framing and Age; miss/bypass uses B's forwarding transaction.
  Accumulate bounded candidates alongside streaming and commit only after complete,
  valid, eligible receipt. Abandon candidate retention at object/global limits
  while continuing to forward. Cache-only failures return 504 with zero DNS or
  origin traffic, including paths that otherwise bypass the cache.
- [ ] **21:** Prefer ETag validation, otherwise Last-Modified. On a matching 304,
  merge permitted metadata against the exact validated snapshot/version, retain
  its body and serve cached 200 to an unconditional client GET. Unexpected or
  mismatching 304 triggers a bounded unconditional retry or 502. Never reuse an
  unrelated body or spin through unlimited retries.
- [ ] Commit a replacement 200 only after complete eligible receipt. Invalidate
  old entries on changed eligibility or an authoritative changed response that
  cannot replace them. On upstream failure return an error, never stale content;
  after downstream output starts retain B's close-on-failure behavior.
- [ ] **24:** Coordinate concurrent misses/validation with storage generations and
  reservations. Duplicate misses are allowed and counted; they must not mix body
  and metadata. Release every candidate/pinned reservation on all cancellation,
  timeout, oversized and failed replacement paths.

**Acceptance:** origin-counted fresh hit, miss, expiry, cache-only/bypass matrix,
304/200 validation, weak validators, Last-Modified fallback, metadata/eligibility
changes, invalid 304, interrupted replacement and upstream failures. L adds real
parallel hits/misses/evictions/revalidation with barriers and slow consumers.

### L — `application-assembly`: application assembly, fairness and lifecycle

**Owner of shared integration files:** `src/main.cpp`, final root CMake wiring,
combined application factory/lifetime code, `.github/workflows/cmake.yml`,
`tests/README.md`, root usage documentation, and the evolving validation record.
L is the designated integration branch, not extra wiring assigned to the user.

- [ ] Compose runtime, configuration, metrics, file/resolver services, optional
  shared cache, application factory and selected driver with explicit lifetimes.
  Keep dependency direction acyclic and ensure all modules are actually linked.
- [ ] Expose the four server/proxy × thread-pool/event-loop choices through one
  executable; cache-off proxy remains the benchmark reference. Keep the minimal
  driver available to the existing tests.
- [ ] Run actual static/proxy/cache suites across both drivers. Complete milestones
  23–26 with real providers; do not count contract-only modules as complete.
- [ ] **24/27:** Prove immutable snapshot/version safety and global accounting
  under concurrent hits, misses, evictions, changed origins and revalidation.
  Saturate accept/helper/connect/output capacities; timers and unrelated requests
  keep progressing, and all capacity recovers when load subsides.
- [ ] **28:** Exercise stop at every active phase. Stop intake, drain/cancel, close
  on the owning thread, quiesce helpers, join threads, release shared resources,
  and destroy `NetworkRuntime` last. Document uncancellable DNS limitations;
  bound client-visible work without inventing a hard resolver-join guarantee.
- [ ] Complete H's real CLI/status/metrics acceptance. Update the CI gate and
  usage docs as modules become verified. Reconcile tracked implementation
  contracts with reality; preserve `PROJECT_ARCHITECTURE.md` if present.

**Exit:** combined production application passes all required local feature and
system tests with zero pending substitutions. Each branch's implementation work
is complete; cross-platform evidence and measured results still belong to M/N.

### M — `correctness-gate`: cross-platform correctness, milestone 30

**Ownership handoff:** after L's assembly freeze, M takes ownership of CI and the
validation record. Feature fixes still go through their module owners; M does
not introduce a second owner for actively changing shared files.

- [ ] Run strict acceptance on Windows, Linux and macOS for both drivers and both
  application modes. Preserve earlier milestone checks and document the actual
  compiler/build/OS matrix. Exercise IPv4 and IPv6 where available; explicitly
  record unavailable coverage.
- [ ] Run supported memory, undefined-behavior and race diagnostics plus a bounded
  resource soak/repeated start-stop test. Record unsupported tooling and unresolved
  failures; do not convert unrun configurations into passes.
- [ ] Confirm runtime behavior uses this project's implementation, standard
  library and OS APIs. Verify all documented commands from a clean checkout.
- [ ] Land fixes through the responsible owner and rerun affected gates, including
  integration. Freeze the correctness-qualified commit for measurement.

**Exit:** reproducible results establish the full target-platform gate. This
milestone stays open while any required platform remains unverified.

### N — `results`: measured comparison and handoff, milestones 31–32

**Ownership handoff:** after J's harness and L's application freeze, N owns the
benchmark outputs/report and final run/results documentation previously held by
J/L. Coordinate any harness fix with J; freeze measured code only after M passes.

- [ ] Finish J's smoke runs for all scenario families, then run the planned
  benchmark matrix on the M-qualified code within documented host limits.
  Preserve raw data for every repeat and identify the exact commit/configuration.
- [ ] Report requests/s, useful bytes/s, p50/p95/p99 latency, errors/timeouts,
  CPU/peak memory, connections/queues, cache statistics and validation traffic.
  Produce throughput/concurrency and p99/concurrency plots with run variation.
- [ ] Explain observed bottlenecks, one-loop-thread versus multiple-worker CPU
  budgets, generator limits and shared-host effects. Limit claims to measured
  systems/workloads; cross-platform correctness does not imply cross-platform
  benchmark measurements.
- [ ] Deliver a complete run/configuration/testing guide, results report, known
  limitations and reproduction commands. Another developer must be able to build,
  run either driver, test the application and reproduce a saved workload.

**Exit:** all original milestones 10–32 have production acceptance and the final
definition of done has evidence. The project is complete after these integration
and evidence branches are merged, not merely after A–J are merged.

## 6. Rules for independently mergeable branches

1. **Freeze once.** Finish 09 and P0, record their SHAs, then branch A–J from
   the same `PARALLEL_BASE`. Use `codex/` branch names. This planning document does
   not create branches, tags, commits or pull requests.
2. **Own files explicitly.** Use the paths above and module-local CMake. C and D
   have separate cache subtrees; F/G/E have separate concurrency subtrees. K owns
   new cache/proxy integration files. Shared headers, common utilities, global
   docs, main and root wiring have one owner; coordinate any exception first.
3. **Deliver integration with the feature.** Include the production factory,
   public includes, explicit target links, module tests and handoff. No untracked
   generated source or locally ignored architecture document may be required to
   build. Do not commit declaration-only substitutes as implemented features.
4. **Track prerequisites by commit.** A dependent branch is based on L after its
   required feature commits land, or merges exactly those prerequisites before
   development. Record them. Do not cherry-pick a second copy of shared code or
   merge unrelated unfinished branches to make a feature compile.
5. **Integrate continuously.** L merges ready independent tracks as available and
   reruns the combined gate. Feature authors resolve conflicts and behavior
   regressions before handoff. Textual separation is not a reason to defer tests.
6. **Change contracts centrally.** A required contract fix lands once through the
   P0 owner, with compatibility tests and updates to the contract document. All
   affected branches incorporate it before they are called merge-ready.
7. **Respect evidence.** Keep separate implementation, local verification and
   platform-acceptance status. Merging code is not permission to check every
   milestone box. Only clocks and I/O may be faked in acceptance tests.
8. **Preserve policy and user work.** Follow `AGENTS.md`; update `CLAUDE.md`
   identically if that policy changes. Never overwrite unrelated changes. Commit,
   push or publish only when the user requests it.

### Merge handoff checklist for every track

- [ ] Required base/prerequisite SHAs, owned files and exported targets/factories
  are recorded in `docs/branches/<name>.md` using the canonical track name.
- [ ] No unresolved markers, duplicate protocol implementations, empty production
  substitutes, unexplained interface changes or undocumented runtime dependencies.
- [ ] The branch builds from its declared prerequisites. Its module's real tests,
  the earlier strict milestone gate and relevant integration checks pass.
- [ ] Pending dependent acceptance is named precisely; it is never reported as
  passed. Resource/deadline/cancellation contracts and actual platform results
  are recorded along with commands.
- [ ] The branch has been combined with current L and its affected system tests
  run. The merger receives ready code and evidence, not a list of wiring tasks.

## 7. Validation commands and final merge order

Use CMake/CTest from the repository root and a separate build directory for each
active branch/worktree. Choose another directory if its generator differs.

```powershell
cmake -S . -B build/agents -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DREQUIRE_ALL_MILESTONES=ON
cmake --build build/agents --config Release
# After milestone 09 is actually implemented and registered:
ctest --test-dir build/agents -C Release -L '^milestone0[1-9]$' --output-on-failure
```

P0 adds the `parallel_foundation` label. Feature tests use their retained
`milestone10` through `milestone32` labels; shared tests may carry several labels.
For example, a static-service branch adds a strict
`-L '^(parallel_foundation|milestone(0[1-9]|1[0-2]))$'` run. Require nonempty test
selection and inspect the registered tests: CTest finding no tests is not proof
of acceptance. Extend CI selection to the actual merged/verified milestones,
not a guessed numeric prefix when parallel branches leave gaps.

The current `REQUIRE_ALL_MILESTONES` option disables treating exit 77 as a skip;
it does not create missing suites. P0/I must make the acceptance inventory
explicit so missing targets/labels cannot silently satisfy a gate. Compile-only
`tests/contracts/` checks remain compile checks. Future pending suites must not
be counted as passing, and full-suite strict runs are appropriate only once their
required implementations are present.

Final integration runs all registered required tests:

```powershell
ctest --test-dir build/agents -C Release --output-on-failure
```

The merge order is **09 → P0 → independent A–J → K after B/C/D → finished L
→ M → N**. L may absorb independent tracks earlier, and tooling work overlaps
implementation. If using a separate integration branch, merge the verified L/M/N
history into the destination branch; do not reassemble a different untested tree.
Run the final gate on the exact resulting commit.

- [ ] Base 01–09 preserved; P0 contracts and streaming reference are verified.
- [ ] Static, uncached proxy and cached proxy run through both drivers.
- [ ] Memory, backpressure, fairness, cancellation and shutdown guarantees hold.
- [ ] Windows/Linux/macOS acceptance is recorded with no required pending suites.
- [ ] Benchmark raw repeats, plots and reproduction/run documentation are saved.
- [ ] Every integration change is included; no manual implementation remains
  after merging the delivered branches.
