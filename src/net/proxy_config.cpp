#include "net/proxy_config.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cwchar>

namespace fgb::net {
namespace {

constexpr wchar_t kInternetSettings[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings";

std::string narrow(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int length = ::WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                             static_cast<int>(text.size()), nullptr, 0, nullptr,
                                             nullptr);
    if (length <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(length), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(),
                          length, nullptr, nullptr);
    return out;
}

// 一次打开读多个值：代理相关有 4 个键，每个都开关一次没必要
class RegKey {
public:
    RegKey(HKEY root, const wchar_t* path) {
        if (::RegOpenKeyExW(root, path, 0, KEY_QUERY_VALUE, &key_) != ERROR_SUCCESS) {
            key_ = nullptr;
        }
    }
    ~RegKey() {
        if (key_ != nullptr) {
            ::RegCloseKey(key_);
        }
    }
    RegKey(const RegKey&)            = delete;
    RegKey& operator=(const RegKey&) = delete;

    std::wstring string(const wchar_t* name) const {
        if (key_ == nullptr) {
            return {};
        }
        DWORD type  = 0;
        DWORD bytes = 0;
        if (::RegQueryValueExW(key_, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
            (type != REG_SZ && type != REG_EXPAND_SZ) || bytes <= sizeof(wchar_t)) {
            return {};
        }
        std::wstring value(bytes / sizeof(wchar_t), L'\0');
        if (::RegQueryValueExW(key_, name, nullptr, &type, reinterpret_cast<LPBYTE>(value.data()),
                               &bytes) != ERROR_SUCCESS) {
            return {};
        }
        value.resize(std::wcslen(value.c_str()));
        return value;
    }

    DWORD dword(const wchar_t* name, DWORD fallback) const {
        if (key_ == nullptr) {
            return fallback;
        }
        DWORD type  = 0;
        DWORD value = fallback;
        DWORD bytes = sizeof(value);
        if (::RegQueryValueExW(key_, name, nullptr, &type, reinterpret_cast<LPBYTE>(&value),
                               &bytes) != ERROR_SUCCESS ||
            type != REG_DWORD) {
            return fallback;
        }
        return value;
    }

private:
    HKEY key_ = nullptr;
};

std::string trim(const std::string& text) {
    const auto not_space = [](unsigned char ch) { return std::isspace(ch) == 0; };
    const auto begin     = std::find_if(text.begin(), text.end(), not_space);
    if (begin == text.end()) {
        return {};
    }
    const auto end = std::find_if(text.rbegin(), text.rend(), not_space).base();
    return std::string(begin, end);
}

std::string lower(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

// IE 的服务地址偶尔会被填成 http://host:port，WinHTTP 只认 host:port
std::string strip_scheme(std::string value) {
    for (const char* prefix : {"http://", "https://"}) {
        if (value.rfind(prefix, 0) == 0) {
            value.erase(0, std::strlen(prefix));
            break;
        }
    }
    return value;
}

} // namespace

std::string proxy_for_winhttp(const std::string& raw) {
    const std::string all = trim(raw);
    if (all.empty()) {
        return {};
    }
    // 没有 "protocol=" 前缀：IE 里填的就是"所有协议都用它"，WinHTTP 也这么理解
    if (all.find('=') == std::string::npos) {
        return strip_scheme(all);
    }

    std::string http;
    std::string https;
    std::size_t start = 0;
    for (;;) {
        const std::size_t end  = all.find(';', start);
        const std::string item = all.substr(start, end == std::string::npos ? std::string::npos
                                                                           : end - start);
        const std::size_t eq = item.find('=');
        if (eq != std::string::npos) {
            const std::string scheme = lower(trim(item.substr(0, eq)));
            const std::string host   = strip_scheme(trim(item.substr(eq + 1)));
            if (!host.empty()) {
                if (scheme == "http" && http.empty()) {
                    http = host;
                } else if (scheme == "https" && https.empty()) {
                    https = host;
                }
            }
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }

    if (http.empty()) {
        http = https;
    }
    if (https.empty()) {
        https = http;
    }
    if (http.empty()) {
        return {}; // 只剩 socks=：WinHTTP 用不了 SOCKS 端口
    }
    return "http=" + http + ";https=" + https;
}

SystemProxy system_proxy() {
    SystemProxy out;
    const RegKey key(HKEY_CURRENT_USER, kInternetSettings);
    out.enabled = key.dword(L"ProxyEnable", 0) != 0;
    out.pac_url = narrow(key.string(L"AutoConfigURL"));
    if (!out.enabled) {
        // 关掉系统代理后 ProxyServer 仍留在注册表里，照用会把请求送进已经退出的代理
        return out;
    }
    out.proxy  = proxy_for_winhttp(narrow(key.string(L"ProxyServer")));
    out.bypass = narrow(key.string(L"ProxyOverride"));
    return out;
}

} // namespace fgb::net
