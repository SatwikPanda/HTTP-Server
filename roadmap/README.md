# Detailed implementation guides

These guides expand [the parallel roadmap](../implementation_roadmap2.md).
They preserve the original milestone 10–32 requirements while explaining how
each owner should implement, integrate and verify its portion of the project.
P0, C and D are implemented and locally verified on Windows; other tracks remain
planned. C/D share `codex/cache-storage-policy`; see their
[storage](../docs/branches/cache-storage.md) and [policy](../docs/branches/cache-policy.md)
handoffs for scope and remaining platform/integration acceptance. See the
[contract reference](../docs/parallel_contracts.md) and
[P0 handoff](../docs/branches/parallel-foundation.md) for exact APIs and evidence.

## Start here

1. Read [AGENTS.md](../AGENTS.md), [the original roadmap](../IMPLEMENTATION_ROADMAP.md),
   [test instructions](../tests/README.md), and [CMakeLists.txt](../CMakeLists.txt).
   Read `PROJECT_ARCHITECTURE.md` if present. It may be absent from another
   checkout until tracked; these guides retain the relevant design.
2. Use the verified milestone 09 base and P0 implementation recorded in the
   [handoff](../docs/branches/parallel-foundation.md).
3. The C/D `PARALLEL_BASE` is `4547b48687d70b4d2d54370f01b6dfa946b80181`,
   containing committed [P0](P0-parallel-foundation.md) and its macOS fixture fixes.
   Start independent feature work from a recorded compatible P0 commit.
4. Read the guide for the assigned track, its linked dependencies, and the actual
   production code. Shared filenames/types are fixed in
   [docs/parallel_contracts.md](../docs/parallel_contracts.md).
5. Develop K after B/C/D. Assemble in L continuously, then qualify the combined
   application in M and produce measurements/documentation in N.

## Track directory

| Track and guide | Branch | Scope | Development prerequisite |
| --- | --- | --- | --- |
| [P0 — parallel-foundation](P0-parallel-foundation.md) | `codex/parallel-foundation` | Shared executable contracts | Milestone 09 |
| [A — static](A-static.md) | `codex/static` | Milestones 10–12 | P0 |
| [B — proxy](B-proxy.md) | `codex/proxy` | Milestones 13–16 | P0 |
| [C — cache-storage](C-cache-storage.md) | `codex/cache-storage` | 17, storage part of 24 | P0 |
| [D — cache-policy](D-cache-policy.md) | `codex/cache-policy` | 18–19, pure decisions for 21 | P0 |
| [E — helpers](E-helpers.md) | `codex/helpers` | 22 | P0 |
| [F — thread-pool](F-thread-pool.md) | `codex/thread-pool` | 23, driver portions of 27–28 | P0 |
| [G — event-loop](G-event-loop.md) | `codex/event-loop` | 25–26, driver portions of 27–28 | P0 |
| [H — config-observability](H-config-observability.md) | `codex/config-observability` | 29, stop hooks for 28 | P0 |
| [I — test-harness](I-test-harness.md) | `codex/test-harness` | Fixtures and matrix preparation for 30 | P0 |
| [J — benchmark-harness](J-benchmark-harness.md) | `codex/benchmark-harness` | Workload/tooling preparation for 31 | P0 |
| [K — cache-proxy](K-cache-proxy.md) | `codex/cache-proxy` | 20–21, integration part of 24 | B + C + D |
| [L — application-assembly](L-application-assembly.md) | `codex/application-assembly` | 24, 27–28 and combined application acceptance | Incrementally after P0; finish after A–I + K |
| [M — correctness-gate](M-correctness-gate.md) | `codex/correctness-gate` | 30 | L + I |
| [N — results](N-results.md) | `codex/results` | Finish 31 and implement 32 | L + J for smoke runs; M for measurements |

Development dependencies and final acceptance dependencies differ. F/G can be
built against P0 interfaces, but their milestone acceptance requires E and real
A/B/K services in L. I prepares tests; M records full platform acceptance. J
prepares measurements; N runs the qualified application and reports evidence.

## Architecture rules shared by every track

- Runtime dependencies remain C++20 standard library and OS APIs. Keep the
  cleartext, bodyless GET/HEAD application scope and all deferred features from
  the main roadmap. Preserve the existing parser/framing restrictions.
- HTTP/application/cache decisions belong in shared code. Drivers execute
  actions; helpers perform file/DNS I/O; L composes factories and owns process
  lifetime. A different driver must not change the response bytes or policy.
- Use `core::Result`, portable errors and existing read/write outcome types.
  Preserve move-only resource ownership, explicit borrowing lifetimes, short
  writes, would-block, EOF and error distinctions.
- One thread owns a socket and its poller registration. Only `Poller::wake()` is
  concurrently callable. Unwatch before close; validate registration and
  connection/transaction generations before applying saved events.
- Bound input, output, queues, pending work, cache allocations and retries.
  Admission must fail before allocation exceeds its budget. Backpressure must
  have an explicit resume condition, and overload must recover.
- Use steady time for deadlines/elapsed durations and wall time for HTTP dates.
  Synchronous OS resolution can outlive a transaction deadline. Document that
  limitation when claiming shutdown guarantees.
- Keep native networking details in the existing platform directories. Keep
  file/platform sampling/signal implementation details behind private adapters.
  No OS handles leak into public HTTP/cache contracts.

## Build integration and ownership

P0 sets explicit optional module-local CMake inclusion points. Each track adds
its implementation and local tests together; it does not edit every other
module, root main, or shared contract to make its own branch compile. Drivers
depend on session interfaces, avoiding a cycle with the current `http` target,
which already links to `concurrency`.

Create no empty production targets. Link new targets explicitly; maintain C++20,
disabled extensions, warning options, `Threads::Threads`, the selected native
backend and Windows `ws2_32`. A present module with an absent required dependency
must report a clear configuration error.

L owns final root CMake/main/CI wiring and shared usage documentation. Contract
changes go through P0's owner, then into every affected branch. I owns shared
fixtures after P0 seeds them; module authors own their feature cases. Coordinate
fixture additions as explicit dependencies instead of silently assuming they
exist. Read the ownership handoffs in M/N before editing their shared files.

## Verification workflow

Use a distinct build directory per worktree and preserve its generator. After
milestone 09/P0 actually exist, run this from the repository root:

```powershell
cmake -S . -B build/agents -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DREQUIRE_ALL_MILESTONES=ON
cmake --build build/agents --config Release
ctest --test-dir build/agents -C Release -N
ctest --test-dir build/agents -C Release -L '^(parallel_foundation|milestone0[1-9])$' --output-on-failure
```

Each guide specifies its additional labels. Run them with `ctest -L` in that
strict configuration and verify that the expected tests were selected. Missing
tests cannot pass by returning an empty selection. P0/I maintain an explicit
acceptance inventory; exit 77 is pending, and contract-object compilation is
never runtime acceptance. Full strict CTest runs become the gate when all
registered required features are implemented.

Use Release-active `CHECK`, bounded test timeouts, controlled clocks and I/O,
plus real loopback sockets/files. Only clocks and I/O may be faked. Application
decisions, cache logic and driver scheduling must execute production code.
Keep networking local; report unavailable IPv6 or diagnostic-tool coverage.

## Implementation handoff

Each track supplies `docs/branches/<canonical-name>.md` with its base/prerequisite
SHAs, owned files, public factories/targets, actual test commands/results,
platform coverage, memory/deadline/lifetime guarantees and remaining dependent
acceptance. Guides in this folder are instructions; handoff files are evidence.

Before handing over a branch, combine it with current L and run affected gates.
The feature owner resolves integration defects. M verifies the exact assembled
commit; N measures that qualified code. Commit/push/publish only when requested.

Final order: **09 → P0 → A–J in parallel → K after B/C/D → completed L → M → N**.
The delivered assembly and evidence branches include all final wiring, so the
merger receives a completed implementation rather than unfinished glue work.
