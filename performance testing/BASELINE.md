# Baseline findings, recorded before production changes

Recorded 2026-10-27, after merge `9c621054a09fb70db068aba59405912dcdd4219a`.
Production source SHA-256:
`f899ad329217e2c334d1d828087005759a78d140a62b1152bbbc70e446ad0623`.
No production optimization had been applied when this report was written.

## Measurement method

Windows 11 build 26200, Ryzen 7 7700X (8 cores/16 logical processors), 31.1 GiB
RAM, existing Ultimate Performance power plan. MSVC 19.29.30159 Release `/O2
/Ob2 /DNDEBUG /Zi`, linked `/DEBUG /INCREMENTAL:NO /OPT:REF /OPT:ICF`. See
`baseline-build.json` for binary/PDB fingerprints and CMake settings. Server,
oha 1.16.0 and the controlled Node origin share this host. Loopback only;
these are warm-filesystem-cache tests, not storage-device or WAN benchmarks.

Both drivers use 16 clients, 4 file/DNS helpers, and 16 thread-pool workers.
Static and warm-cache saturation: five repetitions, 2 s warmup + 10 s measured,
case order reversed every other repetition. The 64 KiB warm-cache workload
must issue zero measured origin requests. Prechecks compare the complete body;
every measured trial checks all statuses are 200, no generator errors, and
completed bytes equal response count times object size. No reported trial
below had errors. Measurements exclude process startup and warmup.

The proxy is also tested at 200 aggregate requests/s (three repetitions,
1 s warmup + 5 s measured). This is a latency/CPU comparison at a fixed offered
rate, not its maximum throughput. Failed saturation attempts remain in `raw/smoke`.
Reported p99 is the median of per-trial p99s, not a pooled percentile. Process
CPU is divided by measured wall time; 1.0 means one occupied logical core.
Generator CPU stops at the last live sample and is an approximation.

## Unprofiled results

| Workload | Driver | Requests/s median [min, max] | p99 ms | CPU us/request | Server cores | Generator cores |
|---|---|---:|---:|---:|---:|---:|
| Static 1 KiB | event loop | 7,239 [5,778, 7,373] | 3.551 | 446.5 | 3.26 | 0.32 |
| Static 1 KiB | thread pool | 6,703 [5,792, 7,201] | 3.800 | 506.6 | 3.38 | 0.25 |
| Static 64 KiB | event loop | 5,658 [5,448, 6,910] | 4.141 | 604.3 | 3.42 | 0.32 |
| Static 64 KiB | thread pool | 5,794 [5,413, 6,823] | 4.146 | 614.1 | 3.57 | 0.33 |
| Warm cache 64 KiB | event loop | 20,857 [16,295, 21,199] | 1.613 | 47.8 | 0.99 | 1.09 |
| Warm cache 64 KiB | thread pool | 86,199 [82,515, 101,447] | 0.492 | 86.1 | 7.26 | 3.39 |
| Proxy off, offered 200/s | event loop | 199.6 | 2.341 | 437.9 | 0.09 | 0.04 |
| Proxy off, offered 200/s | thread pool | 199.7 | 23.717 | 889.7 | 0.18 | 0.04 |

Raw measurements: `raw/baseline`, `raw/baseline-proxy`; aggregates:
`baseline-summary.json`, `baseline-proxy-summary.json`. These numbers are local
observations, not universal capacity limits. The wide ranges require a paired,
alternating before/after comparison rather than comparing one fastest run.

## Stack profiles and bottlenecks

[Very Sleepy 0.91](https://github.com/VerySleepy/verysleepy/tree/v0.91) weights
thread stacks by elapsed time, including waits. These are **thread wall-time
flame graphs, not hardware CPU-event flame graphs**. Busiest-thread captures
select a thread by accumulated CPU at attachment, after warmup. All-thread
captures include idle workers. Fixed server pools exist before attachment.
Profiler symbol lookup uses local PDBs; online symbols and minidumps are disabled.
Some OS symbol names resolve only to an exported neighbor, so conclusions use
resolved project frames and API callers, not misleading leaf names alone.

1. **Static-file metadata is the primary measured static bottleneck.** The
   busiest threads are file helpers. `file_metadata` covers 81.7% (event loop)
   and 79.0% (thread pool) of their sampled time; file-open work covers 89.2%
   and 88.2%. Path-component attribute calls cover 34.7%/30.9%, and separate
   attribute queries cover 27.7%/28.0%. Runtime snapshots show all four helpers
   active and roughly 10–12 jobs queued. Generator CPU is low. The source does
   a component safety walk, canonicalization, and separate status/size/time
   queries on every request. Canonicalization and link rejection are correctness
   protections; redundant final-file attribute queries can be consolidated.
   [Event-loop helper flame graph](flamegraphs/baseline-profiles-static-1k-event-loop-c16-r1.svg),
   [thread-pool helper flame graph](flamegraphs/baseline-profiles-static-1k-thread-pool-c16-r1.svg).

2. **Synchronous proxy readiness creates unnecessary TCP connections.**
   `ReferenceWorkExecutor::execute` creates a new `Poller` for every wait.
   `Poller::create` establishes a loopback TCP wake pair. The thread-pool proxy
   reaches this for outbound connect and subsequent readiness. At 200/s the
   all-thread profile attributes 1.22% of *27-thread elapsed time* to poller
   creation (not 1.22% process CPU). Host loopback TIME_WAIT grows 7,705 during
   its 13 s warmup/measurement, versus 2,612 for the event loop: about three
   connections/request versus one. The Windows dynamic range is 49,152–65,535
   (16,384 ports). An earlier saturation smoke returned 463 successes and 208
   502s in the measured thread-pool trial; that failed result is excluded from
   throughput comparisons. Port pressure is a strongly supported explanation,
   although no native error trace was captured for each 502. Retaining one
   lazy poller per executor should remove the avoidable churn and lower cost.
   [Thread-pool all-thread flame graph](flamegraphs/baseline-proxy-profile-proxy-off-thread-pool-c16-r1.svg),
   [event-loop control](flamegraphs/baseline-proxy-profile-proxy-off-event-loop-c16-r1.svg).

3. **Warm-cache event-loop throughput is limited by its one owner thread.**
   Process CPU is 0.99 core, helpers are idle, and no origin requests occur.
   `Socket::write_some`/`send` covers 55.9% of the sampled event-loop thread;
   session processing also contributes. The thread pool uses more cores and
   reaches higher throughput, at higher CPU/request. Its selected worker spends
   45.4% of sampled time in readiness polling, largely waiting. These facts do
   not demonstrate that cache locking is the primary bottleneck. Scatter/gather
   output could reduce send calls but changes framing/buffering contracts and
   requires a separate targeted design. No speculative cache redesign is justified.
   [Event-loop cache flame graph](flamegraphs/baseline-profiles-cache-warm-event-loop-c16-r1.svg),
   [thread-pool cache flame graph](flamegraphs/baseline-profiles-cache-warm-thread-pool-c16-r1.svg).

The four busiest-thread profiled throughputs are 7,045/7,013 static and
20,473/92,124 cache requests/s. They lie within or near the unprofiled ranges;
this checks for large perturbation but does not prove zero profiler overhead.
The first `profile-smoke` capture briefly overlapped exploratory testing and is
retained only as setup evidence, never as the comparative baseline.

## Changes selected for evaluation

* Reuse a lazily created executor poller. Register/unregister each socket around
  each wait, preserve generations, candidate deadlines, cancellation and owner
  thread rules. File/DNS-only helper executors must not create pollers.
* Reuse Windows final-component attribute data for type and size. Keep every
  parent-component reparse check, canonical containment, opened-stream length,
  and the existing standard-library timestamp conversion. No cross-request
  metadata cache: edits and deletion must become visible on the next request.
* Run meaningful regressions, the strict earlier milestone gate, and paired
  measurements against the preserved original binary. Retain changes only if
  correctness holds and measured results support them.

## Limits and follow-up

This is Windows profiling only. Merged baseline CI passes all 54 suites on
Windows/Linux/macOS and Linux ASan/UBSan; see `merged-main-ci.json`. Those results
do not validate later optimization changes. Kernel ETW CPU sampling was
unavailable without elevation; the per-process sampler is explicitly scoped.
No cold-disk, remote-network, sustained multi-hour soak, WAN loss, or hardware
performance-counter study was run. Shared-host scheduling, thermal state, and
generator CPU affect measurements, particularly high-throughput warm-cache runs.
Existing controlled-tree path checks are not an atomic defense against hostile
concurrent directory replacement. This optimization must not weaken that contract.
