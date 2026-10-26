# LMN native CI qualification — 2026-10-27

Implementation commit: `22e55921ea82570413d181d64a3749cb42738c07`,
[PR #3](https://github.com/SatwikPanda/HTTP-Server/pull/3).
All jobs in [run 40389322316](https://github.com/SatwikPanda/HTTP-Server/actions/runs/40389322316)
passed on the first run. No CI-specific source fix or test suppression was needed.

All four records identify source fingerprint
`f899ad329217e2c334d1d828087005759a78d140a62b1152bbbc70e446ad0623`.
The qualification script independently verified the complete inventory and
JUnit execution: 54 tests per job, zero failed, skipped or missing cases. Tests
cover all earlier milestone labels 01–31, P0, real application/driver modes and
the new resource/report suites. The reporting test's milestone 32 label does not
constitute final performance acceptance.

| Native host | Compiler | Instrumentation | Passed | CTest wall time | Evidence |
| --- | --- | --- | ---: | ---: | --- |
| Windows Server 2025 x64 | MSVC 19.51.36260.0 | none | 54/54 | 51.78 s | [record](qualification-windows.json), [JUnit](qualification-windows.junit.xml), [job](https://github.com/SatwikPanda/HTTP-Server/actions/runs/40389322316/job/121805857272) |
| Linux x64 / glibc 2.39 | GCC 13.3.0 | none | 54/54 | 11.20 s | [record](qualification-linux.json), [JUnit](qualification-linux.junit.xml), [job](https://github.com/SatwikPanda/HTTP-Server/actions/runs/40389322316/job/121805857626) |
| macOS 26.6.2 arm64 | AppleClang 21.0.0.21000101 | none | 54/54 | 18.93 s | [record](qualification-macos.json), [JUnit](qualification-macos.junit.xml), [job](https://github.com/SatwikPanda/HTTP-Server/actions/runs/40389322316/job/121805857312) |
| Linux x64 / glibc 2.39 | GCC 13.3.0 | ASan + UBSan, leak detection enabled | 54/54 | 16.13 s | [record](qualification-asan-ubsan.json), [JUnit](qualification-asan-ubsan.junit.xml), [job](https://github.com/SatwikPanda/HTTP-Server/actions/runs/40389322316/job/121805857009) |

Every native job configured a clean checkout with Release, `BUILD_TESTING=ON`,
`BUILD_BENCHMARKS=ON` and `REQUIRE_ALL_MILESTONES=ON`, built every target and ran
`tools/qualify.py`. Linux diagnostics added `HTTP_SANITIZER=address-undefined`,
`ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` and
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`. The records retain compiler
paths/options, exact build/test commands, source-file hashes and executable hashes.
Workflow artifacts also retain inventories and full test logs.

The resource soak passed six measured cycles after warmup in each job. Linux
returned to five file descriptors and one thread; macOS returned to three file
descriptors and one thread. Windows returned to its recorded native baseline.
All application shutdown, helper, cache reader/candidate/retained-allocation
assertions passed. RSS samples and resource counts are retained in
[the run manifest](run-40389322316.json). Instrumented RSS includes sanitizer
overhead and is not comparable to release memory measurements. No IPv6
unavailable/skip diagnostic appeared in the full test logs.

These results establish native release acceptance and executed memory/undefined
behavior diagnostics for this source. Race diagnostics remain untested; finite
soaks do not establish unlimited-duration resource bounds. Earlier local failures
and candidate benchmarks remain in [the historical validation record](../lmn.md).
Later CI success does not promote the short benchmark campaign whose provenance
check rejected a concurrent source edit. Final N performance comparison remains
separate work.
