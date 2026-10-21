#include "tree_fixture.hpp"

#include "server/path_policy.hpp"

#include <string>
#include <vector>

namespace {
using server::TargetVerdict;

void expect(const server::PathPolicy& policy, std::string_view target, TargetVerdict verdict) {
    const auto result = policy.resolve(target);
    if (result.verdict != verdict) {
        std::cerr << "target [" << target << "] reason: " << result.reason << '\n';
    }
    CHECK(result.verdict == verdict);
}

void expect_segments(const server::PathPolicy& policy, std::string_view target, std::vector<std::string> segments,
                     bool trailing_slash = false) {
    const auto result = policy.resolve(target);
    if (result.verdict != TargetVerdict::ok) std::cerr << "target [" << target << "] reason: " << result.reason << '\n';
    CHECK(result.verdict == TargetVerdict::ok);
    CHECK(result.segments == segments);
    CHECK(result.trailing_slash == trailing_slash);
    auto expected = policy.root();
    for (const auto& segment : segments) expected /= std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(segment.data()), segment.size()));
    CHECK(result.path == expected);
}

void root_configuration() {
    server_test::Tree tree;
    tree.write("file.txt", "x");
    CHECK(server::PathPolicy::create(tree.root()));
    CHECK(!server::PathPolicy::create(tree.root() / "missing"));
    CHECK(!server::PathPolicy::create(tree.root() / "file.txt")); // a file, not a directory
    CHECK(!server::PathPolicy::create(std::filesystem::path{}));
    for (const char* bad : {"", ".", "..", "a/b", "a\\b", "CON", "index.", "a:b"}) {
        CHECK(!server::PathPolicy::create(tree.root(), bad));
    }
    auto policy = test::take(server::PathPolicy::create(tree.root() / ".", "home.html"));
    CHECK(policy.root() == tree.root()); // canonicalized
    expect_segments(policy, "/", {"home.html"});
}

void accepted_targets() {
    server_test::Tree tree;
    auto policy = test::take(server::PathPolicy::create(tree.root()));
    expect_segments(policy, "/", {"index.html"});
    expect_segments(policy, "/a.txt", {"a.txt"});
    expect_segments(policy, "/dir/nested/b.bin", {"dir", "nested", "b.bin"});
    expect_segments(policy, "/a%20b.txt", {"a b.txt"});
    expect_segments(policy, "/a+b", {"a+b"});                  // plus is a literal plus
    expect_segments(policy, "/%C3%A9.txt", {"\xC3\xA9.txt"});  // valid UTF-8 decodes
    expect_segments(policy, "/%41%42", {"AB"});
    expect_segments(policy, "/.hidden", {".hidden"});
    expect_segments(policy, "/a..b", {"a..b"});
    expect_segments(policy, "/..a", {"..a"});
    expect_segments(policy, "/console", {"console"});          // not a device alias
    expect_segments(policy, "/COM10", {"COM10"});
    expect_segments(policy, "/nulled.txt", {"nulled.txt"});
    expect_segments(policy, "/" + std::string(255, 'n'), {std::string(255, 'n')});

    // The query is split before decoding and never affects lookup.
    expect_segments(policy, "/a.txt?x=1", {"a.txt"});
    expect_segments(policy, "/a.txt?bad=%zz&slash=/../..", {"a.txt"});
    expect_segments(policy, "/?q=1", {"index.html"});

    // Lexical normalization that stays inside the root.
    expect_segments(policy, "/a/../b", {"b"});
    expect_segments(policy, "/a/./b", {"a", "b"});
    expect_segments(policy, "/./", {"index.html"});
    expect_segments(policy, "/a/..", {"index.html"});
    expect_segments(policy, "/%2e/a", {"a"});
    expect_segments(policy, "/dir/", {"dir"}, true);
    expect_segments(policy, "/dir/.", {"dir"}, true);

    // Decoding happens once: double encoding stays literal text in a file name.
    expect_segments(policy, "/%252e%252e/x", {"%2e%2e", "x"});
    expect_segments(policy, "/%2541", {"%41"});

    // Absolute-form: only path and query are used, never the whole URL.
    expect_segments(policy, "http://localhost/a/b?q=1", {"a", "b"});
    expect_segments(policy, "http://localhost", {"index.html"});
    expect_segments(policy, "http://localhost:8080/a%20b", {"a b"});
    expect_segments(policy, "HTTP://LocalHost/x", {"x"});
}

void invalid_targets() {
    server_test::Tree tree;
    auto policy = test::take(server::PathPolicy::create(tree.root()));
    for (const std::string_view target : {
             "", "*", "a/b", "ftp://host/a", "http://user@host/a", "http://host:0/a", "http://host/a#f",
             "/a#frag", "/%", "/%2", "/%zz", "/a%", "/a%g1", "/%0", "/%00", "/a%00b", "/a%5Cb", "/a%5cb", "/a\\b",
             "/a%2Fb", "/..%2f", "/%2f", "/%2F%2F", "/a b", "/a\tb", "/\x01", "/\x7f", "/a%0Ab", "/a%0D", "/a%7F", "/%80",
             "/%C3%28", "/%C0%AF", "/%ED%A0%80", "/%F5%80%80%80", "//a", "/a//b", "//", "///", "/a/%2F/b",
             "/\xC3\xA9"}) {
        expect(policy, target, TargetVerdict::invalid);
    }
    expect(policy, "/" + std::string(256, 'n'), TargetVerdict::invalid);
}

void forbidden_targets() {
    server_test::Tree tree;
    auto policy = test::take(server::PathPolicy::create(tree.root()));
    for (const std::string_view target : {
             "/..", "/../x", "/../../x", "/a/../../x", "/a/b/../../../x", "/%2e%2e/x", "/%2E%2E/x", "/.%2e/x",
             "/%2e./x", "/a/%2e%2e/%2e%2e/etc/passwd", "/../", "/./../x", "http://localhost/../x"}) {
        expect(policy, target, TargetVerdict::forbidden);
    }
    for (const std::string_view target : {
             "/C:", "/a/C:/b", "/C%3A/x", "/c:windows", "/a:stream", "/file.txt::$DATA", "/a%3Ab",
             "/CON", "/con", "/Con.txt", "/NUL.", "/nul.tar.gz", "/PRN%20.txt", "/AUX", "/aux.", "/COM1", "/com9.txt",
             "/COM0", "/LPT1", "/lpt9.log", "/a/LPT5", "/CONIN$", "/CONOUT$", "/COM%C2%B9", "/lpt%C2%B2.txt",
             "/a.", "/%20", "/a%20", "/a...", "/a/b./c", "/x<y", "/x>y", "/x%22y", "/x|y", "/x*y", "/x%3Fy",
             "/CON/../x"}) {
        expect(policy, target, TargetVerdict::forbidden);
    }
}

void containment_is_componentwise() {
    server_test::Tree tree;
    auto policy = test::take(server::PathPolicy::create(tree.root()));
    CHECK(policy.contains(tree.root() / "a"));
    CHECK(policy.contains(tree.root() / "a" / "b" / "c.txt"));
    CHECK(!policy.contains(tree.root()));                       // the root itself is not a file
    CHECK(!policy.contains(tree.root().parent_path()));
    CHECK(!policy.contains(tree.root().parent_path() / "other" / "x"));
    // A sibling whose name merely starts with the root's name is outside.
    const std::filesystem::path sibling = tree.root().string() + "-evil";
    CHECK(!policy.contains(sibling / "x"));
    CHECK(!policy.contains(sibling));
    CHECK(!policy.contains(std::filesystem::path{}));
    CHECK(!policy.contains("relative/path"));
}

void device_names() {
    for (const char* name : {"CON", "con", "PrN", "AUX", "NUL", "COM0", "COM1", "com9", "LPT0", "lpt9",
                             "CON.txt", "con.tar.gz", "NUL.", "AUX  ", "CONIN$", "conout$", "COM\xC2\xB9", "LPT\xC2\xB3.txt"}) {
        CHECK(server::is_reserved_device_name(name));
    }
    for (const char* name : {"", "CONSOLE", "COM", "COM10", "LPT", "COMA", "NULL", "AUXILIARY", "xCON", "CO", "a.CON", "COM\xC2\xB4"}) {
        CHECK(!server::is_reserved_device_name(name));
    }
}
} // namespace

int main() {
    root_configuration();
    accepted_targets();
    invalid_targets();
    forbidden_targets();
    containment_is_componentwise();
    device_names();
    return 0;
}
