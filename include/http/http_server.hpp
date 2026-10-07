#pragma once

#include "cache/cache.hpp"
#include "net/socket.hpp"

namespace http {

class HttpServer {

public:

    explicit HttpServer(
        cache::Cache& cache
    );

    void handle(
        net::Socket socket
    );

private:

    cache::Cache& cache_;

    void process_request(
        net::Socket& socket,
        const Request& request
    );
};

}