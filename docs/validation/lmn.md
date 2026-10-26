# LMN candidate validation — 2026-10-27

Base commit: `e24077f`, branch `codex/lmn-completion`. The implementation was
committed as `22e55921ea82570413d181d64a3749cb42738c07` and published in
[PR #3](https://github.com/SatwikPanda/HTTP-Server/pull/3). Its native Windows,
Linux and macOS gates and Linux ASan/UBSan gate each pass all 54 suites; see the
[CI record](lmn-ci/README.md). The earlier local evidence below was collected
before committing. There are two explicitly separate source fingerprints:

- Measured source: `b3013e991e6e2ebb65ade817274615654c671f3d4669ae965eeb84841f29c468`.
  The three `delivery` qualification records and all corrected individual trials
  identify this source, including new untracked code.
- Current source after a concurrent formatting-only edit to `src/main.cpp`:
  `f899ad329217e2c334d1d828087005759a78d140a62b1152bbbc70e446ad0623`.
  The edit was preserved and the strict gates rerun. The campaign wrapper rejected
  the source drift; the measurements are not relabeled as this newer source.

## Implementation and regression coverage

L's integrated production assembly was already present on `main`, including the
static, uncached-proxy and cached-proxy factories under both drivers. This branch
starts from that integrated history rather than rebuilding the assembly from the
older cache branch. The prerequisite commits and factories are listed in the
[L handoff](../branches/application-assembly.md). Existing user roadmap edits
were preserved.

The new M gate builds every target, checks all 54 CTest registrations and labels,
runs the entire strict gate, rejects skipped/missing tests and records exact
source/binary/compiler identities. A production-backed resource soak checks all
six application/driver combinations, concurrent cache replacement and eviction,
shutdown accounting and native handle/thread baselines. Target-scoped diagnostic
builds and native-platform CI artifact retention are implemented. See the
[M guide](../branches/correctness-gate.md).

N adds repeat orchestration, retained exclusions/failures, per-repeat tables,
pooled histogram statistics and standalone plots. Actual execution also found
and fixed an out-of-bounds copy in the static-response test fixture and two
benchmark errors: a sparse-arrival throughput denominator and an idle persistent
connection cycle during open-loop tail drain. Regression checks cover both
generator defects. See the [N guide](../branches/results.md).

## Local strict gates before the formatting-only edit

All 54 suites ran in Release with `BUILD_TESTING=ON`, `BUILD_BENCHMARKS=ON` and
`REQUIRE_ALL_MILESTONES=ON`. Each gate has zero failed, skipped or unexecuted tests.
This includes every earlier milestone label 01–31 and P0; milestone 32's report
tool test is separate from final performance acceptance.

| Native Windows toolchain | Instrumentation | Result | CTest wall time | Record |
| --- | --- | --- | ---: | --- |
| GCC 16.2.0, Ninja | none | 54/54 | 46.32 s | [JSON](lmn-windows/qualification-gcc-delivery.json), [JUnit](lmn-windows/qualification-gcc-delivery.junit.xml), [full log](lmn-windows/lmn-delivery-full-ctest.log) |
| Clang 22.1.8, Ninja | none | 54/54 | 45.92 s | [JSON](lmn-windows/qualification-clang-delivery.json), [JUnit](lmn-windows/qualification-clang-delivery.junit.xml), [full log](lmn-windows/lmn-clang-delivery-full-ctest.log) |
| MSVC 19.29.30159.0, Visual Studio 2019 | AddressSanitizer | 54/54 | 44.53 s | [JSON](lmn-windows/qualification-asan-delivery.json), [JUnit](lmn-windows/qualification-asan-delivery.junit.xml), [full log](lmn-windows/lmn-asan-delivery-full-ctest.log) |

The records retain all build/test commands, compiler/configuration metadata,
source hashes and binary hashes; sibling `.build.log` and `.inventory.log` files
preserve those command outputs. IPv4 and optional IPv6 loopback paths ran without
an unavailable/skip diagnostic. External origins are not used.

### Final current-tree reruns

After preserving the formatting edit, all three complete strict gates passed
again with fingerprint
`f899ad329217e2c334d1d828087005759a78d140a62b1152bbbc70e446ad0623`:

| Toolchain | Result | CTest wall time | Record |
| --- | --- | ---: | --- |
| GCC 16.2.0 Release | 54/54, zero failures/skips | 46.91 s | [JSON](lmn-windows/qualification-gcc-formatted.json), [full log](lmn-windows/qualification-gcc-formatted-full-ctest.log) |
| Clang 22.1.8 Release | 54/54, zero failures/skips | 45.70 s | [JSON](lmn-windows/qualification-clang-formatted.json), [full log](lmn-windows/qualification-clang-formatted-full-ctest.log) |
| MSVC 19.29 Release + ASan | 54/54, zero failures/skips | 45.10 s | [JSON](lmn-windows/qualification-asan-formatted.json), [full log](lmn-windows/qualification-asan-formatted-full-ctest.log) |

Sibling JUnit, build, inventory and CTest output files are retained for each.
No further implementation edits were made after these final gates. The measured
binary predates this formatting-only rebuild and remains separately identified.

The MSVC compiler's original ASan runtime could not initialize on this Windows 11
host (`0xc0000142`). The passing run used the installed runtime at
`C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.44.35207/bin/HostX64/x64`
first on `PATH`, with `ASAN_OPTIONS=halt_on_error=1`. This is explicitly a mixed
compiler/runtime diagnostic run, not a claim of a newer compiler build. Runtime
hash and host details are in [machine.json](lmn-windows/machine.json).

The original 52-suite GCC baseline passed in 44.48 s before these changes. The
previous native Windows/Linux/macOS 52-suite CI is documented separately in
[ci-portability.md](../branches/ci-portability.md); it does not qualify this new
source fingerprint.

## Resource observations and retained failures

- A serial GCC 20-cycle soak kept 120 handles and five threads at every sampled
  quiescent point; RSS moved from 10,014,720 to 13,361,152 bytes. See its
  [log](lmn-windows/soak-20-serial.log). This earlier diagnostic preceded the final
  thread-runtime warmup and handle-type sampling changes.
- The final Clang soak implementation passed 20 cycles with 116 handles and five
  threads at every quiescent sample. RSS moved from 9,854,976 to 13,508,608 bytes;
  all application active/queued/reader/candidate/retained ownership checks passed.
  See the [log](lmn-windows/soak-warmed-runtime.log). Later changes were confined
  to benchmark Python code; the final full gates also reran the six-cycle soak.
- RSS growth is an observation, not proof of bounded long-duration memory or of
  a leak. Windows ASan does not provide POSIX leak/race diagnostic coverage.
  Subsequent Linux CI passed ASan/UBSan with leak detection enabled. Longer
  duration and race-diagnostic coverage remain open.
- The original ASan [initialization failure](lmn-windows/qualification-asan.ctest.log)
  and [fixture buffer-overread report](lmn-windows/qualification-asan-modern-runtime.ctest.log)
  are retained. The fixture now derives the binary-tail length with `sizeof`.
- A 100-cycle soak overlapping another gate failed under local ephemeral-port
  pressure (native error 10048); a host query observed 7,994 TIME_WAIT sockets
  within a 16,384-port dynamic range. The failed
  [GCC](lmn-windows/qualification-gcc-final.ctest.log) and
  [ASan](lmn-windows/qualification-asan-fixed.ctest.log) gate logs remain available.
  These are not counted as successful runs. Later network gates ran sequentially.
- An initial Clang soak retained one extra Event handle while file/socket and
  thread counts stayed constant. The [handle-type log](lmn-windows/soak-handle-types.log)
  is retained. Explicit warmup of contended thread waits and native sampling
  preceded the subsequent stable 20-cycle measurement. No growth allowance or
  sanitizer suppression was introduced.

## Repeated local candidate campaign

All **520 individual trials** completed: 104 configurations, five repeats each,
3,120 validated successful measured responses, zero request errors/timeouts/drops,
and 4,419,993,600 useful representation bytes. The campaign ran from
2026-10-26 19:17:48 to 19:34:07 UTC (2026-10-27 local time).

The campaign wrapper nevertheless exited with failure and retained
`state=interrupted`: its final source-identity check detected the concurrent
`src/main.cpp` formatting edit. The executable and benchmark code used for the
trials did not change; the only differing source entry is `src/main.cpp`.
The original measured source is preserved and hash-verified in
[measured-source.zip](lmn-windows/measured-source.zip). This is historical local
candidate evidence for the original fingerprint, not a successfully qualified
campaign for the current tree. No failed gate was changed to a pass.

Artifacts:

- [Interpretation and limitations](../../benchmarks/reports/lmn-windows-open/README.md),
  [generated tables](../../benchmarks/reports/lmn-windows-open/report.md),
  [per-repeat CSV](../../benchmarks/reports/lmn-windows-open/repeats.csv), and
  [full grouped statistics](../../benchmarks/reports/lmn-windows-open/summary.json).
- [Throughput PNG](../../benchmarks/reports/lmn-windows-open/throughput.png) /
  [SVG](../../benchmarks/reports/lmn-windows-open/throughput.svg), and
  [p99 PNG](../../benchmarks/reports/lmn-windows-open/p99.png) /
  [SVG](../../benchmarks/reports/lmn-windows-open/p99.svg), visually inspected.
- [Corrected raw records and plan](lmn-windows/lmn-windows-open-fixed-raw.zip),
  [initial interrupted campaign](lmn-windows/lmn-windows-open-raw.zip), and
  [archive sizes/SHA-256](lmn-windows/archives.json). The original campaign retains
  14 successful and 12 failed trials plus one incomplete directory; it is excluded
  from the corrected report because the generator implementation differs.

Setup: Clang 22.1.8 Release on Windows 11, Ryzen 7 7700X (8 cores/16 logical),
33,397,133,312 bytes RAM, existing Ultimate Performance power plan. Server,
Python generator and origin share this desktop. No power or TCP settings changed.
The isolated Python dependencies are listed in
[python-packages.txt](lmn-windows/python-packages.txt).

Each trial used 0.2 s warmup and a 1.2 s issue window with open arrivals at five
requests/s, seed 1729, 128 queued generator jobs and a 10 s request deadline.
Actual phase durations, including tail drain, ranged from 1.2004 to 6.0771 s.
The matrix covers all 11 non-demo families, configured client limits 1/10/50,
thread-pool workers 1/2/4/8/16, event-loop workers 1, object sizes
1 KiB/64 KiB/1 MiB/16 MiB, and new/reused connections through anchor sweeps.
The complete plan has 171 points: 66 exceed the selected 50-client limit, and
the single demo point is excluded from production-driver comparison. This is not
a full Cartesian product. Exact settings and exclusions are retained in the plan.

Observed checks and limits:

- Warm-cache runs: 480 measured responses, zero origin requests. Uncached proxy:
  420 responses and exactly 420 origin requests. Revalidation: 180 responses,
  180 origin 304s and ten permitted unconditional version-conflict retries.
- Every trial retained a periodic server snapshot. Sampled server RSS ranged
  from 7,462,912 to 24,715,264 bytes; measured process CPU ranged from zero at the
  sampler's resolution to 0.128 cores. These short shared-host observations do
  not establish capacity or maximum memory. Cache and queue snapshots include
  preparation/warmup; origin counts above are measured interval deltas.
- Twenty trials raised scheduling/saturation hints. They remain in the data.
  The generator and in-process origin can limit delivery, especially for larger
  objects; delayed issue is included in request latency.
- The x-axis is the configured generator client limit, not a claim that all
  clients are simultaneously active. There are six measured requests per trial
  and 30 samples per configuration. P99 is therefore effectively a high-end
  bucket observation, not a statistically stable tail estimate.
- Fixed-worker persistent-session occupancy is consistent with the longer
  latency observed when configured clients exceed workers. This explanation
  remains specific to the tested arrival/reuse pattern. Near-five request/s
  throughput reflects the offered rate; it does not rank server capacity.
- Low-load, short-window runs, uncontrolled filesystem cache, unequal worker CPU
  budgets and shared-host scheduling prevent a universal driver ranking.

The [report README](../../benchmarks/reports/lmn-windows-open/README.md) gives exact
commands to extract the retained raw data and regenerate tables and plots.
Extraction and regeneration were executed into fresh directories: all 104 grouped
configuration summaries and 520 raw hashes matched, and the per-repeat CSV was
byte-identical. Both generated plots were visually inspected. Documentation link,
diff whitespace, and `AGENTS.md`/`CLAUDE.md` equality checks also passed.

## Remaining acceptance

The native release qualification is now verified by matching Windows/Linux/macOS
records. Linux ASan/UBSan with leak detection also passes. The clean CI checkouts
build and run every required suite without local ignored dependencies. No CI
failure required a code fix. Race tooling has not run and no race-diagnostic
success is claimed. WSL/local macOS availability is no longer a blocker to native
platform acceptance because the hosted native jobs supplied that evidence.

N's final performance comparison still requires new measurements of the qualified
committed candidate with longer windows and explicitly reported host limits.
The earlier short candidate dataset is not promoted by the later CI success.
