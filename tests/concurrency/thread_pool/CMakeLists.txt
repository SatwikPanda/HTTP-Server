# Track F tests. Driver cases use production echo sessions; the HTTP cases run
# the real HttpSession, static service and helper pool through the driver.
if(NOT TARGET concurrency_thread_pool)
    message(FATAL_ERROR "tests/concurrency/thread_pool requires src/concurrency/thread_pool")
endif()
set(thread_pool_test_links concurrency_thread_pool concurrency_helpers concurrency http net core Threads::Threads)
if(TARGET server)
    list(APPEND thread_pool_test_links server)
endif()
add_project_test(thread_pool_test SOURCES thread_pool_test.cpp
    LINKS ${thread_pool_test_links} LABELS milestone23 TIMEOUT 90)
if(TARGET server)
    target_compile_definitions(thread_pool_test PRIVATE THREAD_POOL_TEST_STATIC=1)
endif()
