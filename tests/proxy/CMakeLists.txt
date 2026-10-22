# Proxy acceptance suites (milestones 13-16). Registered through the shared
# helper so strict mode and Release CHECKs behave like the base inventory.
if(NOT TARGET proxy)
    message(FATAL_ERROR "tests/proxy requires the proxy module (src/proxy)")
endif()
add_project_test(milestone13_test SOURCES milestone13_test.cpp
    LINKS proxy http LABELS milestone13)

add_project_test(milestone14_test SOURCES milestone14_test.cpp
    LINKS proxy http concurrency net Threads::Threads LABELS milestone14 TIMEOUT 60)
add_project_test(milestone15_test SOURCES milestone15_test.cpp
    LINKS proxy http concurrency net Threads::Threads LABELS milestone15 TIMEOUT 90)
