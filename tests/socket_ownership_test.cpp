#include "core/result.hpp"
#include "net/endpoint.hpp"
#include "net/runtime.hpp"
#include "net/socket.hpp"

#include <cassert>
#include <iostream>
#include <thread>
#include <vector>

void test_network_runtime_initialization() {
    std::cout << "[RUN] test_network_runtime_initialization..." << std::endl;
    auto runtime_res = net::initialize_network();
    assert(runtime_res.is_ok());

    net::NetworkRuntime rt1 = std::move(runtime_res.value());
    assert(rt1.is_initialized());

    net::NetworkRuntime rt2 = std::move(rt1);
    assert(rt2.is_initialized());
    assert(!rt1.is_initialized());

    std::cout << "[PASS] test_network_runtime_initialization" << std::endl;
}

void test_socket_move_semantics() {
    std::cout << "[RUN] test_socket_move_semantics..." << std::endl;
    auto sock_res = net::create_tcp_socket(net::AddressFamily::ipv4);
    assert(sock_res.is_ok());

    net::Socket s1 = std::move(sock_res.value());
    assert(s1.is_valid());

    net::Socket s2 = std::move(s1);
    assert(!s1.is_valid());
    assert(s2.is_valid());

    s2.close();
    assert(!s2.is_valid());

    // Closing already-closed socket must be safe and idempotent
    s2.close();
    assert(!s2.is_valid());

    std::cout << "[PASS] test_socket_move_semantics" << std::endl;
}

void test_double_close_never_closes_another_socket() {
    std::cout << "[RUN] test_double_close_never_closes_another_socket..." << std::endl;
    // Milestone 01 completion check: Moving or closing a socket twice never closes another socket.
    auto sock1_res = net::create_tcp_socket(net::AddressFamily::ipv4);
    assert(sock1_res.is_ok());
    net::Socket s1 = std::move(sock1_res.value());
    assert(s1.is_valid());

    s1.close();
    assert(!s1.is_valid());

    // Create a second socket; OS may reuse the same handle value
    auto sock2_res = net::create_tcp_socket(net::AddressFamily::ipv4);
    assert(sock2_res.is_ok());
    net::Socket s2 = std::move(sock2_res.value());
    assert(s2.is_valid());

    // Close s1 again: must NOT close s2!
    s1.close();
    assert(s2.is_valid());

    // Verify s2 is still completely usable
    auto set_res = s2.set_reuse_address(true);
    assert(set_res.is_ok());

    s2.close();
    assert(!s2.is_valid());

    std::cout << "[PASS] test_double_close_never_closes_another_socket" << std::endl;
}

void test_bind_occupied_port_fails() {
    std::cout << "[RUN] test_bind_occupied_port_fails..." << std::endl;
    auto l1_res = net::create_tcp_socket(net::AddressFamily::ipv4);
    assert(l1_res.is_ok());
    net::Socket l1 = std::move(l1_res.value());

    // Bind to port 0 (ephemeral port)
    auto bind1_res = l1.bind(net::Endpoint::ipv4_loopback(0));
    assert(bind1_res.is_ok());

    auto local_res = l1.local_endpoint();
    assert(local_res.is_ok());
    net::Endpoint bound_endpoint = local_res.value();
    assert(bound_endpoint.port() > 0);

    auto listen1_res = l1.listen(128);
    assert(listen1_res.is_ok());

    // Now attempt to bind socket 2 to the exact same port without SO_REUSEADDR or on same port
    auto l2_res = net::create_tcp_socket(net::AddressFamily::ipv4);
    assert(l2_res.is_ok());
    net::Socket l2 = std::move(l2_res.value());

    auto bind2_res = l2.bind(bound_endpoint);
    // Milestone 01 completion check: Binding an occupied port reports an error
    assert(!bind2_res.is_ok());
    assert(bind2_res.error().category == core::ErrorCategory::address_in_use ||
           bind2_res.error().category == core::ErrorCategory::invalid_argument);

    l1.close();
    l2.close();
    std::cout << "[PASS] test_bind_occupied_port_fails" << std::endl;
}

void test_accepted_socket_owns_handle_independently() {
    std::cout << "[RUN] test_accepted_socket_owns_handle_independently..." << std::endl;
    // Milestone 01 completion check: Accepted sockets must own their handles independently of the listener
    auto l_res = net::create_tcp_socket(net::AddressFamily::ipv4);
    assert(l_res.is_ok());
    net::Socket listener = std::move(l_res.value());

    assert(listener.set_reuse_address(true).is_ok());
    assert(listener.bind(net::Endpoint::ipv4_loopback(0)).is_ok());
    auto ep = listener.local_endpoint().value();
    assert(listener.listen(128).is_ok());

    net::Socket client_side;
    std::thread client_thread([&]() {
        auto c_res = net::create_tcp_socket(net::AddressFamily::ipv4);
        assert(c_res.is_ok());
        client_side = std::move(c_res.value());
        assert(client_side.connect(ep).is_ok());
    });

    auto accept_res = listener.accept();
    assert(accept_res.is_ok());
    net::Socket server_client = std::move(accept_res.value());
    client_thread.join();

    assert(server_client.is_valid());
    assert(client_side.is_valid());

    // Close listener now! Server accepted socket must remain completely unaffected
    listener.close();
    assert(!listener.is_valid());
    assert(server_client.is_valid());

    // Test transferring data between client_side and server_client
    const char msg[] = "ping";
    auto write_res = client_side.write_some(std::as_bytes(std::span(msg, 4)));
    assert(write_res.is_ok() && write_res.bytes_transferred == 4);

    char recv_buf[16]{};
    auto read_res = server_client.read_some(std::as_writable_bytes(std::span(recv_buf, 4)));
    assert(read_res.is_ok() && read_res.bytes_transferred == 4);
    assert(std::string_view(recv_buf, 4) == "ping");

    client_side.close();
    server_client.close();
    std::cout << "[PASS] test_accepted_socket_owns_handle_independently" << std::endl;
}

int main() {
    auto rt = net::initialize_network();
    assert(rt.is_ok());

    test_network_runtime_initialization();
    test_socket_move_semantics();
    test_double_close_never_closes_another_socket();
    test_bind_occupied_port_fails();
    test_accepted_socket_owns_handle_independently();

    std::cout << "\nALL SOCKET OWNERSHIP TESTS PASSED!" << std::endl;
    return 0;
}
