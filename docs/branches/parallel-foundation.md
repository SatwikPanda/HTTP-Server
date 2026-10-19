# Parallel foundation handoff — 2026-10-20

Implementation and local acceptance are complete. Changes are intentionally
uncommitted; no branch, tag, commit or remote publication was created.

## Base identity

- `BASE_09`: `d644e68de34f36e9d4628060de8efbe2ab4026cc` (`fixing milestone9`).
  Its original strict Windows GCC gate passed 18/18 before P0 edits.
- `PARALLEL_BASE`: the reviewed working-tree changes delivered with this handoff,
  based on `BASE_09`, including the milestone 09 corrections below. It has no
  commit SHA yet. Once these changes are committed, record that resulting SHA
  here and start A–J from it. Do not branch from `BASE_09` expecting P0 APIs.
- There were no unmerged Git index entries. Existing `.gitignore`, architecture,
  master parallel-roadmap and per-track guide changes were preserved.

## Delivered behavior

- Milestone 09: buffered pipelines survive a read EOF during output; incomplete
  trailing requests close with EOF after the completed response. Stale timers
  recheck the current progress deadline. Abstract-channel stop delivers StopEvent.
  Pipeline wire-comparison tests use an injected fixed Date. The demo executable
  now enables persistent connections.
- Shared transaction factory, one-request-at-a-time HTTP session, typed identities
  and work actions, common streaming writer and commit/failure rules. Legacy
  demo/echo entry points and aggregate event initialization remain supported.
- Streaming response parsing shares framing/state logic with the buffered API,
  supports backpressure and counts total payload separately from live windows.
- Reference executor handles DNS, candidate connects, upstream read/write/wait/
  close and file info/open/read/close. Real file and fixed-origin applications
  exercise it through the same HttpSession and SingleSessionDriver as the demo.
- Shared authority/target, date and validator utilities; cache value/storage/
  policy contracts; actual RAII memory reservations; typed runtime config,
  validation, no-op metrics and lifecycle/session-factory contracts.
- Optional module CMake paths, a reusable test registrar, explicit required-test
  inventory, controlled local origin and temporary-file fixtures. CI selects
  P0 alongside milestones 01–09 and fails on empty test selection.

Exact APIs, ownership, limits, states and module paths are in
[`../parallel_contracts.md`](../parallel_contracts.md). New production sources
belong to existing `core`, `concurrency` and `http` targets; no empty future module
or dependency cycle was introduced. Legacy cache scaffolding stays unused.

## Executed verification

All builds used C++20, Release, `BUILD_TESTING=ON`, and
`REQUIRE_ALL_MILESTONES=ON`. Each compiler passed the complete **20-test** runtime
gate (18 earlier tests plus two P0 suites), with zero skips and zero failures:

| Platform/compiler | Build directory | Full gate time |
| --- | --- | --- |
| Windows, GCC 16.2 | `build/agents` | 12.17 s |
| Windows, Clang 22.1.8 with libstdc++ | `build/agents-clang` | 12.22 s |
| Windows, MSVC 19.29 / VS 2019 | `build/agents-msvc` | 11.60 s |

Commands used for the primary build:

```powershell
cmake -S . -B build/agents -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DREQUIRE_ALL_MILESTONES=ON
cmake --build build/agents --config Release
ctest --test-dir build/agents -C Release -L 'milestone0[1-9]|parallel_foundation' --no-tests=error --output-on-failure
```

Clang configuration additionally set
`-DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/clang++.exe`; MSVC reused its existing
Visual Studio 16 2019 generator. The same build/test commands used each compiler's
directory. MSVC's intentional `/UNDEBUG` test option produces the pre-existing
D9025 override warning, keeping assertions active in Release.
The final MSVC rebuild used `-- /m:1 /nr:false`; a stalled parallel rebuild was
cancelled before this successful serial build and full gate.

The P0 runtime tests cover framing modes, informational heads, all input window
sizes for a chunked/trailer message, 128 KiB payloads in 13/17-byte receive windows,
zero-capacity pause/resume, malformed/truncated input, writer partial writes,
HEAD/204/304, aborted chunk output, target/date/tag parsing, budget rollback and
shared-reader lifetime. Work tests cover real files, directory/missing metadata,
closed handles, admission failure, timeout/cancellation, stale completion IDs,
fresh-socket candidate fallback, would-block/readiness, upstream EOF, ordered
GET/HEAD pipelines, metrics, HTTP/1.0 fallback, pre/post-commit origin failures,
and real downstream plus upstream sockets. Session destruction cancels retained
work even without an explicit stop. Only clocks and I/O are controlled.

The optional symlink fixture was unavailable locally: GCC/Clang report creation
unimplemented, and MSVC reports missing Windows privilege. Link/reparse rejection
code builds, but that runtime subcase remains unverified here. This is reported
coverage, not a passed check or a CTest skip.

The initial sandbox denied loopback socket creation with Windows error 10013;
socket suites were then executed outside that restriction. A two-second negative
connect test deadline expired on Windows; its shared deadline was corrected to
five seconds, matching the platform's refusal timing without changing production
retry semantics. MSVC's file clock uses `to_utc`/`clock_cast`, while libstdc++ uses
`to_sys`; both paths built and passed the real file tests.

`git diff --check`, an empty unmerged-index check, Markdown link/trackability
checks, and AGENTS/CLAUDE equality are part of the documentation handoff check.

## Scope and remaining platform evidence

Linux/macOS execution and new remote CI results are pending; Windows results do
not establish those platforms. The CI matrix remains Windows/Linux/macOS with the
expanded strict gate. Existing earlier platform evidence remains in the original
roadmap.

The synchronous reference cannot interrupt blocking DNS/file calls; E implements
bounded asynchronous helpers and G must use them. File inspection/open assumes a
controlled tree, with no atomic containment guarantee during concurrent changes.
Static routing/conditional requests, general proxy filtering, cache storage/policy,
async drivers, CLI/logging implementation and final composition remain in A–N.
These are planned tracks, not missing P0 behavior. P0's fixed resource factories
and no-op metrics are working reference implementations, not production feature
completion claims for those tracks.
