#pragma once

#include "net/io_types.hpp"

#include <cstddef>
#include <span>

namespace net {

class Socket;

class StreamChannel {
public:
    virtual ~StreamChannel() = default;

    [[nodiscard]] virtual ReadResult read_some(std::span<std::byte> destination) = 0;
    [[nodiscard]] virtual WriteResult write_some(std::span<const std::byte> source) = 0;
    virtual void close() noexcept = 0;
    [[nodiscard]] virtual bool is_valid() const noexcept = 0;
};

class SocketStreamChannel : public StreamChannel {
public:
    explicit SocketStreamChannel(Socket& socket) noexcept
        : socket_(socket) {}

    [[nodiscard]] ReadResult read_some(std::span<std::byte> destination) override;
    [[nodiscard]] WriteResult write_some(std::span<const std::byte> source) override;
    void close() noexcept override;
    [[nodiscard]] bool is_valid() const noexcept override;

private:
    Socket& socket_;
};

} // namespace net
