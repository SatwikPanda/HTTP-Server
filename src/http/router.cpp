#include "http/router.hpp"

#include "http/target.hpp"

#include <utility>

namespace http {
namespace {
using Routes = std::unordered_map<std::string, std::shared_ptr<const RouteHandler>>;

class RouteTransaction final : public Transaction {
public:
    RouteTransaction(const Request& request, std::shared_ptr<const RouteHandler> handler, StatusCode fallback)
        : request_(request), handler_(std::move(handler)), fallback_(fallback) {}

    TransactionStep on_event(const TransactionEvent& event) override {
        if (started_ || !std::holds_alternative<StartTransaction>(event)) {
            return TransactionFailure{core::make_error(core::ErrorCategory::invalid_argument, "route transaction")};
        }
        started_ = true;
        if (!handler_) {
            auto response = Response::text(std::to_string(static_cast<int>(fallback_)) + " " +
                std::string(default_reason_phrase(fallback_)) + "\n", fallback_);
            if (fallback_ == StatusCode::MethodNotAllowed) response.headers.set("Allow", "GET, HEAD");
            return BufferedResponse{std::move(response)};
        }
        // Application exceptions must never escape a driver thread. The session
        // generates a bounded generic 500 before any response is committed.
        try {
            auto result = (*handler_)(request_);
            if (!result) return TransactionFailure{result.error()};
            if (result.value().status_code < 200) {
                return TransactionFailure{core::make_error(core::ErrorCategory::invalid_argument, "route final response")};
            }
            return BufferedResponse{std::move(result.value())};
        } catch (...) {
            return TransactionFailure{core::make_error(core::ErrorCategory::io_error, "route handler")};
        }
    }
private:
    const Request& request_;
    std::shared_ptr<const RouteHandler> handler_;
    StatusCode fallback_;
    bool started_{};
};

class RouterFactory final : public TransactionFactory {
public:
    explicit RouterFactory(Routes routes) : routes_(std::move(routes)) {}

    core::Result<std::unique_ptr<Transaction>> create(const Request& request, const TransactionContext&) const override {
        std::shared_ptr<const RouteHandler> handler;
        auto fallback = StatusCode::NotFound;
        // HTTP/1.0 may omit Host. Absolute-form targets supply their own authority.
        auto target = parse_target(request.target, request.header("Host").value_or("localhost"));
        if (!target) fallback = StatusCode::BadRequest;
        else if (request.method != Method::GET && request.method != Method::HEAD) fallback = StatusCode::MethodNotAllowed;
        else {
            auto& path = target.value().path_query;
            const auto query = path.find('?');
            if (query != std::string::npos) path.resize(query);
            const auto found = routes_.find(path);
            if (found != routes_.end()) handler = found->second;
        }
        return std::unique_ptr<Transaction>(std::make_unique<RouteTransaction>(request, std::move(handler), fallback));
    }
private:
    const Routes routes_;
};
} // namespace

core::Result<void> Router::get(std::string path, RouteHandler handler) {
    if (!handler || path.empty() || path.front() != '/' || path.find('?') != std::string::npos ||
        !parse_target(path, "localhost")) {
        return core::make_error(core::ErrorCategory::invalid_argument, "register GET route", 0,
            "Expected a valid absolute path without a query and a nonempty handler");
    }
    if (routes_.contains(path)) {
        return core::make_error(core::ErrorCategory::invalid_argument, "register GET route", 0, "Duplicate path");
    }
    if (routes_.size() >= max_routes) {
        return core::make_error(core::ErrorCategory::would_block, "register GET route", 0, "Route limit reached");
    }
    routes_.emplace(std::move(path), std::make_shared<const RouteHandler>(std::move(handler)));
    return {};
}

std::shared_ptr<const TransactionFactory> Router::factory() const {
    return std::make_shared<RouterFactory>(routes_);
}
} // namespace http
