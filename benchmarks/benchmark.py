"""Run with Python 3.10+; all subprocess arguments are passed without a shell."""
import argparse
import asyncio
import json
import sys
from pathlib import Path

sys.dont_write_bytecode = True
from harness.spec import manifest, matrix, load_json, number, validate_result, SpecError
from harness.process import profile
from harness.runner import run

ROOT = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("plan", "run", "validate-result"))
    parser.add_argument("--manifest", default=str(ROOT / "scenarios.json"))
    parser.add_argument("--profile", default=str(ROOT / "profiles/assembled.json"))
    parser.add_argument("--result")
    parser.add_argument("--binary")
    parser.add_argument("--build-dir")
    parser.add_argument("--build", action="store_true")
    parser.add_argument("--scenario", default="demo-smoke")
    parser.add_argument("--driver", choices=("single", "thread-pool", "event-loop"))
    parser.add_argument("--concurrency", type=int, default=1)
    parser.add_argument("--workers", type=int, default=1)
    parser.add_argument("--object-bytes", type=int, default=65536)
    parser.add_argument("--connections", choices=("reuse", "new"), default="reuse")
    parser.add_argument("--arrival", choices=("closed", "open"), default="closed")
    parser.add_argument("--rate", type=float, default=100)
    parser.add_argument("--queue-limit", type=int, default=128)
    parser.add_argument("--timeout", type=float)
    parser.add_argument("--startup-timeout", type=float, default=10)
    parser.add_argument("--max-connections", type=int, default=100)
    parser.add_argument("--max-dataset-mib", type=int, default=256)
    parser.add_argument("--request-limit", type=int, default=1000000)
    parser.add_argument("--filesystem-cache", choices=("uncontrolled", "warm", "cold-external"), default="uncontrolled")
    parser.add_argument("--seed", type=int)
    parser.add_argument("--smoke", action="store_true")
    parser.add_argument("--warmup", type=float)
    parser.add_argument("--measurement", type=float)
    parser.add_argument("--repeats", type=int)
    parser.add_argument("--qualified-commit")
    parser.add_argument("--candidate-record", help="local qualification JSON; results remain provisional")
    parser.add_argument("--platform-record", action="append", default=[], help="M qualification JSON; required for each native OS")
    parser.add_argument("--output", default=str(ROOT / "results"))
    args = parser.parse_args()
    try:
        if args.action == "validate-result":
            if not args.result:
                parser.error("validate-result requires --result")
            validate_result(load_json(args.result))
            print("result schema and count invariants passed")
            return 0
        spec = manifest(args.manifest)
        selected = profile(args.profile)
        for name, low, high in (("max_connections", 1, 1000), ("max_dataset_mib", 1, 1024), ("request_limit", 1, 10000000)):
            number(getattr(args, name), low, high, name, True)
        if args.action == "plan":
            points = matrix(spec, args.max_connections, args.max_dataset_mib * 1024 * 1024)
            families = {family["id"]: family for family in spec["families"]}
            for point in points:
                if families[point["family"]]["application"] not in selected["applications"] or point["driver"] not in selected["drivers"]:
                    point["excluded"].append("selected executable profile does not support application/driver")
            print(json.dumps(dict(schema_version=1, profile=selected["name"], plan=points), indent=2))
            return 0
        if not args.binary:
            parser.error("run requires --binary")
        args.warmup = args.warmup if args.warmup is not None else (.05 if args.smoke else spec["defaults"]["warmup_seconds"])
        args.measurement = args.measurement if args.measurement is not None else (.15 if args.smoke else spec["defaults"]["measurement_seconds"])
        args.repeats = args.repeats if args.repeats is not None else (1 if args.smoke else spec["defaults"]["repeats"])
        number(args.warmup, 0, 600, "warmup")
        number(args.measurement, .01, 3600, "measurement")
        number(args.repeats, 1, 100, "repeats", True)
        number(args.startup_timeout, .1, 60, "startup timeout")
        args.invocation = sys.argv
        return asyncio.run(run(spec, selected, args, ROOT.parent))
    except (SpecError, ValueError, OSError) as error:
        print(f"benchmark rejected: {error}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("benchmark interrupted; owned resources closed and incomplete repeat recorded", file=sys.stderr)
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
