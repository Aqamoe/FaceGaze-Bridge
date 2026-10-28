// 极简 TCP 监听（只为本地控制面板服务）。
//
// 不引 HTTP 库：面板只有一台本机客户端、一把请求，用不着 keep-alive、分块传输、
// TLS 这些东西。用到的部分是全平台都有的 BSD socket 语义，Windows 上是 Winsock。
//
// 阻塞式 accept 用 select 加超时，这样 stop() 能及时生效，不需要额外的事件通知。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace fgb::net {

class TcpListener {
public:
    ~TcpListener();

    TcpListener() = default;
    TcpListener(const TcpListener&)            = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    // 只绑 127.0.0.1：面板不该被局域网访问
    bool listen(std::uint16_t port, std::string& error);
    void close();

    // 等待一个连接，超时返回 -1。成功返回连接句柄（用 recv/send/exchange 操作）。
    std::intptr_t accept(std::uint16_t timeout_ms);

    // 读完请求（直到 \r\n\r\n 或读满 cap），返回长度；-1 表示失败
    static int recv_request(std::intptr_t conn, char* buffer, std::size_t cap);

    // 发出全部数据，返回是否成功
    static bool send_all(std::intptr_t conn, const char* data, std::size_t size);

    static void close_conn(std::intptr_t conn);

    bool is_open() const noexcept { return handle_ >= 0; }
    std::uint16_t port() const noexcept { return port_; }

private:
    std::intptr_t handle_ = -1;
    std::uint16_t port_   = 0;
};

} // namespace fgb::net
