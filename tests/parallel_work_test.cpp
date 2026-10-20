#include "fixtures/parallel_io.hpp"
#include "concurrency/single_session_driver.hpp"
#include "http/http_session.hpp"
#include "http/reference_application.hpp"
#include "http/response_parser.hpp"
#include "http/serializer.hpp"

#include <cstring>

namespace {
using namespace std::chrono_literals;
using namespace concurrency;
auto bytes(std::string_view text) { return std::as_bytes(std::span(text.data(), text.size())); }
class Channel final : public net::StreamChannel {
public:
    explicit Channel(std::string input) : input_(std::move(input)) {}
    net::ReadResult read_some(std::span<std::byte> output) override {
        if (offset_ == input_.size()) return net::ReadResult::eof();
        const auto count = std::min(output.size(), input_.size() - offset_);
        std::memcpy(output.data(), input_.data() + offset_, count); offset_ += count;
        return net::ReadResult::success(count);
    }
    net::WriteResult write_some(std::span<const std::byte> input) override {
        if (block_) { block_ = false; return net::WriteResult::would_block_result(); }
        const auto count = std::min<std::size_t>(7, input.size());
        output.append(reinterpret_cast<const char*>(input.data()), count);
        return net::WriteResult::success(count);
    }
    bool is_valid() const noexcept override { return valid_; }
    void close() noexcept override { valid_ = false; }
    std::string output;
private:
    std::string input_; std::size_t offset_{}; bool block_{true}, valid_{true};
};
std::string request(std::string_view method = "GET", bool close = true) {
    return std::string(method) + " /resource HTTP/1.1\r\nHost: localhost\r\n" + (close ? "Connection: close\r\n" : "") + "\r\n";
}
struct Counts final : core::MetricsSink {
    std::uint64_t completed{}, wire{}, body{};
    void record(core::Metric metric, std::uint64_t amount) noexcept override {
        if (metric == core::Metric::responses_completed) completed += amount;
        if (metric == core::Metric::wire_bytes) wire += amount;
        if (metric == core::Metric::body_bytes) body += amount;
    }
};
void file_session_and_stale_results() {
    test::TemporaryDirectory directory;
    const std::string body(32 * 1024, 'f');
    const auto path = directory.write("body.bin", body);
    http::HttpSessionConfig config; config.max_output_bytes = 256; config.keep_alive = true;
    config.application = http::make_file_reference_factory(path);
    Counts counts; config.metrics = &counts;
    Channel channel(request("GET", false) + request("HEAD", false) + request());
    auto stats = http::run_http_session(channel, config);
    CHECK(stats.completed_cleanly && stats.requests_served == 3 && stats.peak_output_bytes <= 256);
    CHECK(stats.short_write_count > 0 && stats.would_block_write_count == 1);
    CHECK(counts.completed == 3 && counts.body == body.size() * 2 && counts.wire == channel.output.size());
    std::size_t consumed = 0;
    for (const auto method : {http::Method::GET, http::Method::HEAD, http::Method::GET}) {
        http::ResponseParser parser(method);
        const auto result = parser.consume(std::string_view(channel.output).substr(consumed));
        CHECK(result.event == http::ParseEvent::complete); consumed += result.consumed;
        CHECK(parser.response().body == (method == http::Method::HEAD ? "" : body));
        CHECK(parser.response().headers.get("Content-Length") == std::to_string(body.size()));
    }
    CHECK(consumed == channel.output.size());

    // A completion from request 1 must not replace request 2's in-flight work.
    http::HttpSession session(config, {55, 7});
    const auto pipeline = request("HEAD", false) + request("HEAD");
    auto action = session.on_event(ReadDataEvent{bytes(pipeline)});
    CHECK(action.as_work());
    ReferenceWorkExecutor executor;
    const auto stale = executor.execute(*action.as_work());
    action = session.on_event(stale);
    const auto second_id = action.as_work()->id;
    action = session.on_event(stale);
    CHECK(action.as_work() && action.as_work()->id == second_id);
    bool next_request = false;
    for (unsigned n = 0; !session.is_closed() && n < 1000; ++n) {
        if (auto* work = action.as_work()) {
            const auto current_id = work->id;
            if (current_id.transaction.sequence == 2) {
                next_request = true;
                action = session.on_event(stale);
                CHECK(action.as_work() && action.as_work()->id == current_id);
            }
            action = session.on_event(executor.execute(*action.as_work()));
        } else if (auto* write = action.as_write()) {
            auto old = write->id; old.transaction.connection.generation--;
            const auto size = write->data.size();
            action = session.on_event(WriteCompleteEvent{size, old});
            CHECK(action.as_write() && action.as_write()->data.size() == size);
            old = action.as_write()->id;
            action = session.on_event(WriteCompleteEvent{1, old});
            CHECK(action.as_write() && action.as_write()->data.size() == size - 1);
            action = session.on_event(WriteCompleteEvent{1, old}); // same-generation duplicate
            CHECK(action.as_write() && action.as_write()->data.size() == size - 1);
            action = session.on_event(WriteCompleteEvent{size - 1, action.as_write()->id});
        } else CHECK(false);
    }
    CHECK(next_request && session.is_closed() && !session.error());
    std::shared_ptr<Cancellation> abandoned;
    {
        http::HttpSession owner(config);
        auto pending = owner.on_event(ReadDataEvent{bytes(request())});
        CHECK(pending.as_work());
        abandoned = pending.as_work()->cancellation;
    }
    CHECK(abandoned->is_cancelled());
}

void work_results_and_cancellation() {
    test::TemporaryDirectory directory;
    const auto path = directory.write("bytes", "abcdef");
    ReferenceWorkExecutor executor({4, 32, 8192});
    std::uint64_t sequence = 0;
    auto cancellation = std::make_shared<Cancellation>();
    auto action = [&](WorkRequest request) {
        return WorkAction{{{{100, 3}, 1}, endpoint_role(request), ++sequence}, test::Clock::now() + 2s, std::move(request), cancellation};
    };
    auto result = executor.execute(action(FileInfoWork{path}));
    CHECK(result.status == WorkStatus::complete && result.info.regular && result.info.size == 6);
    CHECK(result.info.canonical_path == std::filesystem::canonical(path));
    result = executor.execute(action(FileInfoWork{path.parent_path()}));
    CHECK(result.status == WorkStatus::complete && result.info.directory && !result.info.regular);
    result = executor.execute(action(FileOpenWork{path.parent_path()}));
    CHECK(result.status == WorkStatus::failed && result.file_failure == FileFailure::not_regular);
    std::error_code link_error;
    const auto link = path.parent_path() / "link";
    std::filesystem::create_symlink(path, link, link_error);
    if (!link_error) {
        result = executor.execute(action(FileInfoWork{link}));
        CHECK(result.status == WorkStatus::complete && result.info.has_link_component);
        result = executor.execute(action(FileOpenWork{link}));
        CHECK(result.status == WorkStatus::failed && result.file_failure == FileFailure::forbidden);
    } else std::cout << "Symlink creation unavailable: " << link_error.message() << '\n';
    result = executor.execute(action(FileOpenWork{path})); CHECK(result.status == WorkStatus::complete && result.file);
    auto file = result.file;
    result = executor.execute(action(FileReadWork{file, 0, 4})); CHECK(result.status == WorkStatus::progress && result.transferred == 4);
    result = executor.execute(action(FileReadWork{file, 4, 4})); CHECK(result.status == WorkStatus::progress && result.transferred == 2);
    result = executor.execute(action(FileReadWork{file, 6, 4})); CHECK(result.status == WorkStatus::eof);
    result = executor.execute(action(FileReadWork{file, 0, 5})); CHECK(result.status == WorkStatus::rejected);
    auto expired = action(FileReadWork{file, 0, 4}); expired.deadline = test::Clock::now() - 1ms;
    CHECK(executor.execute(expired).status == WorkStatus::timed_out);
    CHECK(executor.execute(action(FileCloseWork{file})).status == WorkStatus::complete);
    CHECK(executor.execute(action(FileReadWork{file, 0, 1})).status == WorkStatus::failed);
    CHECK(executor.execute(action(FileOpenWork{path / "missing"})).status == WorkStatus::failed);
    result = executor.execute(action(FileInfoWork{path.parent_path() / "missing"}));
    CHECK(result.status == WorkStatus::failed && result.file_failure == FileFailure::missing);
    bool completion = false;
    CHECK(executor.submit(action(FileInfoWork{path}), [&](WorkEvent event) {
        completion = true; CHECK(event.status == WorkStatus::complete);
        bool nested = false;
        auto admitted = executor.submit(action(FileInfoWork{path}), [&](WorkEvent) { nested = true; });
        CHECK(!admitted && admitted.error().category == core::ErrorCategory::would_block && !nested);
    }));
    CHECK(completion);
    cancellation->cancel();
    CHECK(executor.execute(action(FileInfoWork{path})).status == WorkStatus::cancelled);
    auto duplicate = action(FileInfoWork{path});
    CHECK(executor.execute(duplicate).status == WorkStatus::cancelled);
    CHECK(executor.execute(duplicate).status == WorkStatus::cancelled);
}

void upstream_actions() {
    test::ControlledOrigin origin({"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"}, 5, true);
    ReferenceWorkExecutor executor;
    std::uint64_t sequence = 0;
    auto cancellation = std::make_shared<Cancellation>();
    auto make = [&](WorkRequest request) { return WorkAction{{{{9, 2}, 1}, endpoint_role(request), ++sequence}, test::Clock::now() + 5s, std::move(request), cancellation}; };
    auto resolved = executor.execute(make(ResolveWork{{"localhost", origin.target().port}}));
    CHECK(resolved.status == WorkStatus::complete && !resolved.candidates.empty());
    auto reservation = test::listener();
    const auto refused_endpoint = test::take(reservation.local_endpoint());
    CHECK(refused_endpoint != origin.endpoint());
    // Darwin can drop SYNs for a bound, non-listening socket. As in milestone
    // 03, close a temporary listener so this candidate is a genuinely closed port.
    reservation.close();
    auto candidates = std::vector<net::Endpoint>{refused_endpoint, origin.endpoint()};
    auto connected = executor.execute(make(ConnectWork{candidates}));
    if (connected.error) std::cerr << connected.error.to_string() << '\n';
    CHECK(connected.status == WorkStatus::complete);
    CHECK(executor.execute(make(UpstreamWaitWork{net::Interest::write})).status == WorkStatus::complete);
    const auto request_text = request();
    const auto raw = bytes(request_text);
    auto outgoing = std::make_shared<const Bytes>(raw.begin(), raw.end());
    std::size_t offset = 0;
    while (offset < outgoing->size()) {
        auto wrote = executor.execute(make(UpstreamWriteWork{outgoing, offset, outgoing->size() - offset}));
        CHECK(wrote.status == WorkStatus::progress); offset += wrote.transferred;
    }
    origin.wait_for_request();
    CHECK(executor.execute(make(UpstreamReadWork{8})).status == WorkStatus::would_block);
    auto timeout = make(UpstreamWaitWork{net::Interest::read}); timeout.deadline = test::Clock::now() + 25ms;
    CHECK(executor.execute(timeout).status == WorkStatus::timed_out); // timeout closes upstream
    origin.release();
    // Destructor stops the controlled origin if the timeout closed its reader.

    test::ControlledOrigin second({"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"}, 3);
    CHECK(executor.execute(make(ConnectWork{{second.endpoint()}})).status == WorkStatus::complete);
    offset = 0;
    while (offset < outgoing->size()) {
        auto wrote = executor.execute(make(UpstreamWriteWork{outgoing, offset, outgoing->size() - offset}));
        CHECK(wrote.status == WorkStatus::progress); offset += wrote.transferred;
    }
    std::string wire;
    for (;;) {
        auto read = executor.execute(make(UpstreamReadWork{7}));
        if (read.status == WorkStatus::eof) break;
        if (read.status == WorkStatus::would_block) {
            CHECK(executor.execute(make(UpstreamWaitWork{net::Interest::read})).status == WorkStatus::complete);
        } else {
            CHECK(read.status == WorkStatus::progress && read.bytes);
            wire.append(reinterpret_cast<const char*>(read.bytes->data()), read.bytes->size());
        }
    }
    CHECK(wire.ends_with("\r\n\r\nok"));
    CHECK(executor.execute(make(UpstreamCloseWork{})).status == WorkStatus::complete);
    second.join();

    auto stopped = make(ResolveWork{{"localhost", 80}});
    CHECK(executor.execute(stopped, [] { return true; }).status == WorkStatus::cancelled);
}

void origin_session_and_failures() {
    const std::string body(8192, 'o');
    const auto chunk = test::take(http::encode_chunk(body));
    test::ControlledOrigin origin({"HTTP/1.1 103 Early Hints\r\n\r\nHTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n", chunk, "0\r\nX-Test: done\r\n\r\n"}, 43);
    http::HttpSessionConfig config; config.max_output_bytes = 256;
    config.application = http::make_origin_reference_factory(origin.target());
    Channel channel(request());
    const auto stats = http::run_http_session(channel, config);
    CHECK(stats.completed_cleanly && stats.requests_served == 1 && stats.peak_output_bytes <= 256);
    http::ResponseParser parser;
    CHECK(parser.consume(channel.output).event == http::ParseEvent::complete);
    CHECK(parser.response().body == body && parser.response().trailers.get("X-Test") == "done");
    origin.join(); CHECK(origin.request_count() == 1 && origin.request().starts_with("GET / HTTP/1.1\r\n"));

    test::ControlledOrigin old_client_origin({"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"});
    config.application = http::make_origin_reference_factory(old_client_origin.target());
    Channel old_client("GET / HTTP/1.0\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n");
    CHECK(http::run_http_session(old_client, config).completed_cleanly);
    CHECK(old_client.output.starts_with("HTTP/1.0 200") && old_client.output.find("Transfer-Encoding") == std::string::npos);
    CHECK(old_client.output.ends_with("\r\n\r\nok"));
    old_client_origin.join();

    for (bool after_head : {false, true}) {
        test::ControlledOrigin bad({after_head ? "HTTP/1.1 200 OK\r\nContent-Length: 8\r\n\r\nx" : "INVALID\r\n\r\n"});
        config.application = http::make_origin_reference_factory(bad.target());
        Channel broken(request()); const auto failed = http::run_http_session(broken, config);
        if (after_head) {
            CHECK(failed.error && !failed.completed_cleanly && failed.requests_served == 0);
            CHECK(broken.output.starts_with("HTTP/1.1 200") && broken.output.find("502") == std::string::npos);
            CHECK(!broken.output.ends_with("0\r\n\r\n"));
        } else CHECK(broken.output.starts_with("HTTP/1.1 502") && failed.requests_served == 1);
        bad.join();
    }
}

void real_downstream_and_upstream() {
    test::ControlledOrigin origin({"HTTP/1.1 200 OK\r\nContent-Length: 4096\r\n\r\n", std::string(4096, 'r')}, 31);
    auto pair = test::pair();
    http::HttpSessionStats stats;
    http::HttpSessionConfig config; config.max_output_bytes = 256;
    config.application = http::make_origin_reference_factory(origin.target());
    std::thread server([&] { stats = http::run_http_session(pair.server, config); pair.server.close(); });
    auto poller = test::take(net::Poller::create());
    auto token = test::take(poller.watch(pair.client, net::Interest::write, {3, 1}));
    const auto deadline = test::Clock::now() + 5s;
    auto wait = [&](net::Interest interest) {
        CHECK(poller.modify(token, interest));
        while (test::Clock::now() < deadline) {
            auto events = test::take(poller.wait(deadline));
            for (const auto& event : events) if (event.token == token && poller.is_current(event)) return;
        }
        CHECK(false);
    };
    auto outbound = request(); std::size_t offset = 0;
    while (offset < outbound.size()) {
        auto wrote = pair.client.write_some(bytes(outbound).subspan(offset));
        if (wrote.is_ok()) offset += wrote.bytes_transferred;
        else { CHECK(wrote.would_block()); wait(net::Interest::write); }
    }
    std::array<std::byte, 89> input{}; std::string output;
    for (;;) {
        auto read = pair.client.read_some(input);
        if (read.is_eof()) break;
        if (read.is_ok()) output.append(reinterpret_cast<const char*>(input.data()), read.bytes_transferred);
        else { CHECK(read.would_block()); wait(net::Interest::read); }
    }
    CHECK(poller.unwatch(token)); server.join(); origin.join();
    CHECK(stats.completed_cleanly && stats.requests_served == 1);
    http::ResponseParser parser; CHECK(parser.consume(output).event == http::ParseEvent::complete);
    CHECK(parser.response().body == std::string(4096, 'r'));
}
} // namespace
int main() {
    auto runtime = test::take(net::initialize_network());
    file_session_and_stale_results(); work_results_and_cancellation(); upstream_actions();
    origin_session_and_failures(); real_downstream_and_upstream();
}
