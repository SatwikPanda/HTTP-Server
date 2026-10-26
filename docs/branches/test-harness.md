# Test-harness handoff (track I, shared fixtures and matrix preparation for milestone 30)

## Integrated implementation — 2026-10-26

The earlier branch preparation record below is historical. `main` now links the
production application runtime, runs all seven matrix combinations, and has no
pending substitutions. All prior fixture entry points remain available.

`system_matrix_test` runs static GET/HEAD/conditions/containment/pipeline ordering,
proxy framing/fields/errors/truncation, cache-only/hit/Age/304/changed-body behavior,
concurrent readers, a replacement barrier while hits continue, and cancellation
with a blocked origin through both F/G. `application_lifecycle_test` separately
checks bounded overload, recovery, repeated startup/stop and failed startup rollback.
The independent wire oracle rejects ambiguous/overflowing lengths and checks full
chunk trailers. Diagnostics are bounded; observed fixture client counts are not
used as production queue/connection metrics.

`test_inventory.hpp` lists all 49 native/script runtime suites. CMake validates
that each was actually registered, so a compiled target alone is insufficient.
J adds three optional CTests when `BUILD_BENCHMARKS=ON`. `run_matrix.cmake` requires
strict mode and selects all milestone labels. CI runs all registered suites.

Windows GCC/Clang/MSVC pass the complete 52-test gate. Platform details are
recorded in [application-assembly.md](application-assembly.md). M still owns final
Linux/macOS/CI qualification; passing Windows alone is not full milestone 30
portability acceptance.

## Historical branch preparation

## Base and prerequisites

- `PARALLEL_BASE`: `c86509b` (`main`, containing P0 `parallel-foundation` and merged track A `static-service`), plus track H (`7857d31`) on this branch.
- Track: **I (`test-harness`)**.
- Scope: Reusable controlled I/O fixtures, multi-request scripted loopback origin, independent wire oracle, driver/mode-parameterized system harness, and explicit required-suite inventory for **milestone 30** preparation.

## Owned files and targets

- `tests/fixtures/parallel_io.hpp`
  - Preserves P0's `test::TemporaryDirectory` and `test::ControlledOrigin` APIs.
  - Extends `TemporaryDirectory` with `path()`, nested `write()`, `make_directory()`, `set_mtime()`, and `try_symlink()`.
  - Adds `test::ScriptedOrigin`, `test::OriginResponseScript`, `test::CapturedOriginRequest`, and `test::ScriptTerminalBehavior` for multi-connection scripted origin responses, named barriers (`wait_for_barrier` / `release_barrier`), bounded request capture, and controlled truncation/drop behavior.
- `tests/fixtures/io_controls.hpp`
  - `test::ControlledByteChannel` (deterministic short reads/writes, periodic would-block injection, and byte-offset error injection).
  - `test::SynchronizedManualSteadyClock` and `test::SynchronizedManualSystemClock` (thread-safe wrappers around manual clocks).
  - `test::fnv1a_64` and `test::inspect_fixed_wire_response` (independent wire-format oracle so tests do not rely solely on `http::ResponseParser`).
- `tests/fixtures/system_runner.hpp`
  - `test::SystemScenarioSpec`, `test::FeatureAvailability`, `test::check_scenario_availability`, `test::ScenarioDiagnostics`, and `test::LoopbackSystemHarness`.
- `tests/fixtures/CMakeLists.txt`, `tests/fixtures/fixture_harness_test.cpp`
  - Registers `fixture_harness_test` (`parallel_foundation;milestone30`).
- `tests/system/test_inventory.hpp`
  - `test::kRuntimeSuiteInventory` (explicit inventory of implemented CTest targets, tracks, and milestone labels) and `test::kProductMatrixInventory` (the application × driver × cache matrix).
- `tests/system/CMakeLists.txt`, `tests/system/system_matrix_test.cpp`, `tests/system/run_matrix.cmake`
  - Registers `system_matrix_test` (`milestone30`) and enforces `require_project_tests(...)` across all 30 implemented runtime suites.

## Usage and production dependencies for L and M

1. **Fixture lifetime rules:** `net::NetworkRuntime` must outlive `ControlledOrigin`, `ScriptedOrigin`, and `LoopbackSystemHarness`. Always call `stop()` and `join()` (or let the RAII destructor run) before inspecting captured origin requests or destroying `NetworkRuntime`.
2. **Parameterized system suite:** `system_matrix_test` executes shared scenario assertions (`run_static_server_scenario`) against available production combinations (`static_files × single` is active now) and reports explicit pending status (`FeatureAvailability::pending_track_*`) for combinations awaiting tracks B (`proxy`), K (`cache-proxy`), F (`thread-pool`), G (`event-loop`), and L (`application-assembly`). No mock session is substituted for an unmerged production track.
3. **Launch script:** Run the full implemented strict gate with:
   ```powershell
   cmake -P tests/system/run_matrix.cmake
   ```

## Executed verification

Configured and built in `build/agents` (Windows, GCC 16.2, Ninja, Release, `BUILD_TESTING=ON`, `REQUIRE_ALL_MILESTONES=ON`):

```powershell
cmake -S . -B build/agents -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DREQUIRE_ALL_MILESTONES=ON
cmake --build build/agents --config Release
ctest --test-dir build/agents -C Release -L "milestone0[1-9]|milestone1[012]|milestone2[89]|milestone30|parallel_foundation" --no-tests=error --output-on-failure
```

- All **30** registered runtime tests pass (`0` failures, `0` skips), including `fixture_harness_test` and `system_matrix_test`.
- IPv4 and IPv6 loopback socket creation/bind/listen succeeded on this Windows host.

## Remaining dependent acceptance

- Enabling the remaining 6 product matrix combinations (`static × thread-pool/event-loop` and `proxy (cache off/on) × thread-pool/event-loop`) in `check_scenario_availability` and `LoopbackSystemHarness` requires merging tracks B, C, D, E, F, G, K, and L (`application-assembly`).
- Full cross-platform Windows/Linux/macOS qualification and soak/sanitizer evidence for milestone 30 belong to **track M (`correctness-gate`)**.
