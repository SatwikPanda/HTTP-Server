# D — cache-policy

**Status:** implemented; verified locally on Windows. **Delivery branch:**
`codex/cache-storage-policy` (combined C/D). See the
[handoff](../docs/branches/cache-policy.md) for exact checks and limitations.
**Milestones:** 18–19 and pure revalidation decisions for 21.
**Start:** [P0](P0-parallel-foundation.md). [K](K-cache-proxy.md) executes decisions.
Follow the [shared workflow](README.md).

## Outcome and ownership

Implement deterministic HTTP cache decisions over request/response metadata,
snapshot identity and injected clocks. Own `include/cache/policy/`,
`src/cache/policy/`, `tests/cache/policy/` and local CMake. Depend on shared value
types, not C's LRU implementation, a driver or any network operation.

Read `include/http/header.hpp`, `request.hpp`, `response.hpp`, `date.hpp`,
`include/core/clock.hpp` and P0's key/entry contract. HeaderMap preserves repeats;
policy must inspect all relevant field occurrences rather than only `get()`.

## Implementation order

### 1. Parse directives into explicit facts

Build a bounded parser for Cache-Control field values: case-insensitive directive
names, token/quoted values, escaped quoted characters, optional whitespace and
repeated fields. Do not split on commas inside quotes. Parse numeric durations
with checked arithmetic and an explicit upper bound.

Separate absent, valid and malformed/conflicting facts. Conservative handling
must not convert an invalid restrictive/high-priority directive into permission
to use a more generous fallback. Apply field-qualified private/no-cache to the
whole response. Unknown extensions do not create new reuse permissions. Preserve
the request's only-if-cached restriction even when another directive forces bypass.

### 2. Decide lookup, storage and network permission separately

| Input | Initial project policy |
| --- | --- |
| Unconditional GET with no prohibited request fields | Eligible for lookup and possibly storage |
| Authorization, Cookie, Range, client conditionals | Bypass shared cache |
| HEAD | Forward without replacing/reusing the GET body through this cache path |
| Request no-store | Bypass both lookup and insertion |
| Response no-store/private, Set-Cookie, any Vary | Do not store/reuse as an eligible new entry |
| Non-200, partial, invalid, interrupted, oversized | Do not store |
| Close-delimited response | Forward but initially do not store |
| Request/response no-cache | Validation required before reuse |
| only-if-cached | Usable allowed hit or 504; network forbidden |

Only complete 200 responses to unconditional GET qualify for initial storage.
The response's head can make a candidate ineligible early, but only successful
framing completion can make insertion eligible. Opaque Content-Encoding does not
require decompression; eligibility is about metadata and complete bytes.

Use a result such as hit/fetch/revalidate/bypass/cache-only-failure, plus explicit
lookup/store/network permissions and reasons as defined by P0. Decide the
cache-only constraint first, then evaluate whether this request can use a
snapshot. If it needs validation, bypasses lookup, misses or is stale, return
cache-only failure without allowing a resolver/connect action.

### 3. Calculate freshness and age with two clocks

Choose explicit lifetime in order s-maxage, max-age, Expires. With no valid
explicit lifetime, the entry is immediately stale; retain it only if useful for
validation under the conservative policy. Date/Expires and Age use validated
metadata; missing/invalid data must not grant a fresh lifetime accidentally.

Use the architecture's calculations, with saturating durations:

```text
apparent_age = max(0, response_wall_time - Date)
response_delay = max(0, response_monotonic_time - request_monotonic_time)
corrected_initial_age = max(apparent_age, received_Age + response_delay)
current_age = corrected_initial_age + max(0, now_monotonic - response_monotonic_time)
fresh = current_age < freshness_lifetime
```

Freeze timestamp capture points with B/K in P0; do not mix response-head and
body-completion timestamps opportunistically. If timing is recorded at final
head receipt, time spent receiving the body already contributes to resident age
when it is finally inserted. Never reset an old response's age to zero on insert.

Apply request max-age/min-fresh as additional constraints. Request/response
no-cache can require validation even during the nominal fresh lifetime. Respect
must-revalidate/proxy-revalidate; this project never serves stale content, even
when max-stale would permit it. At exact expiry, validation/fetch is required.
Wall-clock jumps after receipt must not reduce monotonic resident age.

Generate the updated Age from the final reuse calculation, clamped to the chosen
wire representation. LRU promotions and cache hits do not restart freshness.

### 4. Define validation and metadata merge decisions

Prefer stored ETag in If-None-Match; otherwise use Last-Modified in
If-Modified-Since. Preserve weak-tag syntax. The validation context carries the
exact cache key and entry version, not only a URL string.

For a 304, decide whether it corresponds to the selected representation and
which end-to-end metadata may refresh it. Never attach headers to an unrelated
body or blindly copy hop/framing fields. New metadata that introduces Vary,
private/no-store or another eligibility restriction must lead to invalidation
and the safe transaction outcome defined with K. A change incompatible with
the retained representation cannot be solved by relabeling its body bytes.

Return a typed merge/refetch/error/invalidate decision. K performs the bounded
retry, storage compare-and-replace and downstream response. A merged response
to an unconditional client GET is a cached 200 with newly generated framing,
not the origin's 304 forwarded as though the client made that condition.

## Verification

Use table-driven production policy tests with manually advanced wall/steady
clocks. Cover every row above individually and in combinations: only-if-cached
with no-store, Authorization, HEAD, missing entry, stale entry and no-cache;
quoted/case/repeated directives; malformed and overflowing numbers; conflicting
lifetimes; exact expiry; received Age and transit delay; future/invalid Date;
clock jumps and Expires boundaries.

For validation, cover ETag preference, weak syntax, Last-Modified fallback,
matching/mismatching or absent identifying metadata, permitted field updates,
changed eligibility and version mismatch. Test returned decisions and values;
network/storage mocks must not simulate policy instead of calling it.

Run `milestone18`, `milestone19`, relevant pure `milestone21` checks and the
base/P0 gate. K/L add wire-level checks proving the decisions are obeyed.

## Done and handoff

- [x] Pure policy uses bounded parsers and deterministic clock inputs.
- [x] Every decision explains network/lookup/storage permission unambiguously.
- [x] `docs/branches/cache-policy.md` contains the policy matrix, timestamp
  conventions, malformed-input rules and actual test results.
- [x] No live resolver, socket or LRU implementation is needed to test this track.
