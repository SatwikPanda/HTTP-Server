# Integrated CI portability verification — 2026-10-26

Candidate: `5b1d4b41e8202b892ee6ae2ef7624dc7d329a099`, checked out cleanly by
[GitHub Actions run 40371796001](https://github.com/SatwikPanda/HTTP-Server/actions/runs/40371796001).
All three jobs succeeded. This record supersedes the pending Linux/macOS CI statements in the earlier
application, I and J handoffs. It does not claim M's resource soak, sanitizer/race
diagnostics or N's performance measurements.

## Initial failures and stable fixes

[Run 40370186351](https://github.com/SatwikPanda/HTTP-Server/actions/runs/40370186351)
at `5151e9a` built successfully on every platform. Windows passed; Linux failed
`milestone27_test`, and macOS failed that suite and `thread_pool_test`.

- `58e8185`: replace the shutdown fixture's send-before-read echo setup with a
  small HTTP request producing a bounded response. Both shutdown cases first
  observe a real blocked write. The drain case checks every response byte and
  clean EOF; the stalled case checks the deadline with unwritten output.
- `5b1d4b4`: stream a bounded 16 MiB response for slow-reader acceptance instead
  of assuming 512 KiB cannot fit in OS socket buffers. Require actual would-block
  I/O, partial progress, a session timeout, a driver timeout and cleanup.

The shared probe delegates every action/event to the production session and only
records observations. These fixes change acceptance fixtures, not runtime behavior.
Existing echo/partial-I/O checks and the strict 52-suite inventory remain present.

## Native CI results

| Platform | Compiler | Runtime suites | CTest time |
| --- | --- | --- | --- |
| Ubuntu 24.04.5 | GCC 13.3.0 | 52/52 passed, no failures/skips | 10.91 s |
| macOS 26.6.2 arm64 | AppleClang 21.0.0.21000101 | 52/52 passed, no failures/skips | 15.27 s |
| Windows Server 2025 (10.0.26100) | MSVC 19.51.36260.0 / VS 2026 | 52/52 passed, no failures/skips | 47.17 s |

CI configures fresh builds with C++20, Release and strict milestones. Benchmarks
use the runner's Python interpreter; production still uses only C++/OS APIs.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DBUILD_BENCHMARKS=ON -DREQUIRE_ALL_MILESTONES=ON
cmake --build build --config Release
ctest --test-dir build -C Release --no-tests=error --output-on-failure
```

The inventory includes earlier milestones/P0, all merged module suites, seven real
system combinations, lifecycle acceptance, benchmark unit checks and all 11
non-demo smoke families through both drivers. These are correctness/smoke results,
not benchmark rankings or exhaustive platform qualification. Passing-suite output
is suppressed by CTest; this log does not independently establish optional IPv6
or symlink-privilege coverage inside those suites.

Before pushing, the full local Windows GCC gate passed 52/52 (42.17 s). Affected
suites also passed Windows Clang and MSVC, with five consecutive Clang repeats.
All recorded runs used Release-active assertions. The earlier failed run remains
linked above; its failures are not hidden by retries or removed assertions.
