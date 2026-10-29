// FaceGaze Bridge —— 桌面入口（WebView2 外壳 + 引擎拉起）。
//
// 产品形态：用户只双击一个 exe。外壳先探一下控制面端口：
//   - 已经有引擎在跑（例如命令行起的）→ 直接连，不动它
//   - 没有 → 隐藏控制台拉起同目录的 fgb-cli.exe，并把它的输出写进
//     %LOCALAPPDATA%\FaceGazeBridge\engine.log
//
// 生命周期用 Job 对象绑定：外壳退出（正常关闭或崩溃）都会带走自己拉起的引擎，
// 不留后台进程。别人起的引擎不受影响（没有句柄关系）。
//
// 页面仍从 http://127.0.0.1:<port>/ 加载，外壳里没有业务逻辑：
//   1. 页面直接连 ws://127.0.0.1:<port>/ws，浏览器打开同一地址也能调试
//   2. 引擎与界面是两个进程，改 UI 不用重编 C++
//   3. 页面与 WebSocket 同源同端口，绕开 mixed-content 限制
//
// 运行时依赖：WebView2 Evergreen Runtime。Win10/11 通常已随 Edge 预装。
#include <winsock2.h>

#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <wrl.h>
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kWindowClass[]  = L"FgbUiWindow";
constexpr wchar_t kWindowTitle[]  = L"FaceGaze Bridge";
constexpr UINT_PTR kRetryTimer    = 1;
constexpr UINT_PTR kEngineTimer   = 2;
constexpr UINT_PTR kRestartTimer  = 3;
constexpr UINT_PTR kLoadingTimer  = 4;
// 引擎拉起后通常 100 ms 内就能响应 /healthz；等 1.5 s 再试只是让首屏白等
constexpr UINT kRetryIntervalMs   = 250;
constexpr UINT kEnginePollMs      = 1000;
constexpr UINT kLoadingTickMs     = 400;
constexpr int kIconResource       = 101;
// 引擎在收到 restart_engine 时用这个退出码结束，外壳据此原地重启而不是关窗
constexpr DWORD kEngineRestartExit = 3;

// 逻辑像素；实际尺寸按显示器 DPI 放大。旧版固定 1180x780 在 125%/150% 缩放下显得很小。
constexpr int kDefaultWidthDip  = 1360;
constexpr int kDefaultHeightDip = 860;
constexpr int kMinWidthDip      = 1100;
constexpr int kMinHeightDip     = 700;

struct Host {
    HWND hwnd = nullptr;
    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
    std::wstring url;
    std::wstring user_data_dir;
    // 非空时把 --remote-debugging-port 传给 WebView2：自动化验证和现场排障都靠它
    std::wstring browser_args;
    std::uint16_t port = 8787;
    bool navigated_ok       = false;
    bool engine_owned       = false;
    // 自己拉起的引擎：页面要等 /healthz 应答再导航，免得先吃一个错误页
    bool expect_engine      = false;
    bool controller_started = false;
    // WebView2 就绪后它自己的子窗口会盖住客户区，加载画面就到此为止
    bool webview_ready = false;
    // WebView2 冷启动一两秒是常态，这段空窗期先画加载状态，不要给用户一块死板的黑屏
    std::wstring status      = L"正在启动";
    unsigned loading_tick    = 0;
    ULONGLONG started_ms     = 0;

    HANDLE engine_process = nullptr;
    HANDLE engine_job     = nullptr;
    std::wstring engine_log;
    std::wstring default_source;
    std::vector<std::wstring> engine_args;
};

Host g_host;

bool engine_present(std::uint16_t port);

void set_title(const wchar_t* suffix) {
    if (g_host.hwnd == nullptr) {
        return;
    }
    std::wstring title{kWindowTitle};
    if (suffix != nullptr && suffix[0] != L'\0') {
        title.append(L" — ");
        title.append(suffix);
    }
    SetWindowTextW(g_host.hwnd, title.c_str());
}

std::wstring local_app_data() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        return std::wstring{buffer, length};
    }
    return L".";
}

std::wstring default_user_data_dir() {
    std::wstring dir = local_app_data();
    dir.append(L"\\FaceGazeBridge\\WebView2");
    return dir;
}

// 启动各阶段耗时写进 engine.log：用户报"启动慢"时先看这几行，
// 一眼分清是外壳、WebView2 运行时还是引擎花的时间。
void trace_startup(const wchar_t* phase) {
    if (g_host.engine_log.empty()) {
        return;
    }
    wchar_t line[256];
    std::swprintf(line, 256, L"[shell] +%llums %s\r\n",
                  static_cast<unsigned long long>(::GetTickCount64() - g_host.started_ms), phase);
    const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, line, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) {
        return;
    }
    std::string utf8(static_cast<std::size_t>(bytes), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8.data(), bytes, nullptr, nullptr);

    // 引擎同时也在往这个文件追加（继承的句柄），双方都用 FILE_APPEND_DATA，
    // 每次写落在当时的文件尾，不会互相覆盖。
    HANDLE log = ::CreateFileW(g_host.engine_log.c_str(), FILE_APPEND_DATA,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    ::WriteFile(log, utf8.data(), static_cast<DWORD>(utf8.size() - 1), &written, nullptr);
    ::CloseHandle(log);
}

UINT window_dpi() {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static const auto get_dpi = reinterpret_cast<GetDpiForWindowFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")));
    if (get_dpi != nullptr && g_host.hwnd != nullptr) {
        const UINT dpi = get_dpi(g_host.hwnd);
        if (dpi != 0) {
            return dpi;
        }
    }
    return 96;
}

// 加载画面用 GDI 自己画：WebView2 的窗口挂在客户区上，controller 一就绪就盖住这里。
// 一屏静态字看着像卡死，所以状态行按 400 ms 追加圆点表示还在动。
void paint_loading(HDC dc, const RECT& client) {
    const UINT dpi     = window_dpi();
    const int center_y = (client.top + client.bottom) / 2;

    HFONT title_font = CreateFontW(-::MulDiv(30, static_cast<int>(dpi), 96), 0, 0, 0, FW_SEMIBOLD,
                                   FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                   CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                                   L"Segoe UI");
    HFONT status_font = CreateFontW(-::MulDiv(15, static_cast<int>(dpi), 96), 0, 0, 0, FW_NORMAL,
                                    FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                                    L"Segoe UI");

    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ previous = SelectObject(dc, title_font);
    SetTextColor(dc, RGB(0xe6, 0xed, 0xfb));
    RECT title_rect{client.left, center_y - ::MulDiv(44, static_cast<int>(dpi), 96), client.right,
                    center_y};
    DrawTextW(dc, L"FaceGaze Bridge", -1, &title_rect,
              DT_CENTER | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);

    static const wchar_t* const kDots[] = {L"", L".", L"..", L"..."};
    std::wstring status = g_host.status;
    status.append(kDots[g_host.loading_tick % 4]);
    SelectObject(dc, status_font);
    SetTextColor(dc, RGB(0x8f, 0xa3, 0xc2));
    RECT status_rect{client.left, center_y + ::MulDiv(10, static_cast<int>(dpi), 96), client.right,
                     center_y + ::MulDiv(36, static_cast<int>(dpi), 96)};
    DrawTextW(dc, status.c_str(), -1, &status_rect,
              DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);

    SelectObject(dc, previous);
    DeleteObject(title_font);
    DeleteObject(status_font);
}

void navigate_once() {
    if (!g_host.webview) {
        return;
    }
    // 引擎没应答就不导航：错误页会闪一下，而且原来的重试间隔让首屏白等 1.5 s
    if (g_host.expect_engine && !engine_present(g_host.port)) {
        set_title(L"等待引擎…");
        g_host.status = L"正在等待引擎";
        SetTimer(g_host.hwnd, kRetryTimer, kRetryIntervalMs, nullptr);
        return;
    }
    g_host.navigated_ok = false;
    g_host.webview->Navigate(g_host.url.c_str());
}

void on_controller_ready(HRESULT result, ICoreWebView2Controller* controller) {
    if (FAILED(result) || controller == nullptr) {
        wchar_t message[256];
        std::swprintf(message, 256,
                      L"WebView2 初始化失败（0x%08lX）。\n\n"
                      L"请确认已安装 Microsoft Edge WebView2 Runtime（Win10/11 通常自带）。",
                      static_cast<unsigned long>(result));
        MessageBoxW(g_host.hwnd, message, kWindowTitle, MB_ICONERROR | MB_OK);
        PostMessageW(g_host.hwnd, WM_CLOSE, 0, 0);
        return;
    }

    g_host.controller    = controller;
    g_host.webview_ready = true;
    KillTimer(g_host.hwnd, kLoadingTimer);
    trace_startup(L"WebView2 控制器就绪（加载画面让位给页面）");

    ComPtr<ICoreWebView2Controller2> controller2;
    if (SUCCEEDED(controller->QueryInterface(IID_PPV_ARGS(&controller2)))) {
        // 与页面 CSS 的底色一致，避免页面加载前的白闪
        const COREWEBVIEW2_COLOR background{255, 0x07, 0x0d, 0x1c};
        controller2->put_DefaultBackgroundColor(background);
    }

    RECT bounds{};
    GetClientRect(g_host.hwnd, &bounds);
    controller->put_Bounds(bounds);

    if (FAILED(controller->get_CoreWebView2(&g_host.webview)) || !g_host.webview) {
        return;
    }

    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(g_host.webview->get_Settings(&settings)) && settings) {
        settings->put_AreDefaultContextMenusEnabled(FALSE);
        settings->put_IsStatusBarEnabled(FALSE);
        settings->put_AreDevToolsEnabled(TRUE); // 排障用；产品化时按构建类型关掉
        settings->put_IsZoomControlEnabled(FALSE);
        settings->put_IsScriptEnabled(TRUE);
    }

    g_host.webview->add_NavigationCompleted(
        Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                BOOL ok = FALSE;
                args->get_IsSuccess(&ok);
                if (ok) {
                    g_host.navigated_ok = true;
                    set_title(L"");
                    KillTimer(g_host.hwnd, kRetryTimer);
                    trace_startup(L"页面已加载");
                } else {
                    // 引擎还没起来是最常见的原因：隔一会儿重试，而不是让用户看白屏
                    set_title(L"等待引擎…");
                    g_host.status = L"正在等待引擎";
                    SetTimer(g_host.hwnd, kRetryTimer, kRetryIntervalMs, nullptr);
                }
                return S_OK;
            })
            .Get(),
        nullptr);

    navigate_once();
}

void attach_webview() {
    if (!g_host.environment || g_host.controller_started || g_host.hwnd == nullptr) {
        return;
    }
    g_host.controller_started = true;
    g_host.environment->CreateCoreWebView2Controller(
        g_host.hwnd,
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [](HRESULT controller_result, ICoreWebView2Controller* controller) -> HRESULT {
                on_controller_ready(controller_result, controller);
                return S_OK;
            })
            .Get());
}

// 环境创建不依赖窗口，所以尽早发起：WebView2 运行时冷启动是首屏最大的一段耗时，
// 让它和窗口创建、引擎拉起并行，而不是等这些都做完才开始。
HRESULT create_environment() {
    ComPtr<CoreWebView2EnvironmentOptions> environment_options;
    if (!g_host.browser_args.empty()) {
        environment_options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
        environment_options->put_AdditionalBrowserArguments(g_host.browser_args.c_str());
    }

    return CreateCoreWebView2EnvironmentWithOptions(
        nullptr, g_host.user_data_dir.c_str(),
        environment_options ? environment_options.Get() : nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result) || env == nullptr) {
                    wchar_t message[256];
                    std::swprintf(message, 256,
                                  L"无法创建 WebView2 环境（0x%08lX）。\n\n"
                                  L"通常是缺少 Microsoft Edge WebView2 Runtime。",
                                  static_cast<unsigned long>(result));
                    if (g_host.hwnd != nullptr) {
                        MessageBoxW(g_host.hwnd, message, kWindowTitle, MB_ICONERROR | MB_OK);
                        PostMessageW(g_host.hwnd, WM_CLOSE, 0, 0);
                    } else {
                        // 窗口还没建起来就失败：直接退出，别让进程留在后台
                        MessageBoxW(nullptr, message, kWindowTitle, MB_ICONERROR | MB_OK);
                        PostQuitMessage(0);
                    }
                    return result;
                }
                g_host.environment = env;
                trace_startup(L"WebView2 环境就绪");
                attach_webview();
                return S_OK;
            })
            .Get());
}

// ---------- 引擎：探测 / 拉起 / 生命周期 ----------

std::wstring quote(const std::wstring& text) {
    std::wstring out{L"\""};
    out.append(text);
    out.push_back(L'"');
    return out;
}

// 只认自家引擎的 /healthz（{"ok":true,...}）。端口被别的程序占着时返回 false，
// 让外层去拉引擎——拉不起来会在日志里说清楚，比"界面永远转圈"好排障。
bool engine_present(std::uint16_t port) {
    const SOCKET socket_handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_handle == INVALID_SOCKET) {
        return false;
    }

    sockaddr_in address{};
    address.sin_family      = AF_INET;
    address.sin_port        = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    u_long nonblocking = 1;
    ::ioctlsocket(socket_handle, FIONBIO, &nonblocking);

    bool connected = false;
    if (::connect(socket_handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
        connected = true;
    } else if (::WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(socket_handle, &writable);
        timeval timeout{0, 200'000};
        if (::select(0, nullptr, &writable, nullptr, &timeout) > 0) {
            int error = 0;
            int length = sizeof(error);
            ::getsockopt(socket_handle, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length);
            connected = error == 0;
        }
    }

    bool found = false;
    if (connected) {
        const char request[] = "GET /healthz HTTP/1.0\r\nConnection: close\r\n\r\n";
        ::send(socket_handle, request, static_cast<int>(sizeof(request) - 1), 0);

        std::string response;
        const ULONGLONG deadline = ::GetTickCount64() + 300;
        char buffer[512];
        for (;;) {
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(socket_handle, &readable);
            timeval timeout{0, 50'000};
            const int ready = ::select(0, &readable, nullptr, nullptr, &timeout);
            if (ready < 0) {
                break;
            }
            if (ready == 0) {
                if (::GetTickCount64() > deadline) {
                    break;
                }
                continue;
            }
            const int received = ::recv(socket_handle, buffer, sizeof(buffer), 0);
            if (received <= 0) {
                break;
            }
            response.append(buffer, static_cast<std::size_t>(received));
            if (response.size() > 4096) {
                break;
            }
        }
        found = response.find("\"ok\":true") != std::string::npos;
    }

    ::closesocket(socket_handle);
    return found;
}

// 端口被占用时不要和用户干瞪眼：挑一个能 bind 的端口给引擎用
bool port_is_free(std::uint16_t port) {
    const SOCKET handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (handle == INVALID_SOCKET) {
        return false;
    }
    BOOL exclusive = TRUE;
    ::setsockopt(handle, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                 reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));

    sockaddr_in address{};
    address.sin_family      = AF_INET;
    address.sin_port        = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const bool free = ::bind(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
    ::closesocket(handle);
    return free;
}

struct PortChoice {
    std::uint16_t port = 0;
    bool attached      = false; // 已有自家引擎在跑
    bool ok            = false;
};

// 从 base 起扫 10 个端口：先找已有引擎（可以附着），再找空闲端口（可以新建）。
//
// 顺序很关键：先用 bind 判断"有没有人在听"，只对有人听的端口才发 /healthz。
// 反过来先探测会踩坑——不少机器（防火墙/安全软件）对未监听端口的 SYN 是静默丢弃，
// 每个端口都要等满 connect 超时，10 个端口白等两秒，用户看到的都是"启动慢"。
PortChoice choose_engine_port(std::uint16_t base) {
    PortChoice choice;
    constexpr unsigned kSpan = 10;
    std::uint16_t candidates[kSpan]{};
    bool free_flags[kSpan]{};
    unsigned count = 0;
    for (unsigned candidate = base; candidate < static_cast<unsigned>(base) + kSpan; ++candidate) {
        if (candidate > 65535) {
            break;
        }
        candidates[count] = static_cast<std::uint16_t>(candidate);
        // 空闲 = 没人监听 = 不可能是自家引擎，省掉一次健康检查
        free_flags[count] = port_is_free(candidates[count]);
        ++count;
    }

    for (unsigned i = 0; i < count; ++i) {
        if (!free_flags[i] && engine_present(candidates[i])) {
            choice.port     = candidates[i];
            choice.attached = true;
            choice.ok       = true;
            return choice;
        }
    }
    for (unsigned i = 0; i < count; ++i) {
        if (free_flags[i]) {
            choice.port = candidates[i];
            choice.ok   = true;
            return choice;
        }
    }
    return choice;
}

std::filesystem::path exe_directory() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = ::GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return {};
    }
    return std::filesystem::path{buffer}.parent_path();
}

void write_log_header(HANDLE log, const std::wstring& command) {
    SYSTEMTIME now{};
    ::GetLocalTime(&now);

    wchar_t line[600];
    std::swprintf(line, 600, L"[shell] %04u-%02u-%02u %02u:%02u:%02u 启动引擎: %s\r\n",
                  static_cast<unsigned>(now.wYear), static_cast<unsigned>(now.wMonth),
                  static_cast<unsigned>(now.wDay), static_cast<unsigned>(now.wHour),
                  static_cast<unsigned>(now.wMinute), static_cast<unsigned>(now.wSecond),
                  command.c_str());

    const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, line, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) {
        return;
    }
    std::string utf8(static_cast<std::size_t>(bytes), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8.data(), bytes, nullptr, nullptr);
    DWORD written = 0;
    ::WriteFile(log, utf8.data(), static_cast<DWORD>(utf8.size() - 1), &written, nullptr);
}

bool spawn_engine(const std::wstring& source) {
    const std::filesystem::path directory = exe_directory();
    const std::filesystem::path engine    = directory / L"fgb-cli.exe";
    std::error_code ec;
    if (engine.empty() || !std::filesystem::exists(engine, ec)) {
        MessageBoxW(g_host.hwnd, L"找不到 fgb-cli.exe（应与本程序在同一目录）。", kWindowTitle,
                    MB_ICONERROR | MB_OK);
        return false;
    }

    std::wstring command = quote(engine.wstring()) + L" run --panel " +
                           std::to_wstring(static_cast<unsigned>(g_host.port));

    bool forwarded_source = false;
    for (const std::wstring& arg : g_host.engine_args) {
        if (arg == L"--source") {
            forwarded_source = true;
        }
    }
    if (!forwarded_source) {
        command.append(L" --source ");
        command.append(source);
    }
    for (const std::wstring& arg : g_host.engine_args) {
        command.push_back(L' ');
        command.append(quote(arg));
    }

    std::filesystem::create_directories(std::filesystem::path{g_host.engine_log}.parent_path(), ec);

    SECURITY_ATTRIBUTES inherit{};
    inherit.nLength        = sizeof(inherit);
    inherit.bInheritHandle = TRUE;
    HANDLE log = ::CreateFileW(g_host.engine_log.c_str(), FILE_APPEND_DATA,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    if (log != INVALID_HANDLE_VALUE) {
        write_log_header(log, command);
        startup.dwFlags    = STARTF_USESTDHANDLES;
        startup.hStdInput  = INVALID_HANDLE_VALUE;
        startup.hStdOutput = log;
        startup.hStdError  = log;
    }

    PROCESS_INFORMATION process{};
    // 日志打不开就不重定向，照样把引擎拉起来——没有输出不该挡住启动
    const BOOL ok = ::CreateProcessW(engine.c_str(), command.data(), nullptr, nullptr,
                                     log != INVALID_HANDLE_VALUE, CREATE_NO_WINDOW, nullptr,
                                     directory.c_str(), &startup, &process);
    if (log != INVALID_HANDLE_VALUE) {
        ::CloseHandle(log);
    }
    if (ok == 0) {
        MessageBoxW(g_host.hwnd, L"启动引擎失败（fgb-cli.exe 无法创建）。", kWindowTitle,
                    MB_ICONERROR | MB_OK);
        return false;
    }

    ::CloseHandle(process.hThread);

    if (g_host.engine_job == nullptr) {
        const HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
        if (job != nullptr) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            ::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info));
            g_host.engine_job = job;
        }
    }
    if (g_host.engine_job != nullptr &&
        ::AssignProcessToJobObject(g_host.engine_job, process.hProcess) == 0) {
        // 失败也能跑，只是关窗时可能留下引擎进程；不为此拦住启动
        ::OutputDebugStringW(L"FaceGaze Bridge: AssignProcessToJobObject failed\n");
    }

    g_host.engine_process = process.hProcess;
    g_host.engine_owned   = true;
    trace_startup(L"引擎已拉起（fgb-cli）");
    ::SetTimer(g_host.hwnd, kEngineTimer, kEnginePollMs, nullptr);
    return true;
}

// 引擎退出：界面已经连上过 = 用户按了"停止引擎"或引擎自己结束，跟着退出；
// 还没连上就退出 = 启动失败（最常见是没装 VRCFT 模块），给一次切 VMC 的机会。
void on_engine_exit(DWORD exit_code) {
    KillTimer(g_host.hwnd, kEngineTimer);
    if (g_host.engine_process != nullptr) {
        ::CloseHandle(g_host.engine_process);
        g_host.engine_process = nullptr;
    }

    // 界面里改了模块配置：原地拉起新引擎，窗口不动
    if (exit_code == kEngineRestartExit && g_host.engine_owned) {
        if (spawn_engine(g_host.default_source)) {
            if (g_host.navigated_ok) {
                // 页面已经加载过：不要重新导航，让它自己的 WebSocket 重连，
                // 避免重启时闪一下白屏/丢页面。
                set_title(L"重启引擎…");
                SetTimer(g_host.hwnd, kRestartTimer, 900, nullptr);
            } else {
                navigate_once();
            }
            return;
        }
        PostMessageW(g_host.hwnd, WM_CLOSE, 0, 0);
        return;
    }

    if (g_host.navigated_ok) {
        PostMessageW(g_host.hwnd, WM_CLOSE, 0, 0);
        return;
    }

    const std::wstring message =
        L"引擎启动失败（退出码 " + std::to_wstring(exit_code) + L"）。\n\n" +
        L"日志：" + g_host.engine_log + L"\n\n" +
        L"常见原因：没安装 VRCFT 模块、VRCFT 本体仍占用模块端口、8787 端口被其它程序占用。\n\n" +
        L"是否改用 VMC 输入重试？（需要 VSeeFace 等程序发 VMC 数据）";
    const int answer = MessageBoxW(g_host.hwnd, message.c_str(), kWindowTitle,
                                   MB_YESNO | MB_ICONERROR | MB_SETFOREGROUND);
    if (answer != IDYES) {
        PostMessageW(g_host.hwnd, WM_CLOSE, 0, 0);
        return;
    }

    std::vector<std::wstring> kept;
    for (std::size_t i = 0; i < g_host.engine_args.size(); ++i) {
        if (g_host.engine_args[i] == L"--source") {
            ++i; // 连同取值一起丢掉，否则 --source vrcft 和新加的 vmc 会同时生效
            continue;
        }
        kept.push_back(g_host.engine_args[i]);
    }
    g_host.engine_args = std::move(kept);
    g_host.default_source = L"vmc";

    if (spawn_engine(L"vmc")) {
        navigate_once();
    } else {
        PostMessageW(g_host.hwnd, WM_CLOSE, 0, 0);
    }
}

void poll_engine() {
    if (!g_host.engine_owned || g_host.engine_process == nullptr) {
        return;
    }
    DWORD code = 0;
    if (::GetExitCodeProcess(g_host.engine_process, &code) == 0 || code == STILL_ACTIVE) {
        return;
    }
    on_engine_exit(code);
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_SIZE:
        if (g_host.controller) {
            RECT bounds{};
            GetClientRect(hwnd, &bounds);
            g_host.controller->put_Bounds(bounds);
        }
        return 0;

    case WM_TIMER:
        if (wparam == kRetryTimer && !g_host.navigated_ok) {
            navigate_once();
        } else if (wparam == kEngineTimer) {
            poll_engine();
        } else if (wparam == kRestartTimer) {
            if (engine_present(g_host.port)) {
                set_title(L"");
                KillTimer(hwnd, kRestartTimer);
            }
        } else if (wparam == kLoadingTimer) {
            if (g_host.webview_ready) {
                KillTimer(hwnd, kLoadingTimer);
            } else {
                ++g_host.loading_tick;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        RECT client{};
        GetClientRect(hwnd, &client);
        HBRUSH background = CreateSolidBrush(RGB(0x07, 0x0d, 0x1c));
        FillRect(dc, &client, background);
        DeleteObject(background);
        if (!g_host.webview_ready) {
            paint_loading(dc, client);
        }
        EndPaint(hwnd, &paint);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1; // WM_PAINT 里整块重画，擦除只会闪一下

    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
        const UINT dpi = window_dpi();
        info->ptMinTrackSize.x = ::MulDiv(kMinWidthDip, static_cast<int>(dpi), 96);
        info->ptMinTrackSize.y = ::MulDiv(kMinHeightDip, static_cast<int>(dpi), 96);
        return 0;
    }

    case WM_CLOSE:
        // WebView2 要求先关控制器再销毁窗口，否则会在退出时留下
        // "msedgewebview2.exe 仍在运行"的尾巴
        if (g_host.controller) {
            g_host.controller->Close();
            g_host.controller.Reset();
        }
        g_host.webview.Reset();
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

void enable_dpi_awareness() {
    // 动态取，避免为了一个 API 提高 minSdk（Win10 1703 才有 PerMonitorV2）
    using SetDpiAwarenessFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    const HMODULE user32   = GetModuleHandleW(L"user32.dll");
    if (user32 == nullptr) {
        return;
    }
    const auto set_context = reinterpret_cast<SetDpiAwarenessFn>(
        reinterpret_cast<void*>(GetProcAddress(user32, "SetProcessDpiAwarenessContext")));
    if (set_context != nullptr) {
        set_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }
}

void apply_dark_title_bar(HWND hwnd) {
    // DWMWA_USE_IMMERSIVE_DARK_MODE = 20（Win10 1809+）；老系统失败就保持系统配色
    const BOOL enabled = TRUE;
    using DwmSetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    static const auto set_attribute = reinterpret_cast<DwmSetWindowAttributeFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"dwmapi.dll"),
                                               "DwmSetWindowAttribute")));
    if (set_attribute != nullptr) {
        set_attribute(hwnd, 20, &enabled, sizeof(enabled));
    }
}

struct Options {
    std::uint16_t port = 8787;
    int debug_port     = 0;
    std::wstring url;
    std::wstring user_data_dir = default_user_data_dir();
    // 认不出来的参数原样转给引擎：--source vmc / --vmc-out / --vrchat 这类不用外壳理解
    std::vector<std::wstring> engine_args;
};

Options parse_options(int argc, wchar_t** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--port" && i + 1 < argc) {
            options.port = static_cast<std::uint16_t>(_wtoi(argv[++i]));
        } else if (arg == L"--url" && i + 1 < argc) {
            options.url = argv[++i];
        } else if (arg == L"--user-data" && i + 1 < argc) {
            options.user_data_dir = argv[++i];
        } else if (arg == L"--debug-port" && i + 1 < argc) {
            options.debug_port = _wtoi(argv[++i]);
        } else {
            options.engine_args.push_back(arg);
        }
    }
    return options;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    g_host.started_ms = ::GetTickCount64();
    g_host.engine_log = local_app_data() + L"\\FaceGazeBridge\\engine.log";
    std::error_code log_dir_ec;
    std::filesystem::create_directories(std::filesystem::path{g_host.engine_log}.parent_path(),
                                        log_dir_ec);
    trace_startup(L"进程启动");

    enable_dpi_awareness();

    int argc       = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const Options options = parse_options(argc, argv);
    if (argv != nullptr) {
        LocalFree(argv);
    }

    WSADATA wsa{};
    const bool wsa_ok = ::WSAStartup(MAKEWORD(2, 2), &wsa) == 0;

    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) {
        MessageBoxW(nullptr, L"COM 初始化失败。", kWindowTitle, MB_ICONERROR | MB_OK);
        if (wsa_ok) {
            ::WSACleanup();
        }
        return 1;
    }

    g_host.user_data_dir = options.user_data_dir;
    g_host.port          = options.port;
    g_host.engine_args   = options.engine_args;
    // 产品默认走 VRCFT 模块；没模块时失败弹窗可以一键切 VMC
    g_host.default_source = L"vrcft";
    if (options.debug_port > 0) {
        g_host.browser_args = L"--remote-debugging-port=" +
                              std::to_wstring(options.debug_port);
    }

    // 先把 WebView2 运行时叫醒：窗口、引擎都很快，剩下的等待几乎全在它身上
    const HRESULT environment_hr = create_environment();
    if (FAILED(environment_hr)) {
        trace_startup(L"WebView2 加载器调用失败");
        wchar_t message[256];
        std::swprintf(message, 256, L"WebView2 加载器调用失败（0x%08lX）。",
                      static_cast<unsigned long>(environment_hr));
        MessageBoxW(nullptr, message, kWindowTitle, MB_ICONERROR | MB_OK);
        CoUninitialize();
        if (wsa_ok) {
            ::WSACleanup();
        }
        return 1;
    }

    const HINSTANCE module = instance != nullptr ? instance : GetModuleHandleW(nullptr);

    WNDCLASSEXW window_class{};
    window_class.cbSize        = sizeof(window_class);
    window_class.style         = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc   = window_proc;
    window_class.hInstance     = module;
    window_class.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = CreateSolidBrush(RGB(0x07, 0x0d, 0x1c));
    window_class.lpszClassName = kWindowClass;
    window_class.hIcon         = LoadIconW(module, MAKEINTRESOURCEW(kIconResource));
    window_class.hIconSm       = reinterpret_cast<HICON>(LoadImageW(
        module, MAKEINTRESOURCEW(kIconResource), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
    if (RegisterClassExW(&window_class) == 0) {
        CoUninitialize();
        if (wsa_ok) {
            ::WSACleanup();
        }
        return 1;
    }

    // 默认尺寸按 DPI 放大后再收进工作区：高分屏上 1180x780 的物理像素只有 ~940 DIP
    UINT dpi = 96;
    using GetDpiForSystemFn = UINT(WINAPI*)();
    const auto get_system_dpi = reinterpret_cast<GetDpiForSystemFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem")));
    if (get_system_dpi != nullptr) {
        const UINT value = get_system_dpi();
        if (value != 0) {
            dpi = value;
        }
    }

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int work_width  = static_cast<int>(work.right - work.left);
    const int work_height = static_cast<int>(work.bottom - work.top);
    const int wanted_w    = ::MulDiv(kDefaultWidthDip, static_cast<int>(dpi), 96);
    const int wanted_h    = ::MulDiv(kDefaultHeightDip, static_cast<int>(dpi), 96);
    const int width       = std::min(wanted_w, work_width > 0 ? work_width - 80 : wanted_w);
    const int height      = std::min(wanted_h, work_height > 0 ? work_height - 80 : wanted_h);
    const int x           = work.left + (work_width - width) / 2;
    const int y           = work.top + (work_height - height) / 2;

    g_host.hwnd = CreateWindowExW(0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW, x, y, width,
                                  height, nullptr, nullptr, module, nullptr);
    if (g_host.hwnd == nullptr) {
        CoUninitialize();
        if (wsa_ok) {
            ::WSACleanup();
        }
        return 1;
    }

    apply_dark_title_bar(g_host.hwnd);
    ShowWindow(g_host.hwnd, show_command == 0 ? SW_SHOWNORMAL : show_command);
    UpdateWindow(g_host.hwnd);
    g_host.status = L"正在加载界面";
    SetTimer(g_host.hwnd, kLoadingTimer, kLoadingTickMs, nullptr);
    trace_startup(L"窗口已显示");

    // 先看有没有自家引擎（可能在新端口上），没有再挑一个空闲端口新建。
    // 8787 被别的程序占用时自动用 8788/8789…，界面地址跟着变。
    bool attached = false;
    if (options.url.empty()) {
        const PortChoice choice = choose_engine_port(options.port);
        if (!choice.ok) {
            MessageBoxW(g_host.hwnd,
                        L"8787–8796 端口都被占用了，无法启动控制面。\n"
                        L"请关闭占用这些端口的程序，或用 --port 指定别的起始端口。",
                        kWindowTitle, MB_ICONERROR | MB_OK);
            CoUninitialize();
            if (wsa_ok) {
                ::WSACleanup();
            }
            return 1;
        }
        g_host.port = choice.port;
        attached    = choice.attached;
        trace_startup(L"引擎端口已选定");
        wchar_t buffer[128];
        std::swprintf(buffer, 128, L"http://127.0.0.1:%u/",
                      static_cast<unsigned>(g_host.port));
        g_host.url = buffer;
    } else {
        g_host.url = options.url;
        attached   = wsa_ok && engine_present(g_host.port);
    }

    // 已有引擎（例如命令行起的）就只连不动；否则拉一个，藏在后台。
    if (attached) {
        trace_startup(L"已有引擎在跑，直接连接");
    } else if (!spawn_engine(g_host.default_source)) {
        // 拉不起来就别留一块永远转圈的窗口；spawn_engine 已经弹过原因
        PostMessageW(g_host.hwnd, WM_CLOSE, 0, 0);
    } else {
        // 只有页面在我们自己的端口上时才等引擎应答；--url 指到别处就照旧直接导航
        g_host.expect_engine = options.url.empty();
    }

    // 环境可能已就绪，也可能还在起；两条路都会走到 attach_webview
    attach_webview();

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (g_host.controller) {
        g_host.controller->Close();
        g_host.controller.Reset();
    }
    g_host.webview.Reset();

    // Job 句柄一关，自己拉起的引擎（及其模块子进程）随之结束
    if (g_host.engine_job != nullptr) {
        ::CloseHandle(g_host.engine_job);
        g_host.engine_job = nullptr;
    }
    if (g_host.engine_process != nullptr) {
        ::CloseHandle(g_host.engine_process);
        g_host.engine_process = nullptr;
    }

    CoUninitialize();
    if (wsa_ok) {
        ::WSACleanup();
    }
    return 0;
}
