#include "http/application.hpp"

namespace http {
namespace {
class DemoTransaction final : public Transaction {
public:
    explicit DemoTransaction(Response response) : response_(std::move(response)) {}
    TransactionStep on_event(const TransactionEvent&) override { return BufferedResponse{std::move(response_)}; }
private:
    Response response_;
};
class DemoFactory final : public TransactionFactory {
public:
    explicit DemoFactory(Response response) : response_(std::move(response)) {}
    core::Result<std::unique_ptr<Transaction>> create(const Request&, const TransactionContext&) const override {
        return std::unique_ptr<Transaction>(std::make_unique<DemoTransaction>(response_));
    }
private:
    Response response_;
};
}
std::shared_ptr<const TransactionFactory> make_demo_factory(Response response) {
    return std::make_shared<DemoFactory>(std::move(response));
}
} // namespace http
