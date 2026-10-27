"""Alternate original/optimized binaries within each workload and repetition."""
import argparse
import asyncio
import json
from pathlib import Path
from types import SimpleNamespace
import run_study as study


async def run(args):
    args.output.mkdir(parents=True, exist_ok=False)
    current = study.source_identity()['sha256']
    baseline_record = json.loads(args.before_record.read_text())
    baseline = baseline_record['source_sha256']
    assert study.sha256(args.before) == baseline_record['binary_sha256'], 'before binary differs from its build record'
    manifest = dict(state='running', expected_trials=args.repeats * len(args.cases.split(',')) * 4,
                    current_source=current, baseline_source=baseline,
                    before_sha256=study.sha256(args.before), after_sha256=study.sha256(args.after),
                    cases=args.cases, duration=args.duration, warmup=args.warmup, repeats=args.repeats,
                    script_sha256=study.sha256(Path(__file__)))
    (args.output / 'paired-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    (args.output / 'run_pairs.py').write_bytes(Path(__file__).read_bytes())
    order = []
    for repeat in range(1, args.repeats + 1):
        cases = args.cases.split(',')
        if repeat % 2 == 0:
            cases.reverse()
        for case in cases:
            for driver in ('event-loop', 'thread-pool'):
                variants = [('before', args.before, baseline), ('after', args.after, current)]
                if repeat % 2 == 0:
                    variants.reverse()
                for label, binary, source in variants:
                    destination = args.output / f'pair-{repeat}-{case}-{driver}-{label}'
                    options = SimpleNamespace(binary=binary.resolve(), output=destination, label=label,
                        binary_source=source, cases=case, drivers=driver, duration=args.duration, warmup=args.warmup, repeats=1,
                        clients=16, workers=16, profile=None, rate=200 if case == 'proxy-off' else 0,
                        tcp=case == 'proxy-off', oha=study.ROOT / 'build/performance-tools/oha.exe',
                        profiler=study.ROOT / 'build/performance-tools/very-sleepy/app/sleepy.exe')
                    order.append(str(destination))
                    (args.output / 'order.json').write_text(json.dumps(order, indent=2) + '\n')
                    await study.run(options)
    assert study.source_identity()['sha256'] == current, 'production source changed during paired comparison'
    assert study.sha256(args.before) == manifest['before_sha256'] and study.sha256(args.after) == manifest['after_sha256']
    manifest['state'] = 'complete'
    manifest['trials'] = len(order)
    (args.output / 'paired-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', type=Path, required=True)
    parser.add_argument('--after', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--before-record', type=Path, default=study.ROOT / 'performance testing/baseline-build.json')
    parser.add_argument('--cases', default=','.join(study.CASES))
    parser.add_argument('--duration', type=float, default=5)
    parser.add_argument('--warmup', type=float, default=1)
    parser.add_argument('--repeats', type=int, default=5)
    args = parser.parse_args()
    assert set(args.cases.split(',')) <= set(study.CASES)
    assert len(set(args.cases.split(','))) == len(args.cases.split(','))
    assert 1 <= args.repeats <= 10 and 1 <= args.duration <= 120 and 1 <= args.warmup <= 30
    asyncio.run(run(args))
