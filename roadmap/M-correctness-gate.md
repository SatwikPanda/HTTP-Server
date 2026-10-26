# M — correctness-gate

**Status:** implemented; 54-suite native Windows/Linux/macOS qualification and Linux ASan/UBSan verified at `22e5592`. Race diagnostics remain untested. **Branch:** `correctness-gate`. **Milestone:** 30.
**Start:** assembled [L](L-application-assembly.md) and
[I](I-test-harness.md)'s complete runtime inventory.
Follow the [shared workflow](README.md).

Implementation and execution evidence: [M handoff](../docs/branches/correctness-gate.md)
and [validation summary](../docs/validation/lmn.md).

## Outcome and ownership

Qualify the actual combined application on Windows, Linux and macOS before
performance conclusions. After L's freeze, take ownership of CI and the validation
record. Module fixes remain coordinated with their owners. Do not qualify a
different tree from the one eventually measured or merged.

Read `.github/workflows/cmake.yml`, CMake, test inventory and every track's handoff.
The planning snapshot's Windows 01–08 evidence does not establish new feature or
platform acceptance. Preserve historical results with their commit/date and add
new evidence separately.

## Execution plan

### 1. Freeze and inventory the candidate

Record commit SHA, whether the tree is dirty, included feature commits, toolchains,
build configurations and runtime/test-only dependencies. Require an explicit
list of runtime suites, milestone labels and mode/platform combinations. Missing
feature suites, empty CTest selections and exit 77 remain open acceptance items.

Compile-only contract targets establish signatures; they cannot replace tests
that run production code. Check CMake source membership so legacy or header-only
scaffolding cannot be mistaken for linked functionality.

### 2. Run clean native platform builds

Use fresh or generator-compatible directories and the documented CMake build/test
workflow with strict milestones. Preserve Windows Winsock and UNIX POSIX source
selection and compiler warnings. Test Release with active assertions; add useful
Debug/diagnostic configurations without substituting them for release acceptance.

For each Windows/Linux/macOS platform, execute static and proxy under both
thread-pool and event-loop, with cache off/on where applicable. Record actual OS,
compiler/version, build type, generator, test counts and commands. IPv4 must be
exercised; attempt IPv6 and report host limitations explicitly.

The final complete tree can run:

```powershell
cmake -S . -B build/agents -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DREQUIRE_ALL_MILESTONES=ON
cmake --build build/agents --config Release
ctest --test-dir build/agents -C Release -N
ctest --test-dir build/agents -C Release --output-on-failure
```

Verify the expected inventory against the listing. Adapt generator/build paths
to the native toolchain; do not reuse a directory created by another generator.
Keep CI selection synchronized with required merged suites, including P0.

### 3. Exercise failures and resource lifetime

Run all parser split/EOF/ambiguous-framing cases, persistence/pipelining,
containment, proxy fields/framing, cache-only/freshness/validation, version races,
overload and cancellation/shutdown. Add mixed-load soak with bounded duration,
controlled origin changes and periodic quiescence to inspect resources.

Track handles/file descriptors, thread counts, active sockets, queue depths,
cache/pin/candidate reservations and process memory. Stable retained allocator
memory alone is not proof of a leak; compare ownership/accounting and repeated
cycles, then investigate sustained unexplained growth.

Use available memory, undefined-behavior and race tooling on supported platforms.
Run diagnostics in suitable dedicated builds; a platform/tool that cannot execute
is untested, not passed. Avoid broad suppressions hiding a real race or lifetime
failure. Preserve reproducible inputs and bounded logs for every defect.

### 4. Resolve defects and rerun affected gates

Classify failures as application behavior, portability, test fixture, environment
capability or nondeterministic scheduling. A flaky result is unresolved evidence;
retries may diagnose it but do not erase the initial failure. Reproduce with
controlled barriers/clocks where possible.

Land fixes through the responsible owner and incorporate them into the candidate.
Rerun affected unit/integration/platform gates and any cross-cutting base checks.
Freeze a new qualified SHA after changes; N must not silently benchmark the old
binary under the new report identifier.

## Validation record and exit

Each record includes commit/tree, date, OS/compiler, exact configure/build/test
commands, selected/expected counts, failures/skips, IPv6/diagnostic availability,
resource observations and remaining limitations. Link CI artifacts when real runs
exist; do not infer CI success from a configuration file being present.

- [x] Strict required suites pass on Windows, Linux and macOS for every production
  mode/driver combination, with zero required pending feature suites.
- [x] Documented build/run commands work from a clean checkout without ignored
  local files or undeclared runtime dependencies.
- [x] Resource soak and supported diagnostics have recorded outcomes; unresolved
  correctness failures block qualification.
- [x] `docs/branches/correctness-gate.md` identifies the exact qualified commit and
  evidence for [N](N-results.md). An unavailable required platform keeps M open.
