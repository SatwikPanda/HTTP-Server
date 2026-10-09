#include "net/echo_server.hpp"
#include "net/endpoint.hpp"
#include "net/runtime.hpp"
#include "net/socket.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

namespace {

void read_all(net::Socket& socket, std::vector<std::byte>& out, std::size_t expected_size) {
    std::byte temp[512];
    auto start = std::chrono::steady_clock::now();

    while (out.size() < expected_size) {
        auto res = socket.read_some(std::span<std::byte>(temp, sizeof(temp)));
        if (res.is_ok()) {
            out.insert(out.end(), temp, temp + res.bytes_transferred);
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
}

} // namespace

void test_real_empty_disconnect(net::EchoServer& server) {
    std::cout << "[RUN] test_real_empty_disconnect..." << std::endl;

    std::thread server_thread([&]() {
        auto res = server.accept_and_handle_one();
        assert(res.is_ok());
    });

    auto client_res = net::create_tcp_socket(net::AddressFamily::ipv4);
    assert(client_res.is_ok());
    net::Socket client = std::move(client_res.value());

    assert(client.connect(server.local_endpoint()).is_ok());
    // Immediately close client without sending any data
    client.close();

    server_thread.join();
    std::cout << "[PASS] test_real_empty_disconnect" << std::endl;
}

void test_real_binary_bytes(net::EchoServer& server) {
    std::cout << "[RUN] test_real_binary_bytes..." << std::endl;

    std::vector<std::byte> payload;
    for (int cycle = 0; cycle < 16; ++cycle) {
        for (int b = 0; b < 256; ++b) {
            payload.push_back(static_cast<std::byte>(b));
        }
    }
    assert(payload.size() == 4096);

    std::thread server_thread([&]() {
        auto res = server.accept_and_handle_one();
        assert(res.is_ok());
    });

    auto client_res = net::create_tcp_socket(net::AddressFamily::ipv4);
    assert(client_res.is_ok());
    net::Socket client = std::move(client_res.value());
    assert(client.connect(server.local_endpoint()).is_ok());

    // Send payload
    std::size_t sent = 0;
    while (sent < payload.size()) {
        auto w_res = client.write_some(
            std::span<const std::byte>(payload.data() + sent, payload.size() - sent)
        );
        assert(w_res.is_ok());
        sent += w_res.bytes_transferred;
    }

    // Shutdown write on client to signal EOF
    assert(client.shutdown_write().is_ok());

    // Read response
    std::vector<std::byte> received;
    read_all(client, received, payload.size());
    assert(received.size() == payload.size());
    assert(received == payload);

    client.close();
    server_thread.join();
    std::cout << "[PASS] test_real_binary_bytes (4096 bytes verified)" << std::endl;
}

void test_real_fragmented_sends(net::EchoServer& server) {
    std::cout << "[RUN] test_real_fragmented_sends..." << std::endl;

    std::vector<std::byte> payload;
    for (int i = 0; i < 300; ++i) {
        payload.push_back(static_cast<std::byte>((i * 7 + 13) & 0xFF));
    }

    std::thread server_thread([&]() {
        auto res = server.accept_and_handle_one();
        assert(res.is_ok());
    });

    auto client_res = net::create_tcp_socket(net::AddressFamily::ipv4);
    assert(client_res.is_ok());
    net::Socket client = std::move(client_res.value());
    assert(client.connect(server.local_endpoint()).is_ok());

    // Send 1 byte at a time
    for (std::size_t i = 0; i < payload.size(); ++i) {
        auto w_res = client.write_some(
            std::span<const std::byte>(&payload[i], 1)
        );
        assert(w_res.is_ok() && w_res.bytes_transferred == 1);
    }

    assert(client.shutdown_write().is_ok());

    std::vector<std::byte> received;
    read_all(client, received, payload.size());
    assert(received == payload);

    client.close();
    server_thread.join();
    std::cout << "[PASS] test_real_fragmented_sends (300 1-byte sends verified)" << std::endl;
}

void test_real_payload_larger_than_buffer(net::EchoServer& server) {
    std::cout << "[RUN] test_real_payload_larger_than_buffer..." << std::endl;

    // Server buffer capacity is small (e.g. 512 bytes).
    // Send 32,768 bytes!
    std::vector<std::byte> payload(32768);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::byte>((i * 31 + 17) & 0xFF);
    }

    std::thread server_thread([&]() {
        auto res = server.accept_and_handle_one();
        assert(res.is_ok());
    });

    auto client_res = net::create_tcp_socket(net::AddressFamily::ipv4);
    assert(client_res.is_ok());
    net::Socket client = std::move(client_res.value());
    assert(client.connect(server.local_endpoint()).is_ok());

    // Client writer thread
    std::thread writer([&]() {
        std::size_t sent = 0;
        while (sent < payload.size()) {
            std::size_t chunk = std::min<std::size_t>(payload.size() - sent, 1024);
            auto w_res = client.write_some(
                std::span<const std::byte>(payload.data() + sent, chunk)
            );
            if (w_res.is_ok()) {
                sent += w_res.bytes_transferred;
            } else if (w_res.would_block()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } else {
                break;
            }
        }
        (void)client.shutdown_write();
    });

    // Client reads response while sending to prevent TCP buffer deadlock
    std::vector<std::byte> received;
    read_all(client, received, payload.size());

    writer.join();
    server_thread.join();

    assert(received.size() == payload.size());
    assert(received == payload);
    client.close();

    std::cout << "[PASS] test_real_payload_larger_than_buffer (32,768 bytes verified)" << std::endl;
}

void test_real_slow_receiver(net::EchoServer& server) {
    std::cout << "[RUN] test_real_slow_receiver..." << std::endl;

    std::vector<std::byte> payload(4096);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::byte>(i & 0xFF);
    }

    std::thread server_thread([&]() {
        auto res = server.accept_and_handle_one();
        assert(res.is_ok());
    });

    auto client_res = net::create_tcp_socket(net::AddressFamily::ipv4);
    assert(client_res.is_ok());
    net::Socket client = std::move(client_res.value());
    assert(client.connect(server.local_endpoint()).is_ok());

    // Write all 4096 bytes and shut down write
    std::size_t sent = 0;
    while (sent < payload.size()) {
        auto w_res = client.write_some(
            std::span<const std::byte>(payload.data() + sent, payload.size() - sent)
        );
        if (w_res.is_ok()) {
            sent += w_res.bytes_transferred;
        } else if (w_res.would_block()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } else {
            break;
        }
    }
    assert(client.shutdown_write().is_ok());

    // Slow read: read in tiny 32-byte chunks with sleep
    std::vector<std::byte> received;
    std::byte temp[32];
    auto start = std::chrono::steady_clock::now();

    while (received.size() < payload.size()) {
        auto r_res = client.read_some(std::span<std::byte>(temp, sizeof(temp)));
        if (r_res.is_ok()) {
            received.insert(received.end(), temp, temp + r_res.bytes_transferred);
            start = std::chrono::steady_clock::now();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } else if (r_res.is_eof()) {
            break;
        } else if (r_res.would_block()) {
            if (std::chrono::steady_clock::now() - start > std::chrono::seconds(5)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        } else if (r_res.is_error()) {
            break;
        }
    }

    server_thread.join();
    assert(received.size() == payload.size());
    assert(received == payload);
    client.close();

    std::cout << "[PASS] test_real_slow_receiver (slow client read 4096 bytes safely)" << std::endl;
}

int main() {
    auto rt = net::initialize_network();
    assert(rt.is_ok());

    net::EchoConfig config;
    config.endpoint = net::Endpoint::ipv4_loopback(0); // bind to ephemeral port
    config.buffer_capacity = 512; // small bounded buffer to stress partial chunks

    net::EchoServer server(config);
    auto start_res = server.start();
    assert(start_res.is_ok());
    std::cout << "Test EchoServer listening on: " << server.local_endpoint().to_string() << "\n" << std::endl;

    test_real_empty_disconnect(server);
    test_real_binary_bytes(server);
    test_real_fragmented_sends(server);
    test_real_payload_larger_than_buffer(server);
    test_real_slow_receiver(server);

    server.stop();
    std::cout << "\nALL REAL SOCKET ECHO INTEGRATION TESTS PASSED!" << std::endl;
    return 0;
}
