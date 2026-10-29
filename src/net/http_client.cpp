#include "net/http_client.hpp"

#include <windows.h>
#include <winhttp.h>

#include <fstream>
#include <vector>

namespace fgb::net {
namespace {

struct Handle {
    HINTERNET value = nullptr;
    Handle()        = default;
    explicit Handle(HINTERNET handle) : value(handle) {}
    ~Handle() {
        if (value != nullptr) {
            ::WinHttpCloseHandle(value);
        }
    }
    Handle(const Handle&)            = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value(other.value) { other.value = nullptr; }
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            if (value != nullptr) {
                ::WinHttpCloseHandle(value);
            }
            value       = other.value;
            other.value = nullptr;
        }
        return *this;
    }
    operator HINTERNET() const { return value; }
};

std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int length = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                            static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(length), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(),
                          length);
    return out;
}

std::string last_error_text(const char* what) {
    const DWORD code = ::GetLastError();
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), "%s（WinHTTP 错误 %lu）", what,
                  static_cast<unsigned long>(code));
    return buffer;
}

// 打开请求但不发；返回的请求句柄与连接句柄由调用方持有
struct Request {
    Handle session;
    Handle connection;
    Handle request;
    std::string error;
};

bool open_request(const std::string& url, Request& out) {
    const std::wstring wide = widen(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize     = sizeof(parts);
    parts.dwSchemeLength   = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength  = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (::WinHttpCrackUrl(wide.c_str(), 0, 0, &parts) == FALSE) {
        out.error = last_error_text("URL 解析失败");
        return false;
    }

    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring target(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength > 0) {
        target.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    }

    out.session = Handle{::WinHttpOpen(L"FaceGazeBridge/0.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (out.session.value == nullptr) {
        out.error = last_error_text("HTTP 会话创建失败");
        return false;
    }
    ::WinHttpSetTimeouts(out.session, 10'000, 10'000, 30'000, 60'000);

    out.connection = Handle{::WinHttpConnect(out.session, host.c_str(), parts.nPort, 0)};
    if (out.connection.value == nullptr) {
        out.error = last_error_text("连接失败");
        return false;
    }

    const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    out.request = Handle{::WinHttpOpenRequest(out.connection, L"GET", target.c_str(), nullptr,
                                              WINHTTP_NO_REFERER,
                                              WINHTTP_DEFAULT_ACCEPT_TYPES, flags)};
    if (out.request.value == nullptr) {
        out.error = last_error_text("请求创建失败");
        return false;
    }

    if (::WinHttpSendRequest(out.request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA,
                             0, 0, 0) == FALSE) {
        out.error = last_error_text("发送请求失败");
        return false;
    }
    if (::WinHttpReceiveResponse(out.request, nullptr) == FALSE) {
        out.error = last_error_text("等待响应失败");
        return false;
    }

    DWORD status = 0;
    DWORD size   = sizeof(status);
    if (::WinHttpQueryHeaders(out.request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                              WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                              WINHTTP_NO_HEADER_INDEX) == FALSE) {
        out.error = last_error_text("读取状态码失败");
        return false;
    }
    if (status < 200 || status >= 300) {
        out.error = "服务器返回 HTTP " + std::to_string(status);
        return false;
    }

    out.error.clear();
    return true;
}

bool content_length(HINTERNET request, std::uint64_t& total) {
    DWORD length = 0;
    DWORD size   = sizeof(length);
    if (::WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                              WINHTTP_HEADER_NAME_BY_INDEX, &length, &size,
                              WINHTTP_NO_HEADER_INDEX) == FALSE) {
        total = 0;
        return false;
    }
    total = length;
    return true;
}

// 逐块读响应；chunk 返回 false 表示调用方要中止
bool read_body(HINTERNET request, std::size_t max_bytes,
               const std::function<bool(const char*, std::size_t)>& chunk, std::string& error) {
    std::vector<char> buffer(64 * 1024);
    std::uint64_t received = 0;
    for (;;) {
        DWORD available = 0;
        if (::WinHttpQueryDataAvailable(request, &available) == FALSE) {
            error = last_error_text("读取响应失败");
            return false;
        }
        if (available == 0) {
            return true;
        }
        const DWORD want =
            static_cast<DWORD>(std::min<std::size_t>(available, buffer.size()));
        DWORD read = 0;
        if (::WinHttpReadData(request, buffer.data(), want, &read) == FALSE) {
            error = last_error_text("读取数据失败");
            return false;
        }
        if (read == 0) {
            return true;
        }
        received += read;
        if (max_bytes != 0 && received > max_bytes) {
            error = "响应超过大小上限";
            return false;
        }
        if (!chunk(buffer.data(), read)) {
            error = "已取消";
            return false;
        }
    }
}

} // namespace

bool http_get(const std::string& url, std::string& body, std::string& error,
              std::size_t max_bytes) {
    body.clear();
    Request request;
    if (!open_request(url, request)) {
        error = request.error;
        return false;
    }
    const bool ok = read_body(
        request.request, max_bytes,
        [&body](const char* data, std::size_t size) {
            body.append(data, size);
            return true;
        },
        error);
    return ok;
}

bool http_download(const std::string& url, const std::filesystem::path& destination,
                   std::string& error,
                   const std::function<void(const HttpProgress&)>& progress,
                   std::size_t max_bytes) {
    std::filesystem::path part = destination;
    part += L".part";

    Request request;
    if (!open_request(url, request)) {
        error = request.error;
        return false;
    }

    HttpProgress state;
    content_length(request.request, state.total);

    std::ofstream out(part, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "无法写入下载文件";
        return false;
    }

    const bool ok = read_body(
        request.request, max_bytes,
        [&](const char* data, std::size_t size) {
            out.write(data, static_cast<std::streamsize>(size));
            state.received += size;
            if (progress) {
                progress(state);
            }
            return static_cast<bool>(out);
        },
        error);
    out.close();
    if (!ok) {
        std::error_code ec;
        std::filesystem::remove(part, ec);
        return false;
    }

    std::error_code ec;
    std::filesystem::remove(destination, ec);
    std::filesystem::rename(part, destination, ec);
    if (ec) {
        std::filesystem::remove(part, ec);
        error = "保存下载文件失败";
        return false;
    }
    return true;
}

} // namespace fgb::net
