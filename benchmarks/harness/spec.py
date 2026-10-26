"""Versioned workload contracts and deterministic, bounded data generation."""
import hashlib
import json
import math
import random
from pathlib import Path

VERSION = 1
APPLICATIONS = {"demo", "server", "proxy"}
DRIVERS = {"single", "thread-pool", "event-loop"}
CACHE_STATES = {"off", "cold", "warm", "revalidation", "mixed"}


class SpecError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise SpecError(message)


def number(value, low, high, name, integer=False):
    require(type(value) in ((int,) if integer else (int, float)) and
            math.isfinite(value) and low <= value <= high, f"invalid {name}: {value!r}")
    return value


def load_json(path):
    require(Path(path).stat().st_size <= 1024 * 1024, "JSON input exceeds 1 MiB")
    def unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, f"duplicate JSON key: {key}")
            result[key] = value
        return result
    return json.loads(Path(path).read_text(encoding="utf-8"), object_pairs_hook=unique,
                      parse_constant=lambda value: require(False, f"non-finite JSON: {value}"))


def manifest(path):
    value = load_json(path)
    require(set(value) == {"schema_version", "axes", "defaults", "families"}, "unknown/missing manifest field")
    require(value["schema_version"] == VERSION, "unsupported manifest version")
    axes = value["axes"]
    require(set(axes) == {"concurrency", "workers", "object_bytes", "reuse"}, "invalid axes")
    for name, high in (("concurrency", 1000), ("workers", 256), ("object_bytes", 16 * 1024 * 1024)):
        require(isinstance(axes[name], list) and 0 < len(axes[name]) <= 32, f"invalid {name} axis")
        for item in axes[name]:
            number(item, 1, high, name, integer=True)
    require(axes["reuse"] == [True, False], "reuse axis must cover persistent and new connections")
    defaults = value["defaults"]
    require(set(defaults) == {"seed", "warmup_seconds", "measurement_seconds", "repeats", "request_timeout_seconds", "settings"}, "invalid defaults")
    number(defaults["seed"], 0, 2**32 - 1, "seed", True)
    number(defaults["warmup_seconds"], 0, 600, "warmup")
    number(defaults["measurement_seconds"], 0.01, 3600, "measurement")
    number(defaults["repeats"], 5, 100, "measured repeats", True)
    number(defaults["request_timeout_seconds"], 0.05, 300, "request timeout")
    require(isinstance(defaults["settings"], dict), "settings must be an object")
    for key, setting in defaults["settings"].items():
        require(key.replace("_", "").isalnum() and isinstance(setting, (str, int)) and not isinstance(setting, bool), "invalid setting")
    names = set()
    require(isinstance(value["families"], list) and 1 <= len(value["families"]) <= 64, "invalid family count")
    fields = {"id", "application", "drivers", "distribution", "objects", "cache", "origin_delay_ms", "origin_bytes_per_second", "client_bytes_per_second", "method", "required"}
    for family in value["families"]:
        require(set(family) == fields, "unknown/missing family field")
        name = family["id"]
        require(isinstance(name, str) and name.replace("-", "").isalnum() and name not in names, "invalid/duplicate family id")
        names.add(name)
        require(family["application"] in APPLICATIONS and family["cache"] in CACHE_STATES, "invalid application/cache")
        require(isinstance(family["drivers"], list) and family["drivers"] and set(family["drivers"]) <= DRIVERS, "invalid drivers")
        require(family["distribution"] in {"hot", "uniform", "zipf"}, "invalid distribution")
        number(family["objects"], 1, 64, "objects", True)
        number(family["origin_delay_ms"], 0, 10000, "origin delay")
        for field in ("origin_bytes_per_second", "client_bytes_per_second"):
            number(family[field], 0, 2**30, field, True)
        require(family["method"] in {"GET", "HEAD"} and type(family["required"]) is bool, "invalid method/requirement")
        require(family["application"] == "proxy" or family["cache"] == "off", "cache requires proxy")
    return value


def payload(seed, index, offset, count):
    """Generate at most one caller-bounded slice; never retain entire objects."""
    block = hashlib.sha256(f"http-bench-v1:{seed}:{index}".encode("ascii")).digest()
    start = offset % len(block)
    return (block * ((start + count + len(block) - 1) // len(block)))[start:start + count]


def choose_index(seed, sequence, count, distribution):
    # Sequence-derived RNG avoids mutable shared state and completion-order bias.
    if distribution == "hot":
        return 0
    rng = random.Random((seed << 32) + sequence)
    if distribution == "uniform":
        return rng.randrange(count)
    return rng.choices(range(count), weights=[1 / (index + 1) for index in range(count)])[0]


def matrix(spec, max_connections=100, max_dataset_bytes=256 * 1024 * 1024):
    """Anchor sweeps, not a Cartesian product. Exclusions remain visible."""
    result = []
    for family in spec["families"]:
        if family["application"] == "demo":
            points = {(1, 1, 15, True)}
        else:
            points = {(clients, 4, 65536, True) for clients in spec["axes"]["concurrency"]}
            if family["id"] in {"hot-file", "varied-files", "proxy-off", "proxy-warm"}:
                points |= {(10, 4, size, True) for size in spec["axes"]["object_bytes"]}
            if family["id"] in {"hot-file", "proxy-warm"}:
                points |= {(50, workers, 65536, True) for workers in spec["axes"]["workers"]}
            if family["id"] in {"hot-file", "proxy-off", "head-file"}:
                points.add((10, 4, 65536, False))
        for driver in family["drivers"]:
            seen = set()
            for clients, workers, size, reuse in sorted(points):
                workers = workers if driver == "thread-pool" else 1
                point = (clients, workers, size, reuse)
                if point in seen:
                    continue
                seen.add(point)
                reasons = []
                if clients > max_connections:
                    reasons.append("operator connection limit")
                if size * family["objects"] > max_dataset_bytes:
                    reasons.append("dataset byte limit")
                result.append(dict(family=family["id"], driver=driver, concurrency=clients,
                                   workers=workers, object_bytes=size, reuse=reuse,
                                   required=family["required"], excluded=reasons))
    return result


def validate_result(value):
    """Structural and arithmetic checks on our emitted raw record, including failures."""
    require(value.get("schema_version") == VERSION, "unsupported result version")
    for key in ("run_id", "repeat", "evidence", "state", "scenario", "provenance", "environment", "processes", "phases", "diagnostics"):
        require(key in value, f"missing result {key}")
    require(value["evidence"] in {"smoke", "measurement"}, "invalid evidence kind")
    require(value["state"] in {"complete", "failed", "interrupted"}, "invalid run state")
    for phase in value["phases"].values():
        counts = phase["counts"]
        for key in ("attempted", "issued", "successful", "errors", "timeouts", "overload_dropped", "useful_bytes", "response_wire_bytes", "request_wire_bytes"):
            number(counts[key], 0, 2**63 - 1, key, True)
        require(counts["attempted"] == counts["successful"] + counts["errors"], "attempt accounting mismatch")
        require(counts["issued"] <= counts["attempted"] and counts["timeouts"] <= counts["errors"], "invalid issue/timeout count")
        require(counts["overload_dropped"] <= counts["errors"], "invalid overload count")
        for name in ("success_latency", "dispatch_latency", "queue_delay"):
            hist = phase[name]
            require(len(hist["upper_bounds_us"]) == len(hist["counts"]), "invalid histogram shape")
            require(sum(hist["counts"]) == hist["count"], "histogram count mismatch")
        require(phase["success_latency"]["count"] == counts["successful"], "successful latency count mismatch")
        require(phase["duration_seconds"] > 0, "invalid duration")
    return value
