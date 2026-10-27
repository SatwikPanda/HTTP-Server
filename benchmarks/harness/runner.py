"""Repeat orchestration, cache preparation, provenance, and raw result retention."""
import asyncio
import copy
import hashlib
import json
import os
import tempfile
import time
import uuid
import sys
from datetime import datetime, timezone
from pathlib import Path

from .http_io import Client, Origin
from .measure import run_phase
from .process import OwnedProcess, command, environment, provenance, sample_process, verify_help
from .spec import VERSION, number, payload, require, validate_result


def scenario_from(spec, args):
    families = [family for family in spec["families"] if family["id"] == args.scenario]
    require(len(families) == 1, "unknown scenario")
    scenario = copy.deepcopy(families[0])
    defaults = spec["defaults"]
    scenario.update(driver=args.driver or scenario["drivers"][0], concurrency=args.concurrency,
                    workers=args.workers, object_bytes=15 if scenario["application"] == "demo" else args.object_bytes,
                    reuse=args.connections == "reuse", arrival=args.arrival, rate=args.rate,
                    queue_limit=args.queue_limit, timeout=args.timeout or defaults["request_timeout_seconds"],
                    seed=args.seed if args.seed is not None else defaults["seed"], settings=copy.deepcopy(defaults["settings"]))
    for name, low, high, integer in (("concurrency", 1, 1000, True), ("workers", 1, 256, True),
            ("object_bytes", 1, 16777216, True), ("queue_limit", 1, 4096, True),
            ("rate", .01, 100000, False), ("timeout", .05, 300, False), ("seed", 0, 2**32 - 1, True)):
        number(scenario[name], low, high, name, integer)
    require(scenario["driver"] in scenario["drivers"], "scenario does not support selected driver")
    require(scenario["concurrency"] <= args.max_connections, "scenario exceeds operator connection limit")
    require(scenario["object_bytes"] * scenario["objects"] <= args.max_dataset_mib * 1024 * 1024, "dataset limit")
    require(scenario["driver"] == "thread-pool" or args.workers == 1, "workers only applies to thread-pool")
    scenario["filesystem_cache"] = args.filesystem_cache
    scenario["generator_connection_limit"] = args.max_connections
    scenario["dataset_limit_bytes"] = args.max_dataset_mib * 1024 * 1024
    scenario["settings_applied"] = scenario["application"] != "demo"
    return scenario


def make_files(root, scenario):
    directory = root / "object"
    directory.mkdir()
    hashes = {}
    for index in range(scenario["objects"]):
        digest = hashlib.sha256()
        with (directory / f"{index}.bin").open("xb") as output:
            for offset in range(0, scenario["object_bytes"], 65536):
                data = payload(scenario["seed"], index, offset, min(65536, scenario["object_bytes"] - offset))
                output.write(data)
                digest.update(data)
        hashes[f"object/{index}.bin"] = digest.hexdigest()
    return hashes


async def probe(scenario, endpoint, origin, index=0, method="GET", cache_only=False, expected=200):
    client = Client(endpoint, origin, scenario["seed"], scenario["object_bytes"], reuse=False,
                    demo=scenario["application"] == "demo")
    try:
        return await asyncio.wait_for(client.request(index, method, cache_only, expected), scenario["timeout"])
    finally:
        await client.close()


async def prepare_cache(scenario, endpoint, origin):
    # The origin is controlled in-process; this is an actual network correctness
    # check of the selected executable, not a simulated cache implementation.
    state = scenario["cache"]
    before = origin.snapshot()
    if state == "cold":
        await probe(scenario, endpoint, origin.endpoint, cache_only=True, expected=504)
        require(origin.counts["requests"] == before["requests"], "cold cache-only probe reached origin")
        return {"method": "restart after warmup; cache-only absence probe", "origin_delta": 0}
    if state in {"warm", "revalidation", "mixed"}:
        indices = range(scenario["objects"]) if state != "mixed" else range(1)
        for index in indices:
            await probe(scenario, endpoint, origin.endpoint, index)
        filled = origin.snapshot()
        require(filled["responses_200"] - before["responses_200"] == len(indices), "prefill did not fetch every expected object")
        for index in indices:
            await probe(scenario, endpoint, origin.endpoint, index, cache_only=state != "revalidation")
        if state == "revalidation":
            require(origin.counts["responses_304"] - filled["responses_304"] == len(indices), "validator setup was not revalidated")
        else:
            require(origin.counts["requests"] == filled["requests"], "warm cache-only probe reached origin")
        return {"method": "prefill then verify validators" if state == "revalidation" else "prefill then cache-only hit probe",
                "objects_prefilled": len(indices), "origin_delta": origin.counts["requests"] - before["requests"]}
    return {"method": "cache disabled"}


def origin_delta(before, after):
    return {key: after[key] - before[key] for key in after if key != "peak_connections"}


def check_origin(scenario, measurement, delta):
    count = measurement["counts"]["successful"]
    require(delta["errors"] == 0 and delta["rejected"] == 0, "origin errors or admission rejection")
    if scenario["cache"] == "warm":
        require(delta["requests"] == 0, "warm measurement contacted origin")
    elif scenario["cache"] == "off":
        require(delta["requests"] == count, "cache-disabled origin count mismatch")
    elif scenario["cache"] == "revalidation":
        # Concurrent validations may select the same version. K retries a
        # conflicting 304 once unconditionally instead of publishing stale metadata.
        require(delta["responses_304"] == count and delta["responses_200"] <= count and
                delta["requests"] == count + delta["responses_200"], "validation traffic mismatch")
        if scenario["concurrency"] == 1:
            require(delta["responses_200"] == 0, "serial validation unexpectedly refetched")
    elif scenario["cache"] == "cold":
        require(delta["responses_200"] > 0, "cold-start measurement never fetched from origin")
    else:
        require(delta["requests"] <= count, "mixed workload unexpected extra origin traffic")


async def repeat(scenario, profile, args, provenance_data, destination, ordinal):
    identifier = uuid.uuid4().hex
    output = destination / identifier
    output.mkdir(parents=True, exist_ok=False)
    record = dict(schema_version=VERSION, run_id=identifier, repeat=ordinal,
                  evidence="smoke" if args.smoke else ("candidate" if args.candidate_record else "measurement"), state="failed",
                  started_utc=datetime.now(timezone.utc).isoformat(), scenario=scenario,
                  timing={"warmup_seconds": args.warmup, "measurement_seconds": args.measurement},
                  provenance=provenance_data, environment=environment(), processes=[], phases={}, diagnostics=[],
                  profile=profile, preparation={}, server_metrics={"available": False,
                      "reason": "no periodic server snapshot observed during this short run"},
                  filesystem_preparation="new deterministic files written immediately before warmup; OS cache not flushed")
    temporary = None
    origin = None
    children = []
    interrupted = False
    try:
        limits = record["environment"]["socket_limits"]
        if limits and limits["file_descriptors_soft"] > 0:
            require(scenario["concurrency"] * (4 if scenario["application"] == "proxy" else 2) + 64 < limits["file_descriptors_soft"], "file descriptor limit insufficient")
        temporary = tempfile.TemporaryDirectory(prefix="http-bench-")
        root = Path(temporary.name)
        record["dataset_hashes"] = make_files(root, scenario) if scenario["application"] == "server" else {}
        argv = command(args.binary, profile, scenario, root)
        record["help_sha256"] = await verify_help(args.binary, argv)
        if scenario["application"] == "proxy":
            origin = await Origin(scenario, min(2048, scenario["concurrency"] + 32)).start()
            record["origin"] = dict(endpoint=origin.endpoint, pid=os.getpid(), placement="in generator process")

        async def launch():
            child = OwnedProcess(argv, profile["readiness_regex"])
            children.append(child)
            await child.start()
            endpoint = await child.wait_ready(args.startup_timeout)
            return child, endpoint

        child, endpoint = await launch()
        origin_endpoint = origin.endpoint if origin else None
        # HEAD validates configuration and no-body behavior without warming HTTP cache.
        await probe(scenario, endpoint, origin_endpoint, method="HEAD")
        if args.warmup > 0:
            warmup = await run_phase(scenario, endpoint, origin_endpoint, args.warmup, args.request_limit)
            record["phases"]["warmup"] = warmup
            require(warmup["counts"]["errors"] == 0 and warmup["counts"]["successful"] > 0, "warmup failed")
        if scenario["cache"] in {"cold", "warm", "revalidation", "mixed"}:
            await child.stop()
            child, endpoint = await launch()
            record["preparation"] = await prepare_cache(scenario, endpoint, origin)
        else:
            record["preparation"] = {"method": "no application cache"}
        before = origin.snapshot() if origin else None
        record["server_process_before"] = sample_process(child.process.pid)
        peak_rss = None
        observations = 0
        sampling_stop = asyncio.Event()

        async def sample():
            nonlocal peak_rss, observations
            while not sampling_stop.is_set():
                observation = sample_process(child.process.pid)
                observations += 1
                if observation["rss_bytes"] is not None:
                    peak_rss = max(peak_rss or 0, observation["rss_bytes"])
                try:
                    await asyncio.wait_for(sampling_stop.wait(), 1)
                except asyncio.TimeoutError:
                    pass

        sampler = asyncio.create_task(sample())
        try:
            measured = await run_phase(scenario, endpoint, origin_endpoint, args.measurement, args.request_limit)
            record["phases"]["measurement"] = measured
        finally:
            sampling_stop.set()
            await sampler
        record["server_process_after"] = sample_process(child.process.pid)
        if child.metrics is not None:
            record["server_metrics"] = dict(available=True, snapshot=child.metrics,
                note="latest periodic process-lifetime snapshot, includes preparation/warmup; not measurement deltas")
        record["server_sampled_peak_rss_bytes"] = peak_rss
        record["server_sample_count"] = observations
        require(child.process.returncode is None, "server exited during measurement")
        require(measured["counts"]["errors"] == 0 and measured["counts"]["successful"] > 0, "measurement contains failed requests or no completions")
        if not args.smoke:
            require(not measured["request_limit_reached"], "request cap ended the measurement early")
        if origin:
            record["origin_measurement"] = origin_delta(before, origin.snapshot())
            record["origin_peak_connections"] = origin.counts["peak_connections"]
            check_origin(scenario, measured, record["origin_measurement"])
        record["state"] = "complete"
    except asyncio.CancelledError:
        interrupted = True
        record["state"] = "interrupted"
        record["diagnostics"].append("interrupted; only completed phase statistics are retained")
    except Exception as error:
        record["diagnostics"].append(f"{type(error).__name__}: {str(error)[:2048]}")
    finally:
        for child in reversed(children):
            try:
                await child.stop()
            except Exception as error:
                record["state"] = "failed"
                record["diagnostics"].append(f"child cleanup: {str(error)[:512]}")
        record["processes"] = [child.diagnostic() for child in children]
        if origin:
            try:
                await origin.close()
            except Exception as error:
                record["state"] = "failed"
                record["diagnostics"].append(f"origin cleanup: {str(error)[:512]}")
        if temporary:
            try:
                temporary.cleanup()  # only the TemporaryDirectory owned by this repeat
            except OSError as error:
                record["state"] = "failed"
                record["diagnostics"].append(f"temporary cleanup: {str(error)[:512]}")
        record["finished_utc"] = datetime.now(timezone.utc).isoformat()
        validate_result(record)
        with (output / "result.json").open("x", encoding="utf-8") as stream:
            json.dump(record, stream, indent=2, allow_nan=False)
            stream.write("\n")
    if interrupted:
        raise asyncio.CancelledError()
    return output / "result.json", record["state"]


async def run(spec, profile, args, repo):
    scenario = scenario_from(spec, args)
    args.binary = Path(args.binary).resolve()
    if args.build:
        require(args.build_dir, "--build requires --build-dir")
        builder = await OwnedProcess(["cmake", "--build", str(Path(args.build_dir).resolve()), "--config", "Release", "--target", "httpserver"]).start()
        try:
            await asyncio.wait_for(builder.process.wait(), 600)
            await asyncio.gather(*builder.readers)
            require(builder.process.returncode == 0, "build failed: " + builder.diagnostic()["tail"][-2048:])
        finally:
            await builder.stop()
    require(args.binary.is_file(), "binary is not a file")
    source = provenance(repo, args.binary, args.build_dir)
    source["qualification_commit_attestation"] = args.qualified_commit
    source["invocation"] = args.invocation
    if not args.smoke:
        sys.path.insert(0, str(repo / "tools"))
        from qualify import verify_record, source_identity
        require(args.repeats >= 5, "measurements require at least five repeats")
        current = source_identity(repo)
        source["candidate_source_sha256"] = current["sha256"]
        if args.candidate_record:
            qualification = json.loads(Path(args.candidate_record).read_text(encoding="utf-8"))
            verify_record(qualification, args.binary, repo)
            source["qualification"] = dict(scope="local candidate only; final M platform gate pending",
                record_sha256=hashlib.sha256(Path(args.candidate_record).read_bytes()).hexdigest())
        else:
            require(args.qualified_commit == source["commit"] and not source["dirty"], "measurement requires clean tree and explicit M-qualified commit attestation")
            records = [json.loads(Path(p).read_text(encoding="utf-8")) for p in args.platform_record]
            require({r.get("platform") for r in records} == {"Windows", "Linux", "Darwin"}, "M needs Windows, Linux and Darwin qualification records")
            for r in records:
                require(r.get("state") == "passed" and r.get("source", {}).get("sha256") == current["sha256"] and
                        r.get("configuration") == "Release" and r.get("sanitizer") == "none", "platform qualification mismatch")
                require(r.get("tests") and all(t.get("passed") for t in r["tests"]), "platform record has missing or failed tests")
            matching = [r for r in records if r.get("binary", {}).get("sha256") == source["binary_sha256"]]
            require(len(matching) == 1, "one local qualification record must identify this binary")
            local = matching[0]
            verify_record(local, args.binary, repo)
            source["qualification"] = dict(scope="three native platform gates", records=args.platform_record)
        cache = source["cmake"]
        require(cache.get("CMAKE_BUILD_TYPE") == "Release" or
                ("Release" in cache.get("CMAKE_CONFIGURATION_TYPES", "").split(";") and args.binary.parent.name == "Release"), "cannot establish Release build")
        require(cache.get("CMAKE_CXX_COMPILER"), "compiler metadata unavailable")
    destination = Path(args.output).resolve()
    failed = False
    for ordinal in range(1, args.repeats + 1):
        path, state = await repeat(scenario, profile, args, source, destination, ordinal)
        print(json.dumps(dict(result=str(path), state=state)), flush=True)
        if state != "complete":
            # CTest captures this output even when its temporary result directory
            # is unavailable. Include counts and bounded child stderr (sanitizers).
            print(path.read_text(encoding="utf-8"), file=sys.stderr, flush=True)
        failed |= state != "complete"
    if not args.smoke:
        require(source_identity(repo)["sha256"] == source["candidate_source_sha256"], "source changed during repeats; results invalid")
    return 1 if failed else 0
