"""Explicit executable profiles, owned child processes, and bounded diagnostics."""
import asyncio
import hashlib
import json
import os
import platform
import re
import subprocess
import tempfile
from pathlib import Path

from .spec import VERSION, DRIVERS, APPLICATIONS, load_json, require, SpecError


def profile(path):
    value = load_json(path)
    require(set(value) == {"schema_version", "name", "applications", "drivers", "options", "readiness_regex"}, "invalid executable profile fields")
    require(value["schema_version"] == VERSION, "unsupported profile version")
    for key, allowed in (("applications", APPLICATIONS), ("drivers", DRIVERS)):
        require(isinstance(value[key], dict) and set(value[key]) <= allowed, f"invalid profile {key}")
        for name, arguments in value[key].items():
            require(isinstance(arguments, list) and len(arguments) <= 8 and all(isinstance(arg, str) and len(arg) <= 256 for arg in arguments), "invalid selector arguments")
            require(arguments or name in {"demo", "single"}, "non-default modes need explicit selectors")
    require(isinstance(value["options"], dict) and len(value["options"]) <= 64, "invalid option map")
    for option in value["options"].values():
        require(isinstance(option, str) and re.fullmatch(r"--[a-z][a-z0-9-]{0,63}", option), "invalid option spelling")
    require(len(set(value["options"].values())) == len(value["options"]), "duplicate option binding")
    require(isinstance(value["readiness_regex"], str) and len(value["readiness_regex"]) <= 256, "invalid readiness pattern")
    expression = re.compile(value["readiness_regex"])
    require({"host", "port"} <= set(expression.groupindex), "readiness pattern needs host and port groups")
    return value


def command(binary, selected, scenario, root):
    require(scenario["application"] in selected["applications"], "application unsupported by executable profile")
    require(scenario["driver"] in selected["drivers"], "driver unsupported by executable profile")
    values = {"bind": "127.0.0.1", "port": 0}
    if scenario["application"] != "demo":
        values.update(scenario["settings"])
        # Cache settings are irrelevant/invalid for static mode in H's contract.
        if scenario["application"] == "server":
            values = {key: value for key, value in values.items() if not key.startswith("cache_")}
            values["root"] = str(root)
        else:
            values["cache"] = "off" if scenario["cache"] == "off" else "on"
        if scenario["driver"] == "thread-pool":
            values["workers"] = scenario["workers"]
    require(set(values) <= set(selected["options"]), "profile lacks explicit settings: " + ", ".join(sorted(set(values) - set(selected["options"]))))
    args = [str(binary), *selected["applications"][scenario["application"]], *selected["drivers"][scenario["driver"]]]
    for key, value in values.items():
        args.extend((selected["options"][key], str(value)))
    require(len(args) <= 256 and all(len(arg) <= 8192 and "\x00" not in arg for arg in args), "command limit")
    flags = [arg for arg in args[1:] if arg.startswith("--")]
    require(len(flags) == len(set(flags)), "duplicate command option")
    return args


def process_options():
    return {"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt" else {}


class OwnedProcess:
    def __init__(self, args, readiness=None):
        self.args, self.pattern = args, re.compile(readiness) if readiness else None
        self.process = None
        self.tail = bytearray()
        self.truncated = False
        self.readers = []
        self.ready = asyncio.Event()
        self.endpoint = None
        self.forced_stop = False
        self.metrics = None

    async def start(self):
        self.process = await asyncio.create_subprocess_exec(*self.args, stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE, stdin=asyncio.subprocess.DEVNULL, **process_options())
        self.readers = [asyncio.create_task(self.drain(self.process.stdout, True)),
                        asyncio.create_task(self.drain(self.process.stderr, False))]
        return self

    async def drain(self, stream, stdout):
        window = ""
        while data := await stream.read(4096):
            self.tail.extend(data)
            if len(self.tail) > 65536:
                del self.tail[:-65536]
                self.truncated = True
            if stdout:
                window = (window + data.decode("utf-8", errors="replace"))[-8192:]
                complete_lines = window.rsplit("\n", 1)[0] if "\n" in window else ""
                match = self.pattern.search(complete_lines) if self.pattern and not self.endpoint else None
                if match:
                    port = int(match["port"])
                    if match["host"] == "127.0.0.1" and 0 < port <= 65535:
                        self.endpoint = match["host"], port
                        self.ready.set()
                for line in complete_lines.splitlines():
                    if line.startswith("METRICS "):
                        try:
                            candidate = json.loads(line[8:])
                            if candidate.get("schema_version") == 1:
                                self.metrics = candidate
                        except (ValueError, AttributeError):
                            pass
                if "\n" in window:
                    window = window.rsplit("\n", 1)[1]

    async def wait_ready(self, timeout):
        waiter = asyncio.create_task(self.ready.wait())
        exited = asyncio.create_task(self.process.wait())
        try:
            done, _ = await asyncio.wait((waiter, exited), timeout=timeout, return_when=asyncio.FIRST_COMPLETED)
            require(waiter in done and self.endpoint and self.process.returncode is None,
                    f"startup failed or timed out; exit={self.process.returncode}; {self.diagnostic()['tail'][-1024:]}")
            # The announced listener must really accept. No application GET here:
            # that would populate a supposedly cold cache.
            _, writer = await asyncio.wait_for(asyncio.open_connection(*self.endpoint), timeout)
            writer.close()
            await asyncio.wait_for(writer.wait_closed(), timeout)
            return self.endpoint
        finally:
            waiter.cancel()
            exited.cancel()
            await asyncio.gather(waiter, exited, return_exceptions=True)

    async def stop(self):
        if not self.process:
            return
        if self.process.returncode is None:
            self.forced_stop = os.name == "nt"  # Windows hidden process: terminate, no console signal.
            try:
                self.process.terminate()
            except ProcessLookupError:
                pass
            try:
                await asyncio.wait_for(self.process.wait(), 3)
            except asyncio.TimeoutError:
                self.forced_stop = True
                try:
                    self.process.kill()
                except ProcessLookupError:
                    pass
                await asyncio.wait_for(self.process.wait(), 3)
        try:
            await asyncio.wait_for(asyncio.gather(*self.readers), 2)
        except asyncio.TimeoutError:
            for reader in self.readers:
                reader.cancel()

    def diagnostic(self):
        return dict(pid=self.process.pid if self.process else None, command=self.args,
                    endpoint=self.endpoint, exit_code=self.process.returncode if self.process else None,
                    forced_stop=self.forced_stop, truncated=self.truncated,
                    tail=bytes(self.tail).decode("utf-8", errors="replace"))


async def verify_help(binary, args):
    child = await OwnedProcess([str(binary), "--help"]).start()
    try:
        await asyncio.wait_for(child.process.wait(), 5)
        await asyncio.wait_for(asyncio.gather(*child.readers), 2)
        require(child.process.returncode == 0 and not child.truncated, "unsupported or oversized --help output")
        help_text = child.diagnostic()["tail"]
        for flag in (arg for arg in args[1:] if arg.startswith("--")):
            require(re.search(r"(?<![\w-])" + re.escape(flag) + r"(?![\w-])", help_text), f"executable --help does not advertise {flag}")
        return hashlib.sha256(help_text.encode()).hexdigest()
    finally:
        await child.stop()


def environment():
    memory = connection_limit = None
    try:
        memory = os.sysconf("SC_PHYS_PAGES") * os.sysconf("SC_PAGE_SIZE")
    except (AttributeError, OSError, ValueError):
        pass
    try:
        import psutil
        if memory is None:
            memory = psutil.virtual_memory().total
    except ImportError:
        pass
    try:
        import resource
        soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
        connection_limit = {"file_descriptors_soft": soft, "file_descriptors_hard": hard}
    except (ImportError, AttributeError, OSError, ValueError):
        pass
    return dict(os=platform.platform(), python=platform.python_version(), cpu=platform.processor(),
                logical_cpus=os.cpu_count(), ram_bytes=memory, socket_limits=connection_limit,
                generator_pid=os.getpid(), placement="server, generator and optional origin share this host",
                origin_cpu_note="in-process origin CPU is included in generator CPU",
                unavailable_reason="null means not exposed by available platform sampling; never zero")


def sample_process(pid):
    # Optional test-only dependency. No psutil import reaches production binaries.
    try:
        import psutil
        process = psutil.Process(pid)
        cpu, memory = process.cpu_times(), process.memory_info()
        return dict(cpu_seconds=cpu.user + cpu.system, rss_bytes=memory.rss, source="psutil")
    except ImportError:
        return dict(cpu_seconds=None, rss_bytes=None, source="unavailable: optional psutil not installed")
    except Exception as error:
        return dict(cpu_seconds=None, rss_bytes=None, source=f"unavailable: {type(error).__name__}")


def provenance(repo, binary, build_dir):
    def git(*args):
        result = subprocess.run(["git", "-C", str(repo), *args], capture_output=True, timeout=5, **process_options())
        require(result.returncode == 0, "cannot record Git provenance")
        return result.stdout
    cache = {}
    if build_dir:
        path = Path(build_dir) / "CMakeCache.txt"
        require(path.is_file() and path.stat().st_size <= 2 * 1024 * 1024, "missing/oversized CMakeCache.txt")
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            if "=" in line and ":" in line.split("=", 1)[0]:
                key, value = line.split("=", 1)
                name = key.split(":", 1)[0]
                if name.startswith(("CMAKE_CXX_", "CMAKE_BUILD_", "CMAKE_CONFIGURATION_", "CMAKE_GENERATOR")):
                    cache[name] = value
    digest = hashlib.sha256()
    with open(binary, "rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    status = git("status", "--porcelain")
    diff_digest = hashlib.sha256()
    with tempfile.TemporaryFile() as diff:
        result = subprocess.run(["git", "-C", str(repo), "diff", "HEAD", "--binary"],
            stdout=diff, stderr=subprocess.PIPE, timeout=5, **process_options())
        require(result.returncode == 0, "cannot hash Git diff")
        diff.seek(0)
        for chunk in iter(lambda: diff.read(65536), b""):
            diff_digest.update(chunk)
    return dict(commit=git("rev-parse", "HEAD").decode().strip(),
                tree=git("rev-parse", "HEAD^{tree}").decode().strip(), dirty=bool(status),
                status=status[:65536].decode(errors="replace"), status_truncated=len(status) > 65536,
                tracked_diff_sha256=diff_digest.hexdigest(), binary=str(binary),
                binary_sha256=digest.hexdigest(), cmake=cache,
                binary_source_match="operator/build provenance; hash alone does not prove source equivalence")
