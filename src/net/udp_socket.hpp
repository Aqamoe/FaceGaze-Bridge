// UDP socket 封装。头文件不引入 winsock2.h，避免宏污染扩散到调用方。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace fgb::net {

class UdpSocket {
public:
    UdpSocket() noexcept = default;
    ~UdpSocket();

    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&& other) noexcept;
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    // 绑定本地端口。port 为 0 时由系统分配，可用 local_port() 查询。
    bool open_receiver(std::uint16_t port) noexcept;
    bool open_sender() noexcept;
    void close() noexcept;

    bool valid() const noexcept;
    std::uint16_t local_port() const noexcept;

    bool set_nonblocking(bool enabled) noexcept;
    bool set_recv_buffer(int bytes) noexcept;
    bool set_reuse_addr(bool enabled) noexcept;
    bool set_broadcast(bool enabled) noexcept;

    // 返回收到的字节数；0 表示暂无数据；-1 表示错误。
    int recv_from(std::span<std::byte> buffer, std::string& from_ip, std::uint16_t& from_port) noexcept;

    bool send_to(std::span<const std::byte> data, const std::string& ip, std::uint16_t port) noexcept;

private:
    static constexpr std::uintptr_t kInvalidHandle = static_cast<std::uintptr_t>(~0ull);
    std::uintptr_t handle_ = kInvalidHandle;
};

} // namespace fgb::net
