// FaceGaze Bridge —— 桌面入口（WebView2 外壳 + 引擎拉起 + 托盘常驻）。
//
// 产品形态：用户只双击一个 exe。外壳先探一下控制面端口：
//   - 已经有引擎在跑（例如命令行起的）→ 直接连，不动它
//   - 没有 → 隐藏控制台拉起同目录的 fgb-cli.exe，并把它的输出写进
//     %LOCALAPPDATA%\FaceGazeBridge\engine.log
//
// 生命周期用 Job 对象绑定：外壳退出（正常关闭或崩溃）都会带走自己拉起的引擎，
// 不留后台进程。别人起的引擎不受影响（没有句柄关系）。
//
// 关窗语义（ADR-0022 修订 ADR-0016）：关闭窗口 = 收进托盘，追踪继续；托盘「退出」
// 才停引擎。`--tray` 启动连窗口都不显示，只驻留托盘——SteamVR 自启动要的就是这个。
//
// 页面仍从 http://127.0.0.1:<port>/ 加载，外壳里没有业务逻辑：
//   1. 页面直接连 ws://127.0.0.1:<port>/ws，浏览器打开同一地址也能调试
//   2. 引擎与界面是两个进程，改 UI 不用重编 C++
//   3. 页面与 WebSocket 同源同端口，绕开 mixed-content 限制
//
// 运行时依赖：WebView2 Evergreen Runtime。Win10/11 通常已随 Edge 预装。
#include "base/crash.hpp"

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

// 托盘：图标回调消息与菜单项 id
constexpr UINT kTrayCallback = WM_APP + 21;
constexpr UINT kMenuShow     = 501;
constexpr UINT kMenuEngine   = 502;
constexpr UINT kMenuRestart  = 503;
constexpr UINT kMenuExit     = 504;
// 单实例：第二个进程不该再开一个窗口/托盘图标，只把第一个带到前台
constexpr wchar_t kInstanceMutex[] = L"Local\\FaceGazeBridge.Shell";
constexpr wchar_t kRestoreMessage[] = L"FaceGazeBridge.RestoreWindow";

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

    // ---- 托盘与生命周期（ADR-0022）----
    bool tray_mode         = false; // --tray：启动时不显示窗口
    bool tray_icon         = false;
    bool quitting          = false; // 只有它为 true 时关窗才真的退出
    bool webview_requested = false; // 托盘启动时按需创建 WebView2，不先养两个浏览器进程
    bool close_hint_shown  = false;
    bool engine_alive      = false; // 托盘菜单据此显示"启动引擎"还是"停止引擎"
    bool tray_vmc_fallback = false;
    bool stop_requested    = false; // 我们自己点了「停止引擎」
    ULONGLONG engine_started_ms = 0;
    HANDLE instance_mutex  = nullptr;
    UINT restore_message   = 0;
    UINT taskbar_created   = 0; // 资源管理器重启后要重新加回托盘图标
    NOTIFYICONDATAW tray{};
};

Host g_host;

bool engine_present(std::uint16_t port);
bool http_control(std::uint16_t port, const char* path);
HRESULT create_environment();
void attach_webview();
void navigate_once();
bool spawn_engine(const std::wstring& source);

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

// ---------- 托盘（ADR-0022） ----------
//
// 托盘是 SteamVR 自启动的硬前置：自启动拉起时不能弹窗口，只能驻留托盘。
// 同时它把"关窗"的语义从"停引擎"改成"收进托盘"——这一点必须在文档里说清，
// 否则用户关了窗口以为追踪停了，实际还在输出。

struct TrayText {
    const wchar_t* show;
    const wchar_t* stop;
    const wchar_t* start;
    const wchar_t* restart;
    const wchar_t* quit;
    const wchar_t* running;
    const wchar_t* stopped;
    const wchar_t* first_hide;
    const wchar_t* engine_failed;
    const wchar_t* vmc_fallback;
};

// 托盘文案跟系统 UI 语言走：它是原生外壳，拿不到页面的 localStorage 选择
const TrayText& tray_text() {
    static const TrayText zh{
        L"显示窗口", L"停止引擎", L"启动引擎", L"重启引擎", L"退出",
        L"引擎运行中", L"引擎已停止",
        L"FaceGaze Bridge 仍在托盘运行，追踪继续；右键托盘图标可以退出。",
        L"引擎启动失败，右键托盘图标查看日志或重试。",
        L"没有可用的 VRCFT 模块，已改用 VMC 输入重试。"};
    static const TrayText en{
        L"Show window", L"Stop engine", L"Start engine", L"Restart engine", L"Exit",
        L"Engine running", L"Engine stopped",
        L"FaceGaze Bridge keeps running in the tray; tracking continues. Right-click to exit.",
        L"Engine failed to start. Right-click the tray icon to check the log or retry.",
        L"No VRCFT module available; retrying with VMC input."};
    static const TrayText ja{
        L"ウィンドウを表示", L"エンジンを停止", L"エンジンを起動", L"エンジンを再起動", L"終了",
        L"エンジン稼働中", L"エンジン停止",
        L"FaceGaze Bridge はトレイで動作を続けます。右クリックで終了できます。",
        L"エンジンの起動に失敗しました。トレイを右クリックしてログを確認するか再試行してください。",
        L"利用できる VRCFT モジュールがないため、VMC 入力で再試行します。"};

    switch (::GetUserDefaultUILanguage() & 0xFF) {
    case 0x04: // 中文
        return zh;
    case 0x11: // 日本語
        return ja;
    default:
        return en;
    }
}

void add_tray_icon() {
    if (g_host.hwnd == nullptr || g_host.tray_icon) {
        return;
    }
    const HINSTANCE module = ::GetModuleHandleW(nullptr);
    // 小图标尺寸跟系统 DPI 走：固定 32×32 在 150% 缩放下会被托盘缩成糊的
    const int size = ::GetSystemMetrics(SM_CXSMICON);
    HICON icon = static_cast<HICON>(::LoadImageW(module, MAKEINTRESOURCEW(kIconResource),
                                                 IMAGE_ICON, size > 0 ? size : 16,
                                                 size > 0 ? size : 16, LR_DEFAULTCOLOR));
    if (icon == nullptr) {
        icon = ::LoadIconW(module, MAKEINTRESOURCEW(kIconResource));
    }

    g_host.tray            = {};
    g_host.tray.cbSize     = sizeof(g_host.tray);
    g_host.tray.hWnd       = g_host.hwnd;
    g_host.tray.uID        = 1;
    g_host.tray.uFlags     = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_host.tray.uCallbackMessage = kTrayCallback;
    g_host.tray.hIcon      = icon;
    ::wcsncpy_s(g_host.tray.szTip, kWindowTitle, _TRUNCATE);

    g_host.tray_icon = ::Shell_NotifyIconW(NIM_ADD, &g_host.tray) != FALSE;
    if (g_host.tray_icon) {
        const std::wstring tip = std::wstring{kWindowTitle} + L" — " +
                                 (g_host.engine_alive ? tray_text().running : tray_text().stopped);
        ::wcsncpy_s(g_host.tray.szTip, tip.c_str(), _TRUNCATE);
        g_host.tray.uFlags = NIF_TIP;
        ::Shell_NotifyIconW(NIM_MODIFY, &g_host.tray);
        g_host.tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    }
}

void remove_tray_icon() {
    if (g_host.tray_icon) {
        ::Shell_NotifyIconW(NIM_DELETE, &g_host.tray);
        g_host.tray_icon = false;
    }
    if (g_host.tray.hIcon != nullptr) {
        ::DestroyIcon(g_host.tray.hIcon);
        g_host.tray.hIcon = nullptr;
    }
}

// 托盘气泡：托盘模式没有窗口，引擎失败/首次收进托盘只能靠它说明白
void show_tray_balloon(const wchar_t* text) {
    if (text == nullptr || text[0] == L'\0') {
        return;
    }
    if (!g_host.tray_icon) {
        trace_startup(text);
        return;
    }
    ::wcsncpy_s(g_host.tray.szInfoTitle, kWindowTitle, _TRUNCATE);
    ::wcsncpy_s(g_host.tray.szInfo, text, _TRUNCATE);
    g_host.tray.dwInfoFlags = NIIF_INFO;
    g_host.tray.uFlags      = NIF_INFO;
    ::Shell_NotifyIconW(NIM_MODIFY, &g_host.tray);
    g_host.tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
}

void update_tray_state() {
    if (!g_host.tray_icon) {
        return;
    }
    const TrayText& text = tray_text();
    std::wstring tip{kWindowTitle};
    tip.append(L" — ");
    tip.append(g_host.engine_alive ? text.running : text.stopped);
    ::wcsncpy_s(g_host.tray.szTip, tip.c_str(), _TRUNCATE);
    g_host.tray.uFlags = NIF_TIP;
    ::Shell_NotifyIconW(NIM_MODIFY, &g_host.tray);
    g_host.tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
}

void show_main_window() {
    if (g_host.hwnd == nullptr) {
        return;
    }
    // WebView2 按需创建：托盘常驻时不该为了一个没打开的窗口先起两个浏览器进程
    if (!g_host.webview_requested) {
        g_host.webview_requested = true;
        if (g_host.environment) {
            attach_webview();
        } else {
            create_environment();
        }
        if (!g_host.webview_ready) {
            g_host.status = L"正在加载界面";
            SetTimer(g_host.hwnd, kLoadingTimer, kLoadingTickMs, nullptr);
        }
    }
    ::ShowWindow(g_host.hwnd, SW_SHOWNORMAL);
    ::SetForegroundWindow(g_host.hwnd);
    attach_webview();
}

void hide_main_window() {
    if (g_host.hwnd == nullptr) {
        return;
    }
    ::ShowWindow(g_host.hwnd, SW_HIDE);
    if (!g_host.close_hint_shown) {
        // 第一次收进托盘必须说一句：否则用户以为"关掉就停了追踪"
        g_host.close_hint_shown = true;
        show_tray_balloon(tray_text().first_hide);
    }
}

void quit_app() {
    g_host.quitting = true;
    if (g_host.hwnd != nullptr) {
        ::PostMessageW(g_host.hwnd, WM_CLOSE, 0, 0);
    } else {
        ::PostQuitMessage(0);
    }
}

// 启动失败时的报错：窗口在就弹窗（用户看得见），在托盘就记日志 + 气泡
void report_error(const std::wstring& text) {
    trace_startup(text.c_str());
    if (g_host.hwnd != nullptr && ::IsWindowVisible(g_host.hwnd)) {
        ::MessageBoxW(g_host.hwnd, text.c_str(), kWindowTitle, MB_ICONERROR | MB_OK);
    } else {
        show_tray_balloon(text.c_str());
    }
}

void handle_tray_command(UINT command) {
    switch (command) {
    case kMenuShow:
        show_main_window();
        break;
    case kMenuEngine:
        if (g_host.engine_alive) {
            g_host.stop_requested = true;
            http_control(g_host.port, "/api/quit");
        } else if (spawn_engine(g_host.default_source)) {
            g_host.engine_alive = true;
            update_tray_state();
            if (g_host.webview_requested) {
                navigate_once();
            }
        }
        break;
    case kMenuRestart:
        if (g_host.engine_alive) {
            // 引擎退出码 3 → on_engine_exit 原地重启，窗口不重新导航
            http_control(g_host.port, "/api/restart");
        } else if (spawn_engine(g_host.default_source)) {
            g_host.engine_alive = true;
            update_tray_state();
            if (g_host.webview_requested) {
                navigate_once();
            }
        }
        break;
    case kMenuExit:
        quit_app();
        break;
    default:
        break;
    }
}

void show_tray_menu() {
    if (g_host.hwnd == nullptr) {
        return;
    }
    const TrayText& text = tray_text();
    HMENU menu = ::CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }
    ::AppendMenuW(menu, MF_STRING, kMenuShow, text.show);
    ::SetMenuDefaultItem(menu, kMenuShow, FALSE);
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    UINT engine_flags = MF_STRING;
    if (!g_host.engine_alive) {
        // 停止/重启只在引擎活着时可用；"启动引擎"则相反
        engine_flags = MF_STRING | (g_host.engine_alive ? 0 : MF_GRAYED);
    }
    ::AppendMenuW(menu, engine_flags, kMenuEngine,
                  g_host.engine_alive ? text.stop : text.start);
    ::AppendMenuW(menu, MF_STRING | (g_host.engine_alive ? 0 : MF_GRAYED), kMenuRestart,
                  text.restart);
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kMenuExit, text.quit);

    POINT cursor{};
    ::GetCursorPos(&cursor);
    // 托盘菜单的经典要求：先把所有者设为前台，否则点别处菜单不消失
    ::SetForegroundWindow(g_host.hwnd);
    const UINT command = ::TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                          cursor.x, cursor.y, 0, g_host.hwnd, nullptr);
    ::DestroyMenu(menu);
    ::PostMessageW(g_host.hwnd, WM_NULL, 0, 0);
    if (command != 0) {
        handle_tray_command(command);
    }
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
                      quit_app(); // 没有 WebView2 就没有界面，留在托盘没有意义
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
                        quit_app();
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

// 回环 TCP 连接（非阻塞 + 短等待）。连不上就说明没有程序在听这个端口。
bool connect_loopback(SOCKET socket_handle, std::uint16_t port) {
    sockaddr_in address{};
    address.sin_family      = AF_INET;
    address.sin_port        = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    u_long nonblocking = 1;
    ::ioctlsocket(socket_handle, FIONBIO, &nonblocking);

    if (::connect(socket_handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
        return true;
    }
    if (::WSAGetLastError() != WSAEWOULDBLOCK) {
        return false;
    }

    fd_set writable;
    FD_ZERO(&writable);
    FD_SET(socket_handle, &writable);
    timeval timeout{0, 200'000};
    if (::select(0, nullptr, &writable, nullptr, &timeout) <= 0) {
        return false;
    }
    int error  = 0;
    int length = sizeof(error);
    ::getsockopt(socket_handle, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length);
    return error == 0;
}

// 只认自家引擎的 /healthz（{"ok":true,...}）。端口被别的程序占着时返回 false，
// 让外层去拉引擎——拉不起来会在日志里说清楚，比"界面永远转圈"好排障。
bool engine_present(std::uint16_t port) {
    const SOCKET socket_handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_handle == INVALID_SOCKET) {
        return false;
    }

    const bool connected = connect_loopback(socket_handle, port);

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

// 让引擎自己收尾（停止/重启）。托盘菜单用得到，而外壳不该为此实现一套 WebSocket
// 客户端：控制面本来就是"本机 HTTP + WebSocket"，外壳只碰这一个动作。
bool http_control(std::uint16_t port, const char* path) {
    if (path == nullptr) {
        return false;
    }
    const SOCKET socket_handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_handle == INVALID_SOCKET) {
        return false;
    }
    if (!connect_loopback(socket_handle, port)) {
        ::closesocket(socket_handle);
        return false;
    }

    std::string request{"POST "};
    request.append(path);
    request.append(" HTTP/1.0\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    const bool sent =
        ::send(socket_handle, request.data(), static_cast<int>(request.size()), 0) > 0;

    // 半关并读干净响应：直接 close 有可能在引擎处理请求前就把连接 RST 掉
    ::shutdown(socket_handle, SD_SEND);
    char buffer[256];
    const ULONGLONG deadline = ::GetTickCount64() + 300;
    while (::GetTickCount64() < deadline) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(socket_handle, &readable);
        timeval timeout{0, 50'000};
        if (::select(0, &readable, nullptr, nullptr, &timeout) <= 0) {
            continue;
        }
        if (::recv(socket_handle, buffer, sizeof(buffer), 0) <= 0) {
            break;
        }
    }
    ::closesocket(socket_handle);
    return sent;
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
        report_error(L"找不到 fgb-cli.exe（应与本程序在同一目录）。");
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
        report_error(L"启动引擎失败（fgb-cli.exe 无法创建）。");
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

    g_host.engine_process    = process.hProcess;
    g_host.engine_owned      = true;
    g_host.engine_alive      = true;
    g_host.stop_requested    = false;
    g_host.engine_started_ms = ::GetTickCount64();
    trace_startup(L"引擎已拉起（fgb-cli）");
    // 引擎可能已经在跑（我们只是重新拉起），托盘菜单状态立刻跟上
    update_tray_state();
    ::SetTimer(g_host.hwnd, kEngineTimer, kEnginePollMs, nullptr);
    return true;
}

// 切到 VMC 重试：把 --source 的旧值一起丢掉，否则 --source vrcft 与新的 vmc 会同时生效
void switch_to_vmc() {
    std::vector<std::wstring> kept;
    for (std::size_t i = 0; i < g_host.engine_args.size(); ++i) {
        if (g_host.engine_args[i] == L"--source") {
            ++i;
            continue;
        }
        kept.push_back(g_host.engine_args[i]);
    }
    g_host.engine_args   = std::move(kept);
    g_host.default_source = L"vmc";
}

// 引擎退出：界面已经连上过 = 用户按了"停止引擎"或引擎自己结束；
// 还没连上就退出 = 启动失败（最常见是没装 VRCFT 模块），给一次切 VMC 的机会。
void on_engine_exit(DWORD exit_code) {
    KillTimer(g_host.hwnd, kEngineTimer);
    if (g_host.engine_process != nullptr) {
        ::CloseHandle(g_host.engine_process);
        g_host.engine_process = nullptr;
    }
    g_host.engine_alive = false;
    update_tray_state();

    // 分清"用户要它停"与"它启动失败了"：托盘菜单点了停止、界面点了停止、
    // 或者它已经正常跑了 3 秒以上，都算前者。否则会对着"退出码 0"报启动失败。
    const bool intentional_stop =
        g_host.stop_requested ||
        (g_host.engine_started_ms != 0 &&
         ::GetTickCount64() - g_host.engine_started_ms > 3000);
    g_host.stop_requested = false;

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
        if (g_host.tray_icon && !g_host.quitting) {
            show_tray_balloon(tray_text().engine_failed);
            return;
        }
        quit_app();
        return;
    }

    if (intentional_stop || g_host.navigated_ok) {
        // 托盘形态：引擎停了但窗口留在托盘，用户可以再启动（ADR-0022）
        if (g_host.tray_icon && !g_host.quitting) {
            set_title(L"引擎已停止");
            show_tray_balloon(tray_text().stopped);
            return;
        }
        quit_app();
        return;
    }

    // ---- 还没起来就退出了：启动失败 ----
    if (g_host.tray_mode && !g_host.quitting) {
        // 自启动/托盘场景不该弹模态框（用户不在电脑前），但也不能静默失败：
        // 先按老规矩尝试一次 VMC（有气泡说明），仍失败就留在托盘等用户处理
        if (!g_host.tray_vmc_fallback && exit_code != 0) {
            g_host.tray_vmc_fallback = true;
            switch_to_vmc();
            if (spawn_engine(L"vmc")) {
                show_tray_balloon(tray_text().vmc_fallback);
                return;
            }
        }
        const std::wstring line = L"引擎启动失败（退出码 " + std::to_wstring(exit_code) +
                                  L"），留在托盘等待重试";
        trace_startup(line.c_str());
        show_tray_balloon(tray_text().engine_failed);
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
        quit_app();
        return;
    }

    switch_to_vmc();
    if (spawn_engine(L"vmc")) {
        navigate_once();
    } else {
        quit_app();
    }
}

void poll_engine() {
    if (g_host.engine_owned) {
        if (g_host.engine_process == nullptr) {
            return;
        }
        DWORD code = 0;
        if (::GetExitCodeProcess(g_host.engine_process, &code) == 0 || code == STILL_ACTIVE) {
            return;
        }
        on_engine_exit(code);
        return;
    }

    // 附着别人起的引擎：只观察、不接管。托盘菜单的状态要跟着变，
    // 否则用户会对着一个"停止引擎"点下去却什么都没发生。
    const bool alive = engine_present(g_host.port);
    if (alive != g_host.engine_alive) {
        g_host.engine_alive = alive;
        set_title(alive ? L"" : L"引擎已停止");
        update_tray_state();
        if (!alive) {
            show_tray_balloon(tray_text().stopped);
        }
    }
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    // 第二个实例发来的"把窗口带出来"：窗口可能在托盘里藏着
    if (g_host.restore_message != 0 && message == g_host.restore_message) {
        show_main_window();
        return 0;
    }

    // 资源管理器崩溃/重启后托盘图标会消失，这是 Shell_NotifyIcon 的经典问题：
    // 收到系统的 TaskbarCreated 广播就重新加一次，否则用户会以为程序没了
    if (g_host.taskbar_created != 0 && message == g_host.taskbar_created) {
        remove_tray_icon(); // 旧图标属于已经不存在的托盘，顺手把句柄释放掉
        add_tray_icon();
        return 0;
    }

    switch (message) {
    case kTrayCallback:
        switch (LOWORD(lparam)) {
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
            // 单击就能打开：托盘应用最常见的抱怨是"点了没反应"
            show_main_window();
            break;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            show_tray_menu();
            break;
        default:
            break;
        }
        return 0;

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
        // 关窗 ≠ 退出（ADR-0022）：收进托盘，追踪继续；托盘「退出」才停引擎。
        // 没有托盘图标时不能藏窗口——那会留下一个用户点不到的进程。
        if (!g_host.quitting && g_host.tray_icon) {
            hide_main_window();
            return 0;
        }
        // WebView2 要求先关控制器再销毁窗口，否则会在退出时留下
        // "msedgewebview2.exe 仍在运行"的尾巴
        if (g_host.controller) {
            g_host.controller->Close();
            g_host.controller.Reset();
        }
        g_host.webview.Reset();
        DestroyWindow(hwnd);
        return 0;

    case WM_QUERYENDSESSION:
        // 关机/注销：允许系统结束我们，否则会被当成"阻止关机"
        g_host.quitting = true;
        return TRUE;

    case WM_ENDSESSION:
        g_host.quitting = true;
        return 0;

    case WM_DESTROY:
        remove_tray_icon();
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
    bool port_explicit = false; // 显式 --port 表示"attach 到这个引擎"，不是普通双击
    int debug_port     = 0;
    bool tray          = false; // --tray：只驻留托盘，启动时不弹窗口（SteamVR 自启动用）
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
            options.port          = static_cast<std::uint16_t>(_wtoi(argv[++i]));
            options.port_explicit = true;
        } else if (arg == L"--url" && i + 1 < argc) {
            options.url = argv[++i];
        } else if (arg == L"--user-data" && i + 1 < argc) {
            options.user_data_dir = argv[++i];
        } else if (arg == L"--debug-port" && i + 1 < argc) {
            options.debug_port = _wtoi(argv[++i]);
        } else if (arg == L"--tray") {
            // 壳自己的参数，不能转给引擎（那边会当成未知参数退出码 2）
            options.tray = true;
        } else {
            options.engine_args.push_back(arg);
        }
    }
    return options;
}

// 已有实例就把它的窗口带到前台，自己退出。返回 true 表示"这次不用继续启动了"。
//
// 例外：带了 --debug-port / --url / --port 的是排障、探针或"attach 到指定引擎"的实例，
// 允许多开（AGENT.md §3.4：CDP 端口要靠单独的 user-data 才监听；
// fgb-cli --ui 就是用 --port 拉起外壳的，不能让它被已有窗口顶掉）。
bool restore_existing_instance(const Options& options) {
    if (options.debug_port > 0 || !options.url.empty() || options.port_explicit) {
        return false;
    }
    // 注册消息号要在两个实例里都注册：窗口自己要认这条消息，第二个实例要发它
    g_host.restore_message = ::RegisterWindowMessageW(kRestoreMessage);

    g_host.instance_mutex = ::CreateMutexW(nullptr, TRUE, kInstanceMutex);
    if (g_host.instance_mutex == nullptr || ::GetLastError() != ERROR_ALREADY_EXISTS) {
        return false; // 第一个实例：继续正常启动，互斥体活着直到进程退出
    }

    // 用广播而不是 FindWindow：注册消息会送到所有顶层窗口，自家窗口认出后显示自己。
    // 这样不用先知道对方的 HWND（托盘形态下窗口可能是隐藏的，按类名找并不总是可靠）。
    if (g_host.restore_message != 0) {
        ::PostMessageW(HWND_BROADCAST, g_host.restore_message, 0, 0);
        // 给第一个实例一点时间把窗口显示出来，再退出；否则用户会看到"什么都没发生"
        ::Sleep(150);
    }
    trace_startup(L"已有实例在运行，已请求恢复现有窗口");
    ::CloseHandle(g_host.instance_mutex);
    g_host.instance_mutex = nullptr;
    return true;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    fgb::crash::install(fgb::crash::default_crash_dir(), FGB_VERSION);
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

    // 单实例守卫必须在 COM/WebView2 之前：第二个进程只该是一次"叫醒"
    if (restore_existing_instance(options)) {
        return 0;
    }
    g_host.taskbar_created = ::RegisterWindowMessageW(L"TaskbarCreated");

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

    g_host.user_data_dir  = options.user_data_dir;
    g_host.port           = options.port;
    g_host.engine_args    = options.engine_args;
    g_host.tray_mode      = options.tray;
    // 产品默认走 VRCFT 模块；没模块时失败弹窗可以一键切 VMC
    g_host.default_source = L"vrcft";
    if (options.debug_port > 0) {
        g_host.browser_args = L"--remote-debugging-port=" +
                              std::to_wstring(options.debug_port);
    }

    // 先把 WebView2 运行时叫醒：窗口、引擎都很快，剩下的等待几乎全在它身上。
    // 托盘启动时反过来——用户可能整晚都不开窗口，没必要先养两个浏览器进程，
    // 等第一次「显示窗口」再按需创建（show_main_window）。
    if (!g_host.tray_mode) {
        g_host.webview_requested = true;
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
    } else {
        trace_startup(L"托盘启动：WebView2 与窗口都按需创建");
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
    if (!g_host.tray_mode) {
        ShowWindow(g_host.hwnd, show_command == 0 ? SW_SHOWNORMAL : show_command);
        UpdateWindow(g_host.hwnd);
        g_host.status = L"正在加载界面";
        SetTimer(g_host.hwnd, kLoadingTimer, kLoadingTickMs, nullptr);
        trace_startup(L"窗口已显示");
    } else {
        g_host.status = L"正在启动";
        trace_startup(L"托盘模式：窗口保持隐藏");
    }
    // 托盘图标两种模式都要有：发版形态下关闭窗口 = 收进托盘（ADR-0022）
    add_tray_icon();

    // 先看有没有自家引擎（可能在新端口上），没有再挑一个空闲端口新建。
    // 8787 被别的程序占用时自动用 8788/8789…，界面地址跟着变。
    bool attached = false;
    bool fatal    = false;
    if (options.url.empty()) {
        const PortChoice choice = choose_engine_port(options.port);
        if (!choice.ok) {
            report_error(L"8787–8796 端口都被占用了，无法启动控制面。\n"
                         L"请关闭占用这些端口的程序，或用 --port 指定别的起始端口。");
            fatal = true;
        } else {
            g_host.port = choice.port;
            attached    = choice.attached;
            trace_startup(L"引擎端口已选定");
            wchar_t buffer[128];
            std::swprintf(buffer, 128, L"http://127.0.0.1:%u/",
                          static_cast<unsigned>(g_host.port));
            g_host.url = buffer;
        }
    } else {
        g_host.url = options.url;
        attached   = wsa_ok && engine_present(g_host.port);
    }

    if (!fatal) {
        // 已有引擎（例如命令行起的）就只连不动；否则拉一个，藏在后台。
        if (attached) {
            g_host.engine_alive = true;
            trace_startup(L"已有引擎在跑，直接连接");
        } else if (!spawn_engine(g_host.default_source)) {
            // 拉不起来就别留一块永远转圈的窗口；spawn_engine 已经报过原因
            fatal = true;
        } else {
            // 只有页面在我们自己的端口上时才等引擎应答；--url 指到别处就照旧直接导航
            g_host.expect_engine = options.url.empty();
        }
    }

    if (fatal) {
        // 托盘形态下留在托盘里（用户可以重试或退出），窗口形态直接退出
        if (!g_host.tray_mode) {
            g_host.quitting = true;
            PostMessageW(g_host.hwnd, WM_CLOSE, 0, 0);
        }
    } else {
        // 附着别人的引擎时也要能发现它退出了：托盘菜单的状态来自这个轮询
        ::SetTimer(g_host.hwnd, kEngineTimer, kEnginePollMs, nullptr);

        // 环境可能已就绪，也可能还在起；两条路都会走到 attach_webview
        attach_webview();
    }

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
    remove_tray_icon();

    // Job 句柄一关，自己拉起的引擎（及其模块子进程）随之结束
    if (g_host.engine_job != nullptr) {
        ::CloseHandle(g_host.engine_job);
        g_host.engine_job = nullptr;
    }
    if (g_host.engine_process != nullptr) {
        ::CloseHandle(g_host.engine_process);
        g_host.engine_process = nullptr;
    }
    if (g_host.instance_mutex != nullptr) {
        ::CloseHandle(g_host.instance_mutex);
        g_host.instance_mutex = nullptr;
    }

    CoUninitialize();
    if (wsa_ok) {
        ::WSACleanup();
    }
    return 0;
}
