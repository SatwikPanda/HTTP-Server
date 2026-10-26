# A — static

**Status:** implemented and integrated; see the application/CI handoffs. **Branch:** `static`. **Milestones:** 10–12.
**Start:** [P0](P0-parallel-foundation.md). **Full driver acceptance:** [L](L-application-assembly.md)
with [E](E-helpers.md), [F](F-thread-pool.md) and [G](G-event-loop.md).
Follow the [shared workflow](README.md).

## Outcome, ownership and entry points

Serve regular files under a controlled document root through the common HTTP
session. Own `include/server/`, `src/server/`, `tests/server/`, `www/` and their
local build definition. Suggested internal pieces are path policy, MIME lookup,
static response preparation and file-body transaction. P0's exact contracts take
precedence over these suggested filenames.

Use P0 target/date/validator utilities, file-work ports and response writer. File
metadata/open/read work must be representable as work for E; do not bury blocking
filesystem calls inside a method G invokes directly on its loop. Application
decisions remain portable server code, even when their file effects run in E.

Inspect `include/http/request.hpp`, `response.hpp`, `header.hpp`, `date.hpp`,
`http_session.hpp`, current serialization tests and P0's factory examples. Keep
method/framing rejection in the shared session; A owns resource-specific results.

## Implementation order

### 1. Resolve paths under a defined root policy

Canonicalize and validate the configured root at startup using a file job if
startup runs on a driver thread. Require a directory and retain its normalized
component representation. Use nonthrowing filesystem overloads for expected
failures and translate errors to portable categories.

For each accepted request:

1. Accept origin-form or use P0 to extract the path/query from supported absolute
   form. Reject malformed targets. Never construct a filesystem path from an
   entire absolute URL.
2. Split the raw query before decoding the path. Decode `%HH` exactly once;
   reject bad/incomplete escapes and decoded NUL/backslash. `+` in a path is a
   literal plus, not form-urlencoded whitespace.
3. Validate decoded segments for escaping traversal and native path hazards.
   Reject drive/UNC/device forms, colon/alternate streams and reserved Windows
   device aliases, including aliases with extensions or trailing dot/space.
   Do not let a decoded separator bypass component validation.
4. Map `/` to `index.html`; resolve relative components against the root, never
   against process working directory. Reject traversal that escapes the root.
5. Check components for symlinks/reparse points before following them; canonical
   resolution alone can conceal the symlink you intended to reject. Compare
   canonical containment by components, not `starts_with(root_string)`.
6. Require a regular file. Do not list directories or serve sockets/FIFOs/devices.

The initial policy assumes a controlled tree. Ordinary canonicalization/open is
not an atomic defense against concurrent replacement of a path component. Keep
that limitation documented. On Windows, generic `is_symlink` may not identify
every reparse point; place any needed native attribute check in the private file
adapter, keeping OS types out of public headers. Define case/normalization rules
explicitly without a locale-sensitive lowercase shortcut.

### 2. Separate preparation from body transfer

Prepare a result containing status, validated metadata, optional opaque file
token and known length. A clean sequence is path validation → open → metadata
consistent with the opened file → response decision. Preserve useful native
diagnostic codes, but do not expose local filesystem paths in response bodies.

Map invalid targets to 400, forbidden traversal/access to 403, missing paths to
404, and unexpected local failure to 500 when a response can still be sent.
Keep 405/Allow handling consistent with the shared GET/HEAD restriction.

Use an explicit extension/MIME table and binary fallback. Generate Date and
Content-Type; known-length framing describes exactly the selected representation.
HEAD performs the metadata/conditional decision for GET but schedules no body
reads. An empty GET still completes normally with length zero.

### 3. Stream under downstream demand

The file transaction can move through prepare, head-ready, reading, writing and
complete/failed states. Keep at most the contract's bounded pending file chunks.
Before submitting a read, reserve payload capacity and limit its size to both
remaining file bytes and available output capacity. Hold a result until the
writer consumes it; do not prefetch an entire file in helper completions.

Track file-read and downstream-body-write offsets separately. Bound new reads by
`advertised_length - bytes_read`; bound completion by
`advertised_length - body_bytes_written`, with checked subtraction. Already-read
but unsent bytes stay in their owned segment and must not be reread. Partial reads
may make progress; EOF before `bytes_read` reaches the advertised length is
truncation. EOF after all expected bytes were read is valid even while some remain
queued for output. Never send bytes beyond the advertised length if a file grows.
A read/open error before the response is committed may produce an error; after
commit, close the connection. Do not append another response or disguise a short
file as successful completion. Serialize operations per file token and close
only after outstanding I/O releases its ownership.

### 4. Add validators and conditional responses

Generate Last-Modified from the selected metadata and a weak ETag from a stable,
documented size/mtime representation. A timestamp/size pair is not a strong
content hash. Preserve the actual timestamp precision used for tag generation
while formatting HTTP dates at their required resolution.

Use P0's validator/date parser. If If-None-Match is present, evaluate it first,
including `*`, repeated/list values and weak comparison for GET/HEAD. Do not fall
back to If-Modified-Since merely because a valid tag does not match. Define
malformed-condition handling consistently and conservatively. Only when the tag
condition is absent should a usable If-Modified-Since drive the time comparison.

A matching condition returns 304 with validator/cache metadata and no body.
Avoid inventing a 304 body length: if representation length is included, it must
describe the corresponding GET. Changed files must change the selected metadata
and response decision; tests should control timestamps rather than sleep.

## Regression matrix

| Area | Required cases and observations |
| --- | --- |
| Targets | Nested files, absolute form, query ignored for lookup, `%20`, literal plus, malformed escape, encoded separators/NUL/backslash |
| Containment | `..`, encoded traversal, double encoding without a second decode, sibling prefix, drive/UNC/device/ADS, symlink/reparse components |
| Resources | Missing/inaccessible file, directory, nonregular file, root misconfiguration; distinguish policy rejection from I/O failure |
| Bodies | Empty/text/binary/large file byte equality; maximum configured limits; HEAD has GET metadata and zero body bytes |
| Flow control | Tiny output window, short writes, would-block, slow reader, bounded outstanding reads and completion bytes |
| Conditions | Wildcard/weak tag/list, matching/nonmatching tag, conflicting tag/date, invalid date, changed file, HEAD/304 |
| Lifetime | Client loss during prepare/read/write, cancelled result, truncated/growing file, repeated cleanup without leaked handles |
| Integration | Several sequential/pipelined requests remain ordered; failed committed response closes; both drivers use the same assertions |

Use real temporary files plus controlled I/O faults. Permission/reparse tests
must report unsupported local capabilities; do not count them as exercised.
Run labels `milestone10`, `milestone11`, `milestone12`, the base/P0 gate, and later
the same cases through L's two drivers.

## Done and handoff

- [ ] Production static factory links from its local CMake without editing main.
- [ ] Minimal-driver real-file acceptance passes with bounded memory.
- [ ] File adapter, path policy, conditional rules and failure/close behavior are
  documented in `docs/branches/static.md` with actual checks.
- [ ] L receives the factory and tests; no socket or driver-specific HTTP path was
  introduced inside the service.
