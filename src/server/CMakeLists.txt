# Static file service (track A). The root CMake discovers this file; it links
# against the shared http/concurrency targets and never edits root main.
add_library(server STATIC
    path_policy.cpp
    mime.cpp
)
target_include_directories(server PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_features(server PUBLIC cxx_std_20)
set_target_properties(server PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(server PUBLIC http concurrency net core Threads::Threads)
if(MSVC)
    target_compile_options(server PRIVATE /W4)
else()
    target_compile_options(server PRIVATE -Wall -Wextra -Wpedantic)
endif()
