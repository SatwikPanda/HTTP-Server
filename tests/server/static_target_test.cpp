#include "static_harness.hpp"

#include <algorithm>

namespace {
using namespace server_test;
using http::Method;

struct Outcome { int status{}; std::string body; Parsed response; };

// One request through the real session, factory and file executor.
Outcome fetch(const std::shared_ptr<const http::TransactionFactory>& factory, std::string_view target,
              std::string_view method = "GET", Fields fields = {}) {
    Channel channel(request(target, method, fields, true));
    auto stats = http::run_http_session(channel, session_config(factory));
    CHECK(stats.completed_cleanly);
    auto parsed = parse_stream(channel.output, {method == "HEAD" ? Method::HEAD : Method::GET});
    CHECK(parsed.complete && parsed.responses.size() == 1);
    return {parsed.responses[0].status, parsed.responses[0].body, parsed.responses[0]};
}

void serves_files_and_maps_targets() {
    Tree tree;
    tree.write("index.html", "<h1>home</h1>");
    tree.write("docs/guide/page.html", "nested");
    tree.write("a b.txt", "space");
    tree.write("a+b.txt", "plus");
    tree.write("%41.txt", "percent literal");
    tree.write("caf\xC3\xA9.txt", "utf8");
    tree.write("sub/index.html", "not the root index");
    const auto factory = make_factory(tree.root());

    CHECK(fetch(factory, "/").body == "<h1>home</h1>");                   // "/" maps to index.html
    CHECK(fetch(factory, "/?x=1").body == "<h1>home</h1>");
    CHECK(fetch(factory, "/docs/guide/page.html").body == "nested");
    CHECK(fetch(factory, "/a%20b.txt").body == "space");
    CHECK(fetch(factory, "/a+b.txt").body == "plus");                      // '+' stays a plus
    CHECK(fetch(factory, "/a%2Bb.txt").body == "plus");
    CHECK(fetch(factory, "/a+b.txt").body != "space");
    CHECK(fetch(factory, "/%2541.txt").body == "percent literal");         // decoded exactly once
    CHECK(fetch(factory, "/%41.txt").status == 404);                       // "A.txt" does not exist
    CHECK(fetch(factory, "/caf%C3%A9.txt").body == "utf8");
    CHECK(fetch(factory, "/docs/guide/page.html?download=1&x=%zz").body == "nested"); // query ignored
    CHECK(fetch(factory, "/docs/./guide/../guide/page.html").body == "nested");
    CHECK(fetch(factory, "http://localhost/docs/guide/page.html?q=1").body == "nested"); // absolute-form
    CHECK(fetch(factory, "http://localhost").body == "<h1>home</h1>");
    CHECK(fetch(factory, "/sub/index.html").body == "not the root index");
    CHECK(fetch(factory, "/sub").status == 403);                            // directories are never served or listed
    CHECK(fetch(factory, "/sub/").status == 403);
    CHECK(fetch(factory, "/docs/guide/page.html/").status == 404);          // a file named as a directory
    CHECK(fetch(factory, "/docs/guide/page.html/extra").status == 404);     // path below a regular file
    CHECK(fetch(factory, "/missing.txt").status == 404);
    CHECK(fetch(factory, "/docs/missing/page.html").status == 404);
    CHECK(fetch(factory, "/a.txt", "HEAD").status == 404);
}

void rejects_malformed_and_hostile_targets() {
    Tree tree;
    tree.write("public/ok.txt", "ok");
    tree.write("secret.txt", "outside the document root");
    const auto sibling = std::filesystem::path(tree.root().string() + "-evil");
    std::filesystem::create_directories(sibling);
    { std::ofstream(sibling / "loot.txt") << "sibling with the same name prefix"; }
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code e; std::filesystem::remove_all(path, e); } } cleanup{sibling};
    const auto factory = make_factory(tree.root() / "public");

    CHECK(fetch(factory, "/ok.txt").body == "ok");
    for (const char* target : {"/%", "/%2", "/%zz", "/ok.txt%", "/%00", "/ok.txt%00.png", "/a%5Cb", "/a%2Fb", "/..%2fsecret.txt",
                               "/%2e%2e%2fsecret.txt", "//ok.txt", "/ok.txt#frag"}) {
        const auto outcome = fetch(factory, target);
        if (outcome.status != 400) std::cerr << target << " -> " << outcome.status << '\n';
        CHECK(outcome.status == 400);
        CHECK(outcome.body.find("secret") == std::string::npos);
    }
    const std::string sibling_name = sibling.filename().string();
    for (const std::string& target : std::vector<std::string>{"/../secret.txt", "/%2e%2e/secret.txt", "/%2E%2E/secret.txt", "/.%2e/secret.txt",
                                     "/a/../../secret.txt", "/ok.txt/../../secret.txt", "/../public/ok.txt",
                                     "/../" + sibling_name + "/loot.txt", "/%2e%2e/" + sibling_name + "/loot.txt",
                                     "http://localhost/../secret.txt",
                                     "/C:/Windows/win.ini", "/C%3A/x", "/ok.txt:stream", "/ok.txt::$DATA", "/CON", "/nul.txt",
                                     "/aux.", "/COM1.log", "/LPT9", "/ok.txt.", "/%20", "/x*y", "/x%3Fy"}) {
        const auto outcome = fetch(factory, target);
        if (outcome.status != 403) std::cerr << target << " -> " << outcome.status << '\n';
        CHECK(outcome.status == 403);
        CHECK(outcome.body.find("outside") == std::string::npos && outcome.body.find("sibling") == std::string::npos);
    }
    // Double encoding is not decoded twice: this names a file literally called "%2e%2e".
    CHECK(fetch(factory, "/%252e%252e/secret.txt").status == 404);
    // Backslashes never act as separators, raw or encoded.
    CHECK(fetch(factory, "/..\\secret.txt").status == 400);

    // Other session-level target errors stay owned by the shared session.
    Channel channel("GET /ok.txt HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
    CHECK(http::run_http_session(channel, session_config(factory)).completed_cleanly);
}

void error_responses_are_bounded_and_consistent() {
    Tree tree;
    tree.write("ok.txt", "ok");
    tree.make_directory("dir");
    const auto factory = make_factory(tree.root());
    const std::string root_text = tree.root().string();
    // Several errors and successes on one persistent, pipelined connection stay ordered.
    const std::string pipeline = request("/missing") + request("/%zz") + request("/../x") + request("/dir") +
                                 request("/ok.txt", "HEAD") + request("/ok.txt", "GET", {}, true);
    Channel channel(pipeline, 5);
    auto stats = http::run_http_session(channel, session_config(factory, 512));
    CHECK(stats.completed_cleanly && stats.requests_served == 6 && stats.peak_output_bytes <= 512);
    const auto parsed = parse_stream(channel.output, {Method::GET, Method::GET, Method::GET, Method::GET, Method::HEAD, Method::GET});
    CHECK(parsed.complete && parsed.responses.size() == 6);
    const int expected[] = {404, 400, 403, 403, 200, 200};
    for (std::size_t i = 0; i < 6; ++i) {
        CHECK(parsed.responses[i].status == expected[i]);
        CHECK(parsed.responses[i].body.find(root_text) == std::string::npos); // no local paths in bodies
        CHECK(header(parsed.responses[i], "Date").size() == 29);
    }
    CHECK(parsed.responses[0].body == "404 Not Found\r\n");
    CHECK(parsed.responses[1].body == "400 Bad Request\r\n");
    CHECK(parsed.responses[2].body == "403 Forbidden\r\n");
    CHECK(parsed.responses[5].body == "ok");

    // 405 and Allow come from the shared session; the connection then closes.
    for (const char* method : {"POST", "PUT", "DELETE", "OPTIONS"}) {
        Channel other(request("/ok.txt", method, {{"Content-Length", "0"}}));
        (void)http::run_http_session(other, session_config(factory));
        const auto reply = parse_stream(other.output, {Method::GET});
        CHECK(reply.responses.size() == 1 && reply.responses[0].status == 405);
        CHECK(header(reply.responses[0], "Allow") == "GET, HEAD");
    }
}

void root_configuration_is_validated() {
    Tree tree;
    tree.write("file.txt", "x");
    server::StaticConfig config;
    config.document_root = tree.root() / "missing";
    CHECK(!server::make_static_factory(config));
    config.document_root = tree.root() / "file.txt";
    CHECK(!server::make_static_factory(config));
    config.document_root.clear();
    CHECK(!server::make_static_factory(config));
    config.document_root = tree.root();
    config.index_file = "../x";
    CHECK(!server::make_static_factory(config));
    config.index_file = "index.html";
    config.max_chunk_bytes = 0;
    CHECK(!server::make_static_factory(config));
    config.max_chunk_bytes = 64 * 1024;
    config.work_timeout = std::chrono::milliseconds{0};
    CHECK(!server::make_static_factory(config));
    config.work_timeout = std::chrono::milliseconds{1000};
    CHECK(server::make_static_factory(config));
    // A non-canonical root spelling is canonicalized at startup.
    tree.make_directory("sub");
    tree.write("sub/in.txt", "in");
    config.document_root = tree.root() / "sub" / ".." / "sub";
    CHECK(fetch(test::take(server::make_static_factory(config)), "/in.txt").body == "in");

    // A missing index file is an ordinary 404 for "/", never a directory listing.
    tree.make_directory("empty");
    config.document_root = tree.root() / "empty";
    const auto empty = test::take(server::make_static_factory(config));
    CHECK(fetch(empty, "/").status == 404);
    // The root is resolved once at startup; later requests never use the working directory.
    tree.make_directory("docroot");
    tree.write("cwd/cwd-only.txt", "only in the working directory");
    config.document_root = tree.root() / "docroot";
    const auto rooted = test::take(server::make_static_factory(config));
    const auto previous = std::filesystem::current_path();
    std::filesystem::current_path(tree.root() / "cwd");
    CHECK(fetch(rooted, "/cwd-only.txt").status == 404);
    std::filesystem::current_path(previous);
}

void links_are_rejected() {
    Tree tree;
    const auto outside = tree.root() / "outside";
    tree.write("outside/secret.txt", "secret outside the root");
    tree.write("www/real.txt", "real");
    tree.make_directory("www/realdir");
    tree.write("www/realdir/inner.txt", "inner");
    const auto www = tree.root() / "www";
    const auto factory = make_factory(www);
    bool file_link = tree.try_symlink(outside / "secret.txt", "www/escape.txt");
    bool inside_file_link = tree.try_symlink(www / "real.txt", "www/alias.txt");
    bool directory_link = tree.try_directory_symlink(outside, "www/linkdir");
    bool inside_directory_link = tree.try_directory_symlink(www / "realdir", "www/aliasdir");
    if (!file_link && !directory_link) {
        unsupported("symbolic links cannot be created here; link/reparse rejection was not exercised");
        return;
    }
    CHECK(fetch(factory, "/real.txt").body == "real");
    if (file_link) CHECK(fetch(factory, "/escape.txt").status == 403);
    if (inside_file_link) CHECK(fetch(factory, "/alias.txt").status == 403);     // even a link that stays inside is rejected
    if (directory_link) {
        CHECK(fetch(factory, "/linkdir/secret.txt").status == 403);
        CHECK(fetch(factory, "/linkdir").status == 403);
    }
    if (inside_directory_link) CHECK(fetch(factory, "/aliasdir/inner.txt").status == 403);
    CHECK(fetch(factory, "/realdir/inner.txt").body == "inner");
}
} // namespace

int main() {
    serves_files_and_maps_targets();
    rejects_malformed_and_hostile_targets();
    error_responses_are_bounded_and_consistent();
    root_configuration_is_validated();
    links_are_rejected();
    return 0;
}
