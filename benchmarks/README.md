# Reproducible benchmark harness (J / milestone 31)

Python 3.10+ and a Release build of `httpserver` are required. The harness uses Python's standard library; optional `psutil` provides additional external process samples. No Python dependency is linked into the C++ server. Run from the repository root.

## Commands

Build and smoke every scenario family on both drivers:

```powershell
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DBUILD_BENCHMARKS=ON -DREQUIRE_ALL_MILESTONES=ON
cmake --build build/release --parallel
ctest --test-dir build/release -C Release -L milestone31 --no-tests=error --output-on-failure
```

Run one short smoke and inspect the practical matrix (excluded points retain their reasons):

```powershell
python benchmarks/benchmark.py run --binary build/release/httpserver.exe --build-dir build/release --scenario proxy-warm --driver event-loop --smoke
python benchmarks/benchmark.py plan --max-connections 100
python benchmarks/benchmark.py validate-result --result benchmarks/results/RUN_ID/result.json
```

For Linux/macOS omit `.exe`; for Visual Studio use `build/release/Release/httpserver.exe`. The default profile is `profiles/assembled.json`, whose options match the executable's H parser. `profiles/demo.json` supports the earlier demo-only executable. Unsupported selectors/options fail before measurement. `--build` builds the executable first, including when it does not exist yet.

After M has qualified a clean committed tree, a selected full measurement is:

```powershell
python benchmarks/benchmark.py run --binary build/release/httpserver.exe --build-dir build/release --scenario hot-file --driver thread-pool --workers 4 --concurrency 10 --object-bytes 65536 --warmup 10 --measurement 60 --repeats 5 --qualified-commit FULL_M_VERIFIED_COMMIT_SHA
```

The qualification flag is an operator attestation, not automatic CI certification. Full runs require at least five repeats, a clean tree, matching commit and identifiable Release/compiler configuration. No final performance measurements are supplied by this change.

## Workload format and matrix

`scenarios.json` is version 1. It defines axes, seeded defaults and named families. Each family fixes application, allowed drivers, object count/distribution, method, HTTP cache preparation and origin/client throttling. A resolved result additionally records clients, workers, bytes per object, reuse, arrival model/rate, queue limit, timeouts, filesystem-cache label and every applied server setting.

Families cover hot/varied files, HEAD, proxy cache off/cold/warm/revalidation/mixed, delayed/slow origins and slow receiving clients. The plan uses anchor sweeps rather than a full Cartesian product: client counts 1/10/50/100/500/1000, object sizes 1 KiB/64 KiB/1 MiB/16 MiB, pool workers 1/2/4/8/16, plus new-connection variants. Default host limits exclude more than 100 clients or 256 MiB of generated files. Increase them explicitly only when the measured host supports the load. File descriptor checks can exclude a run before startup.

Payload bytes and object choices derive from the recorded seed and sequence. Both drivers receive the same bounded settings and the same one-second diagnostic cadence. Thread-pool worker count limits simultaneously served persistent clients; queued clients can time out at higher client/worker ratios. Errors remain errors rather than successful throughput.

## Preparation and lifetime

Each repeat owns a unique temporary directory, local origin (if needed) and server process. It binds loopback port zero, captures the announced endpoint and checks TCP readiness without a cache-populating GET. A HEAD probe validates mode/body metadata. Only owned process handles and temporary directories are stopped/removed. Bounded diagnostics and process IDs are saved even for failed or interrupted repeats.

Cache runs restart the server after warmup. Cold means empty at measurement start; it naturally warms during the interval. A cache-only absence probe verifies that state. Warm runs prefill every selected object and verify cache-only hits without origin traffic. Revalidation runs verify conditional 304 traffic before measurement. Concurrent 304/version conflicts may cause K's one unconditional retry; the check enforces at most one retry per completed request and records actual 200/304 origin counts. Mixed runs prefill only the hottest object.

The OS filesystem cache is separate. Files are written immediately before warmup and the harness never flushes the OS cache. `--filesystem-cache` records the operator's label; `cold-external` is only an external preparation claim and is not established by this harness. Prefer the truthful default `uncontrolled`.

Windows hidden children are terminated via their owned handles and the record marks forced shutdown. POSIX uses termination signals with a bounded kill fallback. Graceful driver/helper teardown is tested separately by I's in-process lifecycle suite. Cancellation closes clients, stops the owned server/origin, and saves an interrupted record. Uninterruptible OS DNS can still extend a graceful server join; controlled benchmark origins use numeric loopback addresses.

## Measurement and raw result contract

Every run writes `OUTPUT/UUID/result.json` once. Version-1 records contain:

| Field | Meaning |
| --- | --- |
| `run_id`, `repeat`, `evidence`, `state` | Identity, ordinal, smoke/measurement and complete/failed/interrupted |
| `scenario`, `profile`, `preparation` | Fully resolved workload, exact option mapping and verified cache preparation |
| `provenance` | Commit/tree/dirty state, tracked diff hash, binary hash/path, CMake compiler/build metadata, command invocation |
| `environment`, `processes`, `origin` | OS/CPU/RAM/limits when available, owned PIDs/commands/endpoints, bounded logs and placement |
| `phases` | Separate warmup/measurement counters, timing, histograms, generator CPU and queue peaks |
| `origin_measurement` | Origin request/200/304/error/rejection deltas over the measured phase |
| `server_process_*`, `server_metrics` | Optional CPU/RSS samples and latest periodic server snapshot with explicit scope |
| `diagnostics`, timestamps | Bounded failure information and repeat lifetime |

`validate-result` checks required structure, numeric ranges, accounting identities, nonnegative histogram buckets and ordered bounds. A complete result requires nonempty successful measurement and no request errors. Missing platform measurements are `null` or explicitly unavailable, never invented zeros. Periodic server counters include setup/warmup and are not exact interval deltas; very short smoke runs may have no periodic sample. External CPU/RSS sampling uses optional psutil; the server's own H sampler also supplies OS CPU/RSS in its periodic snapshot.

Closed-loop clients issue the next request after completion. `--arrival open --rate 1000 --queue-limit 128` schedules arrivals independently; it records queue delay, delayed issue, expired queued requests and overload drops. Client latency runs from scheduled issue to full validated completion, including connection setup and generator queue delay. Reported throughput divides completed requests/useful bytes by actual phase duration including tail drain. The requested issue window and tail time are recorded separately.

The client checks status, full representation bytes, length and HEAD/no-body semantics. Wrong bodies, truncation, transport failures, timeouts and dropped arrivals never count as useful throughput. Wire bytes and useful representation bytes are separate. Histograms use eight buckets per power of two, integer microsecond upper bounds and an overflow bucket; merge only compatible raw buckets, never average p99 values.

The generator and origin share one Python process and host with the server. Their CPU use and connection limits can constrain the result. Saturation indicators flag CPU pressure, delayed scheduling and dropped arrivals; smoke data does not justify driver performance comparisons. N owns final qualified runs and reporting.
