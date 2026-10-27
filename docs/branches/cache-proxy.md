# K — cached proxy handoff (2026-10-26)

Branch: `cache-proxy`. Implements milestones 20–21 at the transaction/reference
driver boundary and the transaction portions of 24. Full E/F/G application
acceptance and command-line assembly remain L; no L branch was available on
origin when this work started. The executable still runs the fixed-response demo.

## Prerequisites and stable checkpoints

- P0: `4547b48687d70b4d2d54370f01b6dfa946b80181`.
- C/D: `55c5333223097ee2e8e0e073c46fcfa056882ce9` (`cache-storage-policy`).
- B: `96ad613ee95fa07f9d78e8ad21b4e49b9f7e9993` (`proxy`).
- `33c6b96`: prerequisite merge, 30 strict runtime tests passed on Windows GCC.
- `8e74c50`: complete cached transaction and acceptance suites; all 32 strict
  tests passed on Windows GCC, Clang and MSVC before this checkpoint was committed.
- `e5c4d54`: additional parallel pinned-hit, reader-exhaustion and client-disconnect
  regressions; the final 32-test strict gate again passed on all three compilers.

An isolated worktree preserves the unrelated roadmap edits in the original
checkout. This branch does not require A or change its implementation.

## Public integration

Link target `cache_proxy` and include `proxy/cache_transaction.hpp`.
`make_cache_factory(shared_ptr<LruStorage>, CacheProxyConfig, CacheStats,
ProxyStats)` returns `Result<shared_ptr<const http::TransactionFactory>>`.
Check the result and assign the factory to `HttpSessionConfig::application`.
Select B's `make_forward_factory` instead to disable caching. Both execute the
same forwarding parser, work actions and common downstream writer.

The factory retains storage/config/stats. Requests and clocks obey the existing
borrowed `TransactionContext` lifetime contract. Factories/storage may be shared
across owner threads; each transaction and its events stay on one owner thread.
Admission uses `ProxyLimits::max_active_transactions`, including hits. B retains
its own forwarding admission/deadlines. Reference DNS remains synchronous and
cannot be interrupted; E is needed to enforce an asynchronous DNS deadline.

Production files live under `src/proxy/cached/`, P0's reserved K module path.
Tests live under `tests/proxy/cached/`, registered by B's test CMake when the K
target exists. Neither new directories nor a new target are placeholders.

## Decision, ownership and conflict rules

Shared B target validation runs first. D decides whether lookup, reuse, storage
and network are permitted. Cache-only failures return 504 without submitting any
work, including on HEAD, request no-store, credentials, ranges, client validators,
no-cache, stale/missing entries or exhausted reader admission. Malformed request
cache directives fail closed. Ordinary bypass requests use B without capture.

Fresh hits retain a budgeted immutable snapshot until the final body segment has
drained. Metadata is sanitized, Age recalculated, Via appended and framing rebuilt;
opaque content-encoded bytes keep their Content-Encoding. Output segments are
bounded by downstream demand and the transaction's response window. `max_read_bytes`
limits origin reads only; resident cache hits can fill the downstream window.
Eviction/replacement cannot alter a reader's body or metadata. Cancellation drops
its pin. The [cached-output follow-up](../../performance%20testing/CACHED_OUTPUT.md)
records the regression checks, memory tradeoff and measured Windows effect.

D's new `can_store_head` is a preliminary eligibility gate. Only the final
`can_store_stream(..., complete=true, decoded_byte_count)` gate permits insertion.
C reserves and charges candidate storage before each body copy, including temporary
growth allocations. Per-object/global/metadata budget failure discards capture and
keeps forwarding. Parser failure, cancellation, malformed trailers and truncated
fixed/chunked responses never insert. Close-delimited responses bypass capture.
Allowed trailers are forwarded on the initial response but are not retained as
cached headers. Stored headers omit transfer framing and hop fields. Policy rejects
Connection nominations of cache/representation metadata so filtering cannot erase
received Age, remove no-cache, or relabel the bytes.

B captures steady time when the request is fully written, then steady/wall time
when the final response head is parsed. Cache insertion preserves those timestamps;
time spent receiving/draining the body contributes to current Age. Subsequent wall
clock jumps do not change resident age.

Every fill compares against its original lookup observation, including an absence
epoch. A late conflicting fill is discarded; it still finishes the original client
response. C intentionally invalidates absence epochs on unrelated mutations too.
There is no request coalescing or blind overwrite.

Validation retains the exact selected snapshot and sends D's ETag condition,
otherwise Last-Modified. B closes the upstream and completes the observer before
K intercepts a 304. D verifies identity/representation metadata and merges permitted
fields; C refresh shares the selected body's allocation and replaces only the
selected version. Success becomes a newly framed 200 for the unconditional client.

An unusable 304, metadata allocation failure or version conflict permits **one**
unconditional retry. The retry takes a new storage observation, uses no proxy
validator and never serves a different resident body's bytes under the old 304's
metadata. Another 304 becomes 502, even if cache admission is now exhausted.
An initial unconditional fetch receiving 304 also becomes 502. Client conditional
requests bypass this handling and retain B's ordinary 304 behavior.

A changed 200 replaces only after complete eligible receipt. If capture/insertion
fails, or the authoritative response changes eligibility, the selected old version
is conditionally erased. Other authoritative 2xx–4xx responses except 304 also
invalidate that version. Neither replacement nor invalidation can affect a newer
competing version. Transport/5xx failure grants no stale fallback. Before downstream
commit B returns its error; after commit the common session closes without another
head or a successful final chunk.

## Counters and bounds

`CacheStats::count(CacheEvent)` reads fixed atomic counters: ordinary hit/miss/bypass,
cache-only failure, validation request/accepted 304/received changed 200/failure,
insertion, discarded capture, version conflict, duplicate miss, cancellation and
admission rejection. Validation 200 counts received heads; a subsequently interrupted
body may also count validation failure. A 304 is not a second client request. Conflict
counts mutations, so a failed replacement and its failed invalidation can count twice.

A mutex-protected, preallocated registry detects overlapping ordinary misses of the
same canonical key. Its size is bounded by admission; entries borrow transaction keys
and are removed on destruction. It stores no bodies and does not coalesce requests.
Storage statistics expose resident/candidate/pinned/reader/total allocation gauges
and a new `evictions` counter for capacity/LRU removals (not erase/clear).
Transaction metadata and output are bounded by B/session limits; C's total budget
covers retained cache allocations, not unrelated socket/driver/process memory.

## Executed validation and remaining acceptance

Release builds with `BUILD_TESTING=ON` and `REQUIRE_ALL_MILESTONES=ON`:

```powershell
cmake -S . -B build/cache-proxy -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DREQUIRE_ALL_MILESTONES=ON -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/g++.exe
cmake --build build/cache-proxy --config Release -j 6
ctest --test-dir build/cache-proxy -C Release -L 'milestone0[1-9]|parallel_foundation|milestone1[3-6]|cache_storage|cache_policy|cache_proxy' --no-tests=error --output-on-failure
```

The same 32-test strict gate passes on Windows GCC 16.2, Clang 22.1.8 (separate
`build/cache-proxy-clang`, compiler `C:/msys64/ucrt64/bin/clang++.exe`) and MSVC
19.29 (`build/cache-proxy-msvc`, generator `Visual Studio 16 2019`, `-A x64`).
Zero failures/skips. MSVC's existing `/DNDEBUG` to `/UNDEBUG` override warning is
expected for Release-active checks. Network tests ran with approved loopback access.

K's two runtime suites use production factories, parsers, policy and storage with
scripted origin I/O/clocks for exact overlaps and zero-DNS assertions. They also run
real fragmented loopback origins, persistent client miss/hit/304 pipelines, committed
chunk failure, client disconnect, and a gated real origin losing to another completion.
Thread barriers cover concurrent candidates and parallel pinned hits during eviction.
Assertions check body/metadata pairing, versions, budgets and eventual charge release.
B's real downstream and optional IPv6 cases run in the dependency gate.

Linux/macOS, remote CI and race/sanitizer diagnostics have not been executed for K.
CI now selects B and K as well as the existing base/C/D gate; that configuration is
not evidence of a completed CI run. L must repeat these scenarios through F/G with E
and wire the factories into the application before claiming full milestone 24 or a
cache-enabled command-line server.
