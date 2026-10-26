# Event-Loop Driver (`concurrency/event_loop`)
add_library(event_loop STATIC
    event_loop_driver.cpp
)
target_include_directories(event_loop PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_features(event_loop PUBLIC cxx_std_20)
set_target_properties(event_loop PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(event_loop PUBLIC concurrency net core Threads::Threads)
if(MSVC)
    target_compile_options(event_loop PRIVATE /W4)
else()
    target_compile_options(event_loop PRIVATE -Wall -Wextra -Wpedantic)
endif()
