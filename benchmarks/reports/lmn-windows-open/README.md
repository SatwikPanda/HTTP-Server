# Local candidate results — interpretation and reproduction

Read this alongside the generated [report](report.md), [summary JSON](summary.json)
and [per-repeat CSV](repeats.csv). These are provisional observations, not final
milestone 32 acceptance or a maximum-throughput benchmark.

All 520 individual trials completed successfully (104 configurations × five).
There were 3,120 validated measured responses and no request errors, timeouts or
overload drops. However, the campaign wrapper's final provenance check failed:
`src/main.cpp` was reformatted concurrently. Its manifest honestly remains
`state=interrupted`. Records identify the original measured source
`b3013e991e6e2ebb65ade817274615654c671f3d4669ae965eeb84841f29c468` and executable
`45ec3c94e57f0875cdfaf0eecda1a6d4753de43506253e66bd8ef81447a3efd3`.
They must not be relabeled as the newer formatted source or a qualified campaign.

The shared Windows desktop ran Clang Release, a Python generator and an in-process
origin. Runs used 0.2 s warmup, a 1.2 s issue window, five open arrivals/s and seed
1729. The x-axis is the configured client limit (1/10/50), not measured simultaneous
client activity. Six measured responses per trial mean only 30 latency samples per
configuration; p99 is a histogram upper bound near the largest observed sample.
Twenty trials raised scheduling/saturation hints. All remain in the tables.

Throughput is successful work divided by actual elapsed phase time, including
tail drain. Points near five requests/s reflect the offered rate, not capacity.
Longer fixed-pool latencies when clients exceed workers are consistent with
persistent sessions occupying workers, but this is a workload-specific observation.
Unequal CPU budgets, shared-host scheduling and uncontrolled filesystem cache
prevent a universal driver ranking. CPU/RSS and cache/queue samples retain their
interval or process-lifetime scope in the raw records and summary.

The [validation record](../../../docs/validation/lmn.md) includes exact gate results,
host information, known limitations, initial failed runs and archive hashes.

## Regenerate from retained raw records

From the repository root, using a new extraction/output directory:

```powershell
python -m zipfile -e docs/validation/lmn-windows/lmn-windows-open-fixed-raw.zip build/lmn-raw-reproduction
python -m venv build/results-tools
./build/results-tools/Scripts/python.exe -m pip install -r docs/validation/lmn-windows/python-packages.txt
./build/results-tools/Scripts/python.exe benchmarks/report.py --raw build/lmn-raw-reproduction/lmn-windows-open-fixed/raw --output build/lmn-report-reproduction
```

For POSIX use `build/results-tools/bin/python`. The generated CSV/configuration
statistics should match the saved report; source paths differ when data is
extracted elsewhere. PNG/SVG files are standalone and can be viewed independently.
The report uses all raw files and does not omit failed records. The separate
initial campaign archive is intentionally not mixed into this dataset because
its generator implementation differs.

To repeat the experiment on a current locally qualified candidate, select new
output filenames and directories:

```powershell
python tools/qualify.py --build-dir build/release --binary build/release/httpserver.exe --output build/release/qualification.json
./build/results-tools/Scripts/python.exe benchmarks/campaign.py --binary build/release/httpserver.exe --build-dir build/release --candidate-record build/release/qualification.json --output benchmarks/results/new-local-candidate --max-connections 50 --max-dataset-mib 256 --warmup 0.2 --measurement 1.2 --repeats 5 --arrival open --rate 5
```

Keep the source tree and binary unchanged during qualification and measurement.
Use longer windows for interpretable performance conclusions after the native
platform qualification is complete. The campaign's plan records every excluded
point before launch; create its `STOP` file to request orderly interruption.
