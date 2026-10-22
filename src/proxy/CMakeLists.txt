# B — uncached forwarding proxy. Depends on http (and transitively on
# concurrency/net/core). src/proxy/cached is reserved for K.
add_library(proxy STATIC
    forwarding.cpp
)
target_include_directories(proxy PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_features(proxy PUBLIC cxx_std_20)
set_target_properties(proxy PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(proxy PUBLIC http concurrency net core Threads::Threads)
if(MSVC)
    target_compile_options(proxy PRIVATE /W4)
else()
    target_compile_options(proxy PRIVATE -Wall -Wextra -Wpedantic)
endif()
