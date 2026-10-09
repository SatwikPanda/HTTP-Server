#include "net/stream.hpp"
#include "net/socket.hpp"

namespace net {

ReadResult SocketStreamChannel::read_some(std::span<std::byte> destination) {
    return socket_.read_some(destination);
}

WriteResult SocketStreamChannel::write_some(std::span<const std::byte> source) {
    return socket_.write_some(source);
}

void SocketStreamChannel::close() noexcept {
    socket_.close();
}

bool SocketStreamChannel::is_valid() const noexcept {
    return socket_.is_valid();
}

} // namespace net
