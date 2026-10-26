# Track I fixture tests. Discovered automatically by cmake/ParallelModules.cmake when BUILD_TESTING is ON.
add_project_test(fixture_harness_test
    SOURCES fixture_harness_test.cpp
    LINKS server http concurrency net core Threads::Threads
    LABELS "parallel_foundation;milestone30"
)
