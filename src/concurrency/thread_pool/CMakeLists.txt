# Thread-pool driver (track F, milestone 23). Discovered by the root CMake after
# the helper module. It consumes session factories and never links HTTP code.
if(NOT TARGET concurrency_helpers)
    message(FATAL_ERROR "src/concurrency/thread_pool requires src/concurrency/helpers")
endif()
add_library(concurrency_thread_pool STATIC
    thread_pool_driver.cpp
)
target_include_directories(concurrency_thread_pool PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_features(concurrency_thread_pool PUBLIC cxx_std_20)
set_target_properties(concurrency_thread_pool PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(concurrency_thread_pool PUBLIC concurrency_helpers concurrency net core Threads::Threads)
if(MSVC)
    target_compile_options(concurrency_thread_pool PRIVATE /W4)
else()
    target_compile_options(concurrency_thread_pool PRIVATE -Wall -Wextra -Wpedantic)
endif()
