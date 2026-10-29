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
#include <fcntl.h>
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
bool set_nonblocking(NativeSocket s) noexcept {
    u_long mode = 1;
    return ::ioctlsocket(s, FIONBIO, &mode) == 0;
}
#else
bool ensure_wsa() noexcept { return true; }
void close_socket(NativeSocket s) noexcept {
    if (s != kInvalidSocket) {
        ::close(s);
    }
}
bool would_block() noexcept { return errno == EAGAIN || errno == EWOULDBLOCK; }
bool set_nonblocking(NativeSocket s) noexcept {
    const int flags = ::fcntl(s, F_GETFL, 0);
    return flags >= 0 && ::fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
}
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
    // 监听 socket 也必须非阻塞：select 说可读只保证至少一个连接，
    // 多收一次就会在没有连接时永久阻塞引擎主循环。
    if (!set_nonblocking(s)) {
        close_socket(s);
        error = "无法把监听 socket 设为非阻塞";
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
    // 一次 poll 最多收一个连接：回环上 UI 只有一个客户端，
    // 多收会挤占引擎这一帧的时间预算
    const NativeSocket conn = ::accept(listener, nullptr, nullptr);
    if (conn == kInvalidSocket) {
        return;
    }

    // 小包为主，关掉 Nagle 让 UI 的即时反馈不要被延迟合并
    int yes = 1;
    ::setsockopt(conn, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof(yes));
    // 非阻塞 + 出站缓冲：对端（暂停的页面、被杀掉的 curl）不能卡住 120 Hz 主循环
    if (!set_nonblocking(conn)) {
        close_socket(conn);
        return;
    }

    Client client;
    client.conn = static_cast<std::intptr_t>(conn);
    clients_.push_back(std::move(client));
}

bool WsServer::send_raw(Client& client, std::string_view data) {
    if (client.dead) {
        return false;
    }
    client.out.append(data.data(), data.size());
    const std::size_t pending = client.out.size() - client.out_sent;
    // 页面暂停/被杀后可能永远不再读；给一个上限，别让出站缓冲无限增长
    const std::size_t limit = client.upgraded ? 512u * 1024u : 4u * 1024u * 1024u;
    if (pending > limit) {
        client.dead = true;
        return false;
    }
    return flush(client);
}

bool WsServer::flush(Client& client) {
    if (client.dead) {
        return false;
    }
    const NativeSocket sock = native(client.conn);
    while (client.out_sent < client.out.size()) {
        const auto remaining = client.out.size() - client.out_sent;
        const int n = ::send(sock, client.out.data() + client.out_sent,
                             static_cast<int>(remaining), 0);
        if (n > 0) {
            client.out_sent += static_cast<std::size_t>(n);
            continue;
        }
        if (would_block()) {
            return true;
        }
        client.dead = true;
        return false;
    }
    client.out.clear();
    client.out_sent = 0;
    if (client.close_after_send) {
        client.dead = true;
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
                    client.close_after_send = true;
                    send_raw(client, ws::http_response(400, "application/json", body));
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
            // 诊断接口不保持连接，但响应可能还没发完：发完再关，别截断大文件
            client.close_after_send = true;
            send_raw(client, ws::http_response(handled ? 200 : 404, content_type, body));
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
            client.close_after_send = true;
            send_raw(client, closing);
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
    fd_set write_set;
    FD_ZERO(&read_set);
    FD_ZERO(&write_set);
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
        if (client.out_sent < client.out.size()) {
            FD_SET(sock, &write_set);
        }
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
    const int ready = ::select(0, &read_set, &write_set, nullptr, &tv);
#else
    const int ready = ::select(max_fd + 1, &read_set, &write_set, nullptr, &tv);
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
        if (FD_ISSET(sock, &write_set)) {
            activity = true;
            flush(client);
            if (client.dead) {
                continue;
            }
        }
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
