// 沙箱服务端：FGB 扮演 VRCFT Core 的角色，在 UDP 上等模块进程连进来。
//
// 只做收发与对端记忆，不含状态机——状态机在 VrcftSource 里，
// 这样换传输方式（比如将来改共享内存）时不用重写业务逻辑。
#pragma once

#include "net/udp_socket.hpp"

#include <cstdint>
#include <span>
#include <string>

namespace fgb::vrcft {

struct Peer {
    std::string ip;
    std::uint16_t port = 0;

    bool valid() const noexcept { return port != 0 && !ip.empty(); }
};

class SandboxServer {
public:
    // port = 0 表示让系统分配（VRCFT 也是这么做的）
    bool open(std::uint16_t port = 0) noexcept;
    void close() noexcept;

    bool valid() const noexcept { return socket_.valid(); }
    std::uint16_t port() const noexcept { return port_; }

    // 返回收到的字节数；0 = 暂无数据；-1 = 错误。
    int receive(std::span<std::byte> buffer, Peer& from) noexcept;
    bool send(std::span<const std::byte> data, const Peer& to) noexcept;

    // 首个握手包的对端即本会话的唯一模块，记住它用于后续定向发送
    const Peer& peer() const noexcept { return peer_; }
    void remember_peer(const Peer& p) noexcept { peer_ = p; }
    void clear_peer() noexcept { peer_ = {}; }

private:
    net::UdpSocket socket_;
    std::uint16_t port_ = 0;
    Peer peer_{};
};

} // namespace fgb::vrcft
