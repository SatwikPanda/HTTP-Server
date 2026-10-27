"""Summarize complete native load trials without dropping failed trials."""
import argparse
from collections import defaultdict
import csv
import json
from pathlib import Path
import statistics


def summarize(root):
    groups = defaultdict(list)
    failures = []
    for path in sorted(root.rglob('record.json')):
        trial = json.loads(path.read_text())
        if trial['state'] != 'complete':
            failures.append(dict(path=str(path), diagnostics=trial['diagnostics']))
            continue
        summary = trial['oha']['summary']
        count = sum(trial['oha']['statusCodeDistribution'].values())
        cpu = trial['after']['cpu_seconds'] - trial['before']['cpu_seconds']
        last = trial['samples'][-1]
        generators = [sample for sample in trial['samples'] if 'generator' in sample]
        generator_cpu = generators[-1]['generator']['cpu_seconds'] - trial['generator_before']['cpu_seconds']
        value = dict(path=str(path), label=trial['label'], case=trial['case'], driver=trial['driver'],
                     profile=trial['profile'], clients=trial['clients'], workers=trial['workers'],
                     offered_rate=trial.get('offered_rate', 0), requests_per_second=summary['requestsPerSec'],
                     p99_ms=trial['oha']['latencyPercentiles']['p99'] * 1000,
                     p50_ms=trial['oha']['latencyPercentiles']['p50'] * 1000,
                     server_cores=cpu / summary['total'], cpu_us_per_request=cpu * 1e6 / count,
                     generator_cores_approx=generator_cpu / max(.001, generators[-1]['seconds']),
                     max_rss_mib=max(s['server']['rss_bytes'] for s in trial['samples']) / 1024**2,
                     max_handles=max(s['server']['handles'] for s in trial['samples']),
                     max_threads=max(s['server']['threads'] for s in trial['samples']),
                     completed_requests=count)
        key = (value['label'], value['case'], value['driver'], value['profile'], value['clients'], value['offered_rate'])
        groups[key].append(value)
    results = []
    for key, trials in groups.items():
        group = dict(label=key[0], case=key[1], driver=key[2], profile=key[3], clients=key[4], offered_rate=key[5], n=len(trials), trials=trials)
        for metric in ('requests_per_second', 'p99_ms', 'p50_ms', 'server_cores', 'cpu_us_per_request', 'generator_cores_approx', 'max_rss_mib', 'max_handles', 'max_threads'):
            values = [t[metric] for t in trials]
            group[metric] = dict(median=statistics.median(values), min=min(values), max=max(values))
        results.append(group)
    return dict(groups=results, failures=failures,
                note='Latency summaries are medians of per-trial percentiles, not a pooled percentile; generator CPU stops at last live sample.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    report = summarize(args.root)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print('| Label | Case | Driver | n | requests/s median [range] | p99 ms | CPU us/request | Server cores | Generator cores |')
    print('|---|---|---|---:|---:|---:|---:|---:|---:|')
    for g in report['groups']:
        r = g['requests_per_second']
        print(f"| {g['label']} | {g['case']} | {g['driver']} | {g['n']} | {r['median']:.0f} [{r['min']:.0f}, {r['max']:.0f}] | {g['p99_ms']['median']:.3f} | {g['cpu_us_per_request']['median']:.1f} | {g['server_cores']['median']:.2f} | {g['generator_cores_approx']['median']:.2f} |")
    for failure in report['failures']:
        print('FAILED:', failure)
