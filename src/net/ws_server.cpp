#include "net/ws_server.hpp"

#include <algorithm>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;
#endif

namespace fgb::net {
namespace {

#ifdef _WIN32
bool ensure_wsa() noexcept {
    static const bool ok = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ok;
}
void close_socket(NativeSocket s) noexcept {
    if (s != kInvalidSocket) {
        ::closesocket(s);
    }
}
bool would_block() noexcept { return WSAGetLastError() == WSAEWOULDBLOCK; }
#else
bool ensure_wsa() noexcept { return true; }
void close_socket(NativeSocket s) noexcept {
    if (s != kInvalidSocket) {
        ::close(s);
    }
}
bool would_block() noexcept { return errno == EAGAIN || errno == EWOULDBLOCK; }
#endif

NativeSocket native(std::intptr_t handle) noexcept { return static_cast<NativeSocket>(handle); }

} // namespace

WsServer::~WsServer() { close(); }

bool WsServer::listen(std::uint16_t port, std::string& error) {
    if (!ensure_wsa()) {
        error = "Winsock 初始化失败";
        return false;
    }
    close();

    const NativeSocket s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kInvalidSocket) {
        error = "无法创建 TCP socket";
        return false;
    }

    int yes = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 只本机：控制面不该暴露给局域网
    addr.sin_port        = htons(port);

    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close_socket(s);
        error = "端口被占用或无权绑定";
        return false;
    }
    if (::listen(s, 8) != 0) {
        close_socket(s);
        error = "listen 失败";
        return false;
    }

    sockaddr_in bound{};
#ifdef _WIN32
    int bound_len = static_cast<int>(sizeof(bound));
#else
    socklen_t bound_len = sizeof(bound);
#endif
    if (::getsockname(s, reinterpret_cast<sockaddr*>(&bound), &bound_len) == 0) {
        port_ = ntohs(bound.sin_port);
    } else {
        port_ = port;
    }

    handle_ = static_cast<std::intptr_t>(s);
    return true;
}

void WsServer::close() {
    for (Client& client : clients_) {
        close_socket(native(client.conn));
    }
    clients_.clear();
    if (handle_ >= 0) {
        close_socket(native(handle_));
        handle_ = -1;
    }
}

std::size_t WsServer::ready_count() const noexcept {
    std::size_t count = 0;
    for (const Client& client : clients_) {
        if (client.upgraded) {
            ++count;
        }
    }
    return count;
}

void WsServer::accept_new() {
    const NativeSocket listener = native(handle_);
    for (;;) {
        const NativeSocket conn = ::accept(listener, nullptr, nullptr);
        if (conn == kInvalidSocket) {
            return;
        }

        // 小包为主，关掉 Nagle 让 UI 的即时反馈不要被延迟合并
        int yes = 1;
        ::setsockopt(conn, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof(yes));

        Client client;
        client.conn = static_cast<std::intptr_t>(conn);
        clients_.push_back(std::move(client));

        // 一次 poll 最多收一个连接：回环上 UI 只有一个客户端，
        // 多收会挤占引擎这一帧的时间预算
        return;
    }
}

bool WsServer::send_raw(Client& client, std::string_view data) {
    const NativeSocket sock = native(client.conn);
    std::size_t sent        = 0;
    while (sent < data.size()) {
        const int n = ::send(sock, data.data() + sent, static_cast<int>(data.size() - sent), 0);
        if (n <= 0) {
            client.dead = true;
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

bool WsServer::send_text(ConnId conn, std::string_view text) {
    for (Client& client : clients_) {
        if (client.conn == conn && client.upgraded && !client.dead) {
            std::string frame;
            ws::append_frame(frame, ws::kOpText, text);
            return send_raw(client, frame);
        }
    }
    return false;
}

std::size_t WsServer::broadcast(std::string_view text) {
    if (ready_count() == 0) {
        return 0;
    }
    std::string frame;
    frame.reserve(text.size() + 8);
    ws::append_frame(frame, ws::kOpText, text);

    std::size_t delivered = 0;
    for (Client& client : clients_) {
        if (!client.upgraded || client.dead) {
            continue;
        }
        if (send_raw(client, frame)) {
            ++delivered;
        }
    }
    return delivered;
}

void WsServer::read_client(Client& client) {
    char buffer[4096];
    const int n = ::recv(native(client.conn), buffer, sizeof(buffer), 0);
    if (n > 0) {
        client.in.append(buffer, static_cast<std::size_t>(n));
        if (client.in.size() > 4u * ws::kMaxPayload) {
            client.dead = true; // 对面在灌垃圾
            return;
        }
        process(client);
        return;
    }
    if (n == 0) {
        client.dead = true;
        return;
    }
    if (!would_block()) {
        client.dead = true;
    }
}

void WsServer::process(Client& client) {
    for (;;) {
        if (client.dead) {
            return;
        }

        if (!client.upgraded) {
            ws::HttpHead head;
            const ws::HttpParse parsed = ws::parse_http_head(client.in, head);
            if (parsed == ws::HttpParse::incomplete) {
                return;
            }
            if (parsed == ws::HttpParse::error) {
                client.dead = true;
                return;
            }

            if (head.upgrade == "websocket") {
                if (head.ws_key.empty()) {
                    const std::string body = R"({"ok":false,"error":"missing Sec-WebSocket-Key"})";
                    send_raw(client, ws::http_response(400, "application/json", body));
                    client.dead = true;
                    return;
                }
                std::string response = "HTTP/1.1 101 Switching Protocols\r\n"
                                       "Upgrade: websocket\r\n"
                                       "Connection: Upgrade\r\n"
                                       "Sec-WebSocket-Accept: ";
                response.append(ws::accept_key(head.ws_key));
                response.append("\r\n\r\n");
                if (!send_raw(client, response)) {
                    return;
                }
                client.in.erase(0, head.head_bytes);
                client.upgraded = true;
                if (on_open) {
                    on_open(client.conn);
                }
                continue;
            }

            std::string body;
            std::string content_type = "application/json";
            const bool handled       = on_http ? on_http(head, body, content_type) : false;
            if (!handled) {
                body = R"({"ok":false,"error":"not found"})";
            }
            send_raw(client, ws::http_response(handled ? 200 : 404, content_type, body));
            client.dead = true; // 诊断接口不保持连接，一个请求一个连接
            return;
        }

        ws::Frame frame;
        std::string error;
        const ws::ParseResult parsed = ws::parse_frame(client.in, frame, error);
        if (parsed == ws::ParseResult::incomplete) {
            return;
        }
        if (parsed == ws::ParseResult::error) {
            client.dead = true;
            return;
        }
        client.in.erase(0, frame.consumed);

        if (!frame.masked) {
            client.dead = true; // RFC 6455：客户端发来的帧必须掩码
            return;
        }

        switch (frame.opcode) {
        case ws::kOpPing: {
            std::string pong;
            pong.reserve(frame.payload.size() + 2);
            ws::append_frame(pong, ws::kOpPong, frame.payload);
            send_raw(client, pong);
            break;
        }
        case ws::kOpPong:
            break;
        case ws::kOpClose: {
            std::string closing;
            ws::append_frame(closing, ws::kOpClose, frame.payload);
            send_raw(client, closing);
            client.dead = true;
            return;
        }
        case ws::kOpContinuation:
        case ws::kOpText: {
            if (frame.opcode == ws::kOpText && !client.fragments.empty()) {
                client.dead = true; // 上一段文本还没收完就又开新消息
                return;
            }
            client.fragments.append(frame.payload);
            if (client.fragments.size() > ws::kMaxPayload) {
                client.dead = true;
                return;
            }
            if (frame.fin) {
                std::string message;
                message.swap(client.fragments);
                if (on_text) {
                    on_text(client.conn, message);
                }
            }
            break;
        }
        default:
            client.dead = true; // 二进制帧不用
            return;
        }
    }
}

void WsServer::sweep() {
    for (auto it = clients_.begin(); it != clients_.end();) {
        if (it->dead) {
            close_socket(native(it->conn));
            const ConnId conn = it->conn;
            const bool upgraded = it->upgraded;
            it                  = clients_.erase(it);
            // 只有真正升级成功的连接才算"客户端断开"；
            // HTTP 诊断请求本来就是一个请求一个连接，不该触发这个回调
            if (upgraded && on_close) {
                on_close(conn);
            }
        } else {
            ++it;
        }
    }
}

bool WsServer::poll(int timeout_ms) {
    if (handle_ < 0) {
        return false;
    }

    fd_set read_set;
    FD_ZERO(&read_set);
    const NativeSocket listener = native(handle_);
    FD_SET(listener, &read_set);

#ifndef _WIN32
    int max_fd = static_cast<int>(listener);
#endif
    for (const Client& client : clients_) {
        if (client.dead) {
            continue;
        }
        const NativeSocket sock = native(client.conn);
        FD_SET(sock, &read_set);
#ifdef _WIN32
        // Winsock 忽略 select 的第一个参数
#else
        max_fd = std::max(max_fd, static_cast<int>(sock));
#endif
    }

    timeval tv{};
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

#ifdef _WIN32
    const int ready = ::select(0, &read_set, nullptr, nullptr, &tv);
#else
    const int ready = ::select(max_fd + 1, &read_set, nullptr, nullptr, &tv);
#endif
    if (ready < 0) {
        return false;
    }
    if (ready == 0) {
        return false;
    }

    bool activity = false;
    if (FD_ISSET(listener, &read_set)) {
        activity = true;
        // 新连接本轮不进 fd_set，自然不会被 recv，下一次 poll 再读
        accept_new();
    }

    for (std::size_t i = 0; i < clients_.size(); ++i) {
        Client& client = clients_[i];
        if (client.dead) {
            continue;
        }
        const NativeSocket sock = native(client.conn);
        if (!FD_ISSET(sock, &read_set)) {
            continue;
        }
        activity = true;
        read_client(client);
    }

    sweep();
    return activity;
}

} // namespace fgb::net
