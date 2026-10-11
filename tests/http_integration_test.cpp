#include "http/http_server.hpp"

#include "http/serializer.hpp"
#include "net/poller.hpp"
#include "net/resolver.hpp"
#include "test_support.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace {
using namespace std::chrono_literals;

// Do not include <future> here: older MSVC's PPL defines a global concurrency
// alias that conflicts with this project's module namespace.
class ServerTask {
public:
    template<class Function>
    explicit ServerTask(Function function) : thread_([this, function = std::move(function)] {
        auto result = function();
        {
            std::lock_guard lock(mutex_);
            result_ = std::move(result);
            completed_ = true;
        }
        completed_cv_.notify_all();
    }) {}

    bool wait_for(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return completed_cv_.wait_for(lock, timeout, [&] { return completed_; });
    }
    core::Result<void> get() {
        std::lock_guard lock(mutex_);
        CHECK(completed_);
        return result_;
    }
private:
    std::mutex mutex_;
    std::condition_variable completed_cv_;
    core::Result<void> result_;
    bool completed_{};
    std::jthread thread_; // Joins before the completion state is destroyed.
};

net::Socket connect_to(const http::HttpServer& server) {
    const std::array candidates{server.local_endpoint()};
    return test::take(net::connect_candidates(candidates, test::Clock::now() + 2s));
}

// All client I/O is nonblocking and bounded, so test timeouts catch server bugs.
class ClientIo {
public:
    explicit ClientIo(net::Socket& socket)
        : socket_(socket), poller_(test::take(net::Poller::create())),
          token_(test::take(poller_.watch(socket_, net::Interest::read, {2, 1}))) {}
    ~ClientIo() { CHECK(poller_.unwatch(token_)); }

    void write(std::string_view data) {
        const auto deadline = test::Clock::now() + 2s;
        std::size_t sent{};
        const auto bytes = std::as_bytes(std::span(data));
        while (sent < bytes.size()) {
            CHECK(test::Clock::now() < deadline);
            const auto result = socket_.write_some(bytes.subspan(sent));
            if (result.is_ok()) {
                CHECK(result.bytes_transferred > 0 && result.bytes_transferred <= bytes.size() - sent);
                sent += result.bytes_transferred;
            } else {
                CHECK(result.would_block());
                wait(net::Interest::write, deadline);
            }
        }
    }

    std::string read_to_eof() {
        std::string output;
        std::array<std::byte, 512> buffer{};
        const auto deadline = test::Clock::now() + 2s;
        for (;;) {
            CHECK(test::Clock::now() < deadline);
            const auto result = socket_.read_some(buffer);
            if (result.is_eof()) return output;
            if (result.is_ok()) {
                CHECK(result.bytes_transferred > 0 && result.bytes_transferred <= buffer.size());
                output.append(reinterpret_cast<const char*>(buffer.data()), result.bytes_transferred);
                CHECK(output.size() <= 2 * 1024 * 1024);
            } else {
                CHECK(result.would_block());
                wait(net::Interest::read, deadline);
            }
        }
    }

private:
    void wait(net::Interest interest, net::Deadline deadline) {
        CHECK(poller_.modify(token_, interest));
        for (;;) {
            CHECK(test::Clock::now() < deadline);
            const auto events = test::take(poller_.wait(deadline));
            for (const auto& event : events) {
                if (event.token == token_ && poller_.is_current(event)) return;
            }
        }
    }
    net::Socket& socket_;
    net::Poller poller_;
    net::RegistrationToken token_;
};

std::string roundtrip(http::HttpServer& server, std::string_view request, bool fragmented = false) {
    ServerTask serving([&] { return server.accept_and_handle_one(); });
    auto client = connect_to(server);
    std::string response;
    {
        ClientIo io(client);
        if (fragmented) {
            for (std::size_t i = 0; i < request.size(); ++i) io.write(request.substr(i, 1));
        } else {
            io.write(request);
        }
        CHECK(client.shutdown_write()); // Readable bytes must survive peer hangup.
        response = io.read_to_eof();
    }
    client.close();
    CHECK(serving.wait_for(1s));
    CHECK(serving.get());
    return response;
}

void test_server_transactions(net::AddressFamily family) {
    http::HttpServerConfig config;
    config.endpoint = family == net::AddressFamily::ipv4 ?
        net::Endpoint::ipv4_loopback(0) : net::Endpoint::ipv6_loopback(0);
    config.session_config.fixed_wall_time = std::chrono::system_clock::from_time_t(784111777);
    config.session_config.max_header_bytes = 128;
    http::HttpServer server(config);
    CHECK(server.start());
    CHECK(!server.start()); // Starting twice must preserve the original listener.
    http::HttpServerConfig occupied_config = config;
    occupied_config.endpoint = server.local_endpoint();
    http::HttpServer occupied(occupied_config);
    const auto occupied_result = occupied.start();
    CHECK(!occupied_result && occupied_result.error().category == core::ErrorCategory::address_in_use);

    const auto expected = test::take(http::serialize_response(test::take(http::make_fixed_response(config.session_config))));
    const std::string request = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    CHECK(roundtrip(server, request) == expected);
    CHECK(roundtrip(server, request, true) == expected);
    CHECK(roundtrip(server, std::string(128, 'X')).starts_with("HTTP/1.1 431 "));
    CHECK(roundtrip(server, "GET / HTTP/1.1\r\n").empty());
    CHECK(roundtrip(server, "").empty());
    CHECK(roundtrip(server, request) == expected); // Client errors do not kill the listener.

    server.stop();
    CHECK(!server.is_running());
    CHECK(!server.accept_and_handle_one());
    CHECK(server.run()); // stop() before run() never auto-restarts.
    CHECK(!server.is_running());
    CHECK(server.start());
    CHECK(roundtrip(server, request) == expected);
    server.stop();

    config.session_config.response_body = std::string("a\0\xff\r\nb", 6);
    http::HttpServer binary(config);
    CHECK(binary.start());
    CHECK(roundtrip(binary, request) ==
          test::take(http::serialize_response(test::take(http::make_fixed_response(config.session_config)))));
    binary.stop();
}

void test_shutdown_and_idle_clients() {
    http::HttpServerConfig config;
    config.endpoint = net::Endpoint::ipv4_loopback(0);
    config.session_config.retry_timeout = 50ms;
    http::HttpServer server(config);
    CHECK(server.start());
    ServerTask serving([&] { return server.accept_and_handle_one(); });
    auto idle = connect_to(server);
    { ClientIo io(idle); CHECK(io.read_to_eof().empty()); }
    CHECK(serving.wait_for(1s));
    CHECK(serving.get());
    idle.close();
    CHECK(roundtrip(server, "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n").ends_with("Hello, World!\r\n"));

    for (bool with_client : {false, true}) {
        config.session_config.retry_timeout = 5s;
        http::HttpServer stopping(config);
        CHECK(stopping.start());
        ServerTask running([&] { return stopping.run(); });
        auto client = with_client ? connect_to(stopping) : net::Socket{};
        if (with_client) {
            ClientIo io(client);
            io.write("GET / HTTP/1.1\r\n"); // Incomplete, so the session waits for reads.
        }
        CHECK(!running.wait_for(30ms));
        stopping.stop();
        CHECK(running.wait_for(1s));
        CHECK(running.get());
        client.close();
        CHECK(!stopping.is_running());
        CHECK(stopping.run());
        CHECK(stopping.start());
        CHECK(roundtrip(stopping, "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n").ends_with("Hello, World!\r\n"));
        stopping.stop();
    }

    config.session_config.max_output_bytes = 64;
    http::HttpServer invalid(config);
    const auto rejected = invalid.start();
    CHECK(!rejected && rejected.error().category == core::ErrorCategory::invalid_argument);
    CHECK(!invalid.is_running());
}

} // namespace

int main() {
    auto runtime = test::take(net::initialize_network());
    test_server_transactions(net::AddressFamily::ipv4);
    auto ipv6 = net::create_tcp_socket(net::AddressFamily::ipv6);
    if (!ipv6) {
        std::cout << "IPv6 unavailable: " << ipv6.error().to_string() << '\n';
    } else {
        auto bound = ipv6.value().bind(net::Endpoint::ipv6_loopback(0));
        if (!bound) {
            std::cout << "IPv6 unavailable: " << bound.error().to_string() << '\n';
        } else {
            ipv6.value().close();
            test_server_transactions(net::AddressFamily::ipv6);
        }
    }
    test_shutdown_and_idle_clients();
}
