#include "http/http_server.hpp"
#include "net/endpoint.hpp"
#include "net/runtime.hpp"
#include "net/socket.hpp"

#include <cassert>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string read_all_to_string(net::Socket& socket) {
    std::string result;
    std::byte temp[512];
    auto start = std::chrono::steady_clock::now();

    while (true) {
        auto res = socket.read_some(std::span<std::byte>(temp, sizeof(temp)));
        if (res.is_ok()) {
            const auto* chars = reinterpret_cast<const char*>(temp);
            result.append(chars, chars + res.bytes_transferred);
            start = std::chrono::steady_clock::now();
        } else if (res.is_eof()) {
            break;
        } else if (res.would_block()) {
            if (std::chrono::steady_clock::now() - start > std::chrono::seconds(5)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } else if (res.is_error()) {
            break;
        }
    }
    return result;
}

void write_all_string(net::Socket& socket, std::string_view data) {
    std::size_t sent = 0;
    const auto* bytes = reinterpret_cast<const std::byte*>(data.data());
    while (sent < data.size()) {
        auto res = socket.write_some(
            std::span<const std::byte>(bytes + sent, data.size() - sent)
        );
        assert(res.is_ok());
        sent += res.bytes_transferred;
    }
}

} // namespace

void test_real_valid_http_response(http::HttpServer& server) {
    std::cout << "[RUN] test_real_valid_http_response..." << std::endl;

    std::thread server_thread([&]() {
        auto res = server.accept_and_handle_one();
        assert(res.is_ok());
    });

    auto client_res = net::create_tcp_socket(net::AddressFamily::ipv4);
    assert(client_res.is_ok());
    net::Socket client = std::move(client_res.value());
    assert(client.connect(server.local_endpoint()).is_ok());

    write_all_string(client, "GET / HTTP/1.1\r\nHost: localhost\r\nUser-Agent: test/1.0\r\n\r\n");

    std::string response = read_all_to_string(client);
    client.close();
    server_thread.join();

    assert(response.starts_with("HTTP/1.1 200 OK\r\n"));
    assert(response.find("\r\nDate: ") != std::string::npos);
    assert(response.find("\r\nContent-Type: text/plain; charset=utf-8\r\n") != std::string::npos);
    assert(response.find("\r\nContent-Length: 15\r\n") != std::string::npos);
    assert(response.find("\r\nConnection: close\r\n") != std::string::npos);
    assert(response.find("\r\nServer: httpserver/1.0\r\n") != std::string::npos);
    assert(response.ends_with("Hello, World!\r\n"));

    std::cout << "[PASS] test_real_valid_http_response" << std::endl;
}

void test_real_fragmented_request_sends(http::HttpServer& server) {
    std::cout << "[RUN] test_real_fragmented_request_sends..." << std::endl;

    std::thread server_thread([&]() {
        auto res = server.accept_and_handle_one();
        assert(res.is_ok());
    });

    auto client_res = net::create_tcp_socket(net::AddressFamily::ipv4);
    assert(client_res.is_ok());
    net::Socket client = std::move(client_res.value());
    assert(client.connect(server.local_endpoint()).is_ok());

    std::string req = "GET /test HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    for (char c : req) {
        std::byte b = static_cast<std::byte>(c);
        auto res = client.write_some(std::span<const std::byte>(&b, 1));
        assert(res.is_ok());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    std::string response = read_all_to_string(client);
    client.close();
    server_thread.join();

    assert(response.starts_with("HTTP/1.1 200 OK\r\n"));
    assert(response.ends_with("Hello, World!\r\n"));

    std::cout << "[PASS] test_real_fragmented_request_sends" << std::endl;
}

void test_real_empty_client_disconnect(http::HttpServer& server) {
    std::cout << "[RUN] test_real_empty_client_disconnect..." << std::endl;

    std::thread server_thread([&]() {
        auto res = server.accept_and_handle_one();
        assert(res.is_ok());
    });

    auto client_res = net::create_tcp_socket(net::AddressFamily::ipv4);
    assert(client_res.is_ok());
    net::Socket client = std::move(client_res.value());
    assert(client.connect(server.local_endpoint()).is_ok());

    // Disconnect without sending headers
    client.close();
    server_thread.join();

    std::cout << "[PASS] test_real_empty_client_disconnect" << std::endl;
}

int main() {
    auto runtime_res = net::initialize_network();
    if (!runtime_res) {
        std::cerr << "Failed to init network: " << runtime_res.error().to_string() << std::endl;
        return 1;
    }

    http::HttpServerConfig config;
    config.endpoint = net::Endpoint::ipv4_loopback(0); // Port 0: dynamic OS port
    config.session_config.response_body = "Hello, World!\r\n";

    http::HttpServer server(config);
    auto start_res = server.start();
    if (!start_res) {
        std::cerr << "Failed to start server: " << start_res.error().to_string() << std::endl;
        return 1;
    }

    std::cout << "Real HTTP Server listening on: " << server.local_endpoint().to_string() << std::endl;

    test_real_valid_http_response(server);
    test_real_fragmented_request_sends(server);
    test_real_empty_client_disconnect(server);

    server.stop();
    std::cout << "All HTTP Integration Tests PASSED!" << std::endl;
    return 0;
}
