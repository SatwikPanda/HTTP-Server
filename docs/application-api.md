# Build your application in `app/`

The `http_app` executable is your application entry point. It uses the project's
existing HTTP parser, response writer, sockets and concurrency drivers.

- `app/main.cpp` creates the server, selects a port and handles Ctrl+C shutdown.
- `app/routes.cpp` registers routes and contains their handlers.
- `app/routes.hpp` declares the registration function.
- `app/CMakeLists.txt` lists your application source files.

The original `httpserver` executable still provides the demo, static-file and
proxy command-line modes. Both executables link the same production libraries.

## Run Hello World

From the repository root, using a configured Ninja build:

```powershell
cmake -S . -B build/app -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/app --target http_app
./build/app/app/http_app.exe 8080
```

In another terminal:

```powershell
curl.exe http://127.0.0.1:8080/
```

The body is `Hello, World!`. Omit the port to use 8080, pass another port such as
9000, or pass 0 to let the OS choose an available port. Startup prints the bound
address. On Linux/macOS omit `.exe`. With a Visual Studio generator, build with
`--config Release` and run `build/app/app/Release/http_app.exe`.

## Register routes

The included `app/routes.cpp` is deliberately small:

```cpp
#include "routes.hpp"

namespace app {
core::Result<void> register_routes(application::Server& server) {
    return server.get("/", [](const http::Request&) {
        return http::Response::text("Hello, World!");
    });
}
}
```

Handlers receive the parsed request and return `http::Response` or
`core::Result<http::Response>`. The request exposes `method`, `target`, `headers`
and `header(name)`. It is borrowed for the call; copy any data you need later.

For several routes, check each registration before registering the next:

```cpp
core::Result<void> register_routes(application::Server& server) {
    auto home = server.get("/", [](const http::Request&) {
        return http::Response::text("Hello, World!");
    });
    if (!home) return home.error();

    return server.get("/api/status", [](const http::Request&) {
        http::Response response;
        response.set_body(R"({"status":"ok"})", "application/json");
        response.headers.set("X-App", "my-app");
        return response;
    });
}
```

Use `Response::text(body, http::StatusCode::NotFound)` or `response.set_status(...)`
to select a status. `set_body` supplies the content type and Content-Length. The
existing serializer validates generated headers and framing. Handler errors and
exceptions produce a generic 500 and close the connection, without exposing
exception text to clients. Informational/upgrade responses are rejected.

Routes match the exact, case-sensitive escaped path. `/api/status?verbose=1`
matches `/api/status`, and the original query remains in `request.target`.
`/api/status/`, `/API/status` and `/api%2Fstatus` are distinct paths. Registration
accepts paths beginning with `/`, excluding query strings and fragments, and
rejects duplicates, invalid paths and empty handlers. The table is capped at
1,024 routes and sealed after successful `listen`.

Unmatched paths return 404. GET routes automatically handle HEAD: the handler
receives `Method::HEAD`, and the session sends representation headers without a
body. Keep GET handlers free of side effects. Other methods retain the existing
405/501 policy, including `Allow: GET, HEAD` for 405. Request bodies, upgrades,
TLS, route parameters, middleware and additional request methods are outside
this API's current scope.

## Own the server lifecycle

```cpp
application::ServerOptions options;
options.workers = 8;
application::Server server(options);

auto registered = server.get("/", [](const http::Request&) {
    return http::Response::text("Hello, World!");
});
if (!registered) {
    std::cerr << registered.error().to_string() << '\n';
    return 1;
}

auto endpoint = server.listen(9000); // background startup, loopback by default
if (!endpoint) {
    std::cerr << endpoint.error().to_string() << '\n';
    return 1;
}
server.wait(); // another control thread must request_stop(), or use app/main.cpp's hooks
```

`listen(port, numeric_bind_address)` returns `core::Result<net::Endpoint>` with
the actual port. Ports must be in 0–65535; a port already in use returns an error.
For an explicit network-facing bind, use `listen(9000, "0.0.0.0")`. IPv6 addresses
such as `::1` use the same API when available on the host.

Failed startup is retryable. A successful instance remains sealed after shutdown;
create a new `Server` to restart. The server owns network initialization and joins
drivers/helpers before releasing it. Destruction requests shutdown automatically.
`request_stop()` is idempotent and may run concurrently after startup from a
control thread. Do not stop, wait or destroy the server inside a handler.
Call `wait()` and destroy the server on the control thread.
Route registration, startup and waiting otherwise have a single owner.

The library does not install process-wide signal hooks. `app/main.cpp` uses the
existing `StopController` and `ScopedStopHooks` so Ctrl+C requests a graceful stop.

## Concurrency and limits

The default driver is the existing four-worker thread pool. Set `options.driver`
to `core::DriverMode::event_loop` or `core::DriverMode::single` to reuse those
drivers. `ServerOptions` exposes worker/session/request limits, input and output
buffers, idle/progress timeouts and the shutdown drain timeout. `max_sessions`
bounds active event-loop sessions; the pool runs at most `workers` active sessions
and queues at most `accept_queue` accepted sockets. The single driver serves one
client at a time. Pool workers must not exceed `max_sessions`.

Handlers run synchronously on session threads and may run concurrently on the
thread pool. Synchronize shared mutable captures. Keep handler work short;
blocking work holds a pool worker and must never run on the event-loop thread.
Arbitrary application code cannot be preempted by socket deadlines, so handlers
must bound their own work, allocations and any external calls. References captured
by handlers must outlive the server; value/shared-pointer captures are owned.

This initial API returns buffered responses. The serialized response, including
headers, must fit `options.buffers.output` (64 KiB by default). Larger or invalid
responses become 500 before commitment. Large streaming applications can use the
existing `TransactionFactory`/`TransactionWork` interface directly. This API adds
no HTTP, routing or networking library dependency.

## Validation

`router_test` checks registration limits, snapshot ownership, escaped paths,
queries, headers, custom statuses, HEAD, 404/405, exceptions, output bounds,
binary data, partial writes and ordered pipelines. `application_server_test`
executes `app/routes.cpp` over real loopback sockets through the sequential,
thread-pool and event-loop drivers, including concurrent clients, startup
rollback, port conflicts, route sealing and automatic resource cleanup.

```powershell
cmake --build build/app --config Release --target router_test application_server_test
ctest --test-dir build/app -C Release -L routing --no-tests=error --output-on-failure
```

On 2026-10-27, the full strict Release inventory passed 56/56 locally on Windows
GCC 16.2 and MSVC 19.29. The runnable GCC app also passed manual GET/HEAD/404/405
checks on port 0 and a fixed available port, plus help and invalid-port checks.
Linux/macOS verification of this extension is pending. See [test notes](../tests/README.md)
for the retained local qualification records.
