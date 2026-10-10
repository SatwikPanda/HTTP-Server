# Repository agent instructions

These instructions apply to every agent working in this repository, including
Codex and Claude. `AGENTS.md` and `CLAUDE.md` must contain identical guidance.
When changing this policy, update both files together and verify they match.
Explicit user instructions and higher-priority agent instructions take precedence.
Read any applicable directory-specific instructions before editing those files.

## Project purpose and sources of truth

- This is a learning project implementing an HTTP/1.1 static file server and
  forward caching proxy directly on TCP sockets, using C++20 and CMake.
- Keep runtime dependencies to the C++ standard library and OS APIs. Implement
  HTTP parsing, framing, proxying, and caching here rather than introducing a
  library that replaces the functionality being learned.
- Target Windows/Winsock and Linux/macOS/POSIX with the same public contracts.
- Initial application scope is cleartext HTTP/1.1 with GET/HEAD. TLS, CONNECT
  tunnels, protocol upgrades, HTTP/2, HTTP/3, dynamic handlers, persistent caches,
  and generated compression require an explicit scope change.
- Read `IMPLEMENTATION_ROADMAP.md`, `tests/README.md`, `CMakeLists.txt`, and the
  relevant implementation and tests before making a change. Read
  `PROJECT_ARCHITECTURE.md` when present; it is currently ignored by Git and may
  be absent from a fresh checkout. Preserve it if it exists.
- The architecture and roadmap describe intended design. Actual source files,
  CMake target membership, and executed tests establish current behavior. Treat
  proposed signatures and CLI examples as plans rather than implemented APIs.
- The recorded baseline is milestones 01-03 implemented, with acceptance suites
  for 04-06 pending. Recheck the roadmap and adapters instead of assuming this
  snapshot remains current. The executable currently runs a TCP echo server.

## Working pattern

1. Inspect the working tree and applicable instructions. Preserve unrelated user
   changes and keep the task focused on the requested behavior.
2. Identify the relevant milestone, module, interfaces, and acceptance checks.
   Follow milestone dependencies; implement only the supporting work needed for
   the request. Keep the project buildable after each incremental change.
3. Reuse existing types and abstractions. Prefer a small, readable implementation
   over speculative frameworks, premature optimization, or unrelated cleanup.
4. Add or adjust meaningful regression checks for behavior changes. Build and run
   the affected suites and the earlier milestone gate as appropriate.
5. Review the diff and update documentation when behavior, commands, contracts,
   or verified milestone status changes. Record only checks actually executed.

Create directories and targets when they gain an implementation; do not generate
empty future modules. Do not commit, push, or publish unless the user requests it.

## Module boundaries and build integration

- Put public headers under `include/<module>/` and implementations under
  `src/<module>/`. Use `core`, `net`, `http`, and `cache` ownership; introduce the
  planned server, proxy, concurrency, or observability modules as needed.
- Keep OS headers and native handles out of public headers, HTTP, and cache code.
  Put platform socket/runtime work in `src/net/windows/` or `src/net/posix/`, and
  shared native helpers in `src/net/detail/`.
- Check `CMakeLists.txt` before choosing a file to extend. Legacy socket files and
  HTTP/cache scaffolding are not evidence that a feature is built or complete.
  Some existing HTTP headers are under `include/net/http/`; inspect their include
  paths and build integration before extending or relocating them.
- Select exactly one native socket/runtime backend through CMake. Add production
  sources to the correct target and explicitly link new targets to consumers and
  acceptance tests. Use target-scoped include paths, definitions, and options.
- Preserve C++20, disabled compiler extensions, `Threads::Threads`, Windows
  `ws2_32` linkage, and the existing compiler warning settings.
- Keep parsing, application decisions, and cache policy independent of drivers.
  Future thread-pool and event-loop drivers must execute the same session logic.

## C++ style and error handling

- Match nearby maintained code: four spaces, braces on the declaration/control
  line, and compact functions with early returns. Avoid reformatting unrelated
  files or copying excessive spacing from older scaffolding.
- Use `snake_case` for files, functions, variables, and enum values; use
  `PascalCase` for types; use a trailing underscore for private data members.
  Keep namespaces aligned with modules and implementation helpers private.
- Use `#pragma once` in headers. Include the matching header first in source
  files where practical, then project headers, then standard headers. Include
  dependencies directly and use module-relative paths such as `"net/socket.hpp"`.
- Prefer RAII, value types, explicit move ownership, `std::unique_ptr` for unique
  resources, `std::span` for byte buffers, and `std::string_view` for borrowed text.
  Make borrowing lifetimes clear; views must not outlive their backing storage.
- Use `core::Result<T>` / `core::Result<void>` and `core::Error` for expected
  failures in new APIs. Check a result before accessing its value. Preserve
  portable error categories, operation names, and native diagnostic codes.
- Use `ReadResult` and `WriteResult` to distinguish progress, EOF, would-block,
  and errors. Do not collapse these outcomes into a Boolean or use exceptions
  for ordinary network failures. `std::expected` is outside the C++20 baseline.
- Apply `[[nodiscard]]`, `const`, and `noexcept` where their contracts are valid.
  Comment ownership, invariants, and non-obvious platform behavior rather than
  narrating each statement.

## Networking, ownership, and resource limits

- `Socket` owns one native handle, is move-only, and closes idempotently. Keep
  opaque implementation destruction and move operations out of line.
- `NetworkRuntime` must outlive sockets, pollers, and driver threads. Stop and
  join the driver before destroying the server; join owned threads at shutdown.
- One thread owns poller registration, waits, and socket operations. Only
  `Poller::wake()` may be called concurrently. Unwatch before closing and use
  `is_current()` to validate saved readiness events and registration generations.
- Configure accepted sockets explicitly. Preserve offsets across short writes,
  handle would-block through readiness, and reject invalid zero-progress writes.
- Keep level-triggered behavior. Watch writes only while output or a connection
  attempt is pending; process readable bytes even when hangup is also reported.
  Verify pending socket errors before declaring an outbound connection successful.
- Capture native errors immediately, retry interrupted operations appropriately,
  cap native buffer lengths, and preserve POSIX SIGPIPE suppression.
- Use steady-clock deadlines for I/O and elapsed time, and a separate wall clock
  for HTTP dates. Outbound candidate attempts share a deadline and use a fresh
  socket after each failure. DNS is synchronous and is not bounded by that deadline.
- Bound buffers, queues, cache storage, retries, and drain time. Use backpressure
  and fairness budgets. Keep blocking DNS, file work, sleeps, and long logging
  operations outside the event-loop thread.
- When adding HTTP behavior, follow the roadmap's incremental parsing and framing
  rules: preserve unconsumed bytes and repeated fields, validate generated headers,
  enforce size limits, and reject ambiguous message framing.

## Validation and milestone completion

Use CMake and CTest rather than ad hoc compilation. The following commands use a
separate Ninja build directory; choose another directory if its generator differs
or Ninja is unavailable. Run from the repository root:

```powershell
cmake -S . -B build/agents -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DREQUIRE_ALL_MILESTONES=OFF
cmake --build build/agents --config Release
ctest --test-dir build/agents -C Release --output-on-failure
```

Enforce the recorded completed milestone gate with:

```powershell
cmake -S . -B build/agents -DREQUIRE_ALL_MILESTONES=ON
cmake --build build/agents --config Release
ctest --test-dir build/agents -C Release -L 'milestone0[123]' --output-on-failure
```

- Extend the strict label selection as additional milestones are verified. To
  complete a new milestone, run its own suite in strict mode as well as earlier
  suites. Running all suites in strict mode fails while future suites are pending.
- Exit code 77 means pending/skipped, never passed. Compilation of
  `tests/contracts/` object targets is a contract check, never runtime acceptance.
- Future `tests/milestone04_adapter.hpp`, `milestone05_adapter.hpp`, and
  `milestone06_adapter.hpp` must invoke production code. Only clocks and I/O may
  be faked; never simulate the feature solely to satisfy acceptance assertions.
- Use `tests/test_support.hpp` and Release-active `CHECK` assertions for new tests.
  Keep CTest timeouts and cover relevant ownership, partial I/O, deadlines, and
  error behavior with controlled tests plus real loopback integration.
- Keep network tests local: loopback, numeric addresses, and localhost. Test IPv6
  when available and report unavailable coverage. Avoid external origins and
  timing assumptions that hide retries, spins, or races.
- Check Windows, Linux, and macOS results when making portability claims. The
  existing CI matrix is in `.github/workflows/cmake.yml`; local Windows success
  alone does not establish cross-platform acceptance.
- Documentation-only changes need a diff and consistency check; a full compile
  is unnecessary unless build behavior or commands also changed.

## Communication and handoff

- Use clear, direct language. Explain the behavior changed, why it fits the
  project, and which checks establish it. Keep progress updates concise.
- Continue routine authorized implementation without repeatedly asking for
  confirmation. Ask when missing information affects scope or correctness.
- Distinguish implemented, verified, pending, skipped, and untested work. Report
  test failures and platform limitations explicitly; never mark a milestone
  complete or claim a CI result without evidence.
- Finish with a concise account of changed files, validation results, and any
  remaining work or blocker relevant to the request.
