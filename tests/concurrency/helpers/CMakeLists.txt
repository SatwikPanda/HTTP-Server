# Track E tests. Registered by the root only when BUILD_TESTING is on.
if(NOT TARGET concurrency_helpers)
    message(FATAL_ERROR "tests/concurrency/helpers requires src/concurrency/helpers")
endif()
add_project_test(helper_pool_test SOURCES helper_pool_test.cpp
    LINKS concurrency_helpers concurrency net core Threads::Threads LABELS milestone22 TIMEOUT 60)
