# Static file service handoff (track A, milestones 10-12)

Branch `static-service`, based on the earlier static branch. Production entry point:
`server::make_static_factory(const StaticConfig&)` in `include/server/static_service.hpp`.
The `server` library is built from `src/server/CMakeLists.txt`; the root `main`
is not edited.

## Behavior

- **Targets.** Origin-form and absolute-form targets are accepted; the query is
  split off before the path is decoded, `%HH` is decoded once, `+` stays literal.
  Bad escapes, decoded NUL and backslash are 400. `/` maps to the configured index file.
- **Containment.** Traversal, drive/UNC/device/alternate-stream forms and reserved
  Windows device names are 403. Symlink components are checked before they are
  followed, and containment compares path components, not string prefixes.
  Directories and non-regular files are never served or listed.
- **Status mapping.** 400 invalid target, 403 policy or permission, 404 missing,
  405 with `Allow: GET, HEAD`, 500 unexpected local failure, 503 file-job timeout.
  Error bodies never include local paths or native diagnostics.
- **Metadata.** Content-Type comes from an explicit extension table with an
  `application/octet-stream` fallback. Length comes from the opened file.
  Last-Modified and a weak ETag (size and mtime) are generated; the ETag is not a
  content hash.
- **Conditionals.** If-None-Match is evaluated first (wildcard, lists, weak
  comparison). If-Modified-Since is used only when If-None-Match is absent. A match
  gives 304 with validators and no body or length. HEAD takes the same decision as
  GET and schedules no reads.
- **Streaming.** The transaction performs no I/O itself. Open, read and close are
  file jobs, so a driver can run them on a helper. Each read is capped by the
  output window, the remaining advertised length and `max_chunk_bytes`. Read and
  written offsets are tracked separately. A file that grows is never sent beyond the
  advertised length. EOF before that length, or any read error or timeout after the
  head was committed, closes the connection without a second response.

## Known limits

- Path checks are not atomic against concurrent replacement of a path component.
- Permission-denied cases need a non-root user. When run as root they print
  `UNSUPPORTED` and are not counted as exercised.
- Tests use the manual single-session driver. The same cases through the thread-pool
  and event-loop drivers belong to track L.

## Checks executed

Linux (GCC 13.3, Release, Ninja), `-DREQUIRE_ALL_MILESTONES=ON`:

    ctest -L 'milestone0[123456789]|milestone1[012]|parallel_foundation' --no-tests=error

Windows and macOS results for milestones 10-12 are pending; the CI workflow does
not yet select the `milestone10`-`milestone12` labels.
