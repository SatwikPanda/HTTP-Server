#include "concurrency/helpers/helper_pool.hpp"

#include "fixtures/parallel_io.hpp"
#include "net/runtime.hpp"
#include "test_support.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace concurrency;
using namespace concurrency::helpers;
using namespace test;

namespace {

WorkAction action(std::uint64_t txn, std::uint64_t seq, WorkRequest request,
                  std::chrono::milliseconds timeout = 3s, core::ConnectionId conn = {7, 1}) {
    WorkAction work;
    work.id = {{conn, txn}, endpoint_role(request), seq};
    work.deadline = Clock::now() + timeout;
    work.request = std::move(request);
    work.cancellation = std::make_shared<Cancellation>();
    return work;
}

// Controlled blocking boundary: production file/DNS code runs, but each call
// first waits on a gate. Only I/O timing is controlled.
class GatedAdapter final : public HelperAdapter {
public:
    explicit GatedAdapter(bool resolver_only = false) : resolver_only_(resolver_only) {}
    WorkEvent execute(const WorkAction& work, const std::function<bool()>& stop) override {
        if (resolver_only_ && !std::holds_alternative<ResolveWork>(work.request)) return inner_->execute(work, stop);
        {
            std::unique_lock lock(mutex_);
            ++entered_;
            cv_.notify_all();
            CHECK(cv_.wait_for(lock, 10s, [&] { return open_ || released_ > 0; }));
            if (!open_) --released_;
        }
        ++executed_;
        return inner_->execute(work, stop);
    }
    void wait_entered(int count) {
        std::unique_lock lock(mutex_);
        CHECK(cv_.wait_for(lock, 5s, [&] { return entered_ >= count; }));
    }
    void release_one() { { std::lock_guard lock(mutex_); ++released_; } cv_.notify_all(); }
    void open() { { std::lock_guard lock(mutex_); open_ = true; } cv_.notify_all(); }
    int executed() const { return executed_.load(); }
private:
    bool resolver_only_{};
    std::shared_ptr<HelperAdapter> inner_ = make_reference_adapter();
    std::mutex mutex_;
    std::condition_variable cv_;
    int entered_{};
    int released_{};
    bool open_{};
    std::atomic<int> executed_{};
};

// The owning driver side: a real poller woken by the port, drained on this thread.
struct Owner {
    net::Poller poller = take(net::Poller::create());
    std::atomic<int> wakes{};
    std::shared_ptr<CompletionPort> port;
    std::vector<WorkEvent> events;
    explicit Owner(std::size_t capacity = 16) {
        port = CompletionPort::create(capacity, [this] { ++wakes; (void)poller.wake(); });
        CHECK(port);
    }
    WorkExecutor::Completion sink() { return [this](WorkEvent event) { events.push_back(std::move(event)); }; }
    // Wait for `count` delivered events or `deadline`; returns false on deadline.
    bool wait_for(std::size_t count, Clock::time_point deadline) {
        while (events.size() < count) {
            port->drain();
            if (events.size() >= count) break;
            if (Clock::now() >= deadline) return false;
            CHECK(poller.wait(deadline));
        }
        return true;
    }
    WorkEvent next(Clock::duration timeout = 5s) {
        const auto before = events.size();
        CHECK(wait_for(before + 1, Clock::now() + timeout));
        return events[before];
    }
};

std::shared_ptr<HelperPool> pool(std::size_t threads, std::size_t queue, std::shared_ptr<HelperAdapter> adapter = nullptr) {
    HelperConfig config;
    config.threads = threads;
    config.queue_capacity = queue;
    return take(HelperPool::start(config, std::move(adapter)));
}

// Results are published before a helper reports itself idle; poll counters.
template <class Predicate> void eventually(Predicate predicate) {
    const auto deadline = Clock::now() + 5s;
    while (!predicate()) { CHECK(Clock::now() < deadline); std::this_thread::sleep_for(1ms); }
}

std::string text(const WorkEvent& event) {
    CHECK(event.bytes);
    return {reinterpret_cast<const char*>(event.bytes->data()), event.bytes->size()};
}

void file_sequence_returns_exact_bounded_chunks() {
    TemporaryDirectory dir;
    std::string content;
    for (int i = 0; i < 20000; ++i) content += char('a' + i % 26);
    const auto path = dir.write("data.bin", content);
    auto helpers = pool(2, 8);
    Owner owner;
    HelperExecutor executor(helpers, owner.port);

    CHECK(executor.submit(action(1, 1, FileInfoWork{path}), owner.sink()));
    auto info = owner.next();
    CHECK(info.status == WorkStatus::complete && info.info.regular && info.info.size == content.size());

    CHECK(executor.submit(action(1, 2, FileOpenWork{path}), owner.sink()));
    auto opened = owner.next();
    CHECK(opened.status == WorkStatus::complete && opened.file);
    const auto token = opened.file;

    std::string received;
    std::uint64_t seq = 3;
    for (;;) {
        CHECK(executor.submit(action(1, seq, FileReadWork{token, received.size(), 4096}), owner.sink()));
        auto read = owner.next();
        CHECK(read.id.sequence == seq++);
        if (read.status == WorkStatus::eof) break;
        CHECK(read.status == WorkStatus::progress && read.transferred > 0 && read.transferred <= 4096);
        received += text(read);
    }
    CHECK(received == content);
    CHECK(executor.submit(action(1, seq++, FileCloseWork{token}), owner.sink()));
    CHECK(owner.next().status == WorkStatus::complete);
    CHECK(executor.submit(action(1, seq++, FileReadWork{token, 0, 16}), owner.sink()));
    CHECK(owner.next().status == WorkStatus::failed); // closed through every alias
    CHECK(owner.port->outstanding() == 0);
}

void localhost_and_numeric_resolution_are_bounded_and_owned() {
    auto helpers = pool(1, 4);
    Owner owner;
    HelperExecutor executor(helpers, owner.port);
    for (const char* host : {"127.0.0.1", "localhost"}) {
        CHECK(executor.submit(action(1, 1, ResolveWork{{host, 8080}}), owner.sink()));
        auto resolved = owner.next();
        CHECK(resolved.status == WorkStatus::complete);
        CHECK(!resolved.candidates.empty() && resolved.candidates.size() <= WorkLimits{}.max_candidates);
        for (const auto& candidate : resolved.candidates) CHECK(candidate.port() == 8080);
    }
}

void saturation_rejects_immediately_without_growth() {
    auto gate = std::make_shared<GatedAdapter>();
    auto helpers = pool(1, 2, gate);
    Owner owner(16);
    TemporaryDirectory dir;
    const auto path = dir.write("f", "x");
    CHECK(helpers->submit(action(1, 1, FileInfoWork{path}), owner.port, owner.sink()) == Admission::accepted);
    gate->wait_entered(1);
    CHECK(helpers->submit(action(1, 2, FileInfoWork{path}), owner.port, owner.sink()) == Admission::accepted);
    CHECK(helpers->submit(action(1, 3, FileInfoWork{path}), owner.port, owner.sink()) == Admission::accepted);
    const auto started = Clock::now();
    CHECK(helpers->submit(action(1, 4, FileInfoWork{path}), owner.port, owner.sink()) == Admission::saturated);
    CHECK(Clock::now() - started < 100ms); // explicit and immediate
    auto stats = helpers->stats();
    CHECK(stats.queued == 2 && stats.running == 1 && stats.threads == 1 && stats.saturated == 1);
    CHECK(owner.port->outstanding() == 3);
    gate->open();
    CHECK(owner.wait_for(3, Clock::now() + 5s));
    for (const auto& event : owner.events) CHECK(event.status == WorkStatus::complete);
    // Pressure ended: admission recovers.
    CHECK(helpers->submit(action(1, 5, FileInfoWork{path}), owner.port, owner.sink()) == Admission::accepted);
    CHECK(owner.wait_for(4, Clock::now() + 5s));
}

void full_result_port_bounds_memory_and_recovers() {
    auto helpers = pool(2, 8);
    Owner owner(2);
    TemporaryDirectory dir;
    const auto path = dir.write("f", "abc");
    CHECK(helpers->submit(action(1, 1, FileInfoWork{path}), owner.port, owner.sink()) == Admission::accepted);
    CHECK(helpers->submit(action(1, 2, FileInfoWork{path}), owner.port, owner.sink()) == Admission::accepted);
    // The owner has not consumed anything; its two slots are taken.
    CHECK(helpers->submit(action(1, 3, FileInfoWork{path}), owner.port, owner.sink()) == Admission::saturated);
    const auto deadline = Clock::now() + 5s;
    while (helpers->stats().executed < 2) { CHECK(Clock::now() < deadline); (void)owner.poller.wait(Clock::now() + 10ms); }
    CHECK(owner.port->has_ready() && owner.port->outstanding() == 2 && owner.wakes.load() >= 1);
    CHECK(owner.port->drain(1) == 1); // budgeted drain; the rest stays queued
    CHECK(owner.port->outstanding() == 1);
    CHECK(helpers->submit(action(1, 3, FileInfoWork{path}), owner.port, owner.sink()) == Admission::accepted);
    while (helpers->stats().executed < 3) { CHECK(Clock::now() < deadline); (void)owner.poller.wait(Clock::now() + 10ms); }
    // Shutdown progresses although the consumer never drains its results.
    helpers->stop();
    CHECK(helpers->submit(action(1, 4, FileInfoWork{path}), owner.port, owner.sink()) == Admission::stopping);
    owner.port->close();
    CHECK(owner.port->outstanding() == 0);
}

void driver_deadline_does_not_wait_for_blocked_dns() {
    auto gate = std::make_shared<GatedAdapter>(true);
    auto helpers = pool(2, 8, gate);
    Owner owner;
    // Consumer context: only the currently expected job may be applied.
    std::optional<core::OperationId> expected;
    std::vector<WorkEvent> applied, stale;
    auto consume = [&](WorkEvent event) {
        if (expected && event.id == *expected) { applied.push_back(std::move(event)); expected.reset(); }
        else stale.push_back(std::move(event));
    };
    auto dns = action(1, 1, ResolveWork{{"localhost", 80}}, 100ms);
    expected = dns.id;
    auto cancel = dns.cancellation;
    const auto deadline = dns.deadline;
    CHECK(helpers->submit(std::move(dns), owner.port, consume) == Admission::accepted);
    gate->wait_entered(1);
    // The owner observes its deadline without the helper returning.
    while (Clock::now() < deadline) { CHECK(owner.poller.wait(deadline)); owner.port->drain(); }
    CHECK(applied.empty());
    cancel->cancel();
    expected.reset(); // timed out: application outcome emitted, consumer invalidated
    // Unrelated work still progresses on the second helper.
    TemporaryDirectory dir;
    const auto path = dir.write("f", "x");
    auto other = action(2, 1, FileInfoWork{path});
    expected = other.id;
    CHECK(helpers->submit(std::move(other), owner.port, consume) == Admission::accepted);
    const auto limit = Clock::now() + 5s;
    while (applied.empty()) { CHECK(Clock::now() < limit); (void)owner.poller.wait(limit); owner.port->drain(); }
    CHECK(applied[0].id.transaction.sequence == 2 && applied[0].status == WorkStatus::complete);
    eventually([&] { return helpers->stats().executed == 1; });
    CHECK(helpers->stats().running == 1); // the abandoned resolver still holds its slot
    gate->open();
    while (stale.empty()) { CHECK(Clock::now() < limit); (void)owner.poller.wait(limit); owner.port->drain(); }
    CHECK(stale[0].id.transaction.sequence == 1);
    CHECK(stale[0].status == WorkStatus::cancelled && stale[0].candidates.empty()); // payload released
    eventually([&] { return helpers->stats().running == 0; });
}

void cancel_before_execution_skips_and_releases() {
    auto gate = std::make_shared<GatedAdapter>();
    auto helpers = pool(1, 4, gate);
    Owner owner;
    TemporaryDirectory dir;
    const auto path = dir.write("f", "x");
    CHECK(helpers->submit(action(1, 1, FileInfoWork{path}), owner.port, owner.sink()) == Admission::accepted);
    gate->wait_entered(1);
    auto queued = action(1, 2, FileOpenWork{path});
    auto cancel = queued.cancellation;
    CHECK(helpers->submit(std::move(queued), owner.port, owner.sink()) == Admission::accepted);
    cancel->cancel();
    gate->open();
    CHECK(owner.wait_for(2, Clock::now() + 5s));
    CHECK(owner.events[1].id.sequence == 2 && owner.events[1].status == WorkStatus::cancelled && !owner.events[1].file);
    CHECK(gate->executed() == 1); // never reached the file system
    eventually([&] { return helpers->stats().skipped_cancelled == 1; });
    CHECK(owner.port->outstanding() == 0);
}

void cancel_during_execution_disposes_late_result() {
    auto gate = std::make_shared<GatedAdapter>();
    auto helpers = pool(1, 4, gate);
    Owner owner;
    TemporaryDirectory dir;
    const auto path = dir.write("f", "content");
    auto running = action(1, 1, FileOpenWork{path});
    auto cancel = running.cancellation;
    CHECK(helpers->submit(std::move(running), owner.port, owner.sink()) == Admission::accepted);
    gate->wait_entered(1);
    cancel->cancel();
    CHECK(helpers->stats().running == 1); // slot held until the call actually returns
    gate->open();
    auto late = owner.next();
    CHECK(late.status == WorkStatus::cancelled && !late.file); // opened stream not handed out
    eventually([&] { return helpers->stats().running == 0; });
}

void reused_connection_ids_cannot_reach_new_work() {
    auto gate = std::make_shared<GatedAdapter>();
    auto helpers = pool(1, 4, gate);
    Owner owner;
    TemporaryDirectory dir;
    const auto path = dir.write("f", "x");
    std::optional<core::OperationId> current;
    int applied = 0, discarded = 0;
    auto consume = [&](WorkEvent event) { (current && event.id == *current) ? ++applied : ++discarded; };
    auto old_job = action(1, 1, FileInfoWork{path}, 3s, {7, 1});
    current = old_job.id;
    CHECK(helpers->submit(std::move(old_job), owner.port, consume) == Admission::accepted);
    gate->wait_entered(1);
    // Connection slot 7 is closed and reused with a new generation; same txn/seq.
    auto new_job = action(1, 1, FileInfoWork{path}, 3s, {7, 2});
    current = new_job.id;
    CHECK(helpers->submit(std::move(new_job), owner.port, consume) == Admission::accepted);
    gate->open();
    const auto deadline = Clock::now() + 5s;
    while (applied + discarded < 2) { CHECK(Clock::now() < deadline); (void)owner.poller.wait(deadline); owner.port->drain(); }
    CHECK(applied == 1 && discarded == 1);
}

void destroyed_destination_is_never_woken() {
    auto gate = std::make_shared<GatedAdapter>();
    auto helpers = pool(1, 4, gate);
    TemporaryDirectory dir;
    const auto path = dir.write("f", "x");
    auto poller = std::make_unique<net::Poller>(take(net::Poller::create()));
    std::atomic<int> wakes{}, callbacks{};
    auto port = CompletionPort::create(4, [&wakes, raw = poller.get()] { ++wakes; (void)raw->wake(); });
    CHECK(helpers->submit(action(1, 1, FileOpenWork{path}), port, [&](WorkEvent) { ++callbacks; }) == Admission::accepted);
    gate->wait_entered(1);
    port->close();
    poller.reset(); // the owner and its poller are gone before the helper returns
    gate->open();
    eventually([&] { return helpers->stats().executed == 1 && helpers->stats().running == 0; });
    CHECK(wakes.load() == 0 && callbacks.load() == 0 && port->outstanding() == 0);
    CHECK(helpers->submit(action(1, 2, FileInfoWork{path}), port, [](WorkEvent) {}) == Admission::saturated);
}

void stop_cancels_queue_and_joins() {
    auto gate = std::make_shared<GatedAdapter>();
    auto helpers = pool(1, 4, gate);
    Owner owner;
    TemporaryDirectory dir;
    const auto path = dir.write("f", "x");
    for (std::uint64_t seq = 1; seq <= 3; ++seq) {
        CHECK(helpers->submit(action(1, seq, FileInfoWork{path}), owner.port, owner.sink()) == Admission::accepted);
    }
    gate->wait_entered(1);
    std::thread stopper([&] { helpers->stop(); });
    // Queued work is cancelled before the running job finishes.
    CHECK(owner.wait_for(2, Clock::now() + 5s));
    CHECK(owner.events[0].status == WorkStatus::cancelled && owner.events[1].status == WorkStatus::cancelled);
    gate->open();
    stopper.join();
    CHECK(owner.wait_for(3, Clock::now() + 5s));
    CHECK(owner.events[2].id.sequence == 1);
    CHECK(helpers->submit(action(1, 9, FileInfoWork{path}), owner.port, owner.sink()) == Admission::stopping);
    helpers->stop(); // idempotent
    CHECK(owner.port->outstanding() == 0);
}

void invalid_work_and_config_are_rejected() {
    HelperConfig bad;
    bad.threads = 0;
    CHECK(!HelperPool::start(bad));
    bad = {};
    bad.queue_capacity = 0;
    CHECK(!HelperPool::start(bad));
    CHECK(!CompletionPort::create(0, {}));
    auto helpers = pool(1, 2);
    Owner owner;
    CHECK(helpers->submit(action(1, 1, UpstreamReadWork{16}), owner.port, owner.sink()) == Admission::invalid);
    CHECK(helpers->submit(action(1, 0, FileInfoWork{"x"}), owner.port, owner.sink()) == Admission::invalid);
    CHECK(helpers->submit(action(0, 1, FileInfoWork{"x"}), owner.port, owner.sink()) == Admission::invalid);
    CHECK(helpers->submit(action(1, 1, FileInfoWork{"x"}), nullptr, owner.sink()) == Admission::invalid);
    CHECK(helpers->submit(action(1, 1, FileInfoWork{"x"}), owner.port, {}) == Admission::invalid);
    HelperExecutor executor(helpers, owner.port);
    auto refused = executor.submit(action(1, 1, ConnectWork{}), owner.sink());
    CHECK(!refused && refused.error().category == core::ErrorCategory::invalid_argument);
    CHECK(owner.port->outstanding() == 0);
}

void expired_queued_work_is_not_executed() {
    auto gate = std::make_shared<GatedAdapter>();
    auto helpers = pool(1, 4, gate);
    Owner owner;
    TemporaryDirectory dir;
    const auto path = dir.write("f", "x");
    CHECK(helpers->submit(action(1, 1, FileInfoWork{path}), owner.port, owner.sink()) == Admission::accepted);
    gate->wait_entered(1);
    CHECK(helpers->submit(action(1, 2, FileInfoWork{path}, 20ms), owner.port, owner.sink()) == Admission::accepted);
    std::this_thread::sleep_for(40ms); // deadline passes while queued (time is the subject here)
    gate->open();
    CHECK(owner.wait_for(2, Clock::now() + 5s));
    CHECK(owner.events[1].status == WorkStatus::timed_out && owner.events[1].error.category == core::ErrorCategory::timed_out);
    CHECK(gate->executed() == 1);
    eventually([&] { return helpers->stats().skipped_expired == 1; });
}

void repeated_and_partial_start_stop_terminate() {
    for (int round = 0; round < 20; ++round) {
        auto helpers = pool(3, 4);
        Owner owner;
        CHECK(helpers->submit(action(1, 1, ResolveWork{{"127.0.0.1", 1}}), owner.port, owner.sink()) == Admission::accepted);
        if (round % 2 == 0) (void)owner.next();
        helpers->stop();
        helpers.reset();
    }
    std::atomic<int> attempts{};
    HelperConfig failing;
    failing.threads = 4;
    failing.before_thread_start = [&](std::size_t index) -> core::Result<void> {
        ++attempts;
        if (index == 2) return core::make_error(core::ErrorCategory::system_error, "injected thread start");
        return {};
    };
    auto started = HelperPool::start(failing);
    CHECK(!started && started.error().operation == "injected thread start");
    CHECK(attempts.load() == 3); // the two created helpers were stopped and joined
}

} // namespace

int main() {
    auto runtime = take(net::initialize_network());
    file_sequence_returns_exact_bounded_chunks();
    localhost_and_numeric_resolution_are_bounded_and_owned();
    saturation_rejects_immediately_without_growth();
    full_result_port_bounds_memory_and_recovers();
    driver_deadline_does_not_wait_for_blocked_dns();
    cancel_before_execution_skips_and_releases();
    cancel_during_execution_disposes_late_result();
    reused_connection_ids_cannot_reach_new_work();
    destroyed_destination_is_never_woken();
    stop_cancels_queue_and_joins();
    invalid_work_and_config_are_rejected();
    expired_queued_work_is_not_executed();
    repeated_and_partial_start_stop_terminate();
    std::cout << "milestone 22 helper tests passed\n";
    return 0;
}
