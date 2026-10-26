# Optional test-only tooling. Production runtime dependencies remain unchanged.
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
if(BUILD_TESTING)
    add_test(NAME results_tool_test
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_results.py")
    set_tests_properties(results_tool_test PROPERTIES TIMEOUT 30 LABELS "correctness_gate;milestone30;milestone32"
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
    require_project_tests(results_tool_test)
    add_test(NAME benchmark_harness_test
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_harness.py")
    set_tests_properties(benchmark_harness_test PROPERTIES
        TIMEOUT 60 LABELS "benchmark_harness;milestone31"
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
    require_project_tests(benchmark_harness_test)
    foreach(driver IN ITEMS thread-pool event-loop)
        add_test(NAME benchmark_smoke_${driver}
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke_matrix.py"
                --binary "$<TARGET_FILE:httpserver>" --build-dir "${PROJECT_BINARY_DIR}" --driver "${driver}")
        set_tests_properties(benchmark_smoke_${driver} PROPERTIES
            TIMEOUT 180 LABELS "benchmark_harness;milestone31;executable_matrix"
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
        require_project_tests(benchmark_smoke_${driver})
    endforeach()
endif()
