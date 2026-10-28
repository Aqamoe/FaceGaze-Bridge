#include "net/tcp_listener.hpp"

#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using NativeSocket = SOCKET;
using NativeConn   = SOCKET;
constexpr NativeSocket kInvalid = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using NativeSocket = int;
using NativeConn   = int;
constexpr NativeSocket kInvalid = -1;
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
void close_native(NativeSocket s) noexcept {
    if (s != kInvalid) {
        ::closesocket(s);
    }
}
#else
bool ensure_wsa() noexcept { return true; }
void close_native(NativeSocket s) noexcept {
    if (s != kInvalid) {
        ::close(s);
    }
}
#endif

NativeConn to_conn(std::intptr_t handle) noexcept { return static_cast<NativeConn>(handle); }
std::intptr_t to_handle(NativeConn conn) noexcept { return static_cast<std::intptr_t>(conn); }

} // namespace

TcpListener::~TcpListener() { close(); }

bool TcpListener::listen(std::uint16_t port, std::string& error) {
    if (!ensure_wsa()) {
        error = "Winsock 初始化失败";
        return false;
    }
    close();

    const NativeSocket s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kInvalid) {
        error = "无法创建 TCP socket";
        return false;
    }

    int yes = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 只本机
    addr.sin_port        = htons(port);

    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close_native(s);
        error = "端口被占用或无权绑定";
        return false;
    }
    if (::listen(s, 4) != 0) {
        close_native(s);
        error = "listen 失败";
        return false;
    }

    handle_ = to_handle(s);
    port_   = port;
    return true;
}

void TcpListener::close() {
    if (handle_ >= 0) {
        close_native(to_conn(handle_));
        handle_ = -1;
    }
}

std::intptr_t TcpListener::accept(std::uint16_t timeout_ms) {
    if (handle_ < 0) {
        return -1;
    }
    const NativeConn listener = to_conn(handle_);

    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(listener, &read_set);

    timeval tv{};
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

#ifdef _WIN32
    const int ready = ::select(0, &read_set, nullptr, nullptr, &tv);
#else
    const int ready = ::select(listener + 1, &read_set, nullptr, nullptr, &tv);
#endif
    if (ready <= 0) {
        return -1; // 超时或出错
    }

    const NativeConn conn = ::accept(listener, nullptr, nullptr);
    if (conn == kInvalid) {
        return -1;
    }
    return to_handle(conn);
}

int TcpListener::recv_request(std::intptr_t handle, char* buffer, std::size_t cap) {
    const NativeConn conn = to_conn(handle);
    std::size_t total     = 0;
    while (total + 1 < cap) {
        const int n = ::recv(conn, buffer + total, static_cast<int>(cap - total - 1), 0);
        if (n <= 0) {
            break;
        }
        total += static_cast<std::size_t>(n);
        buffer[total] = '\0';
        // 请求头结束就够用了；带 body 的请求靠 Content-Length 判断
        if (const char* end = std::strstr(buffer, "\r\n\r\n")) {
            const std::size_t header_len = static_cast<std::size_t>(end - buffer) + 4;
            const char* cl               = std::strstr(buffer, "Content-Length:");
            if (cl == nullptr) {
                return static_cast<int>(header_len);
            }
            const long declared = std::strtol(cl + 15, nullptr, 10);
            if (declared <= 0) {
                return static_cast<int>(header_len);
            }
            if (total >= header_len + static_cast<std::size_t>(declared)) {
                return static_cast<int>(total);
            }
        }
    }
    return total == 0 ? -1 : static_cast<int>(total);
}

bool TcpListener::send_all(std::intptr_t handle, const char* data, std::size_t size) {
    const NativeConn conn = to_conn(handle);
    std::size_t sent      = 0;
    while (sent < size) {
        const int n = ::send(conn, data + sent, static_cast<int>(size - sent), 0);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

void TcpListener::close_conn(std::intptr_t handle) { close_native(to_conn(handle)); }

} // namespace fgb::net
