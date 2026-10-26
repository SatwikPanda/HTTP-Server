# CMake script to run the strict implemented test inventory and fail on empty selection.
if(NOT DEFINED BUILD_DIR)
    set(BUILD_DIR "${CMAKE_CURRENT_LIST_DIR}/../../build/agents")
endif()
if(NOT DEFINED CONFIG)
    set(CONFIG "Release")
endif()
if(NOT DEFINED LABELS)
    set(LABELS "milestone0[1-9]|milestone1[012]|milestone2[89]|milestone30|parallel_foundation")
endif()

execute_process(
    COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${BUILD_DIR}" -C "${CONFIG}" -L "${LABELS}" --no-tests=error --output-on-failure
    RESULT_VARIABLE ctest_result
)
if(NOT ctest_result EQUAL 0)
    message(FATAL_ERROR "Strict test matrix execution failed with exit code ${ctest_result}")
endif()
