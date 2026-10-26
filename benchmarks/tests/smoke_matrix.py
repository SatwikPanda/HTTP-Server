"""Small executable acceptance; every family, no performance conclusions."""
import argparse
import asyncio
from pathlib import Path
import sys
from types import SimpleNamespace

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from harness.spec import manifest
from harness.process import profile
from harness.runner import run


async def smoke(args):
    spec = manifest(ROOT / 'scenarios.json')
    selected = profile(ROOT / 'profiles/assembled.json')
    for family in spec['families']:
        if args.driver not in family['drivers']:
            continue
        options = SimpleNamespace(binary=args.binary, build_dir=args.build_dir, build=False,
            scenario=family['id'], driver=args.driver, object_bytes=1024, concurrency=2,
            workers=2 if args.driver == 'thread-pool' else 1, connections='reuse', arrival='closed',
            rate=100, queue_limit=16, timeout=3, startup_timeout=5, max_connections=100,
            max_dataset_mib=256, request_limit=10000, filesystem_cache='uncontrolled', seed=1729,
            smoke=True, warmup=.05, measurement=.15, repeats=1, qualified_commit=None,
            output=str(Path(args.build_dir) / 'benchmark-smoke'), invocation=sys.argv)
        print(f"{args.driver}/{family['id']}", flush=True)
        # Cancellation runs repeat's finally block and stops the owned server;
        # an outer subprocess timeout must not orphan a grandchild server.
        code = await asyncio.wait_for(run(spec, selected, options, ROOT.parent), 30)
        if code:
            raise RuntimeError(f"failed smoke: {family['id']}")
    return 0


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', required=True)
    parser.add_argument('--build-dir', required=True)
    parser.add_argument('--driver', required=True)
    raise SystemExit(asyncio.run(smoke(parser.parse_args())))
