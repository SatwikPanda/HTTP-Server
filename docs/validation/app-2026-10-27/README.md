# Application tests and resource measurements — 2026-10-27

This record covers the `http_app` executable and the full project regression gate.
It is local Windows evidence. It does not replace Linux/macOS acceptance or the
older static-file/proxy performance study, which used different workloads.

## What ran

- Complete strict Release CMake/CTest gate on GCC 16.2 and MSVC 19.29: 56 suites
  each, including the new route API and regression tests for report accounting.
- 23 additional actual-executable checks per build: CLI help/invalid ports,
  fixed/automatic/occupied ports, GET, HEAD, query and absolute targets, missing
  routes, escaped paths, method rejection, HTTP/1.0, invalid Host/framing/escape,
  an ordered GET/HEAD/404 pipeline and the 100-request keep-alive limit.
- Five trials per compiler and workload: idle, GET with 1/4/16 clients,
  GET with four clients and connection reuse disabled, HEAD with four clients,
  and missing-route GET with four clients. One-second warmup and five-second
  measurement, or three seconds idle. Order alternates each repeat.
- One 60-second sustained GET run at four clients per compiler.

The first, unrestricted campaign completed **72 trials: 56 passed, 16 failed**.
Its native client reported Windows socket-address errors (10048) and timeouts.
A read-only diagnostic found a TCP dynamic range of 49,152–65,535 (16,384 ports)
and 7,601 client sockets in TIME_WAIT for completed benchmark endpoints at the
observation instant. No OS network settings were changed. The original tooling
snapshot is retained in `benchmarks/results/app-2026-10-27/tooling/`.

The separate controlled campaign uses the same executable hashes with a
5,000-requests/s cap for reused connections and a 250-requests/s cap for new TCP
connections. This characterizes resource use at specified loads, not peak
capacity. Unrestricted failures remain in the report, with warmup and timed
measurement errors distinguished. Failed points are never presented as passing
performance medians.

The controlled campaign completed **72/72 passing trials**, including both
60-second soaks, with **1,861,410 measured responses** and no unexpected statuses,
body-byte totals or transport errors. The report's JSON, CSV and all five PNGs
were regenerated into `build/app-report-reproduction` and matched byte-for-byte.
Every chart was visually inspected, README links were checked, and report/input
hashes plus current executable/source fingerprints were verified before merge.

`gcc-qualification.*` and `msvc-qualification.*` retain build logs, exact CTest
inventories, JUnit reports, source fingerprints and executable hashes. Both the
original `httpserver` and the `http_app` binary are tied to the qualification.
The campaign validates those fingerprints before loading either executable.
`gcc-controlled-qualification.*` and `msvc-controlled-qualification.*` cover the
rate-cap/reporting extension; only test/report tooling changed between those
records, and the two app executable hashes remain identical.

The initial sandbox-only results-tool invocation could not access its temporary
files. The retained full runs use normal local temporary-file and loopback access;
all results in the graphs come from those completed runs. A short, separate
measurement preflight checked GET/HEAD/404 accounting before the main campaign.

## Measurement definitions

The actual app has four pool workers, a bounded queue, two helper threads,
loopback binding, buffered responses, a 13-byte Hello World body, and a limit of
100 requests per connection. No benchmark-only handler or tuned app configuration
is substituted. All sockets use IPv4 loopback.

`oha` 1.16.0 supplies native HTTP/1.1 closed-loop traffic, using four generator
threads, a five-second request timeout, and waiting for in-flight requests after
the issue window. Each trial checks expected statuses and total response-body
bytes. The independent probes before/after it compare exact body bytes. Timed
traffic does not perform per-request content hashing. The generator and server
share this host; generator samples are retained separately in the raw records.

`psutil` samples only the owned server and generator processes, every 100 ms.
CPU is process user+kernel CPU seconds divided by measured elapsed wall time,
with 100% equal to one logical core. Machine-normalized CPU is also retained.
The resource window includes native generator startup, in-flight drain and up
to one final sampling interval; throughput uses the generator's own elapsed
duration. These are explicitly different denominators.
RAM is the maximum sampled working set/RSS, not committed/private memory or a
guaranteed OS peak. Private bytes, thread count and handle count are in raw data.
CPU charts for the soak use approximately one-second bins to reduce timer noise.

The report uses per-trial medians with minimum/maximum error bars. Its p50/p95/p99
summaries are medians of per-trial percentiles, not pooled request percentiles.
The soak has one trial per build and is not a leak-proof or long-term soak claim.
The C++ lifecycle/resource suites establish their separately tested ownership
contracts. Windows hidden benchmark processes are forcibly stopped by owned
handle, so graceful shutdown is not inferred from the benchmark cleanup.

Binary sizes are the as-built EXEs, excluding PDB files and runtime DLLs.
GCC Release flags: `-O3 -DNDEBUG`. MSVC Release flags: `/O2 /Ob2 /DNDEBUG /Zi`;
linker: `/DEBUG /INCREMENTAL:NO /OPT:REF /OPT:ICF`. Linker/runtime choices affect
size; this is not a standalone-deployment-size or compiler-efficiency comparison.
No sanitizer or profiler is attached during load measurements.

## Reproduction

The measurement/plot scripts use test-only Python packages `psutil` and
`matplotlib`; the C++ runtime has no new dependencies. The `oha` version/download
and hash were already recorded in
[`performance testing/downloads.json`](../../../performance%20testing/downloads.json).
Its exact executable hash is also in the campaign manifest.

Using the existing build directories, from the repository root:

```powershell
# Choose fresh output paths for every new run. Add your GCC runtime directory
# to PATH when launching its executable from PowerShell.
$env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
python tools/qualify.py --build-dir build/lmn --binary build/lmn/httpserver.exe --output docs/validation/app-2026-10-27/gcc-controlled-qualification.json --jobs 6
python tools/qualify.py --build-dir build/performance-next --binary build/performance-next/Release/httpserver.exe --output docs/validation/app-2026-10-27/msvc-controlled-qualification.json --jobs 1

python benchmarks/app_study.py --variant gcc build/lmn docs/validation/app-2026-10-27/gcc-controlled-qualification.json --variant msvc build/performance-next docs/validation/app-2026-10-27/msvc-controlled-qualification.json --oha build/performance-tools/oha.exe --output benchmarks/results/app-controlled-2026-10-27 --warmup 1 --duration 5 --repeats 5 --soak 60 --rate 5000 --new-connection-rate 250
python benchmarks/app_report.py --raw benchmarks/results/app-controlled-2026-10-27 --stress benchmarks/results/app-2026-10-27 --output benchmarks/reports/app-2026-10-27
```

To regenerate only the published plots without rebuilding or running sockets:

```powershell
python benchmarks/app_report.py --raw benchmarks/results/app-controlled-2026-10-27 --stress benchmarks/results/app-2026-10-27 --output build/app-report-reproduction
```

All output directories/record paths must be fresh; the tools refuse overwriting
evidence. The report consumes the retained relative qualification paths, raw
samples and manifest. `summary.json` records hashes of its raw inputs, and
`ARTIFACTS.sha256` covers the report artifacts. The manifest preserves the source
fingerprint and the pre-commit state at measurement time; subsequent Markdown
edits or commits do not retroactively change that evidence.
