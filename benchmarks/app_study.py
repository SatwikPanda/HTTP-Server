"""Measure the actual app executable on loopback; preserve every trial and failure.

Test-only dependencies: psutil and an existing oha binary. No server options or
routes are substituted. CPU percentages use one logical core as 100%.
"""
import argparse
import asyncio
from datetime import datetime, timezone
import http.client
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import time

import psutil

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "benchmarks"), str(ROOT / "tools")]
from harness.process import OwnedProcess, environment, process_options
from qualify import read_cache, require, sha256, source_identity, verify_record

READY = r"Listening on\s*:\s*(?P<host>127\.0\.0\.1):(?P<port>[0-9]+)"
CASES = [
    dict(id="idle", method="GET", target="/", clients=0, reuse=True, status=200, body="Hello, World!"),
    dict(id="get-c1", method="GET", target="/", clients=1, reuse=True, status=200, body="Hello, World!"),
    dict(id="get-c4", method="GET", target="/", clients=4, reuse=True, status=200, body="Hello, World!"),
    dict(id="get-c16", method="GET", target="/", clients=16, reuse=True, status=200, body="Hello, World!"),
    dict(id="get-new-c4", method="GET", target="/", clients=4, reuse=False, status=200, body="Hello, World!"),
    dict(id="head-c4", method="HEAD", target="/", clients=4, reuse=True, status=200, body=""),
    dict(id="missing-c4", method="GET", target="/missing", clients=4, reuse=True, status=404, body="404 Not Found\n"),
]


def save(path, value):
    with path.open("x", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2)
        stream.write("\n")


def snapshot(pid):
    process = psutil.Process(pid)
    cpu, memory = process.cpu_times(), process.memory_info()
    return dict(cpu_seconds=cpu.user + cpu.system, rss_bytes=memory.rss,
                private_bytes=getattr(memory, "private", None), threads=process.num_threads(),
                handles=process.num_handles() if os.name == "nt" else process.num_fds())


def fetch(endpoint, method, target, body=None):
    connection = http.client.HTTPConnection(*endpoint, timeout=5)
    try:
        connection.request(method, target, body=body)
        response = connection.getresponse()
        return response.status, dict(response.getheaders()), response.read()
    finally:
        connection.close()


def raw_exchange(endpoint, wire):
    with socket.create_connection(endpoint, timeout=5) as client:
        client.sendall(wire)
        chunks = bytearray()
        while True:
            data = client.recv(4096)
            if not data:
                return bytes(chunks)
            chunks.extend(data)
            require(len(chunks) <= 65536, "functional response limit")


def parse_frames(wire, methods):
    frames = []
    for method in methods:
        head, wire = wire.split(b"\r\n\r\n", 1)
        lines = head.split(b"\r\n")
        status = int(lines[0].split()[1])
        headers = {}
        for line in lines[1:]:
            key, value = line.split(b":", 1)
            key = key.lower()
            require(key not in headers, "duplicate generated header")
            headers[key] = value.strip()
        require(b"transfer-encoding" not in headers and b"content-length" in headers, "expected fixed framing")
        length = 0 if method == "HEAD" else int(headers[b"content-length"])
        require(len(wire) >= length, "truncated response")
        frames.append((status, headers, wire[:length]))
        wire = wire[length:]
    require(not wire, "unexpected trailing response bytes")
    return frames


async def functional(binary):
    rows = []

    async def check(name, action):
        start = time.perf_counter()
        row = dict(name=name, passed=False)
        try:
            await action()
            row["passed"] = True
        except Exception as error:
            row["error"] = str(error)
        row["seconds"] = time.perf_counter() - start
        rows.append(row)

    async def cli(arguments, code, text):
        child = await OwnedProcess([str(binary), *arguments]).start()
        try:
            await asyncio.wait_for(child.process.wait(), 5)
            await child.stop()
            require(child.process.returncode == code and text in child.diagnostic()["tail"], "unexpected CLI result")
        finally:
            await child.stop()

    await check("help", lambda: cli(["--help"], 0, "Usage: http_app"))
    for value in ["-1", "65536", "abc", "", "8080junk"]:
        await check("invalid port " + repr(value), lambda value=value: cli([value], 1, "Port must be"))
    await check("too many arguments", lambda: cli(["8080", "extra"], 1, "Usage:"))
    server = await OwnedProcess([str(binary), "0"], READY).start()
    try:
        endpoint = await server.wait_ready(5)
        async def response(method, target, status, expected_body):
            actual, headers, body = await asyncio.to_thread(fetch, endpoint, method, target)
            require(actual == status and body == expected_body, f"unexpected {method} {target}: {actual}, {body!r}")
            if method == "HEAD":
                require(headers.get("Content-Length") == "13", "HEAD length")
            if status == 405:
                require(headers.get("Allow") == "GET, HEAD", "Allow header")

        for name, method, target, status, body in [
            ("GET root", "GET", "/", 200, b"Hello, World!"),
            ("HEAD metadata", "HEAD", "/", 200, b""),
            ("query routing", "GET", "/?x=1", 200, b"Hello, World!"),
            ("absolute target", "GET", f"http://127.0.0.1:{endpoint[1]}/", 200, b"Hello, World!"),
            ("404 route", "GET", "/missing", 404, b"404 Not Found\n"),
            ("escaped path", "GET", "/%2F", 404, b"404 Not Found\n"),
            ("POST rejected", "POST", "/", 405, b"405 Method Not Allowed\r\n"),
        ]:
            await check(name, lambda m=method, t=target, s=status, b=body: response(m, t, s, b))

        async def raw(wire, methods, statuses, bodies=None):
            frames = parse_frames(await asyncio.to_thread(raw_exchange, endpoint, wire), methods)
            require([f[0] for f in frames] == statuses, "unexpected wire statuses")
            if bodies is not None:
                require([f[2] for f in frames] == bodies, "unexpected wire bodies")

        for name, wire, status in [
            ("HTTP/1.0 without Host", b"GET / HTTP/1.0\r\n\r\n", 200),
            ("missing Host", b"GET / HTTP/1.1\r\nConnection: close\r\n\r\n", 400),
            ("duplicate Host", b"GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\nConnection: close\r\n\r\n", 400),
            ("ambiguous framing", b"GET / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n", 400),
            ("invalid escape", b"GET /bad%zz HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n", 400),
        ]:
            await check(name, lambda w=wire, s=status: raw(w, ["GET"], [s]))
        pipeline = b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\nHEAD / HTTP/1.1\r\nHost: localhost\r\n\r\nGET /missing HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"
        await check("ordered GET/HEAD/404 pipeline", lambda: raw(pipeline, ["GET", "HEAD", "GET"], [200, 200, 404],
                                                               [b"Hello, World!", b"", b"404 Not Found\n"]))
        await check("100-request keep-alive limit", lambda: raw(
            b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n" * 101, ["GET"] * 100, [200] * 100, [b"Hello, World!"] * 100))
        await check("occupied port", lambda: cli([str(endpoint[1])], 1, "bind"))
    finally:
        await server.stop()
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    async def fixed_port():
        server = await OwnedProcess([str(binary), str(port)], READY).start()
        try:
            endpoint = await server.wait_ready(5)
            require(endpoint[1] == port, "wrong bound port")
            status, _, body = await asyncio.to_thread(fetch, endpoint, "GET", "/")
            require(status == 200 and body == b"Hello, World!", "fixed port GET")
        finally:
            await server.stop()
    await check("fixed port", fixed_port)
    return rows


def validate_load(result, case):
    counts = result["statusCodeDistribution"]
    requests = sum(counts.values())
    errors = sum(result["errorDistribution"].values())
    require(requests > 0 and errors == 0 and set(counts) == {str(case["status"])}, "unexpected status/transport failures")
    require(result["summary"]["totalData"] == requests * len(case["body"].encode()), "aggregate body length mismatch")
    return requests


async def trial(args, variant, case, repeat, seconds, destination):
    destination.mkdir()
    record = dict(schema_version=1, evidence="local-candidate", variant=variant["label"], case=case,
                  repeat=repeat, state="failed", started_utc=datetime.now(timezone.utc).isoformat(),
                  requested_seconds=seconds, warmup_seconds=args.warmup if case["clients"] else 0,
                  sample_interval_seconds=.1, samples=[], commands={}, diagnostics=[])
    server = load = None
    try:
        server = await OwnedProcess([str(variant["binary"]), "0"], READY).start()
        endpoint = await server.wait_ready(5)
        record["commands"]["server"] = server.args
        async def probe():
            status, _, body = await asyncio.to_thread(fetch, endpoint, case["method"], case["target"])
            require(status == case["status"] and body == case["body"].encode(), "exact response probe failed")
        await probe()
        def command(duration, name):
            cmd = [str(args.oha), "--no-tui", "--no-color", "--output-format", "json", "-o", str(destination / name),
                   "-z", f"{duration}s", "-w", "-t", "5s", "-c", str(case["clients"]), "--worker-threads", "4",
                   "--http-version", "1.1", "--disable-compression", "-m", case["method"]]
            if not case["reuse"]:
                cmd.append("--disable-keepalive")
            if case.get("offered_rate"):
                cmd += ["-q", str(case["offered_rate"])]
            return cmd + [f"http://{endpoint[0]}:{endpoint[1]}{case['target']}"]
        if case["clients"]:
            load = await OwnedProcess(command(args.warmup, "warmup.json")).start()
            record["commands"]["warmup"] = load.args
            await asyncio.wait_for(load.process.wait(), args.warmup + 10)
            await load.stop()
            require(load.process.returncode == 0, "warmup process failed")
            validate_load(json.loads((destination / "warmup.json").read_text()), case)
            load = None
        record["before"] = snapshot(server.process.pid)
        start = time.perf_counter()
        if case["clients"]:
            load = await OwnedProcess(command(seconds, "oha.json")).start()
            record["commands"]["measurement"] = load.args
        while time.perf_counter() - start < seconds or (load and load.process.returncode is None):
            elapsed = time.perf_counter() - start
            require(elapsed < seconds + 12, "measurement exceeded deadline")
            sample = dict(seconds=elapsed, server=snapshot(server.process.pid))
            if load:
                try:
                    sample["generator"] = snapshot(load.process.pid)
                except psutil.NoSuchProcess:
                    pass
            record["samples"].append(sample)
            await asyncio.sleep(.1)
        record["elapsed_seconds"] = time.perf_counter() - start
        record["after"] = snapshot(server.process.pid)
        record["server_cpu_percent"] = 100 * (record["after"]["cpu_seconds"] - record["before"]["cpu_seconds"]) / record["elapsed_seconds"]
        record["server_machine_cpu_percent"] = record["server_cpu_percent"] / (os.cpu_count() or 1)
        snapshots = [record["before"], *(s["server"] for s in record["samples"]), record["after"]]
        record["sampled_peak_rss_bytes"] = max(s["rss_bytes"] for s in snapshots)
        record["peak_handles"] = max(s["handles"] for s in snapshots)
        record["peak_threads"] = max(s["threads"] for s in snapshots)
        if load:
            await load.stop()
            require(load.process.returncode == 0, "load process failed")
            result = json.loads((destination / "oha.json").read_text())
            record["requests"] = validate_load(result, case)
            record["oha"] = result
        await probe()
        record["state"] = "passed"
    except Exception as error:
        record["diagnostics"].append(f"{type(error).__name__}: {error}")
    finally:
        for child in (load, server):
            if child:
                await child.stop()
        record["server_process"] = server.diagnostic() if server else None
        record["finished_utc"] = datetime.now(timezone.utc).isoformat()
        save(destination / "record.json", record)
    print(f"{destination.name}: {record['state']}", flush=True)
    return record["state"] == "passed"


async def run(args):
    require(not args.output.exists(), "select a fresh output directory")
    require(1 <= args.repeats <= 10 and 1 <= args.duration <= 60 and 1 <= args.warmup <= 10 and 1 <= args.soak <= 300, "invalid duration/repeat bounds")
    require(0 <= args.rate <= 1000000 and 0 <= args.new_connection_rate <= 1000000, "invalid offered rate")
    variants = []
    for label, build_name, qualification_name in args.variant:
        require(re.fullmatch(r"[a-z0-9-]+", label) and label not in [v["label"] for v in variants], "invalid/duplicate variant label")
        build, qualification = Path(build_name).resolve(), Path(qualification_name).resolve()
        cache = read_cache(build)
        config = "Release" if cache.get("CMAKE_CONFIGURATION_TYPES") else ""
        binary = build / "app" / config / ("http_app.exe" if os.name == "nt" else "http_app")
        record = json.loads(qualification.read_text())
        verify_record(record, Path(record["binary"]["path"]))
        require(record["additional_binaries"]["http_app"]["sha256"] == sha256(binary), "app differs from qualified binary")
        variants.append(dict(label=label, binary=str(binary), binary_bytes=binary.stat().st_size, binary_sha256=sha256(binary),
                             compiler=record["compiler"], qualification=str(qualification.relative_to(ROOT)), qualification_sha256=sha256(qualification)))
    args.output.mkdir(parents=True)
    manifest = dict(schema_version=1, state="running", source=source_identity(), environment=environment(), variants=variants,
                    settings=dict(warmup_seconds=args.warmup, measurement_seconds=args.duration, repeats=args.repeats,
                                  idle_seconds=3, soak_seconds=args.soak, sample_interval_seconds=.1,
                                  server_workers=4, max_requests_per_connection=100, arrival="closed loop with optional rate cap",
                                  offered_rate=args.rate, new_connection_rate=args.new_connection_rate),
                    oha=dict(path=str(args.oha), sha256=sha256(args.oha), version=subprocess.check_output([str(args.oha), "--version"], text=True, **process_options()).strip()),
                    trials=[], functional={})
    passed = True
    try:
        for variant in variants:
            checks = await functional(Path(variant["binary"]))
            manifest["functional"][variant["label"]] = checks
            print(variant["label"], "functional:", sum(r["passed"] for r in checks), "/", len(checks), flush=True)
            require(all(r["passed"] for r in checks), "functional failures; see campaign.json")
        for repeat in range(1, args.repeats + 1):
            for case in CASES:
                case = dict(case, offered_rate=(args.rate if case["reuse"] else args.new_connection_rate) if case["clients"] else 0)
                # Reverse compiler order each round to reduce time/order bias.
                for variant in (variants if repeat % 2 else list(reversed(variants))):
                    name = f"{variant['label']}-{case['id']}-r{repeat}"
                    passed = await trial(args, variant, case, repeat, 3 if not case["clients"] else args.duration, args.output / name) and passed
                    manifest["trials"].append(name)
        case = dict(CASES[2], id="soak-c4", offered_rate=args.rate)
        for variant in variants:
            name = f"{variant['label']}-soak-c4-r1"
            passed = await trial(args, variant, case, 1, args.soak, args.output / name) and passed
            manifest["trials"].append(name)
        require(source_identity()["sha256"] == manifest["source"]["sha256"], "source changed during measurement")
        for variant in variants:
            require(sha256(variant["binary"]) == variant["binary_sha256"], "binary changed during measurement")
        manifest["state"] = "passed" if passed else "failed"
    finally:
        if manifest["state"] == "running":
            manifest["state"] = "failed"
        save(args.output / "campaign.json", manifest)
    return 0 if manifest["state"] == "passed" else 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--variant", nargs=3, action="append", required=True, metavar=("LABEL", "BUILD", "QUALIFICATION"))
    parser.add_argument("--oha", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--warmup", type=float, default=1)
    parser.add_argument("--duration", type=float, default=5)
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--soak", type=float, default=60)
    parser.add_argument("--rate", type=float, default=0, help="Optional global requests/second cap for reused connections; 0 means unrestricted")
    parser.add_argument("--new-connection-rate", type=float, default=0, help="Optional requests/second cap for the new-TCP workload")
    arguments = parser.parse_args()
    arguments.output = arguments.output.resolve()
    arguments.oha = arguments.oha.resolve()
    raise SystemExit(asyncio.run(run(arguments)))
