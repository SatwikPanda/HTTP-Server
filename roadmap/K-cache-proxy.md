# K — cache-proxy

**Status:** planned. **Branch:** `codex/cache-proxy`.
**Milestones:** 20–21 and transaction/concurrency portions of 24.
**Start:** merged [B](B-proxy.md), [C](C-cache-storage.md), [D](D-cache-policy.md)
on P0. Full concurrent acceptance occurs in [L](L-application-assembly.md).
Follow the [shared workflow](README.md).

## Outcome and ownership

Compose policy, storage and uncached forwarding into a cache-enabled proxy
transaction. Own `include/proxy/cache_transaction.hpp`,
`src/proxy/cache_transaction.cpp`, `tests/proxy_cache/` and separate module CMake.
Names remain subject to P0's published contracts. Keep B's uncached factory fully
usable and avoid copying its network state machine.

Read B's request/final-head/body/completion hooks, C's snapshot/version/reservation
semantics, D's decision matrix and P0's commitment/clock conventions. If an agreed
hook cannot express needed behavior, change it through the owner before introducing
a private workaround that F/G cannot execute consistently.

## Implementation order

### 1. Make one decision path own origin permission

Parse/validate the client request and target through shared code, construct the
canonical cache key, then detect only-if-cached before any origin operation.
Consult D to determine permitted lookup, snapshot reuse, validation and insertion.

```text
valid request -> policy + optional storage lookup
              -> usable hit: serialize cached 200 and body
              -> cache-only with no allowed usable hit: 504, no DNS/connect
              -> bypass: B forwarding without candidate capture
              -> stale validated candidate: conditional B transaction
              -> miss/unusable candidate: ordinary B transaction
```

No bypass, timeout fallback or retry path may evade the network-forbidden decision.
HEAD, Authorization/Cookie/Range/client conditionals and request no-store follow
D's conservative policy. A fresh object in storage alone is not sufficient if
this particular request cannot reuse it.

### 2. Serve snapshots through the common writer

Acquire a budgeted immutable snapshot and retain it through the last body write.
Copy/filter metadata within limits, calculate current Age and regenerate framing
for the current request. Never replay a raw origin wire response or its old
Connection/Transfer-Encoding fields. A cached content-encoded body retains the
matching Content-Encoding metadata.

Stream body segments according to output capacity; preserve snapshot lifetime
across partial writes. An eviction/replacement by another request must not change
the bytes currently being sent. Cancel releases the pin/reservation exactly once.

### 3. Capture eligible miss responses without disrupting forwarding

At final origin head receipt, ask D whether storage can be attempted and reserve
a bounded candidate. Informational responses are not cache entries. Tee decoded
representation segments into the candidate only after reserving their space;
the same bytes continue through B's downstream path.

On per-object/global budget exhaustion, discard candidate retention and release
its reservation while forwarding continues. Do not fail a valid client response
solely because caching is unavailable. Include retained capacity and temporary
copies in accounting, not only the vector's logical length.

Commit only after the origin parser reports complete valid receipt and D confirms
eligibility. EOF on fixed/chunked input, bad trailers, timeout, cancellation or
upstream error cannot produce an entry. Close-delimited responses initially bypass
storage. Capture request/response clock pairs at P0's defined points and preserve
time spent receiving the body when calculating its age at insertion.

Move the completed body into C when possible. A conditional commit against the
observed entry/absence version must not overwrite a newer competing completion.
On conflict, follow the documented discard/re-evaluate policy; never force a
blind overwrite simply because the network request finished last.

### 4. Revalidate the exact snapshot

Retain the selected stale snapshot and its identity/version. Ask D to prepare
If-None-Match, otherwise If-Modified-Since, for B's origin request. Client
conditionals remain on the bypass path; these are proxy-generated validation
requests for an otherwise unconditional client GET.

On origin 304, validate association with the captured representation and run D's
metadata merge rules. Share the captured body's immutable allocation and recompute
freshness/timing; do not let new metadata silently change content encoding or
representation length. Compare-and-replace the original version in C.

If the resident entry changed, never merge the 304 onto its different body.
Use the frozen conflict policy: re-evaluate a new snapshot, discard the attempted
update or perform a bounded unconditional fetch as appropriate. Explicitly bound
retries; an origin repeatedly returning unusable 304 must eventually produce 502.
For the unconditional client GET, a valid refreshed representation is sent as 200.

On new 200, capture and replace only after complete eligible receipt. When an
authoritative changed response cannot replace the entry, invalidate the captured
old version; do not erase a newer version another transaction inserted. An
interrupted response never refreshes the old entry or grants stale-serving rights.
Changed eligibility such as Vary/private/no-store must be handled explicitly.

### 5. Centralize cancellation, failure and metrics

B still owns transport failure and downstream commitment rules. Before response
commit, an origin failure can produce the corresponding 502/504; after commit,
close without a second response or a successful final chunk. Do not serve stale
content as an error fallback.

One terminal path releases candidate, pin, version context and pending observers.
Guard against completion/cancellation races causing duplicate insertion or release.
Count ordinary hit/miss/bypass separately from validation requests/results and
duplicate concurrent misses. A validation 304 is not another client request.

## Verification

Use a real controlled origin and deterministic barriers. Required cases:

- First miss then fresh hit with zero additional origin request; fresh Age grows.
- Cache-only fresh hit and 504 on missing/stale/no-cache/bypass/HEAD/no-store paths,
  with both resolver submissions and origin connections counted as zero.
- Candidate over object/global limit still forwards the complete response;
  truncated, malformed and cancelled origin bodies never become hits.
- Expiry/no-cache validation, weak ETag, Last-Modified fallback, matching 304,
  changed 200, new eligibility restrictions and incompatible/mismatched 304.
- Revalidation failure returns an error, not stale bytes; committed stream failure
  closes with no successful terminator.
- Parallel hits/evictions, duplicate misses, two validations and changed-origin
  responses racing with replacement. Assert exact body/metadata pairing and
  version-conflict outcomes, not only a successful status.
- Slow consumers pin evicted entries; concurrent candidates and replacements stay
  within total/resident/object limits and all reservations release afterward.

Run `milestone20`, `milestone21`, transaction `milestone24`, B/C/D dependencies and
base/P0 tests. L repeats the same integration scenarios through F/G with E.

## Done and handoff

- [ ] All origin traffic passes through B and all cache decisions through D/C.
- [ ] Cache-disabled and enabled factories are selectable by L without alternate
  protocol implementations.
- [ ] `docs/branches/cache-proxy.md` records conflict/retry semantics, memory
  ownership, clock capture points, counters and actual integration results.
