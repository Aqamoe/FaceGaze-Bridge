#include "net/http_client.hpp"

#include "net/proxy_config.hpp"

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
    // PAC 现算出来的代理：WinHttpSetOption 之后 session 还要用这些字符串
    std::wstring proxy;
    std::wstring bypass;
    std::string error;
};

// "自动配置脚本"没法预先翻译成 host:port，只能拿具体 URL 现算。
// 解析失败（脚本拉不到、语法错）就保持原有设置——不能因为 PAC 挂了把本来通的请求弄断。
void apply_pac(Request& request, const std::string& pac_url, const std::string& target) {
    const std::wstring pac = widen(pac_url);
    const std::wstring url = widen(target);

    WINHTTP_AUTOPROXY_OPTIONS options{};
    options.dwFlags           = WINHTTP_AUTOPROXY_CONFIG_URL;
    options.lpszAutoConfigUrl = pac.c_str();

    WINHTTP_PROXY_INFO info{};
    if (::WinHttpGetProxyForUrl(request.session, url.c_str(), &options, &info) == FALSE) {
        return;
    }

    WINHTTP_PROXY_INFO apply{};
    if (info.dwAccessType == WINHTTP_ACCESS_TYPE_NAMED_PROXY && info.lpszProxy != nullptr) {
        request.proxy.assign(info.lpszProxy);
        if (info.lpszProxyBypass != nullptr) {
            request.bypass.assign(info.lpszProxyBypass);
        }
        apply.dwAccessType    = WINHTTP_ACCESS_TYPE_NAMED_PROXY;
        apply.lpszProxy       = request.proxy.data();
        apply.lpszProxyBypass = request.bypass.empty() ? nullptr : request.bypass.data();
    } else {
        apply.dwAccessType = WINHTTP_ACCESS_TYPE_NO_PROXY; // 脚本明确要求直连
    }
    ::WinHttpSetOption(request.session, WINHTTP_OPTION_PROXY, &apply, sizeof(apply));

    if (info.lpszProxy != nullptr) {
        ::GlobalFree(info.lpszProxy);
    }
    if (info.lpszProxyBypass != nullptr) {
        ::GlobalFree(info.lpszProxyBypass);
    }
}

bool open_request(const std::string& url, Request& out, int timeout_ms = 0) {
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

    // 本机服务显式绕代理：127.0.0.1 不该经过用户配置的 HTTP 代理。
    // 公网请求则必须自己读"系统代理"——WinHTTP 的默认代理只认 netsh winhttp 那份配置，
    // Clash 等开关写的 IE 代理它看不到，症状就是"开着代理也下载失败"。
    const bool local_only = timeout_ms > 0;
    const SystemProxy sys = local_only ? SystemProxy{} : system_proxy();
    std::wstring proxy;
    std::wstring bypass;
    DWORD access_type          = WINHTTP_ACCESS_TYPE_DEFAULT_PROXY;
    const wchar_t* proxy_name  = WINHTTP_NO_PROXY_NAME;
    const wchar_t* bypass_name = WINHTTP_NO_PROXY_BYPASS;
    if (local_only) {
        access_type = WINHTTP_ACCESS_TYPE_NO_PROXY;
    } else if (!sys.proxy.empty()) {
        proxy       = widen(sys.proxy);
        bypass      = widen(sys.bypass);
        access_type = WINHTTP_ACCESS_TYPE_NAMED_PROXY;
        proxy_name  = proxy.c_str();
        if (!bypass.empty()) {
            bypass_name = bypass.c_str();
        }
    }
    out.session = Handle{::WinHttpOpen(L"FaceGazeBridge/0.1", access_type, proxy_name, bypass_name,
                                       0)};
    if (out.session.value == nullptr) {
        out.error = last_error_text("HTTP 会话创建失败");
        return false;
    }
    if (!local_only && !sys.pac_url.empty()) {
        apply_pac(out, sys.pac_url, url);
    }
    if (timeout_ms > 0) {
        ::WinHttpSetTimeouts(out.session, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
    } else {
        // 模块库只是十几 KB 的 JSON：连接/接收超时收紧，坏网络尽快失败而不是卡半分钟
        ::WinHttpSetTimeouts(out.session, 4'000, 5'000, 10'000, 15'000);
    }

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

    // GitHub API 不接受没有 User-Agent 的请求；registry.vrcft.io 也无害
    const wchar_t headers[] =
        L"User-Agent: FaceGazeBridge/1.0\r\nAccept: application/json\r\n";
    ::WinHttpAddRequestHeaders(out.request, headers, static_cast<DWORD>(-1),
                               WINHTTP_ADDREQ_FLAG_ADD);

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
              std::size_t max_bytes, int timeout_ms) {
    body.clear();
    Request request;
    if (!open_request(url, request, timeout_ms)) {
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
                   std::size_t max_bytes, const std::atomic<bool>* cancel) {
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
            if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
                return false; // 上层要退出：别把 100 MB 下完才放人
            }
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
        if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
            error = "已取消下载";
        }
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
