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

# HTTP library (Method, Status, Version, Header, Date, Serializer, Session, Server)
add_library(http STATIC
    src/http/method.cpp
    src/http/status.cpp
    src/http/version.cpp
    src/http/header.cpp
    src/http/date.cpp
    src/http/serializer.cpp
    src/http/http_session.cpp
    src/http/http_server.cpp
)
target_include_directories(http PUBLIC
    include
)
target_compile_features(http PUBLIC cxx_std_20)
set_target_properties(http PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(http PUBLIC core net)

find_package(Threads REQUIRED)

# Main executable
add_executable(httpserver
    src/main.cpp
)
target_link_libraries(httpserver PRIVATE http net Threads::Threads)

# Testing
enable_testing()

add_executable(socket_ownership_test
    tests/socket_ownership_test.cpp
)
target_link_libraries(socket_ownership_test PRIVATE net Threads::Threads)
add_test(NAME socket_ownership_test COMMAND socket_ownership_test)

add_executable(echo_test_double_test
    tests/echo_test_double_test.cpp
)
target_link_libraries(echo_test_double_test PRIVATE net Threads::Threads)
add_test(NAME echo_test_double_test COMMAND echo_test_double_test)

add_executable(echo_integration_test
    tests/echo_integration_test.cpp
)
target_link_libraries(echo_integration_test PRIVATE net Threads::Threads)
add_test(NAME echo_integration_test COMMAND echo_integration_test)

add_executable(http_serialization_test
    tests/http_serialization_test.cpp
)
target_link_libraries(http_serialization_test PRIVATE http core Threads::Threads)
add_test(NAME http_serialization_test COMMAND http_serialization_test)

add_executable(http_session_test
    tests/http_session_test.cpp
)
target_link_libraries(http_session_test PRIVATE http net core Threads::Threads)
add_test(NAME http_session_test COMMAND http_session_test)

add_executable(http_integration_test
    tests/http_integration_test.cpp
)
target_link_libraries(http_integration_test PRIVATE http net core Threads::Threads)
add_test(NAME http_integration_test COMMAND http_integration_test)