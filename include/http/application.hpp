#pragma once

#include "concurrency/work.hpp"
#include "http/framing.hpp"
#include "http/request.hpp"
#include "http/response.hpp"

#include <optional>
#include <memory>
#include <variant>

namespace http {
struct TransactionContext {
    core::TransactionId id;
    std::size_t response_window{};
    const core::SteadyClock& steady_clock;
    const core::SystemClock& wall_clock;
};
struct StartTransaction {};
struct OutputDrained { std::size_t body_capacity{}; };
using TransactionEvent = std::variant<StartTransaction, OutputDrained, concurrency::WorkEvent>;
struct ResponseHead { Response response; BodyFraming framing{BodyFraming::fixed_length}; std::optional<std::uint64_t> length; };
struct ResponseBody { concurrency::SharedBytes bytes; };
struct ResponseFinished { HeaderMap trailers; };
struct BufferedResponse { Response response; };
struct TransactionWork { concurrency::WorkRequest request; core::SteadyTimePoint deadline; };
struct TransactionFailure { core::Error error; StatusCode status{StatusCode::InternalServerError}; };
using TransactionStep = std::variant<ResponseHead, ResponseBody, ResponseFinished, BufferedResponse, TransactionWork, TransactionFailure>;

class Transaction {
public:
    virtual ~Transaction() = default;
    [[nodiscard]] virtual TransactionStep on_event(const TransactionEvent&) = 0;
};
class TransactionFactory {
public:
    virtual ~TransactionFactory() = default;
    // Request is immutable and lives until this transaction is destroyed.
    [[nodiscard]] virtual core::Result<std::unique_ptr<Transaction>> create(const Request&, const TransactionContext&) const = 0;
};
// Bounded compatibility demo, also used by HttpSession when no factory is given.
[[nodiscard]] std::shared_ptr<const TransactionFactory> make_demo_factory(Response response);
} // namespace http
