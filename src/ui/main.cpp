// fgb-ui —— WebView2 外壳。
//
// 这一层刻意做得极薄：建窗口、起 WebView2、导航到引擎的控制面地址，剩下的都在页面里。
// 为什么不让 C++ 自己发 WebSocket、再用 postMessage 转发给页面？
//   1. 页面直接连 ws://127.0.0.1:<port>/ws，浏览器里打开同一个地址也能用，调试不用两边跑
//   2. 外壳里没有协议逻辑，就不会出现"页面能连、外壳连不上"这类只有一方复现的问题
//   3. 引擎与 UI 是两个进程，外壳挂了不影响追踪数据流
//
// 页面从 http://127.0.0.1:<port>/ 加载（而不是把资源塞进 exe），原因：
//   - 页面与 WebSocket 同源同端口，绕开 https 页面连 ws:// 回环时的 mixed-content 限制
//   - 改 UI 不用重编 C++，热刷新即可
//   - 资源在哪、改了没有，一眼能看见（发布时随包放在 ui/ 目录）
//
// 运行时依赖：WebView2 Evergreen Runtime。Win10/11 通常已随 Edge 预装。
#include <windows.h>
#include <shellapi.h>
#include <wrl.h>
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kWindowClass[] = L"FgbUiWindow";
constexpr wchar_t kWindowTitle[] = L"FaceGaze Bridge";
constexpr UINT_PTR kRetryTimer   = 1;
constexpr UINT kRetryIntervalMs  = 1500;

struct Host {
    HWND hwnd                        = nullptr;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
    std::wstring url;
    std::wstring user_data_dir;
    // 非空时把 --remote-debugging-port 传给 WebView2：自动化验证和现场排障都靠它
    std::wstring browser_args;
    bool navigated_ok = false;
};

Host g_host;

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

std::wstring default_user_data_dir() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
    std::wstring dir;
    if (length > 0 && length < MAX_PATH) {
        dir.assign(buffer, length);
    } else {
        dir = L".";
    }
    dir.append(L"\\FaceGazeBridge\\WebView2");
    return dir;
}

void navigate_once() {
    if (!g_host.webview) {
        return;
    }
    g_host.navigated_ok = false;
    set_title(L"连接引擎…");
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

    g_host.controller = controller;

    ComPtr<ICoreWebView2Controller2> controller2;
    if (SUCCEEDED(controller->QueryInterface(IID_PPV_ARGS(&controller2)))) {
        // 与页面 CSS 的底色一致，避免页面加载前的白闪
        const COREWEBVIEW2_COLOR background{255, 0x14, 0x16, 0x1a};
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
                } else {
                    // 引擎还没起来是最常见的原因：隔一会儿重试，而不是让用户看白屏
                    set_title(L"等待引擎…");
                    SetTimer(g_host.hwnd, kRetryTimer, kRetryIntervalMs, nullptr);
                }
                return S_OK;
            })
            .Get(),
        nullptr);

    navigate_once();
}

void create_webview() {
    ComPtr<CoreWebView2EnvironmentOptions> environment_options;
    if (!g_host.browser_args.empty()) {
        environment_options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
        environment_options->put_AdditionalBrowserArguments(g_host.browser_args.c_str());
    }

    const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
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
                    MessageBoxW(g_host.hwnd, message, kWindowTitle, MB_ICONERROR | MB_OK);
                    PostMessageW(g_host.hwnd, WM_CLOSE, 0, 0);
                    return result;
                }
                env->CreateCoreWebView2Controller(
                    g_host.hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [](HRESULT controller_result, ICoreWebView2Controller* controller) -> HRESULT {
                            on_controller_ready(controller_result, controller);
                            return S_OK;
                        })
                        .Get());
                return S_OK;
            })
            .Get());

    if (FAILED(hr)) {
        wchar_t message[256];
        std::swprintf(message, 256, L"WebView2 加载器调用失败（0x%08lX）。",
                      static_cast<unsigned long>(hr));
        MessageBoxW(g_host.hwnd, message, kWindowTitle, MB_ICONERROR | MB_OK);
        PostMessageW(g_host.hwnd, WM_CLOSE, 0, 0);
    }
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
        }
        return 0;

    case WM_GETMINMAXINFO: {
        auto* info             = reinterpret_cast<MINMAXINFO*>(lparam);
        info->ptMinTrackSize.x = 820;
        info->ptMinTrackSize.y = 560;
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

struct Options {
    std::uint16_t port = 8787;
    int debug_port     = 0;
    std::wstring url;
    std::wstring user_data_dir = default_user_data_dir();
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
        }
    }
    if (options.url.empty()) {
        wchar_t buffer[128];
        std::swprintf(buffer, 128, L"http://127.0.0.1:%u/", static_cast<unsigned>(options.port));
        options.url = buffer;
    }
    return options;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    enable_dpi_awareness();

    int argc       = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const Options options = parse_options(argc, argv);
    if (argv != nullptr) {
        LocalFree(argv);
    }

    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) {
        MessageBoxW(nullptr, L"COM 初始化失败。", kWindowTitle, MB_ICONERROR | MB_OK);
        return 1;
    }

    const HINSTANCE module = instance != nullptr ? instance : GetModuleHandleW(nullptr);

    WNDCLASSEXW window_class{};
    window_class.cbSize        = sizeof(window_class);
    window_class.style         = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc   = window_proc;
    window_class.hInstance     = module;
    window_class.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = CreateSolidBrush(RGB(0x14, 0x16, 0x1a));
    window_class.lpszClassName = kWindowClass;
    if (RegisterClassExW(&window_class) == 0) {
        CoUninitialize();
        return 1;
    }

    g_host.url           = options.url;
    g_host.user_data_dir = options.user_data_dir;
    if (options.debug_port > 0) {
        g_host.browser_args = L"--remote-debugging-port=" +
                              std::to_wstring(options.debug_port);
    }

    // 默认尺寸按工作区收一下：小屏/远程桌面下 1180x780 会被截掉一截
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int work_width  = static_cast<int>(work.right - work.left);
    const int work_height = static_cast<int>(work.bottom - work.top);
    const int width       = std::min(1180, work_width > 0 ? work_width - 80 : 1180);
    const int height      = std::min(780, work_height > 0 ? work_height - 80 : 780);
    const int x           = work.left + (work_width - width) / 2;
    const int y           = work.top + (work_height - height) / 2;

    g_host.hwnd = CreateWindowExW(0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW, x, y, width,
                                  height, nullptr, nullptr, module, nullptr);
    if (g_host.hwnd == nullptr) {
        CoUninitialize();
        return 1;
    }

    ShowWindow(g_host.hwnd, show_command == 0 ? SW_SHOWNORMAL : show_command);
    UpdateWindow(g_host.hwnd);

    create_webview();

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
    CoUninitialize();
    return 0;
}
