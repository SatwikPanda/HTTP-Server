# Track H tests. Discovered automatically by cmake/ParallelModules.cmake when BUILD_TESTING is ON.
add_project_test(config_parser_test
    SOURCES config_parser_test.cpp
    LINKS observability core Threads::Threads
    LABELS milestone29
)
add_project_test(metrics_and_logging_test
    SOURCES metrics_and_logging_test.cpp
    LINKS observability server http concurrency net core Threads::Threads
    LABELS milestone29
)
add_project_test(stop_hooks_test
    SOURCES stop_hooks_test.cpp
    LINKS observability core Threads::Threads
    LABELS "milestone28;milestone29"
)
