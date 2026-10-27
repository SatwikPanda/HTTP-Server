"""Reproduce paired aggregate tables and plots from retained native trial data."""
from collections import defaultdict
import argparse
import json
from pathlib import Path
import re
import statistics
import sys

sys.dont_write_bytecode = True

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from summarize import summarize

ROOT = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--study', type=Path, default=ROOT / 'raw/paired')
parser.add_argument('--summary', type=Path, default=ROOT / 'paired-summary.json')
parser.add_argument('--plot', type=Path, default=ROOT / 'comparison')
args = parser.parse_args()
study = args.study
manifest = json.loads((study / 'paired-manifest.json').read_text())
assert manifest['state'] == 'complete'
summary = summarize(study)
assert not summary['failures']
assert sum(group['n'] for group in summary['groups']) == manifest['expected_trials']
indexed = {(g['case'], g['driver'], g['label']): g for g in summary['groups']}
comparisons = []
for case in manifest.get('cases', 'static-1k,static-64k,proxy-off,cache-warm').split(','):
    for driver in ('event-loop', 'thread-pool'):
        before, after = (indexed[case, driver, label] for label in ('before', 'after'))
        paired = defaultdict(dict)
        for group in (before, after):
            for trial in group['trials']:
                number = int(re.search(r'pair-(\d+)-', trial['path'])[1])
                paired[number][group['label']] = trial
        ratios = {}
        for metric in ('requests_per_second', 'p99_ms', 'cpu_us_per_request'):
            values = [100 * (p['after'][metric] / p['before'][metric] - 1) for p in paired.values()]
            ratios[metric] = dict(median_percent_change=statistics.median(values), min=min(values), max=max(values))
        connections = defaultdict(list)
        for label, group in (('before', before), ('after', after)):
            for trial in group['trials']:
                path = Path(trial['path'])
                raw = json.loads(path.read_text())
                if 'tcp_counters_before_warmup' not in raw:
                    continue
                warm = json.loads((path.parent / 'warmup.json').read_text())
                count = trial['completed_requests'] + sum(warm['statusCodeDistribution'].values())
                delta = (raw['tcp_counters_after_measurement']['active_opens'] - raw['tcp_counters_before_warmup']['active_opens']) % 2**32
                connections[label].append(delta / count)
        comparisons.append(dict(case=case, driver=driver, before=before, after=after, paired=ratios,
                                host_ipv4_active_opens_per_response=dict(connections)))
args.summary.write_text(json.dumps(dict(comparisons=comparisons, note=summary['note']), indent=2) + '\n')

plt.rcParams.update({'font.size': 10, 'axes.spines.top': False, 'axes.spines.right': False})
fig, axes = plt.subplots(2, 1, figsize=(12, 9), layout='constrained')
for axis, metric, ylabel, selected in [
    (axes[0], 'requests_per_second', 'Completed requests / second', [c for c in comparisons if c['case'] != 'proxy-off']),
    (axes[1], 'cpu_us_per_request', 'Server CPU microseconds / response', comparisons)]:
    for offset, label, color in ((-.19, 'before', '#617385'), (.19, 'after', '#087f8c')):
        values = [c[label][metric]['median'] for c in selected]
        errors = [[c[label][metric]['median'] - c[label][metric]['min'] for c in selected],
                  [c[label][metric]['max'] - c[label][metric]['median'] for c in selected]]
        positions = [i + offset for i in range(len(selected))]
        if axis is axes[0]:
            axis.errorbar(positions, values, yerr=errors, fmt='o', label=label.title(), color=color,
                          capsize=4, markersize=7, linewidth=1.3)
        else:
            axis.bar(positions, values, .36, label=label.title(), color=color,
                     yerr=errors, capsize=3, error_kw={'linewidth': 1})
    axis.set_xticks(range(len(selected)), [c['case'] + '\n' + ('event loop' if c['driver']=='event-loop' else 'thread pool') for c in selected])
    axis.set_ylabel(ylabel)
    axis.grid(axis='y', alpha=.18)
    axis.set_axisbelow(True)
    axis.legend()
axes[0].set_yscale('log')
axes[0].set_ylabel('Completed requests / second (log scale)')
axes[0].set_title(f"Windows loopback: {before['n']} alternating pairs per case; median and observed range")
axes[1].set_title('Proxy-off is offered 200 requests/s; other workloads use closed-loop saturation'
                  if any(c['case'] == 'proxy-off' for c in comparisons) else 'Closed-loop saturation; same host and output limits')
fig.savefig(args.plot.with_suffix('.png'), dpi=180)
fig.savefig(args.plot.with_suffix('.svg'))

print('| Case | Driver | Before req/s | After req/s | Paired throughput change | Before p99 ms | After p99 ms | Before CPU us | After CPU us |')
print('|---|---|---:|---:|---:|---:|---:|---:|---:|')
for c in comparisons:
    b,a=c['before'],c['after']
    print(f"| {c['case']} | {c['driver']} | {b['requests_per_second']['median']:.0f} | {a['requests_per_second']['median']:.0f} | {c['paired']['requests_per_second']['median_percent_change']:+.1f}% | {b['p99_ms']['median']:.3f} | {a['p99_ms']['median']:.3f} | {b['cpu_us_per_request']['median']:.1f} | {a['cpu_us_per_request']['median']:.1f} |")
    if c['host_ipv4_active_opens_per_response']:
        print('Host active opens/response:', {k:statistics.median(v) for k,v in c['host_ipv4_active_opens_per_response'].items()})
