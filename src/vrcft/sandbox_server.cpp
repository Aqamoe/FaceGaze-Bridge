#include "vrcft/sandbox_server.hpp"

namespace fgb::vrcft {

bool SandboxServer::open(std::uint16_t port) noexcept {
    if (!socket_.open_receiver(port)) {
        return false;
    }
    socket_.set_nonblocking(true);
    socket_.set_recv_buffer(1 << 20);
    port_ = socket_.local_port();
    return port_ != 0;
}

void SandboxServer::close() noexcept {
    socket_.close();
    port_  = 0;
    peer_  = {};
}

int SandboxServer::receive(std::span<std::byte> buffer, Peer& from) noexcept {
    std::string ip;
    std::uint16_t port = 0;
    const int n        = socket_.recv_from(buffer, ip, port);
    if (n > 0) {
        from.ip   = std::move(ip);
        from.port = port;
    }
    return n;
}

bool SandboxServer::send(std::span<const std::byte> data, const Peer& to) noexcept {
    if (!to.valid()) {
        return false;
    }
    return socket_.send_to(data, to.ip, to.port);
}

} // namespace fgb::vrcft
