"""Regenerate app figures/tables from retained qualification and load records."""
import argparse
import csv
import json
from pathlib import Path
import statistics
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from qualify import check_junit, require, sha256

LABELS = {"idle": "Idle", "get-c1": "GET · 1 client", "get-c4": "GET · 4 clients",
          "get-c16": "GET · 16 clients", "get-new-c4": "GET · 4, new TCP",
          "head-c4": "HEAD · 4 clients", "missing-c4": "404 · 4 clients", "soak-c4": "GET · 60 s soak"}


def spread(values):
    require(values and all(v >= 0 for v in values), "missing/negative measurement")
    return dict(median=statistics.median(values), min=min(values), max=max(values))


def summarize(records):
    require(records and all(r["state"] == "passed" for r in records), "failed/missing trials cannot be graphed as successful")
    require(len({(r["variant"], r["case"]["id"], r["repeat"]) for r in records}) == len(records), "duplicate trial")
    groups = {}
    for record in records:
        groups.setdefault((record["variant"], record["case"]["id"]), []).append(record)
    rows = []
    for (variant, case), runs in groups.items():
        row = dict(variant=variant, case=case, repeats=len(runs),
                   cpu_percent=spread([r["server_cpu_percent"] for r in runs]),
                   rss_mib=spread([r["sampled_peak_rss_bytes"] / 1048576 for r in runs]),
                   peak_threads=max(r["peak_threads"] for r in runs), peak_handles=max(r["peak_handles"] for r in runs),
                   requests=sum(r.get("requests", 0) for r in runs), errors=0)
        if case != "idle":
            for record in runs:
                expected = str(record["case"]["status"])
                result = record["oha"]
                require(set(result["statusCodeDistribution"]) == {expected} and not result["errorDistribution"], "load errors")
                require(sum(result["statusCodeDistribution"].values()) == record["requests"], "request accounting mismatch")
            row["rps"] = spread([r["requests"] / r["oha"]["summary"]["total"] for r in runs])
            for percentile in ("p50", "p95", "p99"):
                # This is the distribution of per-run percentiles, not a pooled percentile.
                row[percentile + "_ms"] = spread([r["oha"]["latencyPercentiles"][percentile] * 1000 for r in runs])
            row["body_mib_s"] = spread([r["oha"]["summary"]["totalData"] / r["oha"]["summary"]["total"] / 1048576 for r in runs])
        rows.append(row)
    return rows


def read_stress(raw, variants):
    manifest = json.loads((raw / "campaign.json").read_text())
    require({v["label"]: v["binary_sha256"] for v in manifest["variants"]} ==
            {v["label"]: v["binary_sha256"] for v in variants}, "stress campaign used different executables")
    trials, errors, paths = [], {}, [raw / "campaign.json"]
    for name in manifest["trials"]:
        path = raw / name / "record.json"
        paths.append(path)
        record = json.loads(path.read_text())
        row = dict(variant=record["variant"], case=record["case"]["id"], repeat=record["repeat"], state=record["state"],
                   errors=0, requests=0, diagnostics=record["diagnostics"], phases={})
        for phase in ("warmup", "oha"):
            path = raw / name / (phase + ".json")
            if not path.exists():
                continue
            paths.append(path)
            result = json.loads(path.read_text())
            phase_errors = sum(result["errorDistribution"].values())
            row["errors"] += phase_errors
            row["phases"][phase] = dict(errors=phase_errors, requests=sum(result["statusCodeDistribution"].values()),
                                       statuses=result["statusCodeDistribution"], duration=result["summary"]["total"])
            if phase == "oha":
                row["requests"] = row["phases"][phase]["requests"]
            for error, count in result["errorDistribution"].items():
                errors[error] = errors.get(error, 0) + count
        trials.append(row)
    require(len(trials) == len(variants) * (7 * manifest["settings"]["repeats"] + 1), "incomplete stress inventory")
    return dict(state=manifest["state"], trials=trials, errors=errors, source=manifest["source"],
                passed=sum(r["state"] == "passed" for r in trials), failed=sum(r["state"] != "passed" for r in trials),
                inputs=[dict(path=str(p.relative_to(ROOT)), sha256=sha256(p)) for p in paths])


def generate(raw, output, stress=None):
    require(not output.exists(), "select a fresh report directory")
    manifest = json.loads((raw / "campaign.json").read_text())
    require(manifest["state"] == "passed", "campaign did not pass")
    paths = [raw / name / "record.json" for name in manifest["trials"]]
    require(len(set(paths)) == len(paths), "duplicate manifest entry")
    records = [json.loads(path.read_text()) for path in paths]
    rows = summarize(records)
    stress_result = read_stress(stress, manifest["variants"]) if stress else None
    qualifications = {}
    for variant in manifest["variants"]:
        path = ROOT / variant["qualification"]
        require(sha256(path) == variant["qualification_sha256"], "qualification changed")
        record = json.loads(path.read_text())
        require(record["state"] == "passed" and record["source"]["sha256"] == manifest["source"]["sha256"], "qualification source mismatch")
        require(record["additional_binaries"]["http_app"]["sha256"] == variant["binary_sha256"], "qualification binary mismatch")
        check_junit(path.with_suffix(".junit.xml"), record["expected"])
        qualifications[variant["label"]] = record
        require(all(check["passed"] for check in manifest["functional"][variant["label"]]), "functional failure")
        actual = {(r["case"]["id"], r["repeat"]) for r in records if r["variant"] == variant["label"]}
        expected = {(case, repeat) for case in LABELS if case != "soak-c4" for repeat in range(1, manifest["settings"]["repeats"] + 1)} | {("soak-c4", 1)}
        require(actual == expected, "missing/extra trial")
    output.mkdir(parents=True)
    summary = dict(schema_version=1, evidence="local Windows candidate measurements; not a cross-platform capacity claim",
                   source=manifest["source"], environment=manifest["environment"], settings=manifest["settings"],
                   variants=manifest["variants"], measurements=rows, functional=manifest["functional"], stress=stress_result,
                   qualification_tests={key: value["tests"] for key, value in qualifications.items()},
                   inputs=[dict(path=str(path.relative_to(ROOT)), sha256=sha256(path)) for path in [raw / "campaign.json", *paths]])
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    with (output / "trials.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(["variant", "case", "repeat", "state", "requests", "rps", "p50_ms", "p95_ms", "p99_ms", "cpu_percent_one_core", "sampled_peak_rss_mib", "peak_threads", "peak_handles"])
        for r in records:
            result = r.get("oha")
            writer.writerow([r["variant"], r["case"]["id"], r["repeat"], r["state"], r.get("requests", 0),
                             r["requests"] / result["summary"]["total"] if result else "",
                             *[result["latencyPercentiles"][p] * 1000 if result else "" for p in ("p50", "p95", "p99")],
                             r["server_cpu_percent"], r["sampled_peak_rss_bytes"] / 1048576, r["peak_threads"], r["peak_handles"]])
    plots(output, manifest, rows, records, qualifications, stress_result)
    lines = ["# Hello World app: local Windows results", "", "Measured executable: `http_app`, default four-worker pool, 100 requests per connection, loopback HTTP/1.1. Both the native load generator and server share this host.", "",
             "Five repeats per point: 1 s warmup and 5 s load, or 3 s idle. Compiler order alternates each round. Each build also runs one 60 s soak. All original trials are retained. These are local candidate measurements, not universal capacity or cross-platform results.", "",
             f"The controlled campaign caps reused-connection workloads at **{manifest['settings'].get('offered_rate', 0):g} requests/s**, and new TCP connections at **{manifest['settings'].get('new_connection_rate', 0):g} requests/s**. Throughput is delivery at these offered loads, not maximum capacity. The separate unrestricted stress results are retained below, including failures.", "",
             "CPU uses process user+kernel time divided by sampled wall time; **100% = one logical core**, not the whole machine. RAM is the maximum sampled working set/RSS every 100 ms, including shared pages. Binary size is the built, unstripped executable only, excluding PDBs, DLLs and deployment dependencies. Compiler sizes are affected by linker/runtime settings.", "",
             "Rates, CPU and sampled peaks below are per-run medians. P50/P95/P99 are **medians of per-run percentiles**, not pooled percentiles. Error bars in graphs show minimum/maximum across five repeats. Full spreads are retained in `summary.json` and `trials.csv`. Soak has only one trial per build.", "",
             "| Build | Workload | Repeats | Requests/s | P50 ms | P95 ms | P99 ms | CPU % | Peak RAM MiB | Threads | Handles |",
             "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for r in rows:
        values = [f"{r[k]['median']:,.3f}" if k in r else "—" for k in ("rps", "p50_ms", "p95_ms", "p99_ms", "cpu_percent", "rss_mib")]
        lines.append(f"| {r['variant']} | {LABELS[r['case']]} | {r['repeats']} | " + " | ".join(values) + f" | {r['peak_threads']} | {r['peak_handles']} |")
    if stress_result:
        lines += ["", "## Unrestricted stress: failures retained", "",
                  f"**{stress_result['passed']}/{len(stress_result['trials'])} trials passed; {stress_result['failed']} failed** during warmup or measurement. This campaign used the same executable hashes without rate caps. Windows client socket-address errors (10048) and timeouts were recorded; the app's 100-request limit creates substantial connection churn at unrestricted rates. The controlled runs above do not erase these failures.", "",
                  "A read-only diagnostic found the Windows TCP dynamic range was 49,152–65,535 (16,384 ports), with 7,601 client sockets in TIME_WAIT for completed benchmark endpoints at the observation instant. This is evidence of client-side port pressure, not an application HTTP validation failure. No networking settings were changed.", "",
                  "Error counts include warmup and timed measurement, and are shown separately by phase in `summary.json`. Failed trials are not included in the controlled performance medians.", "",
                  "| Native error | Occurrences |", "| --- | ---: |"]
        lines += [f"| {error} | {count:,} |" for error, count in stress_result["errors"].items()]
        lines += ["", "![Every unrestricted stress trial, including failures](stress.png)", ""]
    lines += ["", "The native load generator checks response status and aggregate body bytes; exact content is checked immediately before and after each load run. The functional checks additionally validate bodies, HEAD metadata, malformed requests, pipeline ordering, CLI errors and the 100-request cap. This does not claim per-request body hashing during timed load.", "",
              "Windows benchmark children are stopped through their owned process handles; this is forced shutdown. Graceful lifecycle and cleanup are covered by the C++ CTest suites, not inferred from benchmark cleanup. A 60-second working-set trace is not proof of leak freedom. CPU/RSS do not include the generator or all kernel networking costs.", "",
              "![App measurements](overview.png)", "", "![All regression suites](test-suites.png)", "", "![Executable functional checks](functional.png)", "", "![Soak resource traces](soak.png)", "",
              "## Reproduction", "", "See the root README for qualification and campaign commands. Run `python benchmarks/app_report.py --raw benchmarks/results/app-controlled-2026-10-27 --stress benchmarks/results/app-2026-10-27 --output build/app-report-reproduction` to regenerate figures with matplotlib. `psutil` and `oha` are required only for measurement. Runtime dependencies are unchanged.", ""]
    (output / "report.md").write_text("\n".join(lines), encoding="utf-8")
    artifacts = sorted(p for p in output.iterdir() if p.is_file())
    (output / "ARTIFACTS.sha256").write_text("\n".join(f"{sha256(p)}  {p.name}" for p in artifacts) + "\n", encoding="utf-8")
    return summary


def plots(output, manifest, rows, records, qualifications, stress=None):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import numpy as np
    plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 10, "axes.spines.top": False,
                         "axes.spines.right": False, "axes.titleweight": "bold", "figure.facecolor": "#f6f8fc", "axes.facecolor": "#ffffff"})
    variants = [v["label"] for v in manifest["variants"]]
    colors = ["#1768ac", "#d36b27", "#426e48", "#8b4ba0"]
    require(len(variants) <= len(colors), "too many plotted variants")
    def finish(fig, name):
        fig.savefig(output / (name + ".png"), dpi=160, bbox_inches="tight")
        fig.savefig(output / (name + ".svg"), bbox_inches="tight", metadata={"Date": None})
        plt.close(fig)
    fig, axes = plt.subplots(2, 3, figsize=(16, 10), constrained_layout=True)
    rate, new_rate = manifest["settings"].get("offered_rate", 0), manifest["settings"].get("new_connection_rate", 0)
    fig.suptitle(f"Hello World app · controlled localhost load\nDefault 4-worker pool · {manifest['settings']['repeats']} repeats · {rate:,.0f} req/s cap (new TCP: {new_rate:g}/s)", fontsize=19)
    cases = [case for case in LABELS if case not in {"idle", "soak-c4"}]
    for ax, metric, title, unit in zip(axes.flat[:4], ["rps", "p99_ms", "cpu_percent", "rss_mib"],
                                     ["Delivered at configured rate caps", "Tail latency", "Server CPU", "Server sampled peak RAM"],
                                     ["Responses / second", "Median per-run p99 (ms)", "CPU % · 100% = one core", "Working set / RSS (MiB)"]):
        selected = ["idle", *cases] if metric in {"cpu_percent", "rss_mib"} else cases
        x = np.arange(len(selected))
        width = .72 / len(variants)
        for index, variant in enumerate(variants):
            group = [next(r for r in rows if r["variant"] == variant and r["case"] == case)[metric] for case in selected]
            median = np.array([g["median"] for g in group])
            ax.bar(x + (index - (len(variants) - 1) / 2) * width, median, width, color=colors[index], label=variant.upper(),
                   yerr=[median - [g["min"] for g in group], np.array([g["max"] for g in group]) - median], capsize=2)
        ax.set_xticks(x, [LABELS[c] for c in selected], rotation=35, ha="right", fontsize=8)
        ax.set(title=title, ylabel=unit)
        ax.grid(axis="y", alpha=.18)
        ax.set_axisbelow(True)
        ax.legend(fontsize=8)
    ax = axes[1, 1]
    sizes = [v["binary_bytes"] / 1048576 for v in manifest["variants"]]
    bars = ax.bar([v.upper() for v in variants], sizes, color=colors[:len(variants)], width=.55)
    ax.bar_label(bars, labels=[f"{size:.3f} MiB" for size in sizes], padding=5)
    ax.set(title="Built executable size", ylabel="MiB · EXE only; unstripped", ylim=(0, max(sizes) * 1.2))
    ax = axes[1, 2]
    y = np.arange(len(variants))
    suites = [len(qualifications[v]["tests"]) for v in variants]
    checks = [len(manifest["functional"][v]) for v in variants]
    a = ax.barh(y + .18, suites, .32, color="#1768ac", label="CTest suites")
    b = ax.barh(y - .18, checks, .32, color="#3c845a", label="App executable checks")
    ax.bar_label(a, padding=3); ax.bar_label(b, padding=3)
    ax.set_yticks(y, [v.upper() for v in variants]); ax.set(title="All checks passed", xlabel="Passing checks", xlim=(0, max(suites) * 1.2))
    ax.legend(fontsize=8)
    finish(fig, "overview")

    names = [t["name"] for t in qualifications[variants[0]]["tests"]]
    fig, axes = plt.subplots(1, 2, figsize=(18, 13), constrained_layout=True)
    fig.suptitle(f"Every regression suite passed · {len(names)} per compiler\nDuration is test runtime, not a server throughput score", fontsize=19)
    split = (len(names) + 1) // 2
    for ax, group in zip(axes, [names[:split], names[split:]]):
        y = np.arange(len(group))
        for index, variant in enumerate(variants):
            lookup = {t["name"]: t for t in qualifications[variant]["tests"]}
            require(set(lookup) == set(names) and all(t["passed"] for t in lookup.values()), "different/failed test inventory")
            values = [max(lookup[name]["seconds"], .001) for name in group]
            ax.barh(y + (index - .5) * .32, values, .3, color=colors[index], label=variant.upper())
        ax.set_yticks(y, group, fontsize=8)
        ax.invert_yaxis(); ax.set_xscale("log"); ax.set_xlabel("Seconds · logarithmic scale (floor 0.001 s)")
        ax.grid(axis="x", alpha=.2); ax.set_axisbelow(True); ax.legend(fontsize=9)
    finish(fig, "test-suites")

    checks = manifest["functional"][variants[0]]
    fig, ax = plt.subplots(figsize=(11, 9), constrained_layout=True)
    fig.suptitle("Actual http_app executable · functional checks\nEach check runs against both compiler builds", fontsize=18)
    ax.imshow(np.ones((len(checks), len(variants))), cmap=matplotlib.colors.ListedColormap(["#d5efdf"]), aspect="auto")
    ax.set_xticks(range(len(variants)), [v.upper() for v in variants]); ax.xaxis.tick_top()
    ax.set_yticks(range(len(checks)), [c["name"] for c in checks])
    for x, variant in enumerate(variants):
        require([c["name"] for c in manifest["functional"][variant]] == [c["name"] for c in checks], "different functional inventory")
        for y in range(len(checks)):
            ax.text(x, y, "PASS", ha="center", va="center", color="#226d43", fontsize=9)
    finish(fig, "functional")

    fig, axes = plt.subplots(2, 2, figsize=(15, 8), constrained_layout=True)
    fig.suptitle(f"{manifest['settings']['soak_seconds']:g}-second sustained GET · 4 clients\n100 ms samples · one trial per compiler", fontsize=19)
    for index, variant in enumerate(variants):
        record = next(r for r in records if r["variant"] == variant and r["case"]["id"] == "soak-c4")
        samples = record["samples"]
        xs = [s["seconds"] for s in samples]
        # Bin CPU into one-second windows to avoid presenting Windows timer quantization as spikes.
        bins = [samples[0]]
        for sample in samples[1:]:
            if sample["seconds"] - bins[-1]["seconds"] >= 1:
                bins.append(sample)
        cpu = [100 * (b["server"]["cpu_seconds"] - a["server"]["cpu_seconds"]) / (b["seconds"] - a["seconds"]) for a, b in zip(bins, bins[1:])]
        axes[0, 0].plot([s["seconds"] for s in bins[1:]], cpu, color=colors[index], label=variant.upper())
        for ax, field, divisor in [(axes[0, 1], "rss_bytes", 1048576), (axes[1, 0], "handles", 1), (axes[1, 1], "threads", 1)]:
            ax.plot(xs, [s["server"][field] / divisor for s in samples], color=colors[index], label=variant.upper())
    for ax, title, unit in zip(axes.flat, ["CPU during load", "RAM during load", "Open process handles", "Process threads"], ["CPU % · one core = 100%", "Working set / RSS (MiB)", "Handles", "Threads"]):
        ax.set(title=title, xlabel="Elapsed seconds", ylabel=unit); ax.grid(alpha=.2); ax.legend(); ax.set_ylim(bottom=0)
    finish(fig, "soak")
    if stress:
        group = [(v, case) for v in variants for case in LABELS]
        matrix = np.full((len(group), 5), -1)
        lookup = {(r["variant"], r["case"], r["repeat"]): r for r in stress["trials"]}
        for y, (variant, case) in enumerate(group):
            for x in range(5):
                row = lookup.get((variant, case, x + 1))
                if row:
                    matrix[y, x] = 1 if row["state"] == "passed" else 0
        fig, ax = plt.subplots(figsize=(14, 9), constrained_layout=True)
        fig.suptitle(f"Unrestricted stress · all 72 trials retained\n{stress['passed']} passed / {stress['failed']} failed · client port pressure at high connection churn", fontsize=19)
        ax.imshow(matrix, cmap=matplotlib.colors.ListedColormap(["#eceff3", "#f7cdcc", "#cdebdc"]), vmin=-1, vmax=1, aspect="auto")
        ax.set_yticks(range(len(group)), [f"{v.upper()} · {LABELS[c]}" for v, c in group], fontsize=9)
        ax.set_xticks(range(5), [f"Repeat {i}" for i in range(1, 6)]); ax.xaxis.tick_top()
        for y, (variant, case) in enumerate(group):
            for x in range(5):
                row = lookup.get((variant, case, x + 1))
                text = ("PASS" if row["state"] == "passed" else f"FAIL · {row['errors']:,} errors") if row else "Not scheduled"
                ax.text(x, y, text, ha="center", va="center", fontsize=8)
        finish(fig, "stress")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--raw", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--stress", type=Path, help="Optional retained unrestricted campaign (including failures)")
    arguments = parser.parse_args()
    generate(arguments.raw.resolve(), arguments.output.resolve(), arguments.stress.resolve() if arguments.stress else None)
