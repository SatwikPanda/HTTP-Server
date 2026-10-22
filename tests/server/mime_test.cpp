#include "test_support.hpp"

#include "server/mime.hpp"

int main() {
    using server::mime_type_for;
    CHECK(mime_type_for("index.html") == "text/html; charset=utf-8");
    CHECK(mime_type_for("INDEX.HTML") == "text/html; charset=utf-8"); // ASCII case folding
    CHECK(mime_type_for("a.b.css") == "text/css; charset=utf-8");      // last extension wins
    CHECK(mime_type_for("app.js") == "text/javascript; charset=utf-8");
    CHECK(mime_type_for("logo.PnG") == "image/png");
    CHECK(mime_type_for("photo.jpeg") == "image/jpeg" && mime_type_for("photo.jpg") == "image/jpeg");
    CHECK(mime_type_for("data.json") == "application/json");
    CHECK(mime_type_for("font.woff2") == "font/woff2");
    CHECK(mime_type_for("module.wasm") == "application/wasm");
    // Binary fallback: unknown, missing, empty, trailing-dot and look-alike extensions.
    for (const char* name : {"", "README", "archive.unknownext", "trailing.", ".", "..", "x.htmlx", "x.ht", "noext/dir"}) {
        CHECK(mime_type_for(name) == server::binary_mime_type);
    }
    // A hidden file named like an extension is still looked up by its extension.
    CHECK(mime_type_for(".html") == "text/html; charset=utf-8");
    // No locale-sensitive folding: non-ASCII bytes never match an extension.
    CHECK(mime_type_for("x.\xC4\xB0html") == server::binary_mime_type);
    return 0;
}
