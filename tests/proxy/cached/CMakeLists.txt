add_project_test(cache_proxy_test SOURCES transaction_test.cpp
    LINKS cache_proxy LABELS cache_proxy milestone20 milestone21 TIMEOUT 90)
add_project_test(cache_proxy_concurrency_test SOURCES concurrency_test.cpp
    LINKS cache_proxy Threads::Threads LABELS cache_proxy milestone24 TIMEOUT 90)
require_project_tests(cache_proxy_test cache_proxy_concurrency_test)
