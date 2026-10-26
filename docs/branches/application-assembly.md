# Application integration record — 2026-10-26

A–I and K are integrated into `main`; J is implemented and the executable now
selects real demo/static/forwarding/cached applications. The integration needed
for I/J supplies L's application composition. M's final cross-platform acceptance
and N's performance results are separate remaining work.

## Included branch heads

| Track | Branch | Integrated head |
| --- | --- | --- |
| A | static-service | f6065f9b2dbea902e39babc971d17076bcc5308b |
| B | proxy | 96ad613ee95fa07f9d78e8ad21b4e49b9f7e9993 |
| C/D | cache-storage-policy | 55c5333223097ee2e8e0e073c46fcfa056882ce9 |
| E | helpers | 5c03859fd0e476ed4f4c922cf46cb0b9fe343b19 |
| F | thread-pool | 845c3ae1f2781840cd3aefb11dda46613b005da0 |
| G | event-loop | eda197b0f251bfc36c085a3ac64259a1f158f4fa |
| H/I | config-observability-test-harness | 021d82a06ef9ff576d94f1efc2c890fb813f1e70 |
| K | cache-proxy | 3bbbadcc5e8245131583e0bf668c92803e86d0e4 |
| J initial checkpoint | benchmark-harness | 460203f |

Main started at `c86509b`. Merge checkpoints are `f2a8339` (B/C/D/K),
`44ed459` (H/I), `aeae0a9` (E/F), `b3c5e44` (G), and `7b65572` (J).
Application/matrix implementation is `ad18a0d`; overload/replacement acceptance
is `141c759`; complete J tooling is `2789bf9`. Later commits record portability
fixes and this handoff. Original feature branch history is preserved.

## Composition and fixes

`application` explicitly links `server`, `proxy`, `cache_proxy`, `observability`,
`concurrency_helpers`, `concurrency_thread_pool`, `event_loop` and their shared
HTTP/core/network dependencies. `Runtime::start` validates limits and constructs
A/B/K factories injected into the same `HttpSession`. H parses the actual CLI.
Port zero is supported for isolated tests and startup reports the bound endpoint.

G can now bind E's `HelperPool` directly. Its owner drains E's bounded completion
port before delivering work results to sessions. Helper rejection callbacks run
outside the completion mutex and metrics snapshots do not nest mutexes, preventing
a lock inversion during saturation. The port closes before poller
teardown and helper threads stop only after the driver joins. Wakes retain the
poller lifetime lock, removing a late-callback destruction race. Accepts continue
under their fairness budget at the session cap, allowing prompt overload rejection
instead of leaving clients in the kernel backlog until a session ends.

The sequential path remains the synchronous reference (including synchronous
file/DNS work). F/G use helpers for blocking work. Queue/buffer/admission bounds
are mapped explicitly. Thread-pool completion capacity reserves one result slot
per worker. Output segments drain completely before another is requested; the
low/high-water config fields remain the reserved queued-output contract.

The normal main control thread exports bounded lifetime metrics, cache/storage
statistics and available H CPU/RSS samples once per second. K's admitted
transaction count is labeled as transactions, not active socket count. Unsupported
fields are null. These snapshots are distinct from benchmark measurement deltas.

OS hooks touch the stop flag; a normal thread requests stop. Drivers drain/close
and join, helpers quiesce and join, then services/network runtime are destroyed.
A synchronous OS DNS call may extend helper shutdown. The benchmark records
Windows forced child termination separately from I's tested graceful lifecycle.

## Executed verification

All builds are Release, strict mode, `BUILD_TESTING=ON`, `BUILD_BENCHMARKS=ON`.
The complete inventory is 52 CTests: 49 native/script suites plus three J tests.
The final source, including the helper-pressure/metrics-lock regression, passes
52/52 with no failures or skips on Windows GCC 16.2 (44.09 s), Clang 22.1.8
(44.32 s), and MSVC 19.29 (40.87 s).

```powershell
cmake -S . -B build/integrated -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DBUILD_BENCHMARKS=ON -DREQUIRE_ALL_MILESTONES=ON
cmake --build build/integrated --parallel 6
ctest --test-dir build/integrated -C Release --no-tests=error --output-on-failure
```

Clang uses `build/integrated-clang` and
`-DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/clang++.exe`. MSVC uses
`build/integrated-msvc`, `-G "Visual Studio 16 2019" -A x64`, and
`cmake --build ... --config Release -- /m:2 /nr:false`.

I's seven combinations have no pending substitutions. The independent oracle
checks emitted bytes; lifecycle tests cover overload/recovery, idempotent stop,
occupied-port/root failures and repeated startup. Module suites retain their more
detailed policy/deadline/memory-accounting checks. J runs all 11 non-demo families
on both drivers using real subprocesses and local origins. Extra GCC smoke runs
passed for 16 MiB cached objects on both drivers and open-loop/new connections
with periodic metrics. All are smoke evidence, not performance comparisons.

MSVC exposed two fixture portability issues: `<future>` imports a Microsoft
`concurrency` alias conflicting with this project's namespace, and a narrow
UTF-8 filename was interpreted using the Windows code page. Tests now use joined
`jthread`s and an explicit UTF-8 filesystem path. Assertions were preserved.

Linux/macOS execution, remote CI and race diagnostics for the combined tree are
not claimed here. The CI matrix now enables benchmarks and selects every CTest.
Windows symlink tests report unavailable privileges separately when applicable.
See [test harness](test-harness.md), [benchmark harness](benchmark-harness.md),
and the root [README](../../README.md).
