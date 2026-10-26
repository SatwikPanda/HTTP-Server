# Config-observability handoff (track H, milestone 29 and stop hooks for 28)

## Base and prerequisites

- `PARALLEL_BASE`: `c86509b` (`main`, containing P0 `parallel-foundation` and merged track A `static-service`).
- Track: **H (`config-observability`)**.
- Milestones covered: **29** (configuration validation, metrics aggregation, bounded logging, process sampling, status contract audit) and portable OS stop hooks for **28**.

## Owned files and targets

- `include/core/config.hpp`, `src/core/config.cpp` (part of `core` static library)
  - `core::parse_cli`, `core::format_help`, `core::validate_config`, `core::validate_document_root`, `core::CliParseOutcome`.
- `include/observability/metrics_registry.hpp`, `src/observability/metrics_registry.cpp`
  - `observability::MetricsRegistry` (lock-free `core::MetricsSink` implementation), `observability::MetricsSnapshot`, `observability::HistogramSnapshot`, `observability::MemorySnapshot`.
- `include/observability/bounded_logger.hpp`, `src/observability/bounded_logger.cpp`
  - `observability::BoundedLogger`, `observability::LogRecord`, `observability::make_log_record`, `observability::format_log_record`.
- `include/observability/process_sampler.hpp`, `src/observability/process_sampler.cpp`
  - `observability::ProcessSampler`, `observability::ProcessSample`.
- `include/observability/stop_hooks.hpp`, `src/observability/stop_hooks.cpp`
  - `observability::StopController`, `observability::ScopedStopHooks`, `observability::LifecyclePhase`.
- `include/observability/status_audit.hpp`, `src/observability/status_audit.cpp`
  - `observability::kAuditedStatusCodes`, `observability::status_policy_for`, `observability::can_emit_error_response`, `observability::build_sanitized_error_response`.
- `src/observability/CMakeLists.txt` (exports `observability` static library).
- `tests/observability/CMakeLists.txt`, `tests/observability/config_parser_test.cpp`, `tests/observability/metrics_and_logging_test.cpp`, `tests/observability/stop_hooks_test.cpp`.
- `docs/configuration.md` (CLI reference, metric units/boundaries, logging overflow/redaction policy, and startup examples).

## Guarantees and contracts for assembly (track L)

1. **Pure configuration parsing:** `core::parse_cli` parses and validates all P0 frozen CLI flags plus legacy `--host` / `--buffer-size` aliases without opening sockets, touching the filesystem, or starting worker threads. `--help` returns `help_requested = true` and `help_text`. Conflicting mode-specific options (`--root` outside `--mode server`, `--cache on` or `--cache-*` outside `--mode proxy`) are rejected with `ErrorCategory::invalid_argument`.
2. **Metric units and lifecycle boundaries:** `MetricsRegistry` uses lock-free atomics and fixed dimensions. `requests_started` increments on parsed/rejected request dispatch; `responses_completed` increments only after a response finishes draining; `wire_bytes_completed` counts acknowledged socket writes; `body_bytes_completed` counts representation bytes of completed responses (excluding `HEAD`). Latency histograms use 12 fixed microsecond buckets (`<=100us` to `<=2000000us` plus overflow). `snapshot(const core::MemoryBudget*)` reads `MemoryBudget` categories without locking the cache.
3. **Bounded nonblocking logging:** `BoundedLogger` stores fixed-size `LogRecord` items in a preallocated ring buffer (`LogOverflowPolicy::drop_newest` or `drop_oldest`), counting dropped items in `LoggerStats::dropped` and redacting sensitive header substrings (`Authorization:`, `Proxy-Authorization:`, `Cookie:`, `Set-Cookie:`, `Bearer `, `Basic `) on admission. `shutdown(flush_timeout)` bounds writer flushing and counts any un-drained records as dropped.
4. **Process sampling:** `ProcessSampler` rate-limits OS process CPU (`user`, `system`, `total`, `cpu_utilization_ratio`) and resident memory (`resident_memory_bytes`, `peak_resident_memory_bytes`) queries using an injected `SteadyClock`. Unavailable OS fields remain `std::nullopt`.
5. **Stop hook lifetime:** OS signal handlers (`SIGINT`, `SIGTERM`) and Windows `SetConsoleCtrlHandler` callbacks touch only a `static_assert`-verified lock-free `std::atomic<bool>`. `ScopedStopHooks` bridges that flag on a normal monitor thread to `StopController::request_stop()`. `ScopedStopHooks::~ScopedStopHooks()` joins the monitor thread and unregisters OS handlers before returning so callbacks cannot touch destroyed state.

## Executed verification

Configured and built in `build/agents` (Windows, GCC 16.2, Ninja, Release, `BUILD_TESTING=ON`, `REQUIRE_ALL_MILESTONES=ON`):

```powershell
cmake -S . -B build/agents -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DREQUIRE_ALL_MILESTONES=ON
cmake --build build/agents --config Release
ctest --test-dir build/agents -C Release -L "milestone0[1-9]|milestone1[012]|milestone2[89]|parallel_foundation" --no-tests=error --output-on-failure
```

- All 28 selected tests passed (`0` failures, `0` skips), including `config_parser_test` (`milestone29`), `metrics_and_logging_test` (`milestone29`), `stop_hooks_test` (`milestone28;milestone29`), `cli_validation_test` (`milestone03`), `parallel_foundation_test`, `parallel_work_test`, and milestones 01–12.
- Note: directory symlink creation in `config_parser_test` reported unimplemented/unprivileged on this Windows host; regular directory, missing path, and regular-file rejection were exercised.

## Remaining dependent acceptance

- Wiring `core::parse_cli`, `MetricsRegistry`, `BoundedLogger`, and `ScopedStopHooks` into `src/main.cpp` across all four application/driver combinations belongs to **track L (`application-assembly`)**.
- Cross-platform POSIX/macOS signal execution and full milestone 28/29/30 signoff belong to **track L** and **track M (`correctness-gate`)**.
