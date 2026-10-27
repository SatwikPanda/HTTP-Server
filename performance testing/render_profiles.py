"""Convert Very Sleepy elapsed-time stacks to folded microseconds and SVG."""
import argparse
from collections import Counter
import csv
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def render(capture, output, perl, renderer):
    with zipfile.ZipFile(capture) as archive:
        assert not any('dump' in name.lower() for name in archive.namelist())
        symbols = {}
        for line in archive.read('Symbols.txt').decode().splitlines():
            if not line.strip():
                continue
            address, module, function, source, number = shlex.split(line)
            symbols[address] = dict(name=module + '!' + function, source=source, line=int(number))
        stacks = Counter()
        inclusive, exclusive = Counter(), Counter()
        for line in archive.read('Callstacks.txt').decode().splitlines():
            fields = line.split()
            if not fields:
                continue
            weight = round(float(fields[0]) * 1_000_000)
            names = [symbols.get(address, {'name': address})['name'].replace(';', ':') for address in fields[1:]]
            assert weight >= 0 and names
            stacks[';'.join(reversed(names))] += weight
            exclusive[names[0]] += weight
            for name in set(names):
                inclusive[name] += weight
        stats = archive.read('Stats.txt').decode().replace('\r', '')
    output.parent.mkdir(parents=True, exist_ok=True)
    folded = output.with_suffix('.folded')
    folded.write_bytes(''.join(f'{stack} {weight}\n' for stack, weight in sorted(stacks.items())).encode('utf-8'))
    total = sum(stacks.values())
    summary = dict(capture=str(capture.relative_to(ROOT)), capture_sha256=hashlib.sha256(capture.read_bytes()).hexdigest(),
                   stats=stats, weighted_thread_microseconds=total, folded_stacks=len(stacks),
                   scope='elapsed thread time, INCLUDING waits; not CPU-event samples',
                   inclusive=[dict(function=name, percent=100 * value / total) for name, value in inclusive.most_common()],
                   exclusive=[dict(function=name, percent=100 * value / total) for name, value in exclusive.most_common()])
    output.with_suffix('.json').write_text(json.dumps(summary, indent=2) + '\n')
    with output.with_suffix('.csv').open('w', newline='', encoding='utf-8') as stream:
        writer = csv.writer(stream)
        writer.writerow(['function', 'inclusive_thread_wall_percent', 'exclusive_thread_wall_percent'])
        for name, weight in inclusive.most_common():
            writer.writerow([name, weight * 100 / total, exclusive[name] * 100 / total])
    with output.open('wb') as stream:
        subprocess.run([str(perl), str(renderer), '--title', output.stem,
                        '--subtitle', 'Sampled thread wall time including waits (not CPU events)',
                        '--countname', 'thread microseconds', '--width', '1600', '--minwidth', '0.2',
                        str(folded)], stdout=stream, check=True)
    print(output.name, f'{total / 1e6:.3f} weighted thread-seconds', len(stacks), 'stacks')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('study', type=Path)
    parser.add_argument('--perl', type=Path, default=Path('C:/Program Files/Git/usr/bin/perl.exe'))
    parser.add_argument('--renderer', type=Path, default=ROOT / 'build/performance-tools/flamegraph.pl')
    args = parser.parse_args()
    for capture in sorted(args.study.resolve().rglob('profile.sleepy')):
        render(capture, ROOT / 'performance testing/flamegraphs' / (args.study.name + '-' + capture.parent.name + '.svg'), args.perl, args.renderer)
