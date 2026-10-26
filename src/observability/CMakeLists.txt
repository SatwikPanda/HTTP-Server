# Observability module (track H). Discovered automatically by cmake/ParallelModules.cmake.
add_library(observability STATIC
    metrics_registry.cpp
    bounded_logger.cpp
    process_sampler.cpp
    stop_hooks.cpp
    status_audit.cpp
)
target_include_directories(observability PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_features(observability PUBLIC cxx_std_20)
set_target_properties(observability PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(observability PUBLIC http concurrency net core Threads::Threads)
if(WIN32)
    target_compile_definitions(observability PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()
if(MSVC)
    target_compile_options(observability PRIVATE /W4)
else()
    target_compile_options(observability PRIVATE -Wall -Wextra -Wpedantic)
endif()
