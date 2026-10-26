"""Run a bounded, alternating five-repeat matrix and retain exclusions and failures."""
import argparse
import asyncio
import copy
import json
import sys
from pathlib import Path
from types import SimpleNamespace

sys.dont_write_bytecode = True
from harness.process import profile, provenance
from harness.runner import scenario_from, repeat
from harness.spec import manifest, matrix, require, number

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT.parent / "tools"))
from qualify import verify_record, source_identity


async def campaign(args):
    spec = manifest(ROOT / "scenarios.json")
    selected = profile(ROOT / "profiles/assembled.json")
    qualification = json.loads(args.candidate_record.read_text(encoding="utf-8"))
    verify_record(qualification, args.binary)
    require(not args.output.exists(), "campaign output already exists")
    args.output.mkdir(parents=True)
    plan = matrix(spec, args.max_connections, args.max_dataset_mib * 1024 * 1024)
    for p in plan:
        if p["family"] == "demo-smoke": p["excluded"].append("demonstration mode excluded from driver comparison")
    # Exclusions are saved before launching any subprocess and never disappear.
    record = dict(schema_version=1, evidence="candidate", source=source_identity(),
                  repeats=args.repeats, warmup_seconds=args.warmup, measurement_seconds=args.measurement,
                  plan=plan, runs=[], state="running", invocation=sys.argv,
                  order="five rounds; reverse configuration order on alternating rounds")
    record_path = args.output / "campaign.json"
    def save(): record_path.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    save()
    source = provenance(ROOT.parent, args.binary, args.build_dir)
    source["candidate_source_sha256"] = record["source"]["sha256"]
    source["invocation"] = sys.argv
    source["qualification"] = dict(scope="local candidate only; native platform/diagnostic completion pending", record=str(args.candidate_record))
    base = SimpleNamespace(binary=args.binary, candidate_record=str(args.candidate_record), smoke=False,
        warmup=args.warmup, measurement=args.measurement, startup_timeout=10, request_limit=10000000,
        max_connections=args.max_connections, max_dataset_mib=args.max_dataset_mib,
        arrival=args.arrival, rate=args.rate, queue_limit=128, timeout=10, seed=1729, filesystem_cache="uncontrolled")
    admitted = [p for p in plan if not p["excluded"]]
    try:
        for ordinal in range(1, args.repeats + 1):
            for point in admitted if ordinal % 2 else reversed(admitted):
                if (args.output / "STOP").exists():
                    raise asyncio.CancelledError("campaign stop requested between trials")
                run_args = copy.copy(base)
                run_args.scenario, run_args.driver = point["family"], point["driver"]
                run_args.concurrency, run_args.workers = point["concurrency"], point["workers"]
                run_args.object_bytes = point["object_bytes"]
                run_args.connections = "reuse" if point["reuse"] else "new"
                scenario = scenario_from(spec, run_args)
                path, state = await repeat(scenario, selected, run_args, source, args.output / "raw", ordinal)
                record["runs"].append(dict(point=point, repeat=ordinal, path=str(path), state=state))
                save()
                print(f"{len(record['runs'])}/{len(admitted) * args.repeats}: {point['family']} {point['driver']} clients={point['concurrency']} {state}", flush=True)
        require(source_identity()["sha256"] == record["source"]["sha256"], "source changed during campaign")
        record["state"] = "complete" if all(r["state"] == "complete" for r in record["runs"]) else "failed"
    except BaseException:
        record["state"] = "interrupted"
        raise
    finally:
        save()
    return 0 if record["state"] == "complete" else 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--candidate-record", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--max-connections", type=int, default=10)
    parser.add_argument("--max-dataset-mib", type=int, default=256)
    parser.add_argument("--warmup", type=float, default=1)
    parser.add_argument("--measurement", type=float, default=5)
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--arrival", choices=("closed", "open"), default="closed")
    parser.add_argument("--rate", type=float, default=100)
    args = parser.parse_args()
    number(args.rate, .01, 100000, "arrival rate")
    for name, lo, hi, integer in (("max_connections", 1, 1000, True), ("max_dataset_mib", 1, 1024, True),
                                  ("warmup", 0, 600, False), ("measurement", .01, 3600, False), ("repeats", 5, 100, True)):
        number(getattr(args, name), lo, hi, name, integer)
    for name in ("binary", "build_dir", "candidate_record", "output"):
        setattr(args, name, getattr(args, name).resolve())
    raise SystemExit(asyncio.run(campaign(args)))
