# Reproduce the investigation

Run from the repository root. Preserve separate original and optimized binaries;
do not rebuild the baseline directory after changing production sources. The
original binary was built from merged commit `9c621054a09fb70db068aba59405912dcdd4219a`.
The local paths below refer to that preserved binary and the updated build.

## Tools and environment

* Windows native MSVC 19.29.30159, CMake 4.4.3, Visual Studio 16 2019 generator.
* Python 3.14 with `psutil` for per-process samples and `matplotlib` for plots.
  These are test/report dependencies only. This workspace's environment is
  `build/lmn-tools/Scripts/python.exe`.
* [oha 1.16.0](https://github.com/hatoo/oha/releases/tag/v1.16.0), native Windows
  amd64 executable in `build/performance-tools/oha.exe`.
* [Very Sleepy 0.91](https://github.com/VerySleepy/verysleepy/releases/tag/v0.91),
  extracted locally with [innoextract 1.9](https://github.com/dscharrer/innoextract/releases/tag/1.9).
  No installation or system service is required. Keep the extracted executable
  and its companion symbol/runtime DLLs together in
  `build/performance-tools/very-sleepy/app/`.
* Node.js executes the controlled origin fixture `origin.mjs`.
* [FlameGraph](https://github.com/brendangregg/FlameGraph) revision
  `41fee1f99f9276008b7cd112fca19dc3ea84ac32`; place `flamegraph.pl` in
  `build/performance-tools/`. Renderer uses Git's bundled Perl by default.

Official download URLs and SHA-256 values are in `downloads.json` and
`flamegraph-source.json`. No downloaded tool is part of the production binary.
Very Sleepy preferences are changed temporarily and restored: 10 ms sampling,
local PDB lookup, online symbols disabled, minidumps disabled. Captures contain
sampled stacks/symbol paths, not process-memory dumps. Tool version 0.91 only
samples threads existing at attachment; the fixed server pools start earlier.

## Optimized, symbol-bearing build

Use the same settings for both revisions in separate build directories:

```powershell
cmake -S . -B build/performance-optimized -G "Visual Studio 16 2019" -A x64 -DBUILD_TESTING=ON -DBUILD_BENCHMARKS=ON -DREQUIRE_ALL_MILESTONES=ON -DHTTP_SANITIZER=none "-DCMAKE_CXX_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG /Zi" "-DCMAKE_EXE_LINKER_FLAGS_RELEASE=/DEBUG /INCREMENTAL:NO /OPT:REF /OPT:ICF"
cmake --build build/performance-optimized --config Release --parallel 6
python tools/qualify.py --build-dir build/performance-optimized --binary build/performance-optimized/Release/httpserver.exe --output "performance testing/validation/new-msvc.json" --jobs 6
```

Qualification outputs must be new filenames. Source inventory, compiler, binary
hash, actual commands, JUnit, logs and all 54 test results are retained. Other
compiler builds use separate directories. The local MSVC ASan run uses compiler
19.29 with the installed 14.44 runtime first on PATH, as documented in
`../docs/validation/lmn.md`; this is a mixed compiler/runtime diagnostic check.

## Measurements

Run one load study at a time, with no concurrent build/test campaign. Output
directories must be new. The default test binary uses real production CLI modes.

```powershell
# Broad unchanged-server baseline: 30 trials, five per workload/driver.
& build/lmn-tools/Scripts/python.exe "performance testing/run_study.py" --binary build/performance-baseline/Release/httpserver.exe --output "performance testing/raw/new-baseline" --label baseline --cases static-1k,static-64k,cache-warm --duration 10 --warmup 2 --repeats 5

# Alternating nearby before/after trials: 80 in total, including fixed-rate proxy.
& build/lmn-tools/Scripts/python.exe "performance testing/run_pairs.py" --before build/performance-baseline/Release/httpserver.exe --after build/performance-optimized/Release/httpserver.exe --output "performance testing/raw/new-paired" --repeats 5

# Resolve the busiest native thread while the server is serving requests.
& build/lmn-tools/Scripts/python.exe "performance testing/run_study.py" --binary build/performance-optimized/Release/httpserver.exe --output "performance testing/raw/new-profiles" --label optimized-profile --cases static-1k,cache-warm --duration 12 --warmup 2 --repeats 1 --profile busy

# All-thread proxy capture at a fixed offered rate, including TCP state counters.
& build/lmn-tools/Scripts/python.exe "performance testing/run_study.py" --binary build/performance-optimized/Release/httpserver.exe --output "performance testing/raw/new-proxy-profile" --label optimized-proxy-profile --cases proxy-off --duration 12 --warmup 1 --repeats 1 --profile all --rate 200 --tcp
```

Use `--binary-source` with the source SHA recorded at build time when running an
older binary from a newer checkout. Each manifest separately records the current
checkout source and binary digest. `run_pairs.py` supplies both source identities.
No benchmark changes the system TCP range, power policy or security settings.
Windows hidden child processes are terminated after recording; their benchmark
cleanup does not establish graceful shutdown acceptance. The native suites test
graceful shutdown and quiescent ownership separately.

## Render and audit retained results

```powershell
python "performance testing/render_profiles.py" "performance testing/raw/baseline-profiles"
python "performance testing/render_profiles.py" "performance testing/raw/baseline-proxy-profile"
python "performance testing/summarize.py" "performance testing/raw/baseline" --output "performance testing/baseline-summary.json"
& build/lmn-tools/Scripts/python.exe "performance testing/compare_pairs.py"
& build/lmn-tools/Scripts/python.exe "performance testing/check_junctions.py" --binary build/performance-optimized/Release/httpserver.exe --output "performance testing/validation/new-junctions.json"
```

`compare_pairs.py` intentionally reads the completed retained study at `raw/paired`;
change that input path for a new study. It refuses incomplete studies, missing
trials or failed records. Each profile generates interactive SVG, folded stacks,
CSV and JSON inclusive/exclusive summaries. Folded weights are sampled **thread
microseconds**, including waits; reversing Very Sleepy's leaf-first stacks
produces root-first flame graphs. JSON stores the original capture's digest.
Summaries report descriptive per-trial medians/ranges, not fabricated confidence
intervals or pooled tail latencies. Failed exploratory records remain available.
