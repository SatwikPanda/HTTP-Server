# Cache policy handoff — 2026-10-26

Track D is implemented on `cache-storage-policy`, based on
`4547b48687d70b4d2d54370f01b6dfa946b80181`. Target `cache_policy` implements the
P0 `cache::Policy` port through
[`CachePolicy`](../../include/cache/policy/cache_policy.hpp), independent of
storage and drivers. HTTP syntax/date/tag utilities remain shared with P0.
The pure tests require no resolver, socket, file or LRU instance.

## Decisions and insertion

| Input | Decision |
| --- | --- |
| Eligible GET, absent entry | Fetch; lookup/storage/network permitted |
| Eligible fresh matching entry | Hit; no network or new insertion |
| Stale or no-cache, usable validator | Revalidate; ETag preferred over Last-Modified |
| Stale without validator | Fetch; never serve stale |
| Authorization, Cookie, Range, client conditionals, HEAD, request no-store | Bypass lookup and storage |
| only-if-cached plus any bypass/miss/stale/no-cache | Cache-only failure; network forbidden |
| only-if-cached plus allowed fresh hit | Hit; network forbidden |
| Malformed request Cache-Control | Cache-only failure; network forbidden |
| Response private/no-store/Set-Cookie/any Vary/Content-Range/non-200 | Ineligible for storage/reuse |

Malformed request directives fail closed because a broken quoted suffix or parser
limit could hide `only-if-cached`. K maps cache-only failure to 504 before DNS or
connect. Other bypass cases forward normally. Unknown valid extensions add no
permissions. Every returned decision explicitly separates lookup, insertion and
network permission. `store_allowed` permits considering a future response; it
does not certify that response.

`can_store` checks a complete buffered response. Streaming consumers must use
`can_store_stream(..., complete, body_bytes)` with the parser's actual total
transfer-decoded byte count; an empty streaming `Response::body` is not evidence
of an empty object. These checks reject partial/oversized/interrupted messages,
close-delimited framing, conflicting framing and prohibited trailers. Call with
`complete=true` only after the production parser confirms successful completion.
Body bytes retain their opaque content encoding. Storage independently enforces
its object/global limits and accepts sanitized end-to-end fields.

## Bounded parsing and clocks

Cache-Control parsing inspects all occurrences, preserves quoted commas, decodes
quoted escapes, accepts case-insensitive names/OWS, and checks decimal arithmetic.
Limits are 100 fields, 65536 Cache-Control bytes, 256 directives and 8192 decoded
bytes per quoted value. Durations above 2147483648 seconds are malformed.
Identical repeated durations are accepted; conflicting repeats are malformed.
Malformed response directives reject storage/reuse instead of falling back to a
less restrictive lifetime. Field-qualified private/no-cache apply globally.

Freshness chooses s-maxage, max-age, then valid Expires minus Date. There is no
heuristic lifetime. No/zero/invalid explicit lifetime is useful for insertion
only with a validator. Insertion's date checks are deterministic syntax checks
(RFC 850 uses the epoch reference); actual interpretation/freshness uses the
recorded final-head wall timestamp. Missing/invalid/repeated Date or invalid/
repeated Age cannot produce a fresh hit. A future Date gives zero apparent age.

Timestamp capture follows P0: request fully written, final head received, paired
wall time at final head. Age includes apparent age, received Age plus transit,
then monotonic time since head receipt, including body download time. Moving the
wall clock after receipt cannot rejuvenate an entry. Elapsed calculations avoid
native-duration subtraction overflow, clamp negative intervals to zero and
saturate at 2147483648 seconds. Fractional total age rounds upward conservatively;
this may require validation within the final fractional second of a lifetime.
`age_header` formats the bounded nonnegative wire value. Exact expiry is stale.
Request max-age/min-fresh and no-cache further constrain hits; max-stale never
permits stale delivery, including with must-revalidate/proxy-revalidate.

## Validation execution contract for K

`revalidate(selected_snapshot, origin_response, new_timing)` returns the exact
selected version. Its three frozen P0 outcomes mean:

- `refresh_metadata`: new end-to-end headers and timing for the selected body.
  K uses C's `refresh`, then `replace(key, decision.expected, candidate)`. A version
  conflict discards the proposed update; never apply it to a newer lookup/body.
  The client receives cached 200 with regenerated framing and updated Age.
- `refetch`: the response cannot identify/refresh this representation. K performs
  at most its bounded unconditional retry, or returns an upstream error. No stale
  body may be served. This also covers full non-304 responses; K handles a complete
  eligible 200 as a new candidate using the normal insertion decision.
- `reject`: changed eligibility or incompatible representation metadata. K must
  conditionally erase the selected version and take a bounded unconditional
  fetch/error path, never send the retained body under the changed metadata.

Matching weak tags preserve their syntax; a new strong tag cannot identify an old
weak tag. Last-Modified is the fallback when no usable ETag exists. Missing or
mismatching identifying metadata conservatively requests a refetch. A valid new
Date is required. Content-Type, Content-Encoding and Content-Location changes
reject reuse; any supplied length must match the retained body. Hop/framing and
Connection-nominated fields are excluded, repeated end-to-end fields survive,
and Age is reset to the validation response's Age (zero if absent) with its new
paired timing. Existing snapshots are never mutated. These rules are deliberately
conservative within the project's scope; see
[RFC 9111 validation](https://www.rfc-editor.org/rfc/rfc9111.html#section-4.3.4).

## Executed verification

Windows GCC 16.2, Ninja Release, strict mode, `build/cache-cd`:

```powershell
cmake --build build/cache-cd --config Release
ctest --test-dir build/cache-cd -C Release -L 'milestone18|milestone19|milestone21' --no-tests=error --output-on-failure
```

All three pure policy suites pass without skips. They cover the eligibility and
cache-only matrices, directive syntax/bounds/conflicts, freshness/Age/clock jumps,
expiry and request constraints, validator preference and metadata merge/refetch/
reject outcomes. Linux/macOS and race diagnostics remain pending. Full milestone
21 requires K's live origin transactions and downstream behavior; pure decisions
do not establish its wire-level acceptance.

The combined C/D regression additionally runs the real store: a newer fetch makes
the delayed validation/update/invalidation conflict, while an uncontested 304
refresh preserves body identity and recalculates Age. Final verification also
builds with Windows Clang 22.1.8 and MSVC 19.29; all six cache tests pass on each.
All three pass the full 26-test baseline/C/D gate. Compiler commands and the
MSVC allocation-failure correction are in the [storage handoff](cache-storage.md).
Use `-L 'cache_storage|cache_policy'` for the six C/D tests or
`-L 'milestone0[1-9]|parallel_foundation|cache_storage|cache_policy'` for the full
gate; both selections use `--no-tests=error --output-on-failure` and strict mode.
