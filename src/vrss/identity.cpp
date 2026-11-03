#include "vrss/identity.hpp"

#include "vrss/paths.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace fgb::vrss {
namespace {

#ifdef _WIN32

constexpr const wchar_t* kEnvironmentKey = L"Environment";
constexpr const wchar_t* kVendorName     = L"TEST_VENDOR_ID";
constexpr const wchar_t* kProductName    = L"TEST_PRODUCT_ID";

std::uint64_t filetime_value(const FILETIME& time) {
    return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

std::uint64_t now_filetime() {
    FILETIME time{};
    GetSystemTimeAsFileTime(&time);
    return filetime_value(time);
}

// 读 HKCU\Environment 里的一个值。不存在返回 false（不是错误）。
bool read_user_env(const wchar_t* name, std::wstring& out) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kEnvironmentKey, 0, KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0;
    DWORD bytes = 0;
    LONG status = RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes);
    bool ok = false;
    if (status == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) && bytes >= 2) {
        out.assign(bytes / sizeof(wchar_t), L'\0');
        status = RegQueryValueExW(key, name, nullptr, &type,
                                  reinterpret_cast<LPBYTE>(out.data()), &bytes);
        if (status == ERROR_SUCCESS) {
            if (!out.empty() && out.back() == L'\0') {
                out.pop_back();
            }
            ok = true;
        }
    }
    RegCloseKey(key);
    return ok;
}

bool write_user_env(const wchar_t* name, const std::wstring& value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kEnvironmentKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                        &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    const LONG status = RegSetValueExW(key, name, 0, REG_SZ,
                                       reinterpret_cast<const BYTE*>(value.c_str()), bytes);
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

bool delete_user_env(const wchar_t* name) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kEnvironmentKey, 0, KEY_SET_VALUE, &key) !=
        ERROR_SUCCESS) {
        return false;
    }
    const LONG status = RegDeleteValueW(key, name);
    RegCloseKey(key);
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
}

void broadcast_environment_change() {
    DWORD_PTR result = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                        reinterpret_cast<LPARAM>(L"Environment"), SMTO_ABORTIFHUNG, 5000,
                        &result);
}

bool steam_process_start(std::uint64_t& out) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found   = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"steam.exe") != 0) {
                continue;
            }
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (process == nullptr) {
                continue;
            }
            FILETIME created{}, exited{}, kernel{}, user{};
            if (GetProcessTimes(process, &created, &exited, &kernel, &user)) {
                out   = filetime_value(created);
                found = true;
            }
            CloseHandle(process);
            if (found) {
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

std::wstring from_utf8(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

std::string to_utf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    std::string narrow(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), narrow.data(),
                        length, nullptr, nullptr);
    return narrow;
}

#endif // _WIN32

std::filesystem::path marker_path() {
    const std::filesystem::path dir = data_dir();
    return dir.empty() ? std::filesystem::path{} : dir / "persist-identity.txt";
}

struct Marker {
    std::uint32_t vendor  = 0;
    std::uint32_t product = 0;
    std::uint64_t written = 0;
    bool valid            = false;
};

Marker read_marker() {
    Marker marker;
    const std::filesystem::path path = marker_path();
    if (path.empty()) {
        return marker;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return marker;
    }
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::size_t equal = line.find('=');
        if (equal == std::string::npos) {
            continue;
        }
        const std::string key   = line.substr(0, equal);
        const std::string value = line.substr(equal + 1);
        if (key == "vendor") {
            std::uint32_t parsed = 0;
            if (parse_identity_env_text(value, parsed)) {
                marker.vendor = parsed;
            }
        } else if (key == "product") {
            std::uint32_t parsed = 0;
            if (parse_identity_env_text(value, parsed)) {
                marker.product = parsed;
            }
        } else if (key == "written") {
            marker.written = std::strtoull(value.c_str(), nullptr, 10);
        }
    }
    marker.valid = marker.vendor != 0 && marker.product != 0;
    return marker;
}

bool write_marker(const Marker& marker) {
    const std::filesystem::path path = marker_path();
    if (path.empty()) {
        return false;
    }
    std::error_code ignored;
    std::filesystem::create_directories(path.parent_path(), ignored);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    file << "vendor=" << identity_env_text(marker.vendor) << "\n"
         << "product=" << identity_env_text(marker.product) << "\n"
         << "written=" << marker.written << "\n";
    return file.good();
}

} // namespace

std::string identity_env_text(std::uint32_t value) {
    char buffer[16]{};
    std::snprintf(buffer, sizeof(buffer), "0x%X", value);
    return buffer;
}

bool parse_identity_env_text(std::string_view text, std::uint32_t& out) {
    // 去掉首尾空白；注册表里可能被别的工具写成十进制。
    std::size_t begin = 0;
    std::size_t end   = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    if (begin >= end) {
        return false;
    }
    std::string trimmed{text.substr(begin, end - begin)};
    int base    = 10;
    std::size_t at = 0;
    if (trimmed.size() > 2 && trimmed[0] == '0' && (trimmed[1] == 'x' || trimmed[1] == 'X')) {
        base = 16;
        at   = 2;
    }
    try {
        const unsigned long value = std::stoul(trimmed.substr(at), nullptr, base);
        if (value > 0xFFFFul) {
            return false;
        }
        out = static_cast<std::uint32_t>(value);
        return true;
    } catch (...) {
        return false;
    }
}

bool needs_identity_override(std::string_view identity_source) noexcept {
    if (identity_source.empty()) {
        return false; // 不知道来源就不动，宁可让流程报错也不要瞎写变量
    }
    return identity_source != "openvr-edid";
}

bool apply_persistent_identity(std::uint32_t vendor, std::uint32_t product, std::string& error) {
#ifdef _WIN32
    error.clear();
    if (vendor == 0 || product == 0 || vendor > 0xFFFF || product > 0xFFFF) {
        error = "HMD 身份不合法，无法持久化";
        return false;
    }
    const std::wstring vendor_text  = from_utf8(identity_env_text(vendor));
    const std::wstring product_text = from_utf8(identity_env_text(product));
    if (!write_user_env(kVendorName, vendor_text) || !write_user_env(kProductName, product_text)) {
        error = "写入用户环境变量失败（HKCU\\Environment）";
        return false;
    }
    // 当前进程与后续子进程立即生效；已运行的 Steam 不受影响，需要用户重启它。
    SetEnvironmentVariableW(kVendorName, vendor_text.c_str());
    SetEnvironmentVariableW(kProductName, product_text.c_str());
    broadcast_environment_change();

    Marker marker;
    marker.vendor  = vendor;
    marker.product = product;
    marker.written = now_filetime();
    if (!write_marker(marker)) {
        // marker 只用于"是不是我们写的"与重启判断，写不了不算致命
        error = "身份已写入，但记录文件不可写（换头显后可能无法自动清理）";
    }
    return true;
#else
    (void)vendor;
    (void)product;
    error = "VRSS 仅支持 Windows";
    return false;
#endif
}

bool clear_persistent_identity(std::string& error) {
#ifdef _WIN32
    error.clear();
    const Marker marker = read_marker();
    if (marker.valid) {
        std::wstring vendor, product;
        const bool has_vendor  = read_user_env(kVendorName, vendor);
        const bool has_product = read_user_env(kProductName, product);
        // 按数值比较：用户或别的工具可能写成十进制，字符串比较会误判成"不是我们写的"。
        std::uint32_t vendor_now = 0, product_now = 0;
        const bool vendor_ok = has_vendor && parse_identity_env_text(to_utf8(vendor), vendor_now);
        const bool product_ok =
            has_product && parse_identity_env_text(to_utf8(product), product_now);
        const bool matches = (!has_vendor || (vendor_ok && vendor_now == marker.vendor)) &&
                             (!has_product || (product_ok && product_now == marker.product));
        if (!matches) {
            // 用户手动改过：只删 marker，不碰他们的值。
            std::error_code ignored;
            std::filesystem::remove(marker_path(), ignored);
            return true;
        }
    }
    const bool ok = delete_user_env(kVendorName) && delete_user_env(kProductName);
    SetEnvironmentVariableW(kVendorName, nullptr);
    SetEnvironmentVariableW(kProductName, nullptr);
    broadcast_environment_change();
    std::error_code ignored;
    std::filesystem::remove(marker_path(), ignored);
    if (!ok) {
        error = "清除用户环境变量失败（HKCU\\Environment）";
        return false;
    }
    return true;
#else
    error = "VRSS 仅支持 Windows";
    return false;
#endif
}

PersistentIdentity persistent_identity() {
    PersistentIdentity identity;
#ifdef _WIN32
    std::wstring vendor, product;
    const bool has_vendor  = read_user_env(kVendorName, vendor);
    const bool has_product = read_user_env(kProductName, product);
    identity.read_ok       = true;
    if (!has_vendor || !has_product) {
        return identity;
    }
    std::uint32_t vendor_value = 0, product_value = 0;
    if (!parse_identity_env_text(to_utf8(vendor), vendor_value) ||
        !parse_identity_env_text(to_utf8(product), product_value)) {
        identity.error = "环境变量里的 HMD 身份不是有效数字";
        return identity;
    }
    identity.set     = true;
    identity.vendor  = vendor_value;
    identity.product = product_value;

    const Marker marker = read_marker();
    identity.ours = marker.valid && marker.vendor == identity.vendor &&
                    marker.product == identity.product;

    std::uint64_t steam_start = 0;
    if (identity.ours && marker.written != 0 && steam_process_start(steam_start)) {
        // Steam 早于写入时间启动 → 它的环境块里没有这两个值，从它启动的游戏也拿不到。
        identity.steam_restart_required = steam_start < marker.written;
    }
#endif
    return identity;
}

} // namespace fgb::vrss
