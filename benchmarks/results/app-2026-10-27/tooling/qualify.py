"""Build and run the complete strict inventory, retaining source/binary/test evidence."""
import argparse
import hashlib
import json
import os
import platform
import re
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
import xml.etree.ElementTree as ET

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def git(repo, *args):
    result = subprocess.run(["git", "-C", str(repo), *args], capture_output=True, timeout=30)
    require(result.returncode == 0, "Git provenance unavailable")
    return result.stdout


def source_identity(repo=ROOT):
    # Include untracked code too: HEAD plus a diff hash alone misses new files.
    names = git(repo, "ls-files", "--cached", "--others", "--exclude-standard", "-z").split(b"\0")
    files = {}
    for raw in sorted(set(names)):
        if not raw:
            continue
        name = raw.decode("utf-8")
        path = Path(name)
        if path.suffix.lower() not in {".cpp", ".hpp", ".h", ".cmake", ".txt", ".py", ".json", ".yml", ".yaml"}:
            continue
        if path.parts[0] in {"src", "include", "app", "cmake", "tests", "tools", ".github"} or name == "CMakeLists.txt" or (
                path.parts[0] == "benchmarks" and path.suffix in {".py", ".json", ".txt"} and
                len(path.parts) > 1 and path.parts[1] not in {"results", "reports"}):
            # Git checkouts use CRLF on some Windows hosts. These inputs are text;
            # normalize line endings so native-platform evidence can be matched.
            files[name] = hashlib.sha256((repo / path).read_bytes().replace(b"\r\n", b"\n")).hexdigest() if (repo / path).is_file() else "deleted"
    require(files, "empty source inventory")
    encoded = json.dumps(files, sort_keys=True, separators=(",", ":")).encode()
    return dict(sha256=hashlib.sha256(encoded).hexdigest(), files=files,
                commit=git(repo, "rev-parse", "HEAD").decode().strip(),
                dirty=bool(git(repo, "status", "--porcelain")))


def read_cache(build):
    result = {}
    for line in (build / "CMakeCache.txt").read_text(encoding="utf-8").splitlines():
        if "=" in line and ":" in line.split("=", 1)[0] and not line.startswith("//"):
            key, value = line.split("=", 1)
            result[key.split(":", 1)[0]] = value
    return result


def check_inventory(inventory, repo=ROOT):
    rows = re.findall(r'\{"([a-z_0-9]+)", "[^"]+", "([^"]+)", true\}',
                      (repo / "tests/system/test_inventory.hpp").read_text(encoding="utf-8"))
    required = {name: set(labels.split(";")) for name, labels in rows}
    required.update({"benchmark_harness_test": {"milestone31"},
                     "benchmark_smoke_thread-pool": {"milestone31"},
                     "benchmark_smoke_event-loop": {"milestone31"},
                     "results_tool_test": {"milestone30", "milestone32"}})
    tests = inventory["tests"]
    names = [test["name"] for test in tests]
    require(len(names) == len(set(names)), "duplicate CTest names")
    require(set(names) == set(required), "CTest inventory mismatch: " + str(set(names) ^ set(required)))
    labels_seen = set()
    for test in tests:
        labels = next((set(p["value"]) for p in test["properties"] if p["name"] == "LABELS"), set())
        require(required[test["name"]] <= labels, "incorrect labels: " + test["name"])
        labels_seen |= labels
    require({f"milestone{i:02}" for i in range(1, 32)} <= labels_seen, "missing milestone labels")
    return names


def check_junit(path, expected):
    root = ET.parse(path).getroot()
    cases = list(root.iter("testcase"))
    names = [case.get("name") for case in cases]
    require(len(names) == len(set(names)) and set(names) == set(expected), "executed test inventory differs")
    results = []
    for case in cases:
        passed = case.get("status") == "run" and not any(case.find(tag) is not None for tag in ("failure", "error", "skipped"))
        results.append(dict(name=case.get("name"), passed=passed, seconds=float(case.get("time", "0"))))
    require(all(item["passed"] for item in results), "required tests failed, skipped or did not run")
    return results


def verify_record(record, binary, repo=ROOT):
    require(record.get("schema_version") == 1 and record.get("state") == "passed", "qualification record did not pass")
    require(record.get("source", {}).get("sha256") == source_identity(repo)["sha256"], "source changed since qualification")
    require(record.get("binary", {}).get("sha256") == sha256(binary), "binary changed since qualification")
    require(record.get("configuration") == "Release" and record.get("sanitizer") == "none", "measure uninstrumented Release code")
    require(record.get("platform") == platform.system(), "qualification belongs to another platform")


def qualify(build, binary, output, jobs=4):
    require(not output.exists(), "evidence output already exists; select a new filename")
    output.parent.mkdir(parents=True, exist_ok=True)
    record = dict(schema_version=1, state="failed", platform=platform.system(), os=platform.platform(),
                  started_utc=datetime.now(timezone.utc).isoformat(), configuration="Release", commands=[],
                  scope="local native gate; combine matching records from Windows, Linux and Darwin for M")
    def execute(command, name, capture=False):
        log = output.with_suffix("." + name + ".log")
        record["commands"].append(dict(argv=command, log=str(log)))
        print("Running " + subprocess.list2cmdline(command), flush=True)
        with log.open("xb") as stream:
            run = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, timeout=1800)
        require(run.returncode == 0, f"{name} failed ({run.returncode}); see {log}")
        return json.loads(log.read_text(encoding="utf-8")) if capture else None
    try:
        cache = read_cache(build)
        require(Path(cache["CMAKE_HOME_DIRECTORY"]).resolve() == ROOT, "build belongs to another source tree")
        require(all(cache.get(key) == "ON" for key in ("BUILD_TESTING", "BUILD_BENCHMARKS", "REQUIRE_ALL_MILESTONES")),
                "qualification requires BUILD_TESTING, BUILD_BENCHMARKS and REQUIRE_ALL_MILESTONES=ON")
        require(cache.get("CMAKE_BUILD_TYPE") == "Release" or "Release" in cache.get("CMAKE_CONFIGURATION_TYPES", "").split(";"), "Release unavailable")
        executable = "httpserver.exe" if os.name == "nt" else "httpserver"
        expected_binary = build / "Release" / executable if cache.get("CMAKE_CONFIGURATION_TYPES") else build / executable
        require(binary == expected_binary, "binary must be the selected build's Release executable")
        record["source"] = source_identity()
        record["sanitizer"] = cache.get("HTTP_SANITIZER", "none")
        record["cmake"] = {key: value for key, value in cache.items() if key.startswith(("CMAKE_CXX_", "CMAKE_GENERATOR", "CMAKE_BUILD_", "CMAKE_CONFIGURATION_"))}
        compiler_files = sorted((build / "CMakeFiles").glob("*/CMakeCXXCompiler.cmake"))
        require(compiler_files, "compiler identification unavailable")
        record["compiler"] = dict(re.findall(r'set\((CMAKE_CXX_COMPILER(?:_ID|_VERSION)?) "([^"]*)"\)',
                                             compiler_files[-1].read_text(encoding="utf-8")))
        execute(["cmake", "--build", str(build), "--config", "Release", "--parallel", str(jobs)], "build")
        require(binary.is_file(), "built executable is absent")
        record["binary"] = dict(path=str(binary), sha256=sha256(binary))
        app_binary = build / "app" / ("Release" if cache.get("CMAKE_CONFIGURATION_TYPES") else "") / ("http_app.exe" if os.name == "nt" else "http_app")
        require(app_binary.is_file(), "built application executable is absent")
        record["additional_binaries"] = {"http_app": dict(path=str(app_binary), sha256=sha256(app_binary), bytes=app_binary.stat().st_size)}
        inventory = execute(["ctest", "--test-dir", str(build), "-C", "Release", "--show-only=json-v1"], "inventory", True)
        record["expected"] = check_inventory(inventory)
        junit = output.with_suffix(".junit.xml")
        execute(["ctest", "--test-dir", str(build), "-C", "Release", "--no-tests=error", "--output-on-failure",
                 "--output-junit", str(junit)], "ctest")
        record["tests"] = check_junit(junit, record["expected"])
        require(source_identity()["sha256"] == record["source"]["sha256"], "source changed during validation")
        require(sha256(binary) == record["binary"]["sha256"], "binary changed during validation")
        require(sha256(app_binary) == record["additional_binaries"]["http_app"]["sha256"], "application binary changed during validation")
        record["state"] = "passed"
    except (ValueError, OSError, KeyError, subprocess.TimeoutExpired) as error:
        record["error"] = str(error)
        print(str(error), file=sys.stderr)
    finally:
        record["finished_utc"] = datetime.now(timezone.utc).isoformat()
        output.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    print(f"{record['state']}: {output}", flush=True)
    return 0 if record["state"] == "passed" else 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    require(1 <= args.jobs <= 64, "invalid job count")
    raise SystemExit(qualify(args.build_dir.resolve(), args.binary.resolve(), args.output.resolve(), args.jobs))
