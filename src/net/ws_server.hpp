// 本机 WebSocket 服务端（RFC 6455 子集）。
//
// 只绑 127.0.0.1，只为"UI ↔ 引擎"这一条控制面服务。同一端口顺带接几个只读的
// HTTP 接口：`curl` 也能拿到同一份 JSON，自动化验证不依赖浏览器。
//
// 为什么不用现成的 WebSocket 库：
//   1. vcpkg 里几个实现都拖着 TLS/asio，装不起来就编不过，而这里只需要回环
//   2. 帧格式是协议里最不容易出错的部分，自己写才能逐字节单测（见 tests/test_ws.cpp）
//   3. 非阻塞轮询要嵌进引擎已有的 120 Hz 循环，第三方库的事件循环不好塞
#pragma once

#include "net/ws_codec.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace fgb::net {

class WsServer {
public:
    using ConnId      = std::intptr_t;
    using HttpHandler = std::function<bool(const ws::HttpHead& head, std::string& body,
                                           std::string& content_type)>;

    WsServer() = default;
    ~WsServer();

    WsServer(const WsServer&)            = delete;
    WsServer& operator=(const WsServer&) = delete;

    // port == 0 时由系统分配，port() 返回真实端口
    bool listen(std::uint16_t port, std::string& error);
    void close();

    bool is_open() const noexcept { return handle_ >= 0; }
    std::uint16_t port() const noexcept { return port_; }

    // 处理一轮事件（accept / 收帧），最多阻塞 timeout_ms。返回是否发生了任何事件。
    bool poll(int timeout_ms);

    bool send_text(ConnId conn, std::string_view text);
    // 向所有已升级的连接发一条文本帧，返回成功发送的连接数
    std::size_t broadcast(std::string_view text);

    std::size_t client_count() const noexcept { return clients_.size(); }
    std::size_t ready_count() const noexcept;

    std::function<void(ConnId)> on_open;
    std::function<void(ConnId)> on_close;
    std::function<void(ConnId, std::string_view)> on_text;
    // 非升级请求（curl 诊断接口、静态页面）走这里；返回 false 表示 404
    HttpHandler on_http;

private:
    struct Client {
        ConnId conn       = -1;
        std::string in;         // 未消费的字节
        std::string fragments;  // 分片文本帧的累积
        bool upgraded     = false;
        bool dead         = false;
    };

    void accept_new();
    void read_client(Client& client);
    void process(Client& client);
    bool send_raw(Client& client, std::string_view data);
    void sweep();

    std::intptr_t handle_ = -1;
    std::uint16_t port_   = 0;
    std::vector<Client> clients_;
};

} // namespace fgb::net
