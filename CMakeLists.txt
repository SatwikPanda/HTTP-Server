cmake_minimum_required(VERSION 3.20)

project(httpserver
    VERSION 1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Core library (Result, Error)
add_library(core STATIC
    src/core/error.cpp
)
target_include_directories(core PUBLIC
    include
)
target_compile_features(core PUBLIC cxx_std_20)
set_target_properties(core PROPERTIES CXX_EXTENSIONS OFF)

# Net library (Socket, Endpoint, Runtime, StreamChannel, EchoServer, TcpServer)
add_library(net STATIC
    src/net/endpoint.cpp
    src/net/stream.cpp
    src/net/echo_server.cpp
    src/net/tcp_server.cpp
    src/net/socket.cpp
    src/net/poller.cpp
    src/net/resolver.cpp
)
target_include_directories(net PUBLIC
    include
)
target_compile_features(net PUBLIC cxx_std_20)
set_target_properties(net PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(net PUBLIC core)

if(WIN32)
    target_sources(net PRIVATE
        src/net/windows/runtime.cpp
        src/net/windows/socket.cpp
    )
    target_compile_definitions(net PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(net PRIVATE ws2_32)
elseif(UNIX)
    target_sources(net PRIVATE
        src/net/posix/runtime.cpp
        src/net/posix/socket.cpp
    )
else()
    message(FATAL_ERROR "Unsupported platform")
endif()

find_package(Threads REQUIRED)
target_link_libraries(net PUBLIC Threads::Threads)

# Concurrency library (ConnectionSession, EchoSession, SingleSessionDriver)
add_library(concurrency STATIC
    src/concurrency/session.cpp
    src/concurrency/single_session_driver.cpp
)
target_include_directories(concurrency PUBLIC
    include
)
target_compile_features(concurrency PUBLIC cxx_std_20)
set_target_properties(concurrency PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(concurrency PUBLIC net core Threads::Threads)

foreach(target IN ITEMS core net concurrency)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()

# Main executable
add_executable(httpserver
    src/main.cpp
)
target_link_libraries(httpserver PRIVATE concurrency net Threads::Threads)

include(CTest)
option(REQUIRE_ALL_MILESTONES "Fail rather than skip unimplemented milestone acceptance suites" OFF)
if(BUILD_TESTING)
    # Adding a future production adapter must trigger reconfiguration and rebuild
    # its suite; __has_include alone does not track previously absent headers.
    file(GLOB future_adapters CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/milestone0[456]_adapter.hpp")
    function(add_milestone_test name milestone)
        add_executable(${name} tests/${name}.cpp)
        target_link_libraries(${name} PRIVATE concurrency net Threads::Threads)
        if(MSVC)
            target_compile_options(${name} PRIVATE /UNDEBUG)
        else()
            target_compile_options(${name} PRIVATE -UNDEBUG)
        endif()
        add_test(NAME ${name} COMMAND ${name})
        set_tests_properties(${name} PROPERTIES TIMEOUT 30 LABELS "milestone${milestone}")
        if(NOT REQUIRE_ALL_MILESTONES)
            set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77)
        endif()
    endfunction()
    add_milestone_test(socket_ownership_test 01)
    add_milestone_test(echo_test_double_test 02)
    add_milestone_test(echo_integration_test 02)
    foreach(milestone IN ITEMS 01 02 03 04 05 06)
        add_milestone_test(milestone${milestone}_test ${milestone})
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/milestone${milestone}_adapter.hpp")
            target_compile_definitions(milestone${milestone}_test PRIVATE MILESTONE${milestone}_AVAILABLE=1)
        endif()
    endforeach()
    foreach(milestone IN ITEMS 04 05 06)
        # Compile future assertions now, without linking or simulating a missing
        # production feature. These are compile checks, never passing CTests.
        add_library(milestone${milestone}_contract OBJECT tests/milestone${milestone}_test.cpp)
        target_link_libraries(milestone${milestone}_contract PRIVATE concurrency net)
        target_include_directories(milestone${milestone}_contract PRIVATE tests/contracts)
        target_compile_definitions(milestone${milestone}_contract PRIVATE MILESTONE${milestone}_AVAILABLE=1)
    endforeach()
    add_test(NAME cli_validation_test COMMAND ${CMAKE_COMMAND}
        "-DSERVER=$<TARGET_FILE:httpserver>" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_validation_test.cmake")
    set_tests_properties(cli_validation_test PROPERTIES TIMEOUT 10 LABELS "milestone03")
endif()
