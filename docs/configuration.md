# Configuration, Error Handling, and Observability Reference

This document defines the validated command-line options, runtime defaults, metric semantics, bounded logging policy, and OS stop-hook guarantees implemented by track H (`config-observability`, milestone 29 and stop hooks for milestone 28).

## 1. Command-line options and shared defaults

All configuration defaults are owned in one place: `core::RuntimeConfig` (`include/core/config.hpp`). Argument parsing (`core::parse_cli`) validates syntax and ranges without opening sockets, accessing the filesystem, or starting worker threads. `--help` / `-h` returns the formatted help text immediately.

| Option | Values / Range | Default | Notes |
| --- | --- | --- | --- |
| `--mode` | `demo`, `server`, `proxy` | `demo` | Maps to `ApplicationMode::{demo, static_files, proxy}` |
| `--concurrency` | `single`, `thread-pool`, `event-loop` | `single` | Maps to `DriverMode::{single, thread_pool, event_loop}` |
| `--bind`, `--host` | Non-empty IP/host string (1–255 bytes) | `127.0.0.1` | Loopback by default; remote load tests require explicit `--bind` |
| `--port` | `1`–`65535` | `8080` | Full unsigned integer parsing; `0`, signs, overflow, or trailing junk rejected |
| `--root` | Filesystem path | `.` | Only valid when `--mode server`; directory existence is validated at startup via `validate_document_root()` |
| `--workers` | `1`–`1000000` | `4` | Thread-pool session workers |
| `--helpers` | `1`–`1000000` | `2` | Async file/DNS helper threads |
| `--max-sessions` | `1`–`1000000` | `1024` | Max concurrent downstream sessions |
| `--max-upstreams` | `1`–`1000000` | `256` | Max concurrent upstream proxy sockets |
| `--accept-queue` | `1`–`1000000` | `128` | Max queued accepted sockets |
| `--job-queue` | `1`–`1000000` | `256` | Max queued helper jobs |
| `--completion-queue` | `1`–`1000000` | `256` | Max queued helper completions |
| `--input-bytes` | `4`–`16777216` | `16384` | Per-connection input buffer limit |
| `--output-bytes` | `128`–`16777216` | `65536` | Per-connection output buffer window |
| `--low-water-bytes` | `1` to `< high_water` | `16384` | Output buffer resume threshold |
| `--high-water-bytes` | `> low_water` to `<= output` | `49152` | Output buffer pause threshold |
| `--header-limit`, `--buffer-size` | `4`–`16777216` (`<= input-bytes`) | `8192` | Request header byte limit; `--buffer-size` preserved for CLI migration compatibility |
| `--max-requests-per-connection` | `1`–`1000000` | `100` | Keep-alive transaction cap per connection |
| `--idle-timeout-ms` | `1`–`86400000` (24 h) | `5000` | Idle persistent connection timeout |
| `--progress-timeout-ms` | `1`–`86400000` (24 h) | `5000` | Request/output progress timeout |
| `--connect-timeout-ms` | `1`–`86400000` (24 h) | `5000` | Outbound upstream connect deadline |
| `--work-timeout-ms` | `1`–`86400000` (24 h) | `5000` | File/DNS helper work deadline |
| `--drain-timeout-ms` | `0`–`86400000` (24 h) | `2000` | Graceful shutdown drain deadline (`0` closes immediately) |
| `--cache` | `on`, `off` | `off` | Only valid with `--mode proxy` |
| `--cache-entries` | `1`–`1000000` | `1024` | Explicit cache tuning requires `--mode proxy` |
| `--cache-object-bytes` | `1` to `<= resident_bytes` | `1048576` | Max single cached representation size |
| `--cache-resident-bytes` | `>= object_bytes` to `<= total_bytes` | `16777216` | Max resident LRU bytes |
| `--cache-total-bytes` | `>= resident_bytes` | `33554432` | Total cache + in-flight candidate/pinned budget |

### Validation invariants

- `0 < low_water < high_water <= output <= 16 MiB`
- `4 <= header_limit <= input <= 16 MiB` (if `--header-limit`/`--buffer-size` is raised above the default `16384` without an explicit `--input-bytes`, `input` widens automatically to match)
- `0 < object_bytes <= resident_bytes <= total_bytes`
- Conflicting mode-specific options (`--root` outside `--mode server`, or `--cache on` / `--cache-*` outside `--mode proxy`) are rejected with `ErrorCategory::invalid_argument`.

## 2. Startup and build commands

### Ninja single-config build (Windows / Linux / macOS)

```powershell
cmake -S . -B build/agents -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DREQUIRE_ALL_MILESTONES=ON
cmake --build build/agents --config Release
ctest --test-dir build/agents -C Release -L "milestone0[1-9]|milestone1[012]|milestone2[89]|parallel_foundation" --no-tests=error --output-on-failure
```

Executable path: `./build/agents/httpserver` (or `.\build\agents\httpserver.exe` on Windows).

### MSVC multi-config / Visual Studio build (Windows)

When using MSVC with Ninja on Windows, launch from a **Visual Studio Developer PowerShell / x64 Native Tools Command Prompt** so `cl.exe`, `link.exe`, and the Windows SDK headers/libraries are in `PATH`, `INCLUDE`, and `LIB`. With the Visual Studio multi-config generator:

```powershell
cmake -S . -B build/msvc -DBUILD_TESTING=ON -DREQUIRE_ALL_MILESTONES=ON
cmake --build build/msvc --config Release
ctest --test-dir build/msvc -C Release --output-on-failure
```

Multi-config executable path: `.\build\msvc\Release\httpserver.exe`.

### Documented startup examples (for assembly in track L)

```powershell
# Static file server on loopback with thread-pool driver
./build/agents/httpserver --mode server --concurrency thread-pool --bind 127.0.0.1 --port 8080 --root www --workers 4 --helpers 2

# Static file server with event-loop driver
./build/agents/httpserver --mode server --concurrency event-loop --bind 127.0.0.1 --port 8080 --root www --helpers 2

# Forward proxy with caching disabled (benchmark baseline)
./build/agents/httpserver --mode proxy --concurrency thread-pool --bind 127.0.0.1 --port 8081 --cache off

# Forward proxy with bounded shared cache enabled
./build/agents/httpserver --mode proxy --concurrency event-loop --bind 127.0.0.1 --port 8081 --cache on --cache-object-bytes 1048576 --cache-resident-bytes 16777216 --cache-total-bytes 33554432
```

## 3. Metric definitions and lifecycle boundaries

`observability::MetricsRegistry` (`include/observability/metrics_registry.hpp`) implements `core::MetricsSink` using lock-free atomics and fixed dimensions (no arbitrary URL/header labels).

| Metric group | Counter / Gauge / Histogram | Lifecycle recording boundary and unit |
| --- | --- | --- |
| Requests | `requests_accepted`, `requests_started`, `responses_completed`, `request_errors`, `client_cancellations` | `requests_started` increments once when a parsed or rejected request is dispatched (`HttpSession::prepare_response`). `responses_completed` increments only after the entire response finishes draining (`HttpSession::finish_response`). Partial responses never increment `responses_completed`. |
| Status codes | `status_200`, `status_304`, `status_400`, `status_403`, `status_404`, `status_405`, `status_408`, `status_413`, `status_414`, `status_417`, `status_431`, `status_500`, `status_501`, `status_502`, `status_503`, `status_504`, `status_505`, `status_other` | Recorded once per emitted response head. |
| Transfer | `wire_bytes_attempted`, `wire_bytes_completed`, `body_bytes_completed` | `wire_bytes_completed` counts acknowledged socket write bytes (`WriteCompleteEvent`). `body_bytes_completed` counts useful representation payload bytes of completed responses (excluding `HEAD`, `204`, and `304`). |
| Time | `LatencyKind::{request, queue, origin}` | Monotonic duration converted to microseconds across 12 fixed buckets (`<=100us` up to `<=2000000us` plus overflow), tracking total count, sum, max, and upper-bound percentiles (`p50`, `p95`, `p99`). |
| Capacity | `Gauge::{active_sessions, queued_accepts, active_upstreams, connecting_upstreams, queued_jobs, queued_completions, paused_producers, cache_entries}` plus `work_rejected`, `work_cancelled`, `accept_rejected`, `upstream_rejected`, `timeouts_*` | Current and high-water peak gauges; rejection/cancellation counters recorded on accepted matching completions. |
| Cache | `cache_hits`, `cache_misses`, `cache_bypasses`, `cache_insertions`, `cache_evictions`, `cache_duplicate_misses`, `cache_revalidations_sent`, `cache_revalidated_304`, `cache_revalidated_200`, `cache_revalidation_errors` | Distinct counters for fresh hits, misses, bypasses, and conditional revalidation outcomes. |
| Memory | `MemorySnapshot` (`candidate`, `resident`, `pinned`, `temporary`, `metadata`, `enforced_used`, `enforced_capacity`) | Sampled lock-free from `core::MemoryBudget` without holding cache storage locks. |
| Process | `ProcessSample` (`user_cpu_time`, `system_cpu_time`, `total_cpu_time`, `cpu_utilization_ratio`, `resident_memory_bytes`, `peak_resident_memory_bytes`) | Rate-bounded OS adapter (`ProcessSampler`). Unsupported platform fields remain `std::nullopt`, never fabricated as `0`. |

## 4. Bounded logging and redaction policy

`observability::BoundedLogger` (`include/observability/bounded_logger.hpp`) uses a fixed-size ring buffer of `LogRecord` structs (`48` bytes operation name, `160` bytes message) so log admission never allocates heap strings or blocks the event loop on slow output sinks.

- **Overflow policy:** Configurable as `LogOverflowPolicy::drop_newest` (default) or `drop_oldest`. Every dropped record increments `LoggerStats::dropped`.
- **Redaction:** Messages containing `Authorization:`, `Proxy-Authorization:`, `Cookie:`, `Set-Cookie:`, `Bearer `, or `Basic ` (case-insensitive) are automatically replaced with `[redacted-sensitive]` on admission. Control characters, CR/LF, and NULs are scrubbed.
- **Shutdown flush:** `BoundedLogger::shutdown(flush_timeout)` signals the writer thread and waits up to `flush_timeout` for queued records to drain. Any records still queued when the timeout expires are counted in `dropped` and discarded so shutdown stays bounded.

## 5. Portable stop hooks and status contract

- **Stop hooks (`include/observability/stop_hooks.hpp`):** POSIX `SIGINT`/`SIGTERM` handlers and the Windows `SetConsoleCtrlHandler` callback only store `true` to a `static_assert`-verified lock-free `std::atomic<bool>`. `ScopedStopHooks` runs a lightweight monitor thread that wakes and calls `StopController::request_stop()` on a normal C++ thread. `ScopedStopHooks::~ScopedStopHooks()` joins the monitor thread and unregisters the OS handler before returning, preventing dead-pointer callbacks during teardown.
- **Status audit (`include/observability/status_audit.hpp`):** Covers all 17 architectural status codes (`200`, `304`, `400`, `403`, `404`, `405`, `408`, `413`, `414`, `417`, `431`, `500`, `501`, `502`, `503`, `504`, `505`). Error responses are emitted only before downstream commit (`!writer.committed()`), include `Allow: GET, HEAD` on `405`, omit bodies on `HEAD`/`204`/`304`, and never include local paths or native OS error messages in response bodies.
