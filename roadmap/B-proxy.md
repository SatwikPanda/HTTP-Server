# B — proxy

**Status:** implemented on `proxy` (verified locally on Windows GCC 16.2 Release; see [handoff](../docs/branches/proxy.md)). **Branch:** `proxy`. **Milestones:** 13–16.
**Start:** [P0](P0-parallel-foundation.md). Enforced asynchronous DNS deadlines
also require [E](E-helpers.md); combined driver acceptance belongs to
[L](L-application-assembly.md). Follow the [shared workflow](README.md).

## Outcome and boundaries

Build the complete uncached forwarding transaction, reusable by
[K](K-cache-proxy.md). Own forwarding/target/transaction implementation in
`include/proxy/`, `src/proxy/`, `tests/proxy/` and its module CMake. Reserve K's
cache-transaction files. Expose the P0 factory and origin-exchange seam so K can
intercept a request, add validation fields and observe a response without
duplicating forwarding, framing or failure handling.

Inspect `include/net/resolver.hpp`, `socket.hpp`, `poller.hpp`,
`include/http/response_parser.hpp`, `framing.hpp`, `serializer.hpp`, `header.hpp`
and their tests. Existing `resolve()` is synchronous. `connect_candidates()` is
a bounded synchronous convenience adapter over resolved candidates, not an
event-loop scheduling primitive. Use P0 actions for production driver execution.

## Implementation order

### 1. Prepare a target and outbound request

Use P0's shared target parser. Require an absolute cleartext HTTP URL; reject
userinfo, fragments, unsupported scheme, malformed bracketed IPv6 and invalid
port. Represent the host used for resolution separately from the authority used
in Host, including IPv6 brackets. Normalize host/scheme and effective port only
as defined in the contract. Preserve path escapes, query order and the distinction
between raw target components. An absent path forwards as `/` with any query.

Regenerate Host from the selected authority even if the client's valid Host names
something else. Build the origin-form request using the exact path/query. Validate
all generated fields through the serializer; never concatenate unvalidated URL
or header text into the request line.

Parse every Connection field into a case-insensitive token set, then remove all
nominated fields and the fixed hop-specific set in both request and response
directions. Determine framing from the original validated message before removing
its transfer fields; regenerate downstream framing afterward. Preserve repeated
end-to-end fields without flattening values such as Set-Cookie. Remove proxy
credentials rather than leaking them to origins. Append a bounded Via identity,
check for that identity in received chains and reject loops. This track does not
add proxy authentication or tunneling.

Keep the shared GET/HEAD, nonempty-body, Expect and Upgrade restrictions. Proxy
client conditionals/Range may be forwarded according to scope; cache eligibility
will be K/D's separate decision.

### 2. Implement an explicit transaction state machine

```text
prepared -> resolving -> connecting -> sending request -> reading heads
         -> streaming final body -> validating completion -> complete
any nonterminal state -> failed or cancelled -> release upstream resources
```

Keep downstream connection state separate from upstream transaction state. One
request uses one fresh origin connection; client keep-alive can survive successful
transactions. Track endpoint role, connection generation, transaction sequence,
candidate/attempt ID and current deadlines. A delayed event for an old attempt
must not affect a new socket or the next request.

Resolved candidates share one connect deadline. On failure unwatch/close before
creating the next socket. Set nonblocking explicitly, handle immediate connect
success and in-progress results, and verify pending error with `finish_connect()`
when readiness arrives. Candidate count/retries must be bounded. Record useful
portable diagnostics when all candidates fail.

Retain serialized request bytes and a write offset across short writes. Would-block
arms the correct interest; zero progress on nonempty output is a failure. A
successful upstream write does not mean a response is complete.

### 3. Relay heads and bodies through P0 streaming

Construct the response parser with the originating method. Bound cumulative
informational/final heads and trailers. Handle supported interim responses before
the final response; reject 101/protocol switching. HEAD, 204 and 304 remain
bodyless regardless of subsequent bytes, and an unexpected body cannot become
the next response on a reused origin connection.

Choose downstream framing independently: fixed length when known and safe,
chunked for an allowed unknown-length persistent HTTP/1.1 response, or explicit
close-delimited output when required. Respect the downstream protocol version.
Do not serialize upstream Transfer-Encoding/Content-Length blindly. Preserve
opaque Content-Encoding and the representation bytes after transfer decoding.

Only successful parser completion authorizes response completion or a final
chunk. Fixed/chunked truncation is an error; close-delimited completion requires
EOF. Bound and validate trailers before forwarding allowed metadata or discarding
optional unsupported fields. Trailers cannot redefine routing/framing. Close the
origin after each transaction, including successful close-delimited receipt.

### 4. Apply backpressure and phase deadlines

Budget parser input, decoded body segments, encoded output and any copied metadata.
Before asking the parser or origin for more bytes, check downstream capacity.
At output high-water, pause upstream read interest and consumption of buffered
data; resume below low-water. Count bytes already queued in parser callbacks and
helper completions, not just the socket output vector.

Separate connection, response-head, body-progress and downstream-output deadlines.
Progress means useful bytes or a documented state transition, not repeated wakeups.
When upstream reads are intentionally paused, avoid falsely diagnosing the origin
as stalled; the downstream progress deadline still bounds the slow client. Use
the P0 deadline contract consistently across the minimal driver and F/G.

DNS has a transaction deadline even though the OS call may continue in E. With
the synchronous reference adapter, document that enforcement is deferred until
that call returns; do not claim asynchronous behavior on the minimal path.

### 5. Centralize failure and cancellation

| Condition | Before downstream response commitment | After commitment |
| --- | --- | --- |
| Bad client target/loop/restricted behavior | Appropriate shared client error | Close if a response has already begun |
| Resolution/connect/invalid upstream message | 502 | Close |
| Upstream/DNS/connect deadline | 504 | Close |
| Local capacity exhaustion | Bounded 503 if coherent output is possible | Close/cancel according to current response state |
| Client disconnect | Cancel origin/work and release resources | Same |

Do not append a second response or successful terminator after a truncated body.
Keep commitment semantics in the shared writer so B/K/F/G agree. Cancellation is
idempotent: invalidate outstanding IDs, remove registrations, release output and
file/cache observers, and ignore late results without touching a reused session.

## Verification and pitfalls

Use P0's real controlled origin, maintained by [I](I-test-harness.md). Required
cases include default/explicit ports, IPv6 authorities, exact escaped path/query,
Host replacement, repeated and nominated hop fields, credentials, Via loops,
all body framing, interim/final heads, HEAD/204/304 and unsupported switch.

Force short request writes, split response bytes at every representative boundary,
slow origin/client, refused first candidate then success, all-candidate failure,
DNS timeout, mid-body truncation and client loss. Assert exact bytes/status,
bounded queue peaks, resumed progress and resources returned to baseline. Verify
two client requests produce two origin connections and ordered client responses.

Run `milestone13` through `milestone16` plus the base/P0 gate. Record which DNS
checks require E. Later rerun the same production tests through both drivers.

## Done and handoff

- [x] Uncached factory is usable independently of storage/policy.
- [x] P0's K integration hooks cover request preparation, final metadata, decoded
  segments, successful completion and failure without bypassing budgets.
- [x] `docs/branches/proxy.md` records limits, deadlines, header rules, tests and
  exact dependencies; L owns CLI selection and final main wiring.
