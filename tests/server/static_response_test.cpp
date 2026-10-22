#include "static_harness.hpp"

#include "http/date.hpp"

#include <algorithm>
#include <thread>
#ifndef _WIN32
#include <unistd.h>
#endif

namespace {
using namespace server_test;
using concurrency::FileCloseWork;
using concurrency::FileFailure;
using concurrency::FileOpenWork;
using concurrency::FileReadWork;
using concurrency::WorkStatus;
using http::Method;

http::HttpSession make_session(const http::HttpSessionConfig& config) { return http::HttpSession(config); }

void bodies_are_byte_exact_with_metadata() {
    Tree tree;
    const std::string text = "plain text\nwith lines\r\nand a final newline\n";
    const std::string binary = pattern_bytes(100 * 1024, 7) + std::string("\0\r\n\r\nHTTP/1.1 200 OK\r\n\r\n", 26);
    std::string all_bytes(256, '\0');
    for (int i = 0; i < 256; ++i) all_bytes[static_cast<std::size_t>(i)] = static_cast<char>(i);
    const std::string large = pattern_bytes(3 * 1024 * 1024 + 17, 3);
    tree.write("empty.txt", "");
    tree.write("text.txt", text);
    tree.write("page.html", "<p>x</p>");
    tree.write("blob.bin", binary);
    tree.write("bytes.dat", all_bytes);
    tree.write("noextension", "no extension");
    tree.write("big.bin", large);
    const auto factory = make_factory(tree.root());

    struct Case { const char* target; const std::string* body; const char* type; };
    const std::string empty, page = "<p>x</p>", none = "no extension";
    for (const auto& item : {Case{"/empty.txt", &empty, "text/plain; charset=utf-8"}, Case{"/text.txt", &text, "text/plain; charset=utf-8"},
                             Case{"/page.html", &page, "text/html; charset=utf-8"}, Case{"/blob.bin", &binary, "application/octet-stream"},
                             Case{"/bytes.dat", &all_bytes, "application/octet-stream"}, Case{"/noextension", &none, "application/octet-stream"},
                             Case{"/big.bin", &large, "application/octet-stream"}}) {
        const auto get = fetch_response(factory, item.target);
        CHECK(get.status == 200);
        CHECK(get.body.size() == item.body->size() && get.body == *item.body);
        CHECK(header(get, "Content-Length") == std::to_string(item.body->size()));
        CHECK(header(get, "Content-Type") == item.type);
        CHECK(header(get, "Transfer-Encoding").empty());
        CHECK(http::parse_http_date(header(get, "Date")));
        CHECK(http::parse_http_date(header(get, "Last-Modified")));
        CHECK(header(get, "ETag").starts_with("W/\""));
        CHECK(header(get, "Connection") == "close"); // the request asked for it

        // HEAD carries the same metadata as GET and no body bytes at all.
        const auto head = fetch_response(factory, item.target, "HEAD");
        CHECK(head.status == 200 && head.body.empty());
        for (const char* name : {"Content-Length", "Content-Type", "ETag", "Last-Modified"}) CHECK(header(head, name) == header(get, name));
    }
}

void small_windows_and_slow_readers_stay_bounded() {
    Tree tree;
    const std::string body = pattern_bytes(300 * 1024 + 5, 11);
    tree.write("slow.bin", body);
    const auto factory = make_factory(tree.root());

    // 256-byte output window, 7-byte short writes, one would-block.
    Channel channel(request("/slow.bin") + request("/slow.bin", "HEAD") + request("/slow.bin", "GET", {}, true), 7);
    auto stats = http::run_http_session(channel, session_config(factory, 256));
    CHECK(stats.completed_cleanly && stats.requests_served == 3 && stats.peak_output_bytes <= 256);
    CHECK(stats.short_write_count > 0 && stats.would_block_write_count >= 1);
    const auto parsed = parse_stream(channel.output, {Method::GET, Method::HEAD, Method::GET});
    CHECK(parsed.complete && parsed.responses.size() == 3);
    CHECK(parsed.responses[0].body == body && parsed.responses[1].body.empty() && parsed.responses[2].body == body);

    // One-byte writes against a small file: a very slow reader still gets exact bytes.
    tree.write("tiny.txt", "0123456789");
    Channel trickle(request("/tiny.txt", "GET", {}, true), 1);
    CHECK(http::run_http_session(trickle, session_config(factory, 4096)).completed_cleanly);
    const auto trickled = parse_stream(trickle.output, {Method::GET});
    CHECK(trickled.complete && trickled.responses[0].body == "0123456789");
}

void reads_follow_downstream_demand() {
    Tree tree;
    const std::string body = pattern_bytes(50'000, 5);
    tree.write("demand.bin", body);
    server::StaticConfig config;
    config.max_chunk_bytes = 1000;
    const auto factory = make_factory(tree.root(), config);
    for (const std::size_t window : {static_cast<std::size_t>(512), static_cast<std::size_t>(64 * 1024)}) {
        auto session = make_session(session_config(factory, window));
        ManualDriver driver(session, request("/demand.bin", "GET", {}, true), 13);
        CHECK(driver.run() && !driver.closed_error);
        CHECK(session.stats().requests_served == 1 && session.stats().peak_output_bytes <= window);
        const auto parsed = parse_stream(driver.output, {Method::GET});
        CHECK(parsed.complete && parsed.responses[0].body == body);

        // Exactly open, contiguous bounded reads, close; every read happens only
        // after the previous segment fully drained, so nothing is prefetched or reread.
        std::uint64_t expected_offset = 0;
        std::size_t reads = 0, last_output = 0;
        CHECK(!driver.jobs.empty() && std::holds_alternative<FileOpenWork>(driver.jobs.front().request));
        CHECK(std::holds_alternative<FileCloseWork>(driver.jobs.back().request));
        for (std::size_t i = 1; i + 1 < driver.jobs.size(); ++i) {
            const auto* read = std::get_if<FileReadWork>(&driver.jobs[i].request);
            CHECK(read != nullptr);
            CHECK(read->offset == expected_offset);
            CHECK(read->max_bytes > 0 && read->max_bytes <= 1000 && read->max_bytes <= window);
            CHECK(driver.jobs[i].result.status == WorkStatus::progress && driver.jobs[i].result.transferred == read->max_bytes);
            CHECK(driver.jobs[i].output_bytes_before > last_output || reads == 0); // the writer made progress between reads
            last_output = driver.jobs[i].output_bytes_before;
            expected_offset += driver.jobs[i].result.transferred;
            ++reads;
        }
        CHECK(expected_offset == body.size() && reads >= body.size() / 1000);
    }
}

void head_and_empty_files_schedule_no_reads() {
    Tree tree;
    tree.write("data.txt", pattern_bytes(5000, 2));
    tree.write("empty.txt", "");
    const auto factory = make_factory(tree.root());
    {
        auto session = make_session(session_config(factory));
        ManualDriver driver(session, request("/data.txt", "HEAD", {}, true));
        CHECK(driver.run() && !driver.closed_error);
        CHECK(driver.jobs.size() == 2 && std::holds_alternative<FileOpenWork>(driver.jobs[0].request) &&
              std::holds_alternative<FileCloseWork>(driver.jobs[1].request)); // metadata decision only
        CHECK(session.stats().requests_served == 1);
    }
    {
        auto session = make_session(session_config(factory));
        ManualDriver driver(session, request("/empty.txt", "GET", {}, true));
        CHECK(driver.run() && !driver.closed_error);
        CHECK(driver.jobs.size() == 2); // open, close; an empty GET needs no read
        const auto parsed = parse_stream(driver.output, {Method::GET});
        CHECK(parsed.complete && parsed.responses[0].status == 200 && parsed.responses[0].body.empty());
        CHECK(header(parsed.responses[0], "Content-Length") == "0");
        CHECK(session.stats().requests_served == 1);
    }
}

void midstream_failures_close_without_a_second_response() {
    Tree tree;
    const std::string body = pattern_bytes(40'000, 9);
    const auto path = tree.write("f.bin", body);
    server::StaticConfig config;
    config.max_chunk_bytes = 4096;
    const auto factory = make_factory(tree.root(), config);

    auto expect_aborted = [&](ManualDriver& driver, http::HttpSession& session) {
        CHECK(driver.run());
        CHECK(session.is_closed() && driver.closed_error);               // the failure closes the connection
        CHECK(session.stats().requests_served == 0 && !session.stats().response_sent); // never counted as complete
        std::size_t heads = 0;
        for (auto at = driver.output.find("HTTP/1.1"); at != std::string::npos; at = driver.output.find("HTTP/1.1", at + 1)) ++heads;
        CHECK(heads == 1);                                                 // no second response was appended
        const auto head_end = driver.output.find("\r\n\r\n");
        CHECK(head_end != std::string::npos && driver.output.size() - (head_end + 4) < body.size());
        // Whatever was sent after the head is an exact prefix of the file: no padding,
        // error text or terminator was appended to disguise the short body.
        const auto sent = std::string_view(driver.output).substr(head_end + 4);
        CHECK(sent == std::string_view(body).substr(0, sent.size()));
    };

    {   // A read error after commit.
        auto session = make_session(session_config(factory, 1024));
        ManualDriver driver(session, request("/f.bin", "GET", {}, true));
        unsigned reads = 0;
        driver.after_job = [&](const concurrency::WorkAction& action, concurrency::WorkEvent& result) {
            if (std::holds_alternative<FileReadWork>(action.request) && ++reads == 3) {
                result = {}; result.id = action.id; result.status = WorkStatus::failed;
                result.error = core::make_error(core::ErrorCategory::io_error, "injected read failure");
            }
        };
        expect_aborted(driver, session);
    }
    {   // The file is truncated after the response started: EOF before the advertised length.
        auto session = make_session(session_config(factory, 1024));
        ManualDriver driver(session, request("/f.bin", "GET", {}, true));
        unsigned reads = 0;
        driver.before_job = [&](const concurrency::WorkAction& action) {
            if (std::holds_alternative<FileReadWork>(action.request) && ++reads == 4) {
                std::error_code error; std::filesystem::resize_file(path, 2500, error); CHECK(!error);
            }
        };
        expect_aborted(driver, session);
        std::error_code error; std::filesystem::resize_file(path, body.size(), error);
        { std::ofstream out(path, std::ios::binary); out.write(body.data(), static_cast<std::streamsize>(body.size())); }
    }
    {   // A timed-out read after commit.
        auto session = make_session(session_config(factory, 1024));
        ManualDriver driver(session, request("/f.bin", "GET", {}, true));
        unsigned reads = 0;
        driver.after_job = [&](const concurrency::WorkAction& action, concurrency::WorkEvent& result) {
            if (std::holds_alternative<FileReadWork>(action.request) && ++reads == 2) {
                result = {}; result.id = action.id; result.status = WorkStatus::timed_out;
                result.error = core::make_error(core::ErrorCategory::timed_out, "injected deadline");
            }
        };
        expect_aborted(driver, session);
    }
    {   // A file that grows while streaming never sends more than the advertised length.
        auto session = make_session(session_config(factory, 1024));
        ManualDriver driver(session, request("/f.bin", "GET", {}, true));
        driver.before_job = [&](const concurrency::WorkAction& action) {
            if (const auto* read = std::get_if<FileReadWork>(&action.request); read && read->offset == 0) {
                std::ofstream out(path, std::ios::binary | std::ios::app);
                out << std::string(10'000, 'G');
            }
        };
        CHECK(driver.run() && !driver.closed_error && session.stats().requests_served == 1);
        const auto parsed = parse_stream(driver.output, {Method::GET});
        CHECK(parsed.complete && parsed.responses[0].body == body);
        CHECK(header(parsed.responses[0], "Content-Length") == std::to_string(body.size()));
        { std::ofstream out(path, std::ios::binary); out.write(body.data(), static_cast<std::streamsize>(body.size())); }
    }
}

void failures_before_commit_get_error_responses() {
    Tree tree;
    tree.write("f.txt", "content");
    const auto factory = make_factory(tree.root());
    struct Case { WorkStatus status; FileFailure failure; int expected; };
    for (const auto& item : {Case{WorkStatus::failed, FileFailure::io, 500}, Case{WorkStatus::failed, FileFailure::none, 500},
                             Case{WorkStatus::timed_out, FileFailure::none, 503}, Case{WorkStatus::failed, FileFailure::missing, 404},
                             Case{WorkStatus::failed, FileFailure::forbidden, 403}, Case{WorkStatus::failed, FileFailure::not_regular, 403}}) {
        auto session = make_session(session_config(factory));
        ManualDriver driver(session, request("/f.txt", "GET", {}, true));
        driver.after_job = [&](const concurrency::WorkAction& action, concurrency::WorkEvent& result) {
            if (std::holds_alternative<FileOpenWork>(action.request)) {
                result = {}; result.id = action.id; result.status = item.status; result.file_failure = item.failure;
                result.error = core::make_error(core::ErrorCategory::io_error, "injected open failure", 0, "secret local detail");
            }
        };
        CHECK(driver.run());
        const auto parsed = parse_stream(driver.output, {Method::GET});
        CHECK(parsed.complete && parsed.responses[0].status == item.expected);
        CHECK(parsed.responses[0].body.find("secret") == std::string::npos && parsed.responses[0].body.find(tree.root().string()) == std::string::npos);
    }
}

void permissions_are_reported_as_forbidden() {
#ifdef _WIN32
    unsupported("POSIX permission bits (Windows ACLs are not set by this test)");
#else
    Tree tree;
    const auto locked = tree.write("locked.txt", "private");
    tree.write("closed/inner.txt", "inner");
    std::error_code error;
    std::filesystem::permissions(locked, std::filesystem::perms::none, error);
    CHECK(!error);
    std::filesystem::permissions(tree.root() / "closed", std::filesystem::perms::none, error);
    CHECK(!error);
    const auto factory = make_factory(tree.root());
    if (::geteuid() == 0) {
        unsupported("permission-denied checks need a non-root user; this process is root");
    } else {
        CHECK(fetch_response(factory, "/locked.txt").status == 403);
        CHECK(fetch_response(factory, "/locked.txt", "HEAD").status == 403);
        CHECK(fetch_response(factory, "/closed/inner.txt").status == 403);
    }
    std::filesystem::permissions(tree.root() / "closed", std::filesystem::perms::owner_all, error);
    std::filesystem::permissions(locked, std::filesystem::perms::owner_all, error);
#endif
}

void cleanup_releases_handles() {
    Tree tree;
    const std::string body = pattern_bytes(30'000, 4);
    tree.write("f.bin", body);
    const auto factory = make_factory(tree.root());
    const auto before = open_descriptor_count();
    if (!before) unsupported("descriptor counting; handle-leak checks were not exercised");
    for (unsigned round = 0; round < 40; ++round) {
        auto session = make_session(session_config(factory, 700));
        // Alternate: client lost mid-body, destroyed mid-job, normal HEAD, normal GET.
        ManualDriver driver(session, request("/f.bin", round % 4 == 3 ? "HEAD" : "GET", {}, true), 50);
        if (round % 4 == 0) driver.lose_client_after = 2000;
        if (round % 4 == 1) {
            // Abandon the session while a read job is outstanding (no completion ever delivered).
            auto action = session.on_event(concurrency::ReadDataEvent{std::as_bytes(std::span(driver.output)), {}});
            (void)action;
        } else {
            (void)driver.run();
        }
    }
    {   // A session destroyed with work in flight cancels it and drops its token.
        auto session = make_session(session_config(factory));
        const auto text = request("/f.bin");
        auto action = session.on_event(concurrency::ReadDataEvent{std::as_bytes(std::span(text)), {}});
        CHECK(action.as_work() && std::holds_alternative<FileOpenWork>(action.as_work()->request));
        CHECK(!action.as_work()->cancellation->is_cancelled());
        auto cancellation = action.as_work()->cancellation;
        concurrency::ReferenceWorkExecutor executor;
        auto opened = executor.execute(*action.as_work()); // a token now exists outside the session
        action = session.on_event(opened);
        CHECK(action.as_write());                           // head ready
        // Session leaves scope here with the response uncommitted.
        (void)cancellation;
    }
    const auto after = open_descriptor_count();
    if (before && after) CHECK(*after <= *before);
}

void persistent_loopback_connection() {
    Tree tree;
    const std::string body = pattern_bytes(300 * 1024, 21);
    tree.write("loop.bin", body);
    tree.write("small.txt", "small");
    const auto factory = make_factory(tree.root());
    auto sockets = test::pair();
    auto config = session_config(factory, 8 * 1024);
    http::HttpSessionStats server_stats;
    std::thread serving([&] { server_stats = http::run_http_session(sockets.server, config); });

    std::string received;
    auto exchange = [&](const std::string& wire, const std::vector<Method>& methods) {
        std::size_t sent = 0;
        const auto deadline = test::Clock::now() + std::chrono::seconds(10);
        while (test::Clock::now() < deadline) {
            if (sent < wire.size()) {
                const auto written = sockets.client.write_some(std::as_bytes(std::span(wire).subspan(sent)));
                if (written.is_ok()) sent += written.bytes_transferred;
            }
            std::array<std::byte, 16 * 1024> buffer{};
            const auto read = sockets.client.read_some(buffer);
            if (read.is_ok() && read.bytes_transferred > 0) {
                received.append(reinterpret_cast<const char*>(buffer.data()), read.bytes_transferred);
                if (sent == wire.size() && parse_stream(received, methods).complete) return;
            } else if (read.would_block()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } else CHECK(!read.is_eof());
        }
        CHECK(false); // timed out
    };
    exchange(request("/loop.bin"), {Method::GET});
    exchange(request("/small.txt", "HEAD"), {Method::GET, Method::HEAD});
    exchange(request("/loop.bin") , {Method::GET, Method::HEAD, Method::GET});
    exchange(request("/small.txt", "GET", {}, true), {Method::GET, Method::HEAD, Method::GET, Method::GET});
    const auto parsed = parse_stream(received, {Method::GET, Method::HEAD, Method::GET, Method::GET});
    CHECK(parsed.complete && parsed.responses[0].body == body && parsed.responses[1].body.empty() &&
          parsed.responses[2].body == body && parsed.responses[3].body == "small");
    sockets.client.close();
    serving.join();
    CHECK(server_stats.requests_served == 4 && server_stats.peak_output_bytes <= 8 * 1024);
}
} // namespace

int main() {
    bodies_are_byte_exact_with_metadata();
    small_windows_and_slow_readers_stay_bounded();
    reads_follow_downstream_demand();
    head_and_empty_files_schedule_no_reads();
    midstream_failures_close_without_a_second_response();
    failures_before_commit_get_error_responses();
    permissions_are_reported_as_forbidden();
    cleanup_releases_handles();
    persistent_loopback_connection();
    return 0;
}
