"""Recompute statistics from retained repeats; never average latency percentiles."""
import csv
import hashlib
import json
import math
import statistics
from collections import defaultdict
from pathlib import Path

from .spec import load_json, require, validate_result


def merged_latency(phases):
    require(phases, "no latency samples")
    bounds = phases[0]["success_latency"]["upper_bounds_us"]
    counts = [0] * len(bounds)
    for phase in phases:
        histogram = phase["success_latency"]
        require(histogram["upper_bounds_us"] == bounds, "incompatible latency buckets")
        counts = [a + b for a, b in zip(counts, histogram["counts"])]
    count = sum(counts)
    result = {}
    for name, fraction in (("p50", .50), ("p95", .95), ("p99", .99)):
        result[name] = None
        if count:
            total = 0
            for bound, frequency in zip(bounds, counts):
                total += frequency
                if total >= math.ceil(count * fraction):
                    result[name] = bound
                    break
    return dict(unit="microseconds", upper_bounds_us=bounds, counts=counts, count=count,
                percentile_upper_bounds_us=result)


def spread(values):
    return dict(min=min(values), median=statistics.median(values), max=max(values)) if values else None


def configuration(record):
    provenance = record["provenance"]
    environment = record["environment"]
    phases = record["phases"]
    return dict(scenario=record["scenario"], evidence=record["evidence"],
                source=provenance.get("candidate_source_sha256", provenance.get("tree")),
                binary=provenance.get("binary_sha256"), profile=record.get("profile"),
                environment={key: environment.get(key) for key in ("os", "cpu", "logical_cpus", "ram_bytes")},
                warmup_seconds=record.get("timing", {}).get("warmup_seconds", phases.get("warmup", {}).get("requested_issue_window_seconds")),
                measurement_seconds=record.get("timing", {}).get("measurement_seconds", phases.get("measurement", {}).get("requested_issue_window_seconds")))


def summarize(records):
    groups = defaultdict(list)
    seen = set()
    for record in records:
        validate_result(record)
        require(record["run_id"] not in seen, "duplicate run ID")
        seen.add(record["run_id"])
        groups[json.dumps(configuration(record), sort_keys=True)].append(record)
    rows = []
    for key, repeats in sorted(groups.items()):
        valid = [r for r in repeats if r["state"] == "complete"]
        phases = [r["phases"]["measurement"] for r in valid]
        rates = [p["counts"]["successful"] / p["duration_seconds"] for p in phases]
        bandwidth = [p["counts"]["useful_bytes"] / p["duration_seconds"] for p in phases]
        cpus = []
        for r in valid:
            before, after = r.get("server_process_before", {}), r.get("server_process_after", {})
            if before.get("cpu_seconds") is not None and after.get("cpu_seconds") is not None:
                cpus.append((after["cpu_seconds"] - before["cpu_seconds"]) / r["phases"]["measurement"]["duration_seconds"])
        config = json.loads(key)
        complete = len(valid) >= 5 and len(valid) == len(repeats)
        row = dict(configuration=config, repeats=len(repeats), complete_repeats=len(valid),
                   failed_repeats=len(repeats) - len(valid), comparison_eligible=complete and config["evidence"] == "measurement",
                   successful_requests_per_second=spread(rates), useful_bytes_per_second=spread(bandwidth),
                   server_cpu_cores=spread(cpus),
                   server_sampled_peak_rss_bytes=spread([r["server_sampled_peak_rss_bytes"] for r in valid
                                                        if r.get("server_sampled_peak_rss_bytes") is not None]),
                   merged_success_latency=merged_latency(phases) if phases else None,
                   errors=sum(r["phases"].get("measurement", {}).get("counts", {}).get("errors", 0) for r in repeats),
                   timeouts=sum(r["phases"].get("measurement", {}).get("counts", {}).get("timeouts", 0) for r in repeats),
                   generator_saturation_repeats=sum(bool(p.get("saturation_suspected")) for p in phases),
                   run_ids=[r["run_id"] for r in repeats],
                   origin_measurement=[r.get("origin_measurement") for r in repeats],
                   server_lifetime_metrics=[r.get("server_metrics") for r in repeats])
        rows.append(row)
    return rows


def write_report(raw, output, include_smoke=False, plots=True):
    paths = sorted(raw.glob("*/result.json"))
    require(0 < len(paths) <= 10000, "expected 1..10000 raw repeat files")
    records = [load_json(path) for path in paths]
    require(include_smoke or all(r["evidence"] != "smoke" for r in records), "smoke evidence requires --include-smoke")
    rows = summarize(records)
    output.mkdir(parents=True, exist_ok=False)
    summary = dict(schema_version=1, provisional=any(not r["comparison_eligible"] for r in rows),
                   statistic="per-repeat median [min,max]; percentiles from pooled compatible successful-request buckets, upper bounds",
                   sources=[dict(path=str(p.resolve()), sha256=hashlib.sha256(p.read_bytes()).hexdigest()) for p in paths],
                   configurations=rows)
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    with (output / "repeats.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(["run_id", "evidence", "state", "family", "driver", "clients", "workers", "object_bytes", "reuse",
                         "seconds", "successful", "errors", "timeouts", "useful_bytes", "requests_per_second", "p50_us", "p95_us", "p99_us"])
        for r in records:
            s, p = r["scenario"], r["phases"].get("measurement", {})
            c = p.get("counts", {})
            percentiles = merged_latency([p])["percentile_upper_bounds_us"] if p else {}
            writer.writerow([r["run_id"], r["evidence"], r["state"], s["id"], s["driver"], s["concurrency"], s["workers"], s["object_bytes"], s["reuse"],
                             p.get("duration_seconds"), c.get("successful"), c.get("errors"), c.get("timeouts"), c.get("useful_bytes"),
                             c.get("successful", 0) / p["duration_seconds"] if p else None,
                             *[percentiles.get(key) for key in ("p50", "p95", "p99")]])
    lines = ["# Local repeated-run results", "", "Provisional candidate evidence; final M qualification is pending." if summary["provisional"] else "Qualified repeated measurements.", "",
             "Rates are per-repeat median [minimum, maximum]. Latencies pool raw histogram counts; values are bucket upper bounds. Failed trials remain in the raw data and error totals. CPU values are cores consumed, not percentages of the machine. RSS is sampled, not a guaranteed peak.", "",
             "| Family | Driver/workers | Clients | Bytes | Reuse | Passed/total | Requests/s median [min,max] | P99 (ms) | Errors |",
             "| --- | --- | ---: | ---: | --- | ---: | --- | ---: | ---: |"]
    for r in rows:
        s = r["configuration"]["scenario"]
        rate = r["successful_requests_per_second"]
        latency = r["merged_success_latency"]
        rate_text = f"{rate['median']:.1f} [{rate['min']:.1f}, {rate['max']:.1f}]" if rate else "unavailable"
        p99 = latency["percentile_upper_bounds_us"]["p99"] if latency else None
        p99_text = f"{p99 / 1000:.3f}" if p99 is not None else "unavailable"
        lines.append(f"| {s['id']} | {s['driver']}/{s['workers']} | {s['concurrency']} | {s['object_bytes']} | {s['reuse']} | {r['complete_repeats']}/{r['repeats']} | {rate_text} | {p99_text} | {r['errors']} |")
    lines += ["", "The server, Python generator and origin share one host. Generator saturation, short intervals, filesystem cache and unequal CPU budgets limit interpretation. These results do not establish a universal driver ranking. Exact settings, raw histograms, origin counts and process-lifetime cache/queue samples are in summary.json and each raw record.", ""]
    if plots:
        make_plots(rows, output, summary["provisional"])
        lines += ["![Throughput](throughput.png)", "", "![Latency](p99.png)", ""]
    (output / "report.md").write_text("\n".join(lines), encoding="utf-8")
    return summary


def make_plots(rows, output, provisional):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    # Plot only like-for-like concurrency sweeps; other axes stay in the tables.
    families = sorted({r["configuration"]["scenario"]["id"] for r in rows})
    for metric, filename, unit in (("rate", "throughput", "Successful requests / s"), ("latency", "p99", "P99 upper bound (ms)")):
        fig, axes = plt.subplots(math.ceil(len(families) / 3), 3, figsize=(15, 3.5 * math.ceil(len(families) / 3)), squeeze=False)
        for ax, family in zip(axes.flat, families):
            series = defaultdict(list)
            for r in rows:
                s = r["configuration"]["scenario"]
                if s["id"] == family and s["object_bytes"] == 65536 and s["reuse"]:
                    # Do not connect across distinct source, environment, timing or settings.
                    config = json.loads(json.dumps(r["configuration"]))
                    del config["scenario"]["concurrency"]
                    series[json.dumps(config, sort_keys=True)].append(r)
            for key, values in series.items():
                s = json.loads(key)["scenario"]
                points = []
                for r in values:
                    if r["complete_repeats"] < 5 or r["failed_repeats"]:
                        continue
                    x = r["configuration"]["scenario"]["concurrency"]
                    if metric == "rate":
                        v = r["successful_requests_per_second"]
                        points.append((x, v["median"], v["min"], v["max"]))
                    else:
                        v = r["merged_success_latency"]["percentile_upper_bounds_us"]["p99"]
                        if v is not None: points.append((x, v / 1000, v / 1000, v / 1000))
                points.sort()
                if points:
                    x, y, low, high = zip(*points)
                    ax.errorbar(x, y, yerr=[[b-a for a,b in zip(low,y)], [b-a for a,b in zip(y,high)]],
                                fmt="o", capsize=3, label=f"{s['driver']}/{s['workers']}, {s['arrival']}" +
                                    (f" {s['rate']}/s" if s['arrival'] == 'open' else ""))
            ax.set_title(family); ax.set_xlabel("Concurrent clients"); ax.set_ylabel(unit)
            ax.grid(alpha=.2)
            if ax.has_data(): ax.legend(fontsize=7)
        for ax in list(axes.flat)[len(families):]: ax.set_visible(False)
        fig.suptitle(("Provisional candidate — " if provisional else "") + "64 KiB, reused connections; missing/failed points excluded")
        fig.tight_layout(rect=(0, 0, 1, .96))
        fig.savefig(output / f"{filename}.png", dpi=160)
        fig.savefig(output / f"{filename}.svg")
        plt.close(fig)
