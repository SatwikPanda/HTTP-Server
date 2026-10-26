# Helper service (track E, milestone 22). Discovered by the root CMake. It runs
# file/DNS work off driver threads and depends only on concurrency/net/core.
add_library(concurrency_helpers STATIC
    helper_pool.cpp
)
target_include_directories(concurrency_helpers PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_features(concurrency_helpers PUBLIC cxx_std_20)
set_target_properties(concurrency_helpers PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(concurrency_helpers PUBLIC concurrency net core Threads::Threads)
if(MSVC)
    target_compile_options(concurrency_helpers PRIVATE /W4)
else()
    target_compile_options(concurrency_helpers PRIVATE -Wall -Wextra -Wpedantic)
endif()
