"""Check native Windows reparse rejection when symlink privileges are unavailable."""
import argparse
import asyncio
import json
import os
from pathlib import Path
import subprocess
import tempfile
from run_study import OwnedProcess, READY, ROOT, fetch, process_options, sha256


async def run(binary, output):
    evidence = dict(state='failed', binary_sha256=sha256(binary), checks=[])
    with tempfile.TemporaryDirectory(prefix='http-junction-check-') as directory:
        root = Path(directory).resolve()
        www, outside = root / 'www', root / 'outside'
        (www / 'realdir').mkdir(parents=True)
        outside.mkdir()
        (www / 'realdir/inner.txt').write_text('inside')
        (outside / 'secret.txt').write_text('outside')
        links = []
        try:
            for name, target in [('alias', www / 'realdir'), ('escape', outside)]:
                link = www / name
                assert link.absolute().is_relative_to(root) and target.resolve().is_relative_to(root)
                subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive', '-File',
                                str(ROOT / 'performance testing/create_junction.ps1'),
                                '-Link', str(link), '-Target', str(target)], check=True, timeout=10, **process_options())
                links.append(link)
                assert link.is_junction()
            for driver in ['event-loop', 'thread-pool']:
                command = [str(binary), '--mode', 'server', '--concurrency', driver, '--root', str(www), '--port', '0']
                server = await OwnedProcess(command, READY).start()
                try:
                    endpoint = await server.wait_ready(10)
                    for target, expected in [('/realdir/inner.txt', 200), ('/alias', 403), ('/alias/inner.txt', 403),
                                             ('/escape', 403), ('/escape/secret.txt', 403)]:
                        status, _, body = await asyncio.to_thread(fetch, endpoint, target)
                        evidence['checks'].append(dict(driver=driver, target=target, status=status, expected=expected))
                        assert status == expected
                        if status == 200:
                            assert body == b'inside'
                        else:
                            assert b'outside' not in body
                finally:
                    await server.stop()
            evidence['state'] = 'passed'
        finally:
            # Remove only our junction entries before TemporaryDirectory cleanup;
            # both their lexical locations and resolved targets stay in this fixture.
            for link in reversed(links):
                assert link.absolute().is_relative_to(root) and link.resolve().is_relative_to(root)
                assert link.is_junction()
                os.rmdir(link)
            output.write_text(json.dumps(evidence, indent=2) + '\n')
    print(evidence['state'], len(evidence['checks']), 'native junction/static-response checks')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    assert not args.output.exists()
    asyncio.run(run(args.binary.resolve(), args.output))
