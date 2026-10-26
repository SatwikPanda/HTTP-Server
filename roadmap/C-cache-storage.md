# C — cache-storage

**Status:** implemented; verified locally on Windows. **Delivery branch:**
`cache-storage-policy` (combined C/D). See the
[handoff](../docs/branches/cache-storage.md) for exact checks and limitations.
**Milestones:** 17 and storage/accounting portions of 24.
**Start:** [P0](P0-parallel-foundation.md). Runtime consumers are
[K](K-cache-proxy.md) and [L](L-application-assembly.md). [D](D-cache-policy.md)
shares the P0 value contracts and remains independent of this implementation.
Follow the [shared workflow](README.md).

## Outcome and ownership

Implement bounded, thread-safe storage of immutable cache entries. Own
`include/cache/storage/`, `src/cache/storage/`, `tests/cache/storage/` and local
CMake. Do not implement HTTP freshness or open sockets inside storage. The old
`include/cache/cache.hpp` and `lru_cache.hpp` are scaffolding; P0 decides their
compatibility/retirement before this branch starts.

Use the shared key/entry/version/reservation contracts. An entry includes status,
end-to-end fields, immutable body bytes, content encoding, validators, timing
and accounted cost. It does not contain a raw downstream wire response. Cache
body bytes are transfer-decoded but retain their content encoding.

## Implementation order

### 1. Specify identity, costs and outcomes

Key equality/hash cover scheme, normalized host, effective port and exact escaped
path/query. Do not decode percent escapes, sort query parameters, remove significant
empty components or re-normalize keys differently from B/P0. Test deliberate
non-equivalences as well as default-port equivalence.

Freeze the logical accounting model before implementing eviction. Include body
capacity, owned header/key/validator metadata and chosen container overhead.
If the budget is logical rather than exact allocator RSS, say so and also bound
container counts and temporary work. Checked addition/subtraction is required
before reserving or allocating attacker-influenced lengths.

Storage results must distinguish miss, hit snapshot, oversized rejection, exhausted
budget, version conflict and expected allocation/operation failure. Ordinary
capacity rejection is not exceptional control flow.

### 2. Implement a consistent map/list LRU

Use `unordered_map<Key, Node>` plus a recency list. Each node owns an immutable
shared entry and its list iterator. Keep the invariant that every resident map
entry has exactly one recency node, resident cost is the sum of resident entries,
and front/back recency meaning is documented.

Lookup takes a short mutex, finds/promotes the node, acquires a snapshot/pin
according to the memory contract, then returns after unlocking. Insert/replace
computes costs and reserves resources before mutation. Evict least-recently-used
entries until count/resident constraints allow admission. Rejection must not
leave broken iterators, leaked reservations or a half-replaced existing entry.

Build allocating metadata outside the critical section where possible. Recheck
the target version/capacity while locked. Use rollback guards for multi-container
mutation; test allocation failures at structural transitions. Move retired shared
entries out of the lock and destroy them afterward so freeing a large body does
not hold the global storage mutex.

### 3. Make allocation lifetime and category accounting explicit

Maintain separate observations for resident bytes, retained/pinned bytes and
candidate bytes. A response can hold an entry after it leaves the LRU; eviction
reduces resident cost but does not free its underlying allocation.

Use P0's reservation token per owned allocation and transfer it across candidate,
resident and retained states. Define whether categories overlap for reporting;
the enforced total must count unique live allocations once. If pinning needs an
additional in-flight allowance, reserve it before returning a pin or use a
bounded snapshot handle. Do not discover only at eviction that an already-issued
snapshot has unbudgeted lifetime.

For example, one 1 MiB body referenced by two readers is one live body allocation,
two bounded reader states, and possibly one resident entry. Evicting it removes
the resident entry; the body remains charged until its last owner releases it.
Replacing it with a new body can require both bodies to be charged at once.

Candidate builders reserve before growth, cap object bytes and release on abort.
Transfer/move the completed allocation into an immutable entry when possible.
If immutable construction copies bytes, reserve the temporary duplicate too.
If memory cannot be reserved, K stops caching and continues forwarding.

### 4. Protect replacement and revalidation against races

Return an entry identity/version with each snapshot. Conditional erase/update/
replace succeeds only if that identity is still current. A monotonically unique
version or equivalent token must survive remove/reinsert of the same key without
an ABA collision; document overflow behavior rather than wrapping silently.

K retains the exact snapshot it validated. A late 304 may derive metadata only
for that snapshot's body and must not update another resident generation. A
late miss/200 commit checks the version/absence token captured at start and must
not overwrite a newer authoritative update accidentally. Duplicate fetches are
allowed; their completion ordering still needs a defined conflict result.

No caller may mutate headers/body obtained through a snapshot. Metadata refresh
creates a new immutable entry, sharing the old body when allowed and charging
new metadata. Cache clear/eviction does not invalidate active readers.

## Nuances

- Returning a naked shared pointer is not sufficient if it bypasses pin-budget
  admission. Keep pin ownership/reservations inseparable in the frozen API.
- Locks protect list/map/version/counter changes, not response serialization,
  filesystem/DNS work, waits or metric callbacks that might acquire other locks.
- Define one lock order if reservations have their own mutex; release tokens
  outside the cache mutex when their destructor can call into accounting.
- Total-memory accounting must include bodies evicted but still referenced,
  temporary replacements, candidates and bounded metadata/container growth.
- A cache hit is not proof of freshness; D makes that decision. LRU promotion
  does not reset HTTP age or freshness timestamps.

## Verification

Use deterministic access sequences for promotion/eviction, exact byte/count limits,
replacement delta, key equivalences, oversized objects, empty bodies and metadata
cost. Hold snapshots while replacing/evicting/clearing and verify their bytes and
accounting until final release.

Use barriers for parallel lookup/evict/replace and version conflicts. Interleave
delete/reinsert/late-validation to catch ABA. Inject allocation/reservation failures
and cancellation during candidate growth; assert map/list invariants and all
reservations returned. Run supported race diagnostics later with M.

Run `milestone17` and storage portions labeled `milestone24`, plus base/P0 tests.
Storage-only tests cannot complete full concurrent proxy milestone 24; K/L must
exercise real cache transactions under both drivers.

## Done and handoff

- [x] Production store, immutable snapshots, conditional updates and reservation
  ownership work under concurrent callers without I/O under locks.
- [x] Publish cost/category definitions, conflict outcomes and lock order in
  `docs/branches/cache-storage.md` with actual verification results.
- [x] K receives an API that cannot accidentally combine old metadata with a new
  body or bypass the global memory bound.
