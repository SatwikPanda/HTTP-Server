# Optional test-only tooling. Production runtime dependencies remain unchanged.
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
if(BUILD_TESTING)
    add_test(NAME benchmark_harness_test
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_harness.py")
    set_tests_properties(benchmark_harness_test PROPERTIES
        TIMEOUT 60 LABELS "benchmark_harness;milestone31"
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
    require_project_tests(benchmark_harness_test)
endif()
