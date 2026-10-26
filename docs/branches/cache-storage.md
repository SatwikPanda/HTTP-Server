# Cache storage handoff — 2026-10-26

Track C is implemented on `cache-storage-policy`, based on
`4547b48687d70b4d2d54370f01b6dfa946b80181` (P0 plus the macOS fixture fixes).
The production target is `cache_storage`; its public entry point is
`cache::storage::LruStorage::create(Limits)` in
[`lru_storage.hpp`](../../include/cache/storage/lru_storage.hpp).
The legacy cache scaffolding remains unused. No executable mode is changed.

## Consumer contract

Use `begin(key, sanitized_headers, timing)` to start a bounded candidate,
`Candidate::append` for transfer-decoded bytes, and `finish` only after successful
message completion. The store is independent of HTTP eligibility: K must call D
before beginning/committing a candidate. Destroying the builder aborts retention.
Append failure preserves existing bytes; stop caching on budget/object rejection.
Headers must exclude hop-specific and framing fields; the store rejects them.
Keys come from P0 `parse_target`, preserving exact escapes/query order.

`lookup` returns a bounded immutable reader snapshot and an observation token.
`replace`/`erase` compare that token under the storage mutex. All stores draw from
one process-wide monotonically increasing version sequence; exhaustion rejects
mutation instead of wrapping. A miss observes the store's latest mutation epoch.
Any intervening mutation conservatively invalidates an absent-key observation,
including an unrelated insertion. This avoids an unbounded tombstone table.
Duplicate fetches may proceed, but only the first valid completion commits.

`refresh` creates new immutable metadata sharing exactly the selected body.
Commit it with the selected observation, never a new lookup's version. Existing
readers survive replacement, eviction, clear and store destruction. Fabricated
or foreign-store candidates are rejected; only this store's charged allocations
can enter the LRU. Reader handles and candidates may outlive the store; concurrent
callers must be joined before destroying the store itself.

## Bounds and accounting

Accounting is logical allocation cost, not allocator RSS. Body cost is vector
capacity plus `sizeof(Body) + 256` for owners/control structures. Growth reserves
the entire new buffer before allocating and frees the old buffer before releasing
its charge, so the transient duplicate is counted. The vector implementation
must supply the requested capacity for this exact-size construction, otherwise
the growth is rejected. Empty bodies still incur ownership cost.

Metadata cost is `sizeof(Entry) + 512` plus four times each owned key/header
component's length and fixed allowances (32 bytes per string, header structures).
This covers copied string capacity, duplicated map keys, parsed validators,
list/map nodes and ownership blocks in the logical model. Header count is at
most 100; metadata, object size, resident cost, total cost and entry count have
separate limits. Bucket allowance is `(entries + 1) * 4 * sizeof(void*) + 512`,
reserved before building the map. One temporary map/list entry during insertion
is included in the candidate metadata allowance. Metadata is cloned and charged
before replacement; old and new allocations may coexist.

Each body allocation has one reservation. Shared readers or metadata refreshes
do not double-charge it. Its category is resident if any resident entry uses it,
otherwise candidate if any candidate uses it, otherwise retained/pinned. Entry
metadata follows its own lifecycle. Categories in the global budget are disjoint.
The resident admission cost sums full entry costs (including a shared body), a
conservative limit separate from unique global allocation cost. Escaped body
references remain charged even after all entry snapshots disappear.

Every successful lookup reserves 128 logical bytes and one reader slot. Copies
of that snapshot share the same reader state; separate lookups need separate
admission. Saturation returns `exhausted`. Reader overhead is reported separately
as temporary bytes. Candidates are bounded by total capacity and nonzero overhead.

Lock order is storage mutex → shared-body accounting mutex → budget mutex.
Candidate creation and destruction never acquire the storage mutex. Container
changes are transactional: allocating insertion nodes precede eviction; failed
allocation leaves the prior entry and LRU intact. Retired bodies are destroyed
after unlocking. No I/O or callbacks run under these locks.

## Executed checks

Windows GCC 16.2, Ninja Release, strict mode, `build/cache-cd`:

```powershell
cmake -S . -B build/cache-cd -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DREQUIRE_ALL_MILESTONES=ON -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/g++.exe
cmake --build build/cache-cd --config Release
ctest --test-dir build/cache-cd -C Release -L 'milestone17|milestone24' --no-tests=error --output-on-failure
ctest --test-dir build/cache-cd -C Release -L 'milestone0[1-9]|parallel_foundation' --no-tests=error --output-on-failure
```

Both storage tests and all 20 baseline/P0 tests pass without skips. The baseline
socket tests required approved loopback access after sandbox error 10013.
Storage tests cover exact limits, recency, binary/empty bodies, held readers,
shared-body refresh, stale miss/ABA conflicts, budget rejection, cancellation,
real allocation-failure injection and eight synchronized concurrent callers.
Linux/macOS execution and race diagnostics remain pending. Full milestone 24
still requires K/L's real proxy transactions and both concurrency drivers.

### Combined C/D follow-up

`cache_policy_storage_test` additionally verifies late-304 compare-and-replace,
version-checked invalidation, normal metadata refresh and pinned body identity
using both production modules. `Body`'s private members now destroy its vector
before releasing its reservation; public P0 signatures are unchanged.
MSVC's allocating `std::list` default constructor exposed an uncaught allocation
failure before insertion. Replace/erase/clear now translate that failure with
unchanged storage. Allocation-failure tests cover those constructors, replacement,
erase and clear, in addition to candidate creation/growth/publication and pins.

All targets build on Windows GCC 16.2 (`build/cache-cd`), Clang 22.1.8 with
libstdc++ (`build/cache-cd-clang`), and MSVC 19.29 (`build/cache-cd-msvc`). The
26-test combined gate passes on all three, with zero failures/skips; the final
MSVC gate took 9.43 seconds. After the MSVC-specific correction the six affected
C/D tests were rerun successfully on GCC/Clang too. GCC/Clang combined gates took
11.54/12.17 seconds before that correction.
Clang uses the GCC configure command with its compiler path changed to
`C:/msys64/ucrt64/bin/clang++.exe`. MSVC uses:

```powershell
cmake -S . -B build/cache-cd-msvc -G 'Visual Studio 16 2019' -A x64 -DBUILD_TESTING=ON -DREQUIRE_ALL_MILESTONES=ON
cmake --build build/cache-cd-msvc --config Release -- /m:1 /nr:false
ctest --test-dir build/cache-cd-msvc -C Release -L 'milestone0[1-9]|parallel_foundation|cache_storage|cache_policy' --no-tests=error --output-on-failure
```

The same final label selection applies to the GCC/Clang directories and CI.
Transport buffers owned by callers are outside the storage budget; K/L must
account for their separate bounded I/O windows when composing total process limits.
