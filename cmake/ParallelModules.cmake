# Paths are owned by the named tracks. Missing modules do not create targets.
# CONFIGURE_DEPENDS discovers a newly merged module on the next build.
set(parallel_modules
    src/concurrency/helpers
    src/observability
    src/cache/storage
    src/cache/policy
    src/server
    src/proxy
    src/proxy/cached
    src/concurrency/thread_pool
    src/concurrency/event_loop
    src/application
)
if(BUILD_TESTING)
    list(APPEND parallel_modules
        tests/fixtures tests/server tests/proxy tests/cache/storage tests/cache/policy
        tests/concurrency/helpers tests/concurrency/thread_pool tests/concurrency/event_loop
        tests/observability tests/system)
endif()
option(BUILD_BENCHMARKS "Build optional benchmark tooling" OFF)
if(BUILD_BENCHMARKS)
    list(APPEND parallel_modules benchmarks)
endif()
foreach(module IN LISTS parallel_modules)
    file(GLOB module_cmake CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/${module}/CMakeLists.txt")
    if(module_cmake)
        add_subdirectory("${module}")
    endif()
endforeach()
