# M — correctness qualification

Branch: `codex/lmn-completion`, based on `e24077f`. The production A–L assembly
and prerequisite history are described in [application-assembly.md](application-assembly.md).
This delivery is an uncommitted candidate. Native Windows evidence is recorded
separately below; new Linux/macOS runs remain required for final M acceptance.
Historical [52-suite CI](ci-portability.md) is not evidence for the new tests.

## Executable gate

The strict inventory now has **54 suites** with benchmarks enabled: the previous
52, `resource_soak_test`, and `results_tool_test`. The C++ inventory checks native
registration at configure time. `tools/qualify.py` checks the complete CTest
inventory and labels, builds every target, runs all suites and verifies JUnit
contains every expected name with no failure, skip or unexecuted test. Contract
object targets do not count as runtime tests. Milestone 32's tool test establishes
reporting correctness; it does not establish completed performance qualification.

```powershell
cmake -S . -B build/lmn -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DBUILD_BENCHMARKS=ON -DREQUIRE_ALL_MILESTONES=ON
python tools/qualify.py --build-dir build/lmn --binary build/lmn/httpserver.exe --output build/lmn/qualification.json --jobs 6
```

On Linux/macOS omit `.exe`. With Visual Studio, select its generator in a new
build directory and use `build/lmn/Release/httpserver.exe`. Each invocation needs
a new output filename. Logs, the inventory and JUnit sit beside the JSON result.
The JSON records compiler identity, OS, configure flags, exact commands, source
file hashes (including untracked code), Git commit/dirty state and executable
hash. Source text hashes normalize CRLF to LF so native-platform records match.
Source and binary changes during validation fail the record. Documentation and
generated results are excluded from the code fingerprint.

The updated CI retains native qualification records and failure logs as artifacts.
It also runs the full Linux AddressSanitizer/UndefinedBehaviorSanitizer gate.
This configuration is implemented; its new remote results have not been run here.

## Resource and diagnostic coverage

The soak runs all six required application/driver combinations repeatedly, using
real loopback sockets, files, production sessions, helpers, cache and drivers.
Four clients mix static GET/HEAD, uncached forwarding and cache hits. An origin
barrier holds replacement while readers check the old ETag/body pair; releasing
it publishes the new pair. More keys than a two-entry cache force eviction.
Every cycle joins drivers/helpers and checks zero active clients/transactions,
queued/running helper jobs, readers, candidates and retained cache allocations.
Total cache charge is checked against the configured default bound.

After explicit thread-runtime warmup (32 contending waiters), sampler startup and
one application warmup cycle, six complete cycles must return native handles/file
descriptors and threads to baseline. Windows/OS bookkeeping may retire just after
join, so the sampler waits at most two seconds for the **same** baseline; it does
not add a growth allowance. RSS is recorded as an observation, not treated as a
direct allocation/leak counter. Windows uses native handle/thread queries;
Linux uses `/proc/self/fd` and `/proc/self/task`; macOS uses libproc/Mach queries.
An optional Windows process snapshot also records handle types, which helps
distinguish file/socket growth from process-level event initialization.

For a longer local run, execute `resource_soak_test 20` directly from its build
directory (`tests/system/Release/` for Visual Studio). Retain stdout. Avoid
overlapping network gates: a 100-cycle run alongside another gate exhausted this
host's ephemeral ports. No global TCP settings are changed by the tests.

Diagnostic options are target-scoped and propagate to the whole application:

```powershell
# MSVC, in a separate build directory
cmake -S . -B build/asan -G "Visual Studio 16 2019" -A x64 -DBUILD_TESTING=ON -DBUILD_BENCHMARKS=ON -DREQUIRE_ALL_MILESTONES=ON -DHTTP_SANITIZER=address
# GCC/Clang on supported POSIX hosts
cmake -S . -B build/asan -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DBUILD_BENCHMARKS=ON -DREQUIRE_ALL_MILESTONES=ON -DHTTP_SANITIZER=address-undefined
# Separate supported POSIX race-diagnostic build
cmake -S . -B build/tsan -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DBUILD_BENCHMARKS=ON -DREQUIRE_ALL_MILESTONES=ON -DHTTP_SANITIZER=thread
```

Run the same qualification command against that directory. Unsupported options
fail configuration instead of silently producing an uninstrumented executable.
MSVC sanitizer DLLs must be available on PATH. ThreadSanitizer and POSIX
sanitizers are untested locally; WSL is not installed and no macOS executor is
available. ASan is not a race detector and MSVC ASan does not establish leak
sanitizer coverage.

## Defects and initial failures retained

- The original 52-suite GCC baseline passed, 44.48 seconds.
- ASan found a global buffer overread in `static_response_test.cpp`: an explicit
  26-byte copy read beyond a 25-byte literal. The fixture now derives the count
  from `sizeof`, retaining the embedded NUL and HTTP-looking binary bytes.
- An initial soak sampled one transient extra handle before OS retirement. The
  test now waits for both handle and thread counts to return to baseline, with
  the original no-growth assertion intact.
- A later Clang run retained one additional Event handle (file/socket and thread
  counts unchanged). Explicitly warming contended thread waits before measuring
  resolved the observed startup variation in a subsequent 20-cycle run. Native
  handle-type samples are retained, and the exact no-growth assertion remains;
  no general handle allowance or sanitizer suppression was added.
- The installed MSVC 19.29 ASan DLL could not initialize on Windows 11
  (`0xc0000142`). The newer installed 14.44 ASan runtime can run these binaries;
  compiler/runtime identities are recorded rather than calling the first run a pass.
- A simultaneous 100-cycle soak and ASan gate produced native error 10048
  (`address_in_use`) and failed requests. A host query observed 7,994 TIME_WAIT
  connections and a 16,384-port ephemeral range. These attempts are failed,
  host-limited evidence, not successful repeats. Network gates are then run
  sequentially. Retained local logs are under `build/lmn*`.

Final execution records and outstanding platform acceptance are listed in the
[LMN validation summary](../validation/lmn.md).
