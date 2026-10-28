#include "net/udp_socket.hpp"

#include <cerrno>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace fgb::net {
namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;

struct WsaGuard {
    bool ok = false;
    WsaGuard() noexcept {
        WSADATA data{};
        ok = (WSAStartup(MAKEWORD(2, 2), &data) == 0);
    }
    ~WsaGuard() {
        if (ok) {
            WSACleanup();
        }
    }
};

bool ensure_wsa() noexcept {
    static WsaGuard guard;
    return guard.ok;
}

void close_native(NativeSocket s) noexcept {
    if (s != INVALID_SOCKET) {
        closesocket(s);
    }
}

int last_error() noexcept { return WSAGetLastError(); }
bool would_block(int e) noexcept { return e == WSAEWOULDBLOCK; }

#else
using NativeSocket = int;

bool ensure_wsa() noexcept { return true; }

void close_native(NativeSocket s) noexcept {
    if (s >= 0) {
        ::close(s);
    }
}

int last_error() noexcept { return errno; }
bool would_block(int e) noexcept { return e == EWOULDBLOCK || e == EAGAIN; }
#endif

NativeSocket from_handle(std::uintptr_t h) noexcept { return static_cast<NativeSocket>(h); }
std::uintptr_t to_handle(NativeSocket s) noexcept { return static_cast<std::uintptr_t>(s); }

bool is_bad(NativeSocket s) noexcept {
#ifdef _WIN32
    return s == INVALID_SOCKET;
#else
    return s < 0;
#endif
}

} // namespace

UdpSocket::~UdpSocket() { close(); }

UdpSocket::UdpSocket(UdpSocket&& other) noexcept : handle_(other.handle_) { other.handle_ = kInvalidHandle; }

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept {
    if (this != &other) {
        close();
        handle_       = other.handle_;
        other.handle_ = kInvalidHandle;
    }
    return *this;
}

void UdpSocket::close() noexcept {
    if (handle_ != kInvalidHandle) {
        close_native(from_handle(handle_));
        handle_ = kInvalidHandle;
    }
}

bool UdpSocket::valid() const noexcept { return handle_ != kInvalidHandle; }

bool UdpSocket::open_receiver(std::uint16_t port) noexcept {
    if (!ensure_wsa()) {
        return false;
    }
    close();

    const NativeSocket s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (is_bad(s)) {
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);

    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close_native(s);
        return false;
    }

    handle_ = to_handle(s);
    return true;
}

bool UdpSocket::open_sender() noexcept {
    if (!ensure_wsa()) {
        return false;
    }
    close();

    const NativeSocket s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (is_bad(s)) {
        return false;
    }
    handle_ = to_handle(s);
    return true;
}

std::uint16_t UdpSocket::local_port() const noexcept {
    if (!valid()) {
        return 0;
    }
    sockaddr_in addr{};
#ifdef _WIN32
    int len = sizeof(addr);
#else
    socklen_t len = sizeof(addr);
#endif
    if (::getsockname(from_handle(handle_), reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        return 0;
    }
    return ntohs(addr.sin_port);
}

bool UdpSocket::set_nonblocking(bool enabled) noexcept {
    if (!valid()) {
        return false;
    }
#ifdef _WIN32
    u_long mode = enabled ? 1UL : 0UL;
    return ioctlsocket(from_handle(handle_), FIONBIO, &mode) == 0;
#else
    const int flags = fcntl(from_handle(handle_), F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    const int next = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(from_handle(handle_), F_SETFL, next) == 0;
#endif
}

bool UdpSocket::set_recv_buffer(int bytes) noexcept {
    if (!valid()) {
        return false;
    }
    return ::setsockopt(from_handle(handle_), SOL_SOCKET, SO_RCVBUF,
                        reinterpret_cast<const char*>(&bytes), sizeof(bytes)) == 0;
}

bool UdpSocket::set_reuse_addr(bool enabled) noexcept {
    if (!valid()) {
        return false;
    }
    const int value = enabled ? 1 : 0;
    return ::setsockopt(from_handle(handle_), SOL_SOCKET, SO_REUSEADDR,
                        reinterpret_cast<const char*>(&value), sizeof(value)) == 0;
}

bool UdpSocket::set_broadcast(bool enabled) noexcept {
    if (!valid()) {
        return false;
    }
    const int value = enabled ? 1 : 0;
    return ::setsockopt(from_handle(handle_), SOL_SOCKET, SO_BROADCAST,
                        reinterpret_cast<const char*>(&value), sizeof(value)) == 0;
}

int UdpSocket::recv_from(std::span<std::byte> buffer, std::string& from_ip,
                         std::uint16_t& from_port) noexcept {
    if (!valid() || buffer.empty()) {
        return -1;
    }

    sockaddr_in addr{};
#ifdef _WIN32
    int len = sizeof(addr);
#else
    socklen_t len = sizeof(addr);
#endif

    const int n = static_cast<int>(::recvfrom(from_handle(handle_), reinterpret_cast<char*>(buffer.data()),
                                              static_cast<int>(buffer.size()), 0,
                                              reinterpret_cast<sockaddr*>(&addr), &len));
    if (n < 0) {
        if (would_block(last_error())) {
            return 0;
        }
        return -1;
    }

    char text[INET_ADDRSTRLEN] = {};
    if (::inet_ntop(AF_INET, &addr.sin_addr, text, sizeof(text)) != nullptr) {
        from_ip = text;
    } else {
        from_ip.clear();
    }
    from_port = ntohs(addr.sin_port);
    return n;
}

bool UdpSocket::send_to(std::span<const std::byte> data, const std::string& ip, std::uint16_t port) noexcept {
    if (!valid() || data.empty()) {
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (::inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        return false;
    }

    const int sent = static_cast<int>(::sendto(from_handle(handle_), reinterpret_cast<const char*>(data.data()),
                                              static_cast<int>(data.size()), 0,
                                              reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)));
    return sent == static_cast<int>(data.size());
}

} // namespace fgb::net
