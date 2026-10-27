# Performance investigation

Baseline: merged commit `9c621054a09fb70db068aba59405912dcdd4219a`.
The implementation passed 54 tests on native Windows, Linux and macOS and under
Linux ASan/UBSan before merge. The performance work is on the separate local
branch `codex/performance-profiling`.

## Results

The [cached-output follow-up](CACHED_OUTPUT.md) adds a measured improvement over
the earlier optimized binary: warm-cache median paired throughput rises 24.4%
on the event loop and 18.3% on the thread pool, with about 20% less CPU per
response. All 20 trials pass (11,687,369 responses). All 54 strict suites pass
on Windows GCC and MSVC; Linux/macOS and sanitizer checks for this follow-up
remain pending. The report includes the bounded memory tradeoff and raw results.

The initial investigation is complete. The [detailed findings and before/after results](RESULTS.md)
show 23–29% higher static throughput and lower CPU per response after two targeted
optimizations. Thread-pool proxy median p99 fell from 23.6 ms to 1.85 ms at 200
requests/second. All 80 paired trials passed, completing 6,627,877 HTTP 200 responses.

Uncached proxy saturation still exhausts the host's ephemeral ports because each
request opens an origin connection. That failed stress run is retained and
excluded from the successful comparison; see the detailed report's limitations.

Read the [baseline report written before changes](BASELINE.md),
[correctness validation](VALIDATION.md), and [reproduction instructions](REPRODUCE.md).
Those initial performance changes are local on `codex/performance-profiling` and
pass all 54 strict suites on Windows MSVC, GCC, Clang and MSVC ASan. Their
Linux/macOS validation is pending; the earlier merged LMN CI remains separate.

![Measured comparison](comparison.png)

## Method

The study uses an optimized MSVC Release build with PDB symbols, oha's native
load generator, process CPU/RSS/handle/thread samples, server queue/cache metrics,
and Very Sleepy stack sampling. Only controlled loopback servers/origins are used.
The Node origin serves a prebuilt buffer with explicit cache metadata and reports
origin request counts; it is a benchmark fixture, not a production dependency.

Static 1 KiB and 64 KiB responses, uncached forwarding, and warm-cache hits run
under both drivers. Repeated unprofiled trials establish throughput and latency.
Profiler runs are separately labeled so sampling overhead cannot be mistaken for
server cost. Warm-cache trials must perform no measured origin requests; every
load test checks status, completed byte totals and a full-body precheck.

Very Sleepy 0.91 weights sampled thread stacks by elapsed time, including waits.
Its output is presented as thread wall-time flame graphs, alongside measured
process CPU, rather than mislabeled as hardware CPU-event samples. The busiest
thread selector and all-thread captures have different scopes. Sampling only
observes threads present at attachment; this server creates its fixed worker and
helper pools before attachment. Official source behavior was inspected.

Windows kernel ETW sampling requires an elevated token, which this session does
not have. A portable per-process profiler avoids changing system privileges.
Temporary Very Sleepy symbol/throttle settings are restored after each study;
minidumps and online symbol-server lookup are disabled.

Raw records, tool identities, flame graphs, conclusions, and paired comparisons
are retained in this folder. Shared-host load-generation limits, finite sample
counts and unmeasured platforms are explicitly documented.
`ARTIFACTS.sha256` records the exact bytes of all report and evidence files.
