# N — reproducible results and developer handoff

Branch: `codex/lmn-completion`, based on `e24077f`. The completed A–L executable
is used directly. The result tooling and native Windows candidate runs are
separate from M's later native qualification. Windows/Linux/macOS and Linux
ASan/UBSan now pass the 54-suite gate; the short candidate measurements remain
tied to their older source/binary identity and are not promoted retroactively.

## Implementation

`benchmarks/campaign.py` expands J's anchor matrix, saves exclusions before
starting work, alternates configuration order between rounds, and retains at
least five repeats per admitted point. Each repeat owns its temporary files,
origin and process handles. Failures remain in the raw records; a failure does
not suppress the remaining scheduled repeats. `campaign.json` records every
trial and whether the whole campaign completed, failed or was interrupted.

`benchmarks/report.py` regenerates per-repeat CSV, grouped JSON, a Markdown report,
and standalone PNG/SVG throughput and p99 plots. It groups by all scenario
settings, source/binary identity, host identity and measurement windows. Distinct
CPU budgets, reuse, arrival models, object sizes and seeds cannot merge silently.
Rates use per-repeat median and min/max. Latency percentiles are recomputed from
compatible raw histogram counts; percentile values are never averaged. Missing,
failed or incomplete points are absent from plots and remain visible in tables.

`results_tool_test` rejects missing/skipped test evidence, corrupt/duplicate raw
records and incompatible histogram buckets. It checks pooled percentile math,
failure grouping, separate configurations and reproducible reports.

N also fixes J's sparse open-arrival denominator: if the last issued request
finishes before the configured measurement window ends, the remaining idle time
is now included. A fast single completion no longer reports its inverse latency
as the offered-window throughput. A controlled-origin regression checks this.

An initial candidate campaign also exposed an open-loop generator drain cycle:
idle persistent clients pinned finite server workers while the generator waited
for queued requests before closing those idle clients. Sentinels now close idle
connections before the final queue join; a one-worker I/O model reproduces the
failure and checks three clients complete without timeout. The interrupted
original campaign and failed trial records are retained separately, not used for
the corrected comparison. To stop a campaign gracefully, create a `STOP` file in
its output directory; it finishes cleanup of the current trial before stopping.

## Evidence rules

Smoke runs remain `evidence=smoke`. Repeated local trials require a passing
`--candidate-record` from `tools/qualify.py`, matching the exact code fingerprint
and uninstrumented Release binary. They are explicitly `evidence=candidate`.
Dirty/uncommitted candidate data cannot become publishable results merely by
passing an attestation flag.

Final `evidence=measurement` runs require a clean matching commit plus native
Windows, Linux and Darwin qualification records with matching source hashes,
passing tests, Release configuration and the actual local executable hash.
The record files are execution evidence, not signed third-party certificates.
Their provenance must still be reviewed. A runtime change returns to M before N
can produce a final comparison.

## Reproduction

Production builds require only C++20/CMake and OS APIs. Python 3.10+ is test-only;
matplotlib is needed for plots, and optional psutil supplies external CPU/RSS/RAM
measurements. An isolated environment keeps these outside production:

```powershell
python -m venv build/results-tools
./build/results-tools/Scripts/python.exe -m pip install matplotlib psutil
python tools/qualify.py --build-dir build/release --binary build/release/httpserver.exe --output build/release/qualification.json
./build/results-tools/Scripts/python.exe benchmarks/campaign.py --binary build/release/httpserver.exe --build-dir build/release --candidate-record build/release/qualification.json --output benchmarks/results/local-candidate --max-connections 10 --warmup 1 --measurement 5 --repeats 5
./build/results-tools/Scripts/python.exe benchmarks/report.py --raw benchmarks/results/local-candidate/raw --output benchmarks/reports/local-candidate
```

For POSIX use the venv's `bin/python`, and omit `.exe`. For Visual Studio use
`build/release/Release/httpserver.exe`. Use a new output directory per campaign
and report. `--arrival open --rate N` selects a separately labeled arrival-rate
experiment. It measures latency/useful delivery at that offered load, not maximum
capacity. No failed trial is reported as zero performance or silently retried.

The [README](../../README.md), [configuration reference](../configuration.md),
[test instructions](../../tests/README.md), [J harness guide](../../benchmarks/README.md)
and [M qualification guide](correctness-gate.md) document actual commands and
limits. The synchronous resolver can extend helper shutdown beyond the drain
deadline. Filesystem cache is uncontrolled unless explicitly prepared externally.
The generator and origin share the server's host, so measurements cannot be
extrapolated to a separate generator or other OS.

Executed campaign details, raw artifacts, exclusions and observations are linked
from the [validation summary](../validation/lmn.md). Final N acceptance remains
open until the now-qualified committed candidate is measured with suitable
windows. See [the native CI evidence](../validation/lmn-ci/README.md).
