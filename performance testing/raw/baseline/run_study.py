"""Local native load tests and optional Very Sleepy captures, with owned children."""
import argparse
import asyncio
from contextlib import contextmanager
from datetime import datetime, timezone
import hashlib
import http.client
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile
import time

import psutil

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / 'benchmarks'), str(ROOT / 'tools')]
from harness.process import OwnedProcess, process_options
from qualify import source_identity, sha256

READY = r'Listening on\s*:\s*(?P<host>127\.0\.0\.1):(?P<port>[0-9]+)'
CASES = {'static-1k': ('server', 1024), 'static-64k': ('server', 65536),
         'proxy-off': ('proxy', 65536), 'cache-warm': ('proxy', 65536)}


def fetch(endpoint, target, headers=None):
    connection = http.client.HTTPConnection(*endpoint, timeout=10)
    try:
        connection.request('GET', target, headers=headers or {})
        response = connection.getresponse()
        return response.status, dict(response.getheaders()), response.read()
    finally:
        connection.close()


def snapshot(pid):
    process = psutil.Process(pid)
    cpu = process.cpu_times()
    return dict(cpu_seconds=cpu.user + cpu.system, rss_bytes=process.memory_info().rss,
                threads=process.num_threads(), handles=process.num_handles() if os.name == 'nt' else process.num_fds())


@contextmanager
def profiler_settings(symbols, cache):
    import winreg
    path = r'Software\codersnotes.com\Very Sleepy'
    values = {'UseSymbolServer': (0, winreg.REG_DWORD),
              'SymbolSearchPath': (str(symbols), winreg.REG_SZ),
              'SymbolCache': (str(cache), winreg.REG_SZ),
              'SpeedThrottle': (10, winreg.REG_DWORD),
              'SaveMinidump': (0xffffffff, winreg.REG_DWORD)}
    with winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, path, 0, winreg.KEY_READ | winreg.KEY_WRITE) as key:
        saved = {}
        for name, (value, kind) in values.items():
            try:
                saved[name] = winreg.QueryValueEx(key, name)
            except FileNotFoundError:
                saved[name] = None
            winreg.SetValueEx(key, name, 0, kind, value)
        try:
            yield
        finally:
            for name, previous in saved.items():
                if previous is None:
                    try:
                        winreg.DeleteValue(key, name)
                    except FileNotFoundError:
                        pass
                else:
                    winreg.SetValueEx(key, name, 0, previous[1], previous[0])


async def load(args, endpoint, target, destination, seconds, proxy=False):
    command = [str(args.oha), '--no-tui', '--no-color', '--output-format', 'json',
               '-o', str(destination), '-z', str(seconds) + 's', '-w', '-t', '10s',
               '-c', str(args.clients), '--worker-threads', '4', '--http-version', '1.1',
               '--disable-compression']
    if proxy:
        command += ['-x', 'http://' + endpoint[0] + ':' + str(endpoint[1]), '--proxy-http-version', '1.1']
    if args.rate:
        command += ['-q', str(args.rate)]
    command.append(target)
    process = await OwnedProcess(command).start()
    return process


async def trial(args, case, driver, repeat, output):
    mode, size = CASES[case]
    name = f'{case}-{driver}-c{args.clients}-r{repeat}'
    path = output / name
    path.mkdir()
    children = []
    profiler = None
    record = dict(name=name, label=args.label, case=case, driver=driver,
                  clients=args.clients, workers=args.workers if driver == 'thread-pool' else 1,
                  object_bytes=size, profile=args.profile, repeat=repeat, state='failed',
                  offered_rate=args.rate,
                  started_utc=datetime.now(timezone.utc).isoformat(), samples=[], diagnostics=[])
    try:
        with tempfile.TemporaryDirectory(prefix='http-performance-') as directory:
            root = Path(directory)
            (root / 'object.bin').write_bytes(b'a' * size)
            origin = None
            if mode == 'proxy':
                origin = await OwnedProcess([shutil.which('node'), str(ROOT / 'performance testing/origin.mjs'), str(size)], READY).start()
                children.append(origin)
                origin_endpoint = await origin.wait_ready(10)
                target = f'http://{origin_endpoint[0]}:{origin_endpoint[1]}/object.bin'
            command = [str(args.binary), '--mode', mode, '--concurrency', driver,
                       '--bind', '127.0.0.1', '--port', '0', '--helpers', '4',
                       '--max-sessions', '2048', '--max-upstreams', '2048',
                       '--max-requests-per-connection', '100000']
            if driver == 'thread-pool':
                command += ['--workers', str(args.workers)]
            if mode == 'server':
                command += ['--root', str(root)]
            else:
                command += ['--cache', 'on' if case == 'cache-warm' else 'off']
            server = await OwnedProcess(command, READY).start()
            children.append(server)
            endpoint = await server.wait_ready(10)
            if mode == 'server':
                target = f'http://{endpoint[0]}:{endpoint[1]}/object.bin'
            wire_target = target if mode == 'proxy' else '/object.bin'
            status, headers, body = await asyncio.to_thread(fetch, endpoint, wire_target)
            assert status == 200 and body == b'a' * size, 'representation precheck failed'
            if case == 'cache-warm':
                status, _, body = await asyncio.to_thread(fetch, endpoint, wire_target, {'Cache-Control': 'only-if-cached'})
                assert status == 200 and body == b'a' * size, 'cache-only precheck failed'
            warm = await load(args, endpoint, target, path / 'warmup.json', args.warmup, mode == 'proxy')
            children.append(warm)
            await asyncio.wait_for(warm.process.wait(), args.warmup + 20)
            assert warm.process.returncode == 0, warm.diagnostic()
            await warm.stop()
            if origin:
                record['origin_before'] = json.loads((await asyncio.to_thread(fetch, origin_endpoint, '/__stats'))[2])
            record['before'] = snapshot(server.process.pid)
            record['metrics_before'] = server.metrics
            measured = await load(args, endpoint, target, path / 'oha.json', args.duration, mode == 'proxy')
            children.append(measured)
            record['commands'] = {'server': command, 'load': measured.args}
            record['generator_before'] = snapshot(measured.process.pid)
            if origin:
                record['origin_process_before'] = snapshot(origin.process.pid)
            started = time.perf_counter()
            if args.profile:
                startup = subprocess.STARTUPINFO()
                startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
                startup.wShowWindow = 0
                capture = path / 'profile.sleepy'
                profile_command = [str(args.profiler), '-a', str(server.process.pid), '-o', str(capture),
                                   '-t', str(max(1, int(args.duration) - 2)), '-q']
                if args.profile == 'busy':
                    profile_command.append('-mbt')
                record['commands']['profiler'] = profile_command
                environment = dict(os.environ, SLEEPY_SILENT_CRASH='1')
                profiler = await asyncio.create_subprocess_exec(*profile_command, startupinfo=startup,
                    env=environment, stdout=asyncio.subprocess.DEVNULL, stderr=asyncio.subprocess.DEVNULL,
                    **process_options())
            while measured.process.returncode is None:
                sample = {'seconds': time.perf_counter() - started, 'server': snapshot(server.process.pid)}
                try:
                    sample['generator'] = snapshot(measured.process.pid)
                except psutil.NoSuchProcess:
                    break
                if origin:
                    sample['origin'] = snapshot(origin.process.pid)
                record['samples'].append(sample)
                try:
                    await asyncio.wait_for(measured.process.wait(), .25)
                except asyncio.TimeoutError:
                    assert time.perf_counter() - started < args.duration + 20, 'load generator exceeded bound'
            await measured.stop()
            assert measured.process.returncode == 0, measured.diagnostic()
            record['after'] = snapshot(server.process.pid)
            record['server_metrics'] = server.metrics
            if origin:
                record['origin_after'] = json.loads((await asyncio.to_thread(fetch, origin_endpoint, '/__stats'))[2])
                record['origin_process_after'] = snapshot(origin.process.pid)
                delta = record['origin_after']['requests'] - record['origin_before']['requests']
                if case == 'cache-warm':
                    assert delta == 0, 'warm cache reached origin'
            result = json.loads((path / 'oha.json').read_text())
            record['oha'] = result
            assert not result.get('errorDistribution'), 'load generator reported request failures'
            assert set(result['statusCodeDistribution']) == {'200'}, 'unexpected response status'
            total = sum(result['statusCodeDistribution'].values())
            assert total > 0 and result['summary']['totalData'] == total * size, 'completed representation byte count mismatch'
            if profiler:
                await asyncio.wait_for(profiler.wait(), 90)
                assert profiler.returncode == 0 and (path / 'profile.sleepy').is_file(), 'native capture failed'
            record['state'] = 'complete'
    except Exception as error:
        record['diagnostics'].append(type(error).__name__ + ': ' + str(error)[:3000])
        raise
    finally:
        if profiler and profiler.returncode is None:
            profiler.kill()
            await profiler.wait()
        for child in reversed(children):
            await child.stop()
        record['processes'] = [child.diagnostic() for child in children]
        record['finished_utc'] = datetime.now(timezone.utc).isoformat()
        (path / 'record.json').write_text(json.dumps(record, indent=2) + '\n')
    print(name, record['state'], f"{record['oha']['summary']['requestsPerSec']:.1f} requests/s", flush=True)


async def run(args):
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    manifest = dict(label=args.label, source=source_identity(), binary=str(args.binary),
                    binary_sha256=sha256(args.binary), os=platform.platform(), cpu=platform.processor(),
                    logical_cpus=psutil.cpu_count(), physical_cpus=psutil.cpu_count(logical=False),
                    ram_bytes=psutil.virtual_memory().total, argv=sys.argv,
                    tools={str(p): sha256(p) for p in [args.oha, args.profiler] if p.is_file()},
                    script_sha256=sha256(Path(__file__)), origin_sha256=sha256(ROOT / 'performance testing/origin.mjs'),
                    state='running')
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    try:
        cases = [(case, driver) for case in args.cases.split(',') for driver in args.drivers.split(',')]
        for repeat in range(1, args.repeats + 1):
            for case, driver in cases if repeat % 2 else reversed(cases):
                await trial(args, case, driver, repeat, output)
        manifest['state'] = 'complete'
    finally:
        if manifest['state'] != 'complete':
            manifest['state'] = 'failed'
        manifest['source_after'] = source_identity()['sha256']
        if manifest['source_after'] != manifest['source']['sha256']:
            manifest['state'] = 'source_changed'
        (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--label', required=True)
    parser.add_argument('--cases', default=','.join(CASES))
    parser.add_argument('--drivers', default='event-loop,thread-pool')
    parser.add_argument('--duration', type=float, default=10)
    parser.add_argument('--warmup', type=float, default=2)
    parser.add_argument('--repeats', type=int, default=5)
    parser.add_argument('--clients', type=int, default=16)
    parser.add_argument('--workers', type=int, default=16)
    parser.add_argument('--rate', type=int, default=0, help='0: closed-loop saturation; otherwise aggregate requests/second')
    parser.add_argument('--profile', choices=['busy', 'all'])
    parser.add_argument('--oha', type=Path, default=ROOT / 'build/performance-tools/oha.exe')
    parser.add_argument('--profiler', type=Path, default=ROOT / 'build/performance-tools/very-sleepy/app/sleepy.exe')
    args = parser.parse_args()
    args.binary = args.binary.resolve()
    assert 1 <= args.repeats <= 10 and 1 <= args.clients <= 128 and 1 <= args.workers <= 128
    assert 1 <= args.duration <= 120 and 1 <= args.warmup <= 30
    assert set(args.cases.split(',')) <= set(CASES)
    assert set(args.drivers.split(',')) <= {'event-loop', 'thread-pool'}
    if args.profile:
        with profiler_settings(args.binary.parent, ROOT / 'build/performance-tools/symbol-cache'):
            asyncio.run(run(args))
    else:
        asyncio.run(run(args))
