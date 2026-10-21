#pragma once

#include "core/result.hpp"
#include "http/application.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>

namespace server {

struct StaticConfig {
    std::filesystem::path document_root;
    std::string index_file{"index.html"};           // served for "/"
    std::size_t max_chunk_bytes{64 * 1024};          // one file read; also capped by the output window
    std::chrono::milliseconds work_timeout{5000};    // deadline for each file job
};

// Creates the static-file service for the shared session boundary. The factory is
// immutable and safe to share across connections and drivers; each request gets an
// independent transaction. The transaction does no I/O itself: it requests file
// open/read/close through the shared file port, so a driver may run those jobs
// on a helper and never block its event loop.
//
// Startup is blocking: the document root is canonicalized and checked with the
// nonthrowing filesystem overloads, so call this before drivers start (or on a
// helper), never from an event-loop callback.
[[nodiscard]] core::Result<std::shared_ptr<const http::TransactionFactory>> make_static_factory(const StaticConfig& config);

} // namespace server
