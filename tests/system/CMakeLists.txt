# Track I system matrix tests and required-suite inventory check.
add_project_test(system_matrix_test
    SOURCES system_matrix_test.cpp
    LINKS observability server http concurrency net core Threads::Threads
    LABELS milestone30
)

require_project_tests(system_matrix_test)

# Enforce that all implemented runtime acceptance targets across the repository exist.
foreach(required_target IN ITEMS
    socket_ownership_test
    echo_test_double_test
    echo_integration_test
    milestone01_test
    milestone02_test
    milestone03_test
    milestone04_test
    milestone05_test
    milestone06_test
    milestone07_test
    milestone08_test
    milestone09_test
    http_serialization_test
    http_session_test
    http_integration_test
    http_parser_test
    http_response_test
    path_policy_test
    mime_test
    conditional_test
    static_target_test
    static_response_test
    config_parser_test
    metrics_and_logging_test
    stop_hooks_test
    fixture_harness_test
    system_matrix_test
)
    if(NOT TARGET ${required_target})
        message(FATAL_ERROR "Required runtime test target is missing from inventory: ${required_target}")
    endif()
endforeach()
