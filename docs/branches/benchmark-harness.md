# Benchmark harness handoff — J / milestone 31

Implemented on `benchmark-harness` and integrated into `main`. Checkpoint
`460203f` introduced the scenario manifest, explicit executable profiles,
controlled origin/client, scheduling, raw results and the demo smoke. `2789bf9`
completes the real executable profile, both-driver smoke matrix, error/retry
accounting, bounded diagnostics, cleanup tests and documentation.

The production dependency is the assembled executable from A–I/K plus L's
runtime wiring. Python 3.10+ is test-only. `benchmarks/README.md` documents exact
commands, schema version 1, settings, cache preparation, histograms, full-run
qualification and resource limitations.

Executed: all 11 non-demo families on both drivers under the Windows GCC,
Clang and MSVC 52-suite strict gates; details are in the integration record. Additional
GCC smoke validates 16 MiB warm cached objects on both drivers and open-loop new
connections with a periodic server metrics snapshot. Unit checks exercise wrong/
truncated/HEAD bodies, zero successful throughput for corrupt data, deterministic
seeds, bounded open-loop overload, malformed records and cancellation/child cleanup.

Two smoke-discovered fixture errors were fixed: concurrent valid 304 version
conflicts must allow K's one bounded unconditional retry, and origin throttling
must delay delivery before a chunk rather than keeping a completed connection
occupied afterward. Neither adjustment counts an incorrect body as successful.

Raw files live under each build directory's `benchmark-smoke`/`extended-smoke`
or `benchmarks/results`; they are ignored generated artifacts. Each repeat is
saved independently and failed records remain available. Windows process cleanup
is explicitly marked forced; I separately tests graceful shutdown in process.
Missing metrics remain unavailable, and lifetime server snapshots are not interval
deltas. No qualified M performance run or N comparison is claimed.

N takes result/report ownership after qualification. The strict Windows/Linux/macOS
CI gates, including J's three suites, now pass; see the [CI record](ci-portability.md)
and [integration record](application-assembly.md). Full performance runs remain pending.
