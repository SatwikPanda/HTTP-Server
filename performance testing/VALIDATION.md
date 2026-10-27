# Correctness and scope of validation

The optimized production/test source fingerprint is
`dd61ed1349d8a31de0d515d4ca66c9fd00b69e49ecbd72650e62f9609006fb79`.
All four complete strict gates below passed **54/54, zero failed/skipped suites**.
They include the earlier milestones, both production drivers, static/proxy/cache
applications, partial I/O, candidate/deadline behavior, cancellation, cache
concurrency, resource soak, lifecycle, benchmark smoke and report-tool tests.

| Windows build | Evidence |
|---|---|
| MSVC 19.29.30159, optimized Release with PDB | [record](validation/msvc.json), [JUnit](validation/msvc.junit.xml), [CTest log](validation/msvc.ctest.log) |
| GCC 16.2.0, Release | [record](validation/gcc.json), [JUnit](validation/gcc.junit.xml), [CTest log](validation/gcc.ctest.log) |
| Clang 22.1.8, Release | [record](validation/clang.json), [JUnit](validation/clang.junit.xml), [CTest log](validation/clang.ctest.log) |
| MSVC 19.29, AddressSanitizer | [record](validation/asan.json), [JUnit](validation/asan.junit.xml), [CTest log](validation/asan.ctest.log) |

The ASan run uses the installed MSVC 14.44 runtime with compiler 19.29, because
the older runtime cannot initialize on this Windows 11 host. The same documented
workaround was used for LMN. `halt_on_error=1` was set; this is not a claim that
a newer compiler built the executable or that Linux leak checking ran here.

`parallel_work_test.cpp` now checks file size/mtime refresh after replacement,
canonical paths containing `.`, and repeated readiness registrations before the
existing candidate failure, timeout and reconnect checks. The implementation
keeps the full path-component walk and canonical containment. The opened stream
still supplies authoritative response length, and timestamp conversion retains
the existing standard-library path. No metadata is cached between requests.

The readiness cache is lazy and per executor, owned by one thread. A scope guard
unregisters every waited socket before candidate replacement, close or timeout.
File/DNS-only helper jobs allocate no poller. Creation now uses the existing work
deadline. The cache creates no new worker threads or unbounded process-wide pool.

The supplemental [native junction check](validation/junctions.json) passes ten
real HTTP checks across both drivers. It creates controlled inside/outside
directory junctions without requiring symbolic-link privilege, verifies 403 for
both final and intermediate reparse paths, and verifies an ordinary file still
returns its exact body. The fixture stops its servers and removes only its own
junction entries before cleaning the temporary tree. This supplements rather
than relabels the unsupported symbolic-link assertions in the native suites.

Some assertions inside otherwise passing native suites explicitly report
unsupported host capabilities. Windows symbolic-link creation lacks privilege;
POSIX permission-bit tests do not exercise Windows ACLs. A static-test-specific
descriptor-count check is unsupported on Windows, while the separate resource
soak does check native Windows handle/thread baselines. Zero skipped CTest suites
does not mean these unsupported subcases were exercised. IPv6-unavailable
diagnostics were not observed. ThreadSanitizer/race instrumentation was not run.

The **merged LMN baseline**, a different source fingerprint, passed native
Windows/Linux/macOS and Linux ASan/UBSan CI before merge. The main push also
passed all four jobs: [run 40392338558](https://github.com/SatwikPanda/HTTP-Server/actions/runs/40392338558).
Those platform results must not be attributed to the later local performance
changes. New Linux/macOS execution remains pending; no cross-platform speedup
is claimed from these Windows measurements. The complete milestone 32 campaign
also remains broader than this focused investigation.
