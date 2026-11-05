#include "vrss/runtime.hpp"

#include "base/json_value.hpp"
#include "base/log.hpp"
#include "vrss/identity.hpp"
#include "vrss/paths.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cwchar>
#include <fstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <winreg.h>
#endif

namespace fgb::vrss {
namespace {

#ifdef _WIN32

std::wstring to_wide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

std::wstring quote(const std::wstring& value) {
    return L"\"" + value + L"\"";
}

bool running_process(const wchar_t* name) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found    = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, name) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

bool token_is_admin() {
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    PSID sid                            = nullptr;
    BOOL member                         = FALSE;
    if (!AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                  DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &sid)) {
        return false;
    }
    const BOOL ok = CheckTokenMembership(nullptr, sid, &member);
    FreeSid(sid);
    return ok && member;
}

bool run_capture(const std::filesystem::path& exe, const std::string& arguments, int timeout_ms,
                 int& exit_code, std::string& output) {
    exit_code = -1;
    output.clear();
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE read_pipe = nullptr, write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &security, 0)) {
        output = "无法创建进程输出管道";
        return false;
    }
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb         = sizeof(startup);
    startup.dwFlags    = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = write_pipe;
    startup.hStdError  = write_pipe;

    PROCESS_INFORMATION process{};
    std::wstring command = quote(exe.wstring());
    if (!arguments.empty()) {
        command += L" " + to_wide(arguments);
    }
    const std::wstring directory = exe.parent_path().wstring();
    const BOOL created = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                                       CREATE_NO_WINDOW, nullptr, directory.c_str(), &startup,
                                       &process);
    CloseHandle(write_pipe);
    if (!created) {
        CloseHandle(read_pipe);
        output = "无法启动 " + exe.filename().string() + "（错误码 " +
                 std::to_string(GetLastError()) + "）";
        return false;
    }

    const DWORD waited = WaitForSingleObject(process.hProcess, static_cast<DWORD>(timeout_ms));
    if (waited == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 1);
        output += "\n（进程超时，已终止）";
    }
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    exit_code = static_cast<int>(code);

    char buffer[4096];
    DWORD available = 0;
    while (PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
        DWORD read = 0;
        const DWORD want = std::min<DWORD>(available, sizeof(buffer));
        if (!ReadFile(read_pipe, buffer, want, &read, nullptr) || read == 0) {
            break;
        }
        output.append(buffer, read);
    }
    CloseHandle(read_pipe);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return waited != WAIT_TIMEOUT;
}

std::string field_value(const std::string& text, const std::string& key) {
    const std::string prefix = key + "=";
    std::size_t at           = 0;
    while (at < text.size()) {
        const std::size_t end = text.find('\n', at);
        const std::string_view line{text.data() + at,
                                    (end == std::string::npos ? text.size() : end) - at};
        if (line.rfind(prefix, 0) == 0) {
            std::string value{line.substr(prefix.size())};
            if (!value.empty() && value.back() == '\r') {
                value.pop_back();
            }
            return value;
        }
        if (end == std::string::npos) {
            break;
        }
        at = end + 1;
    }
    return {};
}

bool contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(file)), {});
}

double mtime_age_seconds(const std::filesystem::path& path) {
    std::error_code error;
    const auto stamp = std::filesystem::last_write_time(path, error);
    if (error) {
        return -1.0;
    }
    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::filesystem::file_time_type::clock::now() - stamp);
    return static_cast<double>(age.count()) / 1000.0;
}

// provider 的错误原文是英文的、面向开发者的；页面必须给可操作的中文原因。
// 只映射会影响"用户下一步做什么"的几条，其余原样返回（诊断包里仍有英文原文）。
std::string friendly_provider_error(std::string text) {
    if (text.empty()) {
        return text;
    }
    const auto has = [&text](const char* needle) { return text.find(needle) != std::string::npos; };
    if (has("Another generic VRSS provider")) {
        return "另一个通用 VRSS 提供程序（common_vrss_provider.exe）正在运行；请先结束它再启动注视路径";
    }
    if (has("Another Tuner provider owns")) {
        return "这个头显路由已被另一个 VRSS 提供程序占用（常见是上一轮的提权进程没退）；"
               "请在任务管理器结束 fgb-vrss-provider.exe 后重试";
    }
    if (has("existing provider event is inaccessible")) {
        return "上一轮的 VRSS 事件无法访问（权限残留）；请重启本软件，必要时重启系统后再试";
    }
    if (has("existing provider bridge")) {
        return "上一轮的 VRSS 桥仍存在且未允许热重连；请先正常关闭 VRChat，再启动注视路径";
    }
    if (has("retains the previous connection")) {
        return "NVIDIA 拒绝热重连：VRChat 仍持有上一轮连接。"
               "请先正常关闭 VRChat，启动注视路径，再重新打开 VRChat";
    }
    return text;
}

std::string contract_key(const std::filesystem::path& path) {
    const std::string body = read_text(path);
    const std::string api  = field_value(body, "api_sha256");
    const std::string umd  = field_value(body, "umd_sha256");
    if (api.size() < 12 || umd.size() < 12) {
        return {};
    }
    return api.substr(0, 12) + "-" + umd.substr(0, 12);
}

struct AreaResult {
    bool ok      = false;
    bool found   = false;
    float percent = 0.0f;
    std::string error;
};

AreaResult read_area_result() {
    AreaResult result;
    const std::string body = read_text(run_dir() / "vrss-area.txt");
    if (body.empty()) {
        result.error = "没有收到 NVIDIA profile 写入结果";
        return result;
    }
    result.ok      = field_value(body, "ok") == "1";
    result.found   = field_value(body, "found") == "1";
    result.error   = field_value(body, "error");
    const std::string area = field_value(body, "area");
    if (!area.empty()) {
        result.percent = static_cast<float>(std::atof(area.c_str()));
    }
    if (!result.ok && result.error.empty()) {
        result.error = "NVIDIA profile 写入失败";
    }
    return result;
}

#endif // _WIN32

} // namespace

std::filesystem::path Runtime::provider_exe() { return runtime_dir() / "fgb-vrss-provider.exe"; }

std::filesystem::path Runtime::exporter_exe() { return runtime_dir() / "fgb-export-geometry.exe"; }

bool Runtime::installed(std::string& error) const {
    error.clear();
    if (!std::filesystem::is_regular_file(provider_exe())) {
        error = "缺少 " + provider_exe().string();
        return false;
    }
    if (!std::filesystem::is_regular_file(exporter_exe())) {
        error = "缺少 " + exporter_exe().string();
        return false;
    }
    return true;
}

bool Runtime::export_geometry(HmdGeometry& out, std::string& error) {
#ifdef _WIN32
    error.clear();
    if (!installed(error)) {
        return false;
    }
    if (!running_process(L"vrserver.exe")) {
        // 关键：SteamVR 没运行时不能调用 VR_Init——它会尝试拉起 SteamVR 并长时间阻塞。
        error = "SteamVR 未运行，请先启动 SteamVR 并连接头显";
        return false;
    }
    std::filesystem::create_directories(run_dir());
    const std::filesystem::path target = run_dir() / "geometry.txt";

    // 每次导出都覆盖同一份文件；返回 false 时保留上一次成功的内容不参与判断。
    const auto export_once = [&](const std::string& extra) {
        std::error_code remove_error;
        std::filesystem::remove(target, remove_error);
        const std::string arguments = "\"" + target.string() + "\"" + extra;
        FGB_INFO("vrss", "运行 %s %s", exporter_exe().string().c_str(), arguments.c_str());
        int code = 0;
        std::string output;
        if (!run_capture(exporter_exe(), arguments, 10000, code, output) || code != 0 ||
            !std::filesystem::is_regular_file(target)) {
            FGB_WARN("vrss", "几何导出失败 code=%d output=%s", code, output.c_str());
            error = output.empty() ? "OpenVR 几何导出失败（请确认 SteamVR 已启动并连接头显）"
                                   : "OpenVR 几何导出失败：" + output;
            return false;
        }
        std::string parse_error;
        if (!load_geometry(target, out, parse_error)) {
            error = parse_error;
            return false;
        }
        return true;
    };

    if (!export_once({})) {
        return false;
    }
    // 没有 EDID 的头显（PICO/串流）：第一次拿到的是按序列号哈希的 candidate 身份，
    // 串流会话换序列号就会变。持久化的环境变量必须是常量，所以改用固定会话键。
    // 有真 EDID 时保留头显自己的身份，不替换（换成 0xFEEE 会让真机反而失配）。
    if (needs_identity_override(out.identity_source) && out.identity_source != "tuner-session") {
        FGB_INFO("vrss", "身份来源 %s：改用固定会话键 %s:%s",
                 out.identity_source.c_str(), identity_env_text(kSessionVendor).c_str(),
                 identity_env_text(kSessionProduct).c_str());
        if (!export_once(" --common-session-key")) {
            FGB_WARN("vrss", "固定会话键导出失败，沿用序列号派生身份：%s", error.c_str());
            // 退回第一次的结果：功能仍可用，只是持久身份会随序列号变化
            if (!export_once({})) {
                return false;
            }
        }
    }
    return true;
#else
    error = "VRSS 仅支持 Windows";
    return false;
#endif
}

bool Runtime::check_driver(DriverInfo& out, std::string& error) {
#ifdef _WIN32
    out = DriverInfo{};
    error.clear();
    contract_.clear();
    if (!installed(error)) {
        out.message = error;
        return false;
    }

    auto check_with = [&](const std::string& arguments, int& code, std::string& output) {
        return run_capture(provider_exe(), arguments, 20000, code, output);
    };

    int code = 0;
    std::string output;
    if (!check_with("--driver-check-only 1", code, output)) {
        out.message = output;
        error      = output;
        return false;
    }
    out.nvidia = contains(output, "nvapi64Sha256=") || contains(output, "driverTarget=");
    if (code == 0 && contains(output, "DRIVER_CHECK_PASS")) {
        out.supported = true;
        out.contract  = "audited";
        out.version   = field_value(output, "driverTarget");
        out.message   = "内建白名单通过";
        return true;
    }

    // 复用已缓存的实验合同：只认当前镜像哈希匹配的那些。
    std::error_code ignored;
    if (std::filesystem::is_directory(contracts_dir(), ignored)) {
        for (const auto& entry : std::filesystem::directory_iterator(contracts_dir(), ignored)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".txt") {
                continue;
            }
            int cached_code = 0;
            std::string cached_output;
            const std::string arguments =
                "--driver-check-only 1 --experimental-contract \"" + entry.path().string() + "\"";
            if (check_with(arguments, cached_code, cached_output) && cached_code == 0 &&
                contains(cached_output, "DRIVER_CHECK_PASS")) {
                contract_      = entry.path().string();
                out.supported  = true;
                out.experimental = true;
                out.contract   = "experimental";
                out.version    = field_value(cached_output, "driverTarget");
                out.message    = "复用已探测合同：" + entry.path().filename().string();
                return true;
            }
        }
    }

    // 自动只读探测：只读 vtable 地址，不调用任何私有注册/提交方法。
    std::filesystem::create_directories(contracts_dir());
    const std::filesystem::path candidate =
        contracts_dir() / ("candidate-" + std::to_string(GetCurrentProcessId()) + ".txt");
    std::filesystem::remove(candidate);
    const std::string audit_arguments =
        "--driver-audit 1 --contract-out \"" + candidate.string() + "\"";
    int audit_code = 0;
    std::string audit_output;
    if (!run_capture(provider_exe(), audit_arguments, 30000, audit_code, audit_output) ||
        audit_code != 0 || !std::filesystem::is_regular_file(candidate)) {
        out.nvidia  = contains(audit_output, "nvapi64Sha256=");
        out.message = audit_output.empty() ? "驱动探测失败（可能没有 NVIDIA GPU）" : audit_output;
        error       = out.message;
        return false;
    }

    const std::string key = contract_key(candidate);
    if (key.empty()) {
        std::filesystem::remove(candidate);
        out.message = "驱动探测输出不完整";
        error       = out.message;
        return false;
    }
    const std::filesystem::path stored = contracts_dir() / (key + ".txt");
    std::filesystem::remove(stored);
    std::filesystem::rename(candidate, stored, ignored);

    int verify_code = 0;
    std::string verify_output;
    const std::string verify_arguments =
        "--driver-check-only 1 --experimental-contract \"" + stored.string() + "\"";
    if (!run_capture(provider_exe(), verify_arguments, 20000, verify_code, verify_output) ||
        verify_code != 0 || !contains(verify_output, "DRIVER_CHECK_PASS")) {
        std::filesystem::remove(stored, ignored);
        out.message = verify_output.empty() ? "实验合同二次校验失败" : verify_output;
        error       = out.message;
        return false;
    }

    contract_        = stored.string();
    out.nvidia       = true;
    out.supported    = true;
    out.experimental = true;
    out.contract     = "experimental";
    out.version      = field_value(audit_output, "driverVersion");
    out.message      = "只读探测通过；需真机看四方向才能确认渲染生效";
    return true;
#else
    out.message = "VRSS 仅支持 Windows";
    error       = out.message;
    return false;
#endif
}

bool Runtime::start_provider(int port, const std::string& contract, float area_percent,
                             std::string& error) {
#ifdef _WIN32
    error.clear();
    if (!installed(error)) {
        return false;
    }
    if (port < 1 || port > 65535) {
        error = "VRSS 端口不合法";
        return false;
    }
    std::filesystem::create_directories(run_dir());
    std::filesystem::create_directories(data_dir());
    const std::filesystem::path calibration = data_dir() / "calibration.txt";
    if (!std::filesystem::is_regular_file(calibration)) {
        const std::filesystem::path template_file = runtime_dir() / "calibration.txt";
        if (std::filesystem::is_regular_file(template_file)) {
            std::filesystem::copy_file(template_file, calibration,
                                       std::filesystem::copy_options::overwrite_existing);
        } else {
            std::ofstream(calibration) << "left_deg=0\nright_deg=0\nx_gain=1\ny_gain=1\n";
        }
    }
    const std::filesystem::path geometry = run_dir() / "geometry.txt";
    if (!std::filesystem::is_regular_file(geometry)) {
        error = "缺少几何文件，请先执行 HMD 检测";
        return false;
    }
    std::error_code ignored;
    std::filesystem::remove(run_dir() / "status.json", ignored);
    std::filesystem::remove(run_dir() / "error.txt", ignored);
    std::filesystem::remove(run_dir() / "stop.request", ignored);
    write_lease();

    std::string arguments = "--geometry \"" + geometry.string() + "\"" +
                            " --config \"" + calibration.string() + "\"" +
                            " --output \"" + run_dir().string() + "\"" +
                            " --source udp --port " + std::to_string(port) +
                            " --allow-reconnect 1 --seconds 86400" +
                            " --lease \"" + (data_dir() / "lease.txt").string() + "\" --csv 0" +
                            " --vrss-area " + std::to_string(area_percent);
    if (!contract.empty()) {
        arguments += " --experimental-contract \"" + contract + "\"";
    }

    // lpFile / lpDirectory 指向的宽字符串必须在调用期间一直存活。以前写成
    // provider_exe().wstring().c_str()：临时字符串在语句结束就析构，ShellExecuteExW
    // 拿到的是悬垂指针 —— 表现是 Windows 自己弹"找不到文件"（文件名还是空的），
    // 而不是我们的错误文案。
    const std::wstring provider_path = provider_exe().wstring();
    const std::wstring working_dir   = runtime_dir().wstring();
    const std::wstring wide_args     = to_wide(arguments);
    if (!std::filesystem::is_regular_file(provider_exe())) {
        error = "找不到 " + provider_exe().string() + "（VRSS 组件未安装或被杀软隔离）";
        return false;
    }

    SHELLEXECUTEINFOW info{};
    info.cbSize      = sizeof(info);
    // FLAG_NO_UI：失败时不要弹系统的错误框，让原因回到页面上，别再出现"文件名是空的"。
    info.fMask       = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    info.lpVerb      = L"runas";
    info.lpFile      = provider_path.c_str();
    info.lpParameters = wide_args.c_str();
    info.lpDirectory  = working_dir.c_str();
    info.nShow        = SW_HIDE;
    if (!ShellExecuteExW(&info)) {
        const DWORD code = GetLastError();
        error = code == ERROR_CANCELLED
                    ? "需要管理员权限（UAC 已取消）"
                    : "启动 provider 失败（错误码 " + std::to_string(code) + "，路径 " +
                          provider_exe().string() + "）";
        return false;
    }
    if (info.hProcess) {
        CloseHandle(info.hProcess);
    }

    // 等 provider 写出 status.json；启动失败时它会写 error.txt。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::filesystem::is_regular_file(run_dir() / "status.json")) {
            return true;
        }
        if (std::filesystem::is_regular_file(run_dir() / "error.txt")) {
            error = last_error_text();
            return false;
        }
        if (!running_process(L"fgb-vrss-provider.exe")) {
            error = last_error_text();
            if (error.empty()) {
                error = "provider 启动后立即退出";
            }
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    error = "provider 启动超时（8 秒内没有写出状态）";
    return false;
#else
    error = "VRSS 仅支持 Windows";
    return false;
#endif
}

bool Runtime::stop_provider(std::string& error) {
#ifdef _WIN32
    error.clear();
    std::filesystem::create_directories(run_dir());
    std::ofstream(run_dir() / "stop.request") << "stop";
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!provider_alive()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    error = "provider 仍在运行；它已被提权，请在任务管理器中结束 fgb-vrss-provider.exe";
    return false;
#else
    error = "VRSS 仅支持 Windows";
    return false;
#endif
}

bool Runtime::set_area(float percent, std::string& error) {
#ifdef _WIN32
    error.clear();
    if (!installed(error)) {
        return false;
    }
    if (percent < 1.0f || percent > 50.0f) {
        error = "VRSS 抗锯齿范围必须在 1..50%";
        return false;
    }
    std::filesystem::create_directories(run_dir());
    std::error_code ignored;
    std::filesystem::remove(run_dir() / "vrss-area.txt", ignored);
    const std::string arguments =
        "--set-vrss-area " + std::to_string(percent) + " --output \"" + run_dir().string() + "\"";

    // 同 start_provider：路径字符串必须活到 ShellExecuteExW 返回之后。
    const std::wstring provider_path = provider_exe().wstring();
    const std::wstring working_dir   = runtime_dir().wstring();
    const std::wstring wide_args     = to_wide(arguments);

    SHELLEXECUTEINFOW info{};
    info.cbSize      = sizeof(info);
    info.fMask       = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    info.lpVerb      = L"runas";
    info.lpFile      = provider_path.c_str();
    info.lpParameters = wide_args.c_str();
    info.lpDirectory  = working_dir.c_str();
    info.nShow        = SW_HIDE;
    if (!ShellExecuteExW(&info)) {
        const DWORD code = GetLastError();
        error = code == ERROR_CANCELLED
                    ? "需要管理员权限（UAC 已取消）"
                    : "无法启动 VRSS 设置助手（错误码 " + std::to_string(code) + "，路径 " +
                          provider_exe().string() + "）";
        return false;
    }
    if (info.hProcess == nullptr) {
        error = "VRSS 设置助手没有返回进程句柄";
        return false;
    }
    const DWORD waited = WaitForSingleObject(info.hProcess, 12000);
    DWORD exit_code = 0;
    GetExitCodeProcess(info.hProcess, &exit_code);
    CloseHandle(info.hProcess);
    if (waited == WAIT_TIMEOUT) {
        error = "写入 NVIDIA profile 超时";
        return false;
    }
    const AreaResult result = read_area_result();
    if (!result.ok) {
        error = result.error.empty() ? "写入 NVIDIA profile 失败（退出码 " +
                                           std::to_string(exit_code) + "）"
                                     : result.error;
        return false;
    }
    return true;
#else
    (void)percent;
    error = "VRSS 仅支持 Windows";
    return false;
#endif
}

bool Runtime::read_area_record(float& percent, bool& found) const {
    percent = 0.0f;
    found   = false;
#ifdef _WIN32
    const AreaResult result = read_area_result();
    if (!result.ok) {
        return false;
    }
    percent = result.percent;
    found   = result.found;
    return true;
#else
    return false;
#endif
}

bool Runtime::get_area(float& percent, bool& found, std::string& error) {
#ifdef _WIN32
    error.clear();
    percent = 0.0f;
    found   = false;
    if (!installed(error)) {
        return false;
    }
    std::filesystem::create_directories(run_dir());
    std::error_code ignored;
    std::filesystem::remove(run_dir() / "vrss-area.txt", ignored);
    int code = 0;
    std::string output;
    const std::string arguments =
        "--get-vrss-area 1 --output \"" + run_dir().string() + "\"";
    if (!run_capture(provider_exe(), arguments, 10000, code, output) || code != 0) {
        error = output.empty() ? "读取 NVIDIA profile 失败" : output;
        return false;
    }
    const AreaResult result = read_area_result();
    if (!result.ok) {
        error = result.error.empty() ? "读取 NVIDIA profile 失败" : result.error;
        return false;
    }
    percent = result.percent;
    found   = result.found;
    return true;
#else
    (void)percent;
    (void)found;
    error = "VRSS 仅支持 Windows";
    return false;
#endif
}

bool Runtime::provider_alive() const {
#ifdef _WIN32
    ProviderStatus status;
    if (read_status(status) && status.pid != 0) {
        HANDLE process =
            OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(status.pid));
        if (process) {
            DWORD code = 0;
            const bool alive = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
            CloseHandle(process);
            return alive;
        }
    }
    return running_process(L"fgb-vrss-provider.exe");
#else
    return false;
#endif
}

bool Runtime::read_status(ProviderStatus& out) const {
    out = ProviderStatus{};
#ifdef _WIN32
    const std::filesystem::path path = run_dir() / "status.json";
    const std::string body           = read_text(path);
    if (body.empty()) {
        out.running = running_process(L"fgb-vrss-provider.exe");
        return false;
    }
    json::Value root;
    std::string error;
    if (!json::parse(body, root, error) || !root.is_object()) {
        out.running = running_process(L"fgb-vrss-provider.exe");
        return false;
    }
    out.running           = running_process(L"fgb-vrss-provider.exe");
    out.pid               = static_cast<int>(root.number_at("pid"));
    out.registered        = root.bool_at("registered");
    out.fresh             = root.bool_at("fresh");
    out.consumer_count    = static_cast<int>(root.number_at("consumerCount"));
    out.sent              = static_cast<std::uint64_t>(root.number_at("sent"));
    out.submit_failures   = static_cast<std::uint64_t>(root.number_at("submitFailures"));
    out.delivery_state    = root.string_at("deliveryState");
    out.input_route       = root.string_at("inputRoute");
    out.age_seconds       = mtime_age_seconds(path);
    out.last_error        = last_error_text();
    return true;
#else
    return false;
#endif
}

std::string Runtime::last_error_text() const {
    const std::string body = read_text(run_dir() / "error.txt");
    if (body.empty()) {
        return {};
    }
    const std::size_t newline = body.find('\n');
    return friendly_provider_error(newline == std::string::npos ? body : body.substr(newline + 1));
}

void Runtime::write_lease() const {
#ifdef _WIN32
    std::filesystem::create_directories(data_dir());
    std::ofstream(data_dir() / "lease.txt") << GetTickCount64();
#endif
}

#ifdef _WIN32
namespace {

std::wstring steam_install_path() {
    wchar_t buffer[1024]{};
    DWORD size = sizeof(buffer);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ,
                     nullptr, buffer, &size) == ERROR_SUCCESS) {
        return buffer;
    }
    size = sizeof(buffer);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath",
                     RRF_RT_REG_SZ, nullptr, buffer, &size) == ERROR_SUCCESS) {
        return buffer;
    }
    return {};
}

std::vector<std::filesystem::path> steam_library_paths() {
    std::vector<std::filesystem::path> paths;
    const std::wstring steam = steam_install_path();
    if (steam.empty()) {
        return paths;
    }
    paths.emplace_back(steam);
    std::ifstream file(std::filesystem::path(steam) / "steamapps" / "libraryfolders.vdf",
                       std::ios::binary);
    if (!file) {
        return paths;
    }
    std::string text((std::istreambuf_iterator<char>(file)), {});
    std::size_t at = 0;
    while ((at = text.find("\"path\"", at)) != std::string::npos) {
        at += 6;
        const std::size_t first = text.find('"', at);
        if (first == std::string::npos) {
            break;
        }
        const std::size_t last = text.find('"', first + 1);
        if (last == std::string::npos) {
            break;
        }
        const std::string value = text.substr(first + 1, last - first - 1);
        std::string unescaped;
        for (std::size_t i = 0; i < value.size(); ++i) {
            if (value[i] == '\\' && i + 1 < value.size() && value[i + 1] == '\\') {
                unescaped.push_back('\\');
                ++i;
            } else {
                unescaped.push_back(value[i]);
            }
        }
        paths.emplace_back(unescaped);
        at = last + 1;
    }
    return paths;
}

std::filesystem::path find_vrchat_launcher() {
    for (const auto& root : steam_library_paths()) {
        const auto candidate =
            root / "steamapps" / "common" / "VRChat" / "start_protected_game.exe";
        if (std::filesystem::is_regular_file(candidate)) {
            return candidate;
        }
    }
    for (const wchar_t* root : {L"C:\\Program Files (x86)\\Steam", L"C:\\Program Files\\Steam"}) {
        const auto candidate = std::filesystem::path(root) / "steamapps" / "common" / "VRChat" /
                               "start_protected_game.exe";
        if (std::filesystem::is_regular_file(candidate)) {
            return candidate;
        }
    }
    return {};
}

bool set_env_temp(const wchar_t* name, const std::wstring& value, std::wstring& old,
                  bool& had_old) {
    wchar_t buffer[1024]{};
    const DWORD length = GetEnvironmentVariableW(name, buffer, 1024);
    had_old = length > 0 && length < 1024;
    old     = had_old ? std::wstring(buffer) : std::wstring{};
    return SetEnvironmentVariableW(name, value.c_str()) != FALSE;
}

void restore_env(const wchar_t* name, const std::wstring& old, bool had_old) {
    SetEnvironmentVariableW(name, had_old ? old.c_str() : nullptr);
}

} // namespace
#endif

bool Runtime::steamvr_running() const {
#ifdef _WIN32
    return running_process(L"vrserver.exe");
#else
    return false;
#endif
}

bool Runtime::vrchat_running() const {
#ifdef _WIN32
    return running_process(L"VRChat.exe") || running_process(L"start_protected_game.exe");
#else
    return false;
#endif
}

bool Runtime::admin_enabled() const {
#ifdef _WIN32
    return token_is_admin();
#else
    return false;
#endif
}

bool Runtime::port_available(int port) const {
#ifdef _WIN32
    if (port < 1 || port > 65535) {
        return false;
    }
    SOCKET socket_handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_handle == INVALID_SOCKET) {
        return false;
    }
    sockaddr_in address{};
    address.sin_family      = AF_INET;
    address.sin_port        = htons(static_cast<u_short>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const BOOL exclusive    = TRUE;
    setsockopt(socket_handle, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
               reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    const bool ok = bind(socket_handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
    closesocket(socket_handle);
    return ok;
#else
    (void)port;
    return true;
#endif
}

bool Runtime::launch_vrchat(std::string& error) const {
#ifdef _WIN32
    error.clear();
    HmdGeometry geometry;
    std::string parse_error;
    const bool have_geometry = load_geometry(run_dir() / "geometry.txt", geometry, parse_error);
    // 无 EDID 的串流头显（PICO 等）在 provider 里用的是 candidate 身份；
    // 正常 Steam 启动看到的 HMD 身份不同，NVIDIA 不会把游戏和 provider 关联起来。
    // 必须把同一个身份通过 TEST_VENDOR_ID / TEST_PRODUCT_ID 传给受保护启动器。
    const bool needs_identity_injection =
        have_geometry && !geometry.identity_source.empty() &&
        geometry.identity_source != "openvr-edid";
    if (!needs_identity_injection) {
        const HINSTANCE result = ShellExecuteW(nullptr, L"open", L"steam://rungameid/438100",
                                               nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) <= 32) {
            error = "无法通过 Steam 启动 VRChat（请确认 Steam 已安装并登录）";
            return false;
        }
        return true;
    }

    const std::filesystem::path launcher = find_vrchat_launcher();
    if (launcher.empty()) {
        error = "找不到 VRChat 的 start_protected_game.exe；请确认 Steam 库中已安装 VRChat";
        return false;
    }
    wchar_t vendor_text[32]{};
    wchar_t product_text[32]{};
    std::swprintf(vendor_text, 32, L"0x%X", geometry.vendor);
    std::swprintf(product_text, 32, L"0x%X", geometry.product);
    std::wstring old_vendor, old_product;
    bool had_vendor = false, had_product = false;
    const bool vendor_ok = set_env_temp(L"TEST_VENDOR_ID", vendor_text, old_vendor, had_vendor);
    const bool product_ok =
        set_env_temp(L"TEST_PRODUCT_ID", product_text, old_product, had_product);
    if (!vendor_ok || !product_ok) {
        restore_env(L"TEST_VENDOR_ID", old_vendor, had_vendor);
        restore_env(L"TEST_PRODUCT_ID", old_product, had_product);
        error = "无法设置 VRChat 的 HMD 身份环境变量";
        return false;
    }

    const std::wstring command = L"\"" + launcher.wstring() + L"\" --profile=0";
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(0);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const std::wstring directory = launcher.parent_path().wstring();
    const BOOL created =
        CreateProcessW(launcher.c_str(), mutable_command.data(), nullptr, nullptr, FALSE, 0,
                       nullptr, directory.c_str(), &startup, &process);
    restore_env(L"TEST_VENDOR_ID", old_vendor, had_vendor);
    restore_env(L"TEST_PRODUCT_ID", old_product, had_product);
    if (!created) {
        error = "启动 VRChat 失败（错误码 " + std::to_string(GetLastError()) + "）";
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
#else
    error = "VRSS 仅支持 Windows";
    return false;
#endif
}

} // namespace fgb::vrss
