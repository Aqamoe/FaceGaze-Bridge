#include "openvr/steamvr.hpp"

#include "base/log.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <exception>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "openvr/openvr.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

// openvr.h 把接口都放在 vr 命名空间里；官方示例同样在全局 using。
using namespace vr;

namespace fgb::steamvr {

#ifdef _WIN32
struct ApiImpl {
    using InitFn = std::uint32_t(VR_CALLTYPE*)(EVRInitError*, EVRApplicationType, const char*);
    using ShutdownFn = void(VR_CALLTYPE*)();
    using InterfaceFn = void*(VR_CALLTYPE*)(const char*, EVRInitError*);
    using DescribeFn = const char*(VR_CALLTYPE*)(EVRInitError);
    using SymbolFn = const char*(VR_CALLTYPE*)(EVRInitError);

    InitFn init                     = nullptr;
    ShutdownFn shutdown             = nullptr;
    InterfaceFn interface           = nullptr;
    DescribeFn describe             = nullptr;
    SymbolFn symbol                 = nullptr;
};

struct Session {
    ApiImpl* api          = nullptr;
    IVRApplications* apps = nullptr;
    bool initialized      = false;

    ~Session() {
        if (initialized && api != nullptr && api->shutdown != nullptr) {
            api->shutdown();
        }
    }
};
#endif

namespace {

constexpr char kTag[] = "steamvr";

std::string utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string{reinterpret_cast<const char*>(text.data()), text.size()};
}

std::filesystem::path from_utf8(std::string_view text) {
    return std::filesystem::path{std::u8string{reinterpret_cast<const char8_t*>(text.data()),
                                               text.size()}};
}

bool same_path(std::string_view a, std::string_view b) {
    auto normalize = [](std::string_view text) {
        std::string out;
        out.reserve(text.size());
        for (char c : text) {
            if (c == '/') {
                c = '\\';
            }
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        return out;
    };
    return normalize(a) == normalize(b);
}

#ifdef _WIN32
std::filesystem::path module_directory() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = ::GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return {};
    }
    return std::filesystem::path{buffer}.parent_path();
}

std::filesystem::path read_registry_steam_path() {
    wchar_t buffer[MAX_PATH * 4]{};
    DWORD size = sizeof(buffer);
    const LSTATUS status =
        ::RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath",
                       RRF_RT_REG_SZ, nullptr, buffer, &size);
    if (status != ERROR_SUCCESS) {
        return {};
    }
    return std::filesystem::path{buffer};
}

std::filesystem::path env_path(const wchar_t* name) {
    wchar_t buffer[MAX_PATH * 4]{};
    const DWORD length = ::GetEnvironmentVariableW(name, buffer, static_cast<DWORD>(std::size(buffer)));
    if (length == 0 || length >= std::size(buffer)) {
        return {};
    }
    return std::filesystem::path{buffer};
}
#endif

// 只解析 "runtime": ["..."] 这一种形状；openvrpaths.vrpath 由 SteamVR 自己写，
// 我们不去实现一个通用 JSON 解析器（见 base/json.hpp 的取舍）。
bool parse_json_string(std::string_view text, std::size_t& pos, std::string& out) {
    if (pos >= text.size() || text[pos] != '"') {
        return false;
    }
    ++pos;
    out.clear();
    while (pos < text.size()) {
        const char c = text[pos++];
        if (c == '"') {
            return true;
        }
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        if (pos >= text.size()) {
            return false;
        }
        const char escape = text[pos++];
        switch (escape) {
        case '"':
        case '\\':
        case '/':
            out.push_back(escape);
            break;
        case 'b':
            out.push_back('\b');
            break;
        case 'f':
            out.push_back('\f');
            break;
        case 'n':
            out.push_back('\n');
            break;
        case 'r':
            out.push_back('\r');
            break;
        case 't':
            out.push_back('\t');
            break;
        case 'u': {
            if (pos + 4 > text.size()) {
                return false;
            }
            unsigned code = 0;
            for (int i = 0; i < 4; ++i) {
                const char digit = text[pos++];
                code <<= 4;
                if (digit >= '0' && digit <= '9') {
                    code |= static_cast<unsigned>(digit - '0');
                } else if (digit >= 'a' && digit <= 'f') {
                    code |= static_cast<unsigned>(digit - 'a' + 10);
                } else if (digit >= 'A' && digit <= 'F') {
                    code |= static_cast<unsigned>(digit - 'A' + 10);
                } else {
                    return false;
                }
            }
            // 路径里常见的是 BMP 字符；代理对不常见，按单个码元写 UTF-8 即可。
            if (code < 0x80) {
                out.push_back(static_cast<char>(code));
            } else if (code < 0x800) {
                out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
            } else {
                out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
            }
            break;
        }
        default:
            return false;
        }
    }
    return false;
}

void skip_space(std::string_view text, std::size_t& pos) {
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) {
        ++pos;
    }
}

#ifdef _WIN32
std::string init_error_text(const ApiImpl& api, EVRInitError error) {
    std::string out;
    if (api.symbol != nullptr) {
        const char* text = api.symbol(error);
        if (text != nullptr && *text != '\0') {
            out = text;
        }
    }
    if (out.empty() && api.describe != nullptr) {
        const char* text = api.describe(error);
        if (text != nullptr && *text != '\0') {
            out = text;
        }
    }
    if (out.empty()) {
        out = "VR_Init error " + std::to_string(static_cast<int>(error));
    }
    return out;
}

bool open_session(ApiImpl& api, Session& session, Status& status) {
    EVRInitError error = VRInitError_None;
    session.api        = &api;
    api.init(&error, VRApplication_Background, nullptr);
    if (error != VRInitError_None) {
        status.state            = classify_init_error(static_cast<int>(error));
        status.steamvr_running  = false;
        status.reason           = status.state == State::NotRunning ? "steamvr_not_running"
                                                                    : "steamvr_init_failed";
        status.detail           = init_error_text(api, error);
        return false;
    }
    session.initialized   = true;
    status.steamvr_running = true;

    EVRInitError interface_error = VRInitError_None;
    session.apps = static_cast<IVRApplications*>(
        api.interface(IVRApplications_Version, &interface_error));
    if (session.apps == nullptr || interface_error != VRInitError_None) {
        status.state  = State::Error;
        status.reason = "openvr_interface_failed";
        status.detail = interface_error != VRInitError_None
                            ? init_error_text(api, interface_error)
                            : "IVRApplications 不可用";
        return false;
    }
    return true;
}
#endif

void fill_paths(const std::filesystem::path& exe_dir, Status& status) {
    status.app_key       = kAppKey;
    status.manifest_path = utf8(manifest_path_for(exe_dir));
    status.binary_path   = utf8(exe_dir / "FaceGaze Bridge.exe");
}

} // namespace

const char* state_name(State state) noexcept {
    switch (state) {
    case State::Unknown:
        return "unknown";
    case State::NotInstalled:
        return "not_installed";
    case State::NotRunning:
        return "not_running";
    case State::Off:
        return "off";
    case State::On:
        return "on";
    case State::Error:
        return "error";
    }
    return "unknown";
}

State classify_init_error(int error) noexcept {
    switch (error) {
    case VRInitError_None:
        return State::Off; // 初始化成功后的初始态，由调用方再读自启动
    case VRInitError_Init_InstallationNotFound:
    case VRInitError_Init_InstallationCorrupt:
    case VRInitError_Init_VRClientDLLNotFound:
    case VRInitError_Init_FileNotFound:
    case VRInitError_Init_PathRegistryNotFound:
    case VRInitError_Init_NoConfigPath:
        return State::NotInstalled;
    case VRInitError_Init_HmdNotFound:
    case VRInitError_Init_HmdNotFoundPresenceFailed:
    case VRInitError_Init_NoServerForBackgroundApp:
    case VRInitError_Init_ShuttingDown:
    case VRInitError_Init_AnotherAppLaunching:
    case VRInitError_Init_TooManyObjects:
        return State::NotRunning;
    default:
        return State::Error;
    }
}

bool parse_openvrpaths_runtime(std::string_view text, std::string& runtime_dir) {
    runtime_dir.clear();
    std::size_t pos = 0;
    while ((pos = text.find("\"runtime\"", pos)) != std::string_view::npos) {
        pos += std::string_view{"\"runtime\""}.size();
        skip_space(text, pos);
        if (pos >= text.size() || text[pos] != ':') {
            continue;
        }
        ++pos;
        skip_space(text, pos);
        if (pos < text.size() && text[pos] == '[') {
            ++pos;
            skip_space(text, pos);
            if (parse_json_string(text, pos, runtime_dir) && !runtime_dir.empty()) {
                return true;
            }
        } else if (parse_json_string(text, pos, runtime_dir) && !runtime_dir.empty()) {
            // 兼容手写成单个字符串的情况
            return true;
        }
    }
    return false;
}

std::vector<std::string> parse_manifest_paths(std::string_view text) {
    std::vector<std::string> paths;
    std::size_t pos = text.find("\"manifest_paths\"");
    if (pos == std::string_view::npos) {
        return paths;
    }
    pos = text.find('[', pos);
    if (pos == std::string_view::npos) {
        return paths;
    }
    ++pos;
    for (;;) {
        skip_space(text, pos);
        if (pos >= text.size() || text[pos] == ']') {
            break;
        }
        if (text[pos] == ',') {
            ++pos;
            continue;
        }
        std::string value;
        if (!parse_json_string(text, pos, value)) {
            break;
        }
        paths.push_back(std::move(value));
    }
    return paths;
}

std::string build_manifest_json(std::string_view exe_name, std::string_view arguments,
                                std::string_view app_key) {
    std::string out;
    out.reserve(512);
    out += "{\n";
    out += "  \"source\": \"builtin\",\n";
    out += "  \"applications\": [\n";
    out += "    {\n";
    out += "      \"app_key\": \"";
    out += app_key;
    out += "\",\n";
    out += "      \"launch_type\": \"binary\",\n";
    out += "      \"binary_path_windows\": \"";
    out += exe_name;
    out += "\",\n";
    out += "      \"arguments\": \"";
    out += arguments;
    out += "\",\n";
    out += "      \"is_dashboard_overlay\": true,\n";
    out += "      \"strings\": {\n";
    out += "        \"en_us\": {\n";
    out += "          \"name\": \"FaceGaze Bridge\",\n";
    out += "          \"description\": \"Starts FaceGaze Bridge with SteamVR.\"\n";
    out += "        },\n";
    out += "        \"zh_cn\": {\n";
    out += "          \"name\": \"FaceGaze Bridge\",\n";
    out += "          \"description\": \"随 SteamVR 启动 FaceGaze Bridge。\"\n";
    out += "        }\n";
    out += "      }\n";
    out += "    }\n";
    out += "  ]\n";
    out += "}\n";
    return out;
}

std::filesystem::path manifest_path_for(const std::filesystem::path& exe_dir) {
    return exe_dir / "app.vrmanifest";
}

std::filesystem::path dll_path_for(const std::filesystem::path& runtime_dir) {
    return runtime_dir / "bin" / "win64" / "openvr_api.dll";
}

std::filesystem::path find_runtime_dir() {
#ifdef _WIN32
    std::vector<std::filesystem::path> candidates;

    const auto local = env_path(L"LOCALAPPDATA");
    if (!local.empty()) {
        const auto vrpath = local / "openvr" / "openvrpaths.vrpath";
        std::ifstream input{vrpath, std::ios::binary};
        if (input) {
            const std::string text{std::istreambuf_iterator<char>(input),
                                   std::istreambuf_iterator<char>()};
            std::string runtime;
            if (parse_openvrpaths_runtime(text, runtime) && !runtime.empty()) {
                candidates.push_back(from_utf8(runtime));
            }
        }
    }

    const auto steam = read_registry_steam_path();
    if (!steam.empty()) {
        candidates.push_back(steam / "steamapps" / "common" / "SteamVR");
    }
    if (const auto program_files_x86 = env_path(L"ProgramFiles(x86)"); !program_files_x86.empty()) {
        candidates.push_back(program_files_x86 / "Steam" / "steamapps" / "common" / "SteamVR");
    }
    if (const auto program_files = env_path(L"ProgramFiles"); !program_files.empty()) {
        candidates.push_back(program_files / "Steam" / "steamapps" / "common" / "SteamVR");
    }

    std::error_code ec;
    for (const auto& candidate : candidates) {
        if (std::filesystem::exists(dll_path_for(candidate), ec)) {
            return candidate;
        }
    }
    return {};
#else
    return {};
#endif
}

namespace {

#ifdef _WIN32
// Steam 根目录下的 config：appconfig.json 记录所有登记过的 manifest 路径。
std::filesystem::path steam_config_dir() {
    std::error_code ec;
    const auto steam = read_registry_steam_path();
    if (!steam.empty() && std::filesystem::is_directory(steam / "config", ec)) {
        return steam / "config";
    }
    // 注册表没写时从运行库反推：<Steam>/steamapps/common/SteamVR
    const auto runtime = find_runtime_dir();
    if (!runtime.empty()) {
        const auto root = runtime.parent_path().parent_path().parent_path();
        if (std::filesystem::is_directory(root / "config", ec)) {
            return root / "config";
        }
    }
    return {};
}

bool manifest_mentions_app_key(const std::filesystem::path& manifest) {
    std::ifstream input{manifest, std::ios::binary};
    if (!input) {
        return false;
    }
    const std::string text{std::istreambuf_iterator<char>(input),
                           std::istreambuf_iterator<char>()};
    return text.find(kAppKey) != std::string::npos;
}

std::vector<std::string> registered_manifest_paths() {
    const auto config = steam_config_dir();
    if (config.empty()) {
        return {};
    }
    std::ifstream input{config / "appconfig.json", std::ios::binary};
    if (!input) {
        return {};
    }
    const std::string text{std::istreambuf_iterator<char>(input),
                           std::istreambuf_iterator<char>()};
    return parse_manifest_paths(text);
}

bool listed_manifest(const std::vector<std::string>& paths, const std::string& manifest_path) {
    return std::any_of(paths.begin(), paths.end(), [&manifest_path](const std::string& path) {
        return same_path(path, manifest_path);
    });
}

// appconfig.json 里还有别的目录登记着同一个 app_key（旧安装）。
bool foreign_manifest(const std::vector<std::string>& paths, const std::string& keep_manifest) {
    for (const std::string& path : paths) {
        if (same_path(path, keep_manifest)) {
            continue;
        }
        std::error_code ec;
        const auto manifest = from_utf8(path);
        if (std::filesystem::exists(manifest, ec) && manifest_mentions_app_key(manifest)) {
            return true;
        }
    }
    return false;
}

bool current_manifest_listed(const std::string& manifest_path) {
    return listed_manifest(registered_manifest_paths(), manifest_path);
}

bool foreign_manifest_listed(const std::string& keep_manifest) {
    return foreign_manifest(registered_manifest_paths(), keep_manifest);
}

// SteamVR 以 app_key 为唯一键：旧安装目录的 manifest 还登记着时，
// AddApplicationManifest 只会回 AppKeyAlreadyExists，自启动永远指向旧 exe。
// 这里把 keep_manifest 之外、内容里仍写着同一个 app_key 的 manifest 摘掉。
int drop_stale_manifests(Session& session, const std::string& keep_manifest) {
    int dropped = 0;
    for (const std::string& path : registered_manifest_paths()) {
        if (same_path(path, keep_manifest)) {
            continue;
        }
        std::error_code ec;
        const auto manifest = from_utf8(path);
        if (!std::filesystem::exists(manifest, ec) || !manifest_mentions_app_key(manifest)) {
            continue; // 文件不在就无法确认归属，别动别人的登记
        }
        const EVRApplicationError error = session.apps->RemoveApplicationManifest(path.c_str());
        if (error == VRApplicationError_None) {
            ++dropped;
            FGB_INFO(kTag, "已摘除旧安装目录的 SteamVR manifest：%s", path.c_str());
        } else {
            FGB_WARN(kTag, "摘除旧 manifest 失败（%d）：%s", static_cast<int>(error),
                     path.c_str());
        }
    }
    return dropped;
}

std::string registered_binary(IVRApplications* apps) {
    std::array<char, 1024> actual{};
    EVRApplicationError error = VRApplicationError_None;
    const std::uint32_t length = apps->GetApplicationPropertyString(
        kAppKey, VRApplicationProperty_BinaryPath_String, actual.data(),
        static_cast<std::uint32_t>(actual.size()), &error);
    return error == VRApplicationError_None && length > 0 ? std::string{actual.data()}
                                                          : std::string{};
}

// 让 app_key 指向当前目录。返回 true 表示"下一次启动 SteamVR 会拉起当前 exe"；
// pending_restart 表示运行中的 vrserver 还缓存着旧 app 列表，本会话读不到新路径。
bool ensure_target_is_current(Session& session, const std::string& manifest,
                              const std::string& expected, std::string& detail,
                              bool& pending_restart) {
    pending_restart = false;
    if (same_path(registered_binary(session.apps), expected)) {
        return true;
    }
    drop_stale_manifests(session, manifest);
    session.apps->AddApplicationManifest(manifest.c_str(), false);
    if (same_path(registered_binary(session.apps), expected)) {
        return true;
    }
    // vrserver 的 app 列表在启动时读取、运行中不重读旧引用的路径；磁盘上只剩
    // 当前 manifest 时，下次启动 SteamVR 自然会用新路径。
    if (current_manifest_listed(manifest) && !foreign_manifest_listed(manifest)) {
        pending_restart = true;
        return true;
    }
    const std::string seen = registered_binary(session.apps);
    detail = seen.empty() ? expected : seen;
    return false;
}
#endif

} // namespace

Manager::Manager(std::filesystem::path exe_dir) : exe_dir_(std::move(exe_dir)) {
    if (exe_dir_.empty()) {
#ifdef _WIN32
        exe_dir_ = module_directory();
#endif
    }
}

Manager::~Manager() {
    delete api_;
#ifdef _WIN32
    if (module_ != nullptr) {
        ::FreeLibrary(static_cast<HMODULE>(module_));
    }
#endif
}

bool Manager::load_openvr(std::string& error) {
#ifdef _WIN32
    if (api_ != nullptr) {
        return true;
    }
    const std::filesystem::path runtime = find_runtime_dir();
    if (runtime.empty()) {
        error = "未找到 SteamVR 运行库";
        return false;
    }
    const std::filesystem::path dll = dll_path_for(runtime);
    auto* module = static_cast<HMODULE>(
        ::LoadLibraryW(dll.wstring().c_str()));
    if (module == nullptr) {
        error = "加载失败：" + utf8(dll);
        return false;
    }
    auto* api = new ApiImpl{};
    api->init      = reinterpret_cast<ApiImpl::InitFn>(::GetProcAddress(module, "VR_InitInternal2"));
    api->shutdown  = reinterpret_cast<ApiImpl::ShutdownFn>(::GetProcAddress(module, "VR_ShutdownInternal"));
    api->interface = reinterpret_cast<ApiImpl::InterfaceFn>(
        ::GetProcAddress(module, "VR_GetGenericInterface"));
    api->describe = reinterpret_cast<ApiImpl::DescribeFn>(
        ::GetProcAddress(module, "VR_GetVRInitErrorAsEnglishDescription"));
    api->symbol = reinterpret_cast<ApiImpl::SymbolFn>(
        ::GetProcAddress(module, "VR_GetVRInitErrorAsSymbol"));
    if (api->init == nullptr || api->shutdown == nullptr || api->interface == nullptr) {
        delete api;
        ::FreeLibrary(module);
        error = "openvr_api.dll 缺少必要导出（版本过旧？）";
        return false;
    }
    module_ = module;
    api_    = api;
    return true;
#else
    (void)error;
    return false;
#endif
}

Status Manager::probe(std::string_view action) {
    Status status;
    status.action = std::string{action};
    fill_paths(exe_dir_, status);

#ifdef _WIN32
    const std::filesystem::path runtime = find_runtime_dir();
    if (runtime.empty()) {
        status.state  = State::NotInstalled;
        status.reason = "runtime_missing";
        status.detail = "openvrpaths.vrpath 与常见 Steam 安装目录都没有找到 SteamVR";
        return status;
    }
    status.runtime_found = true;
    status.runtime_dir   = utf8(runtime);
#endif

    std::error_code ec;
    if (!std::filesystem::exists(from_utf8(status.manifest_path), ec)) {
        status.state  = State::Error;
        status.reason = "manifest_missing";
        status.detail = status.manifest_path;
        return status;
    }
    if (!std::filesystem::exists(from_utf8(status.binary_path), ec)) {
        status.state  = State::Error;
        status.reason = "binary_missing";
        status.detail = status.binary_path;
        return status;
    }

    std::string load_error;
    if (!load_openvr(load_error)) {
        status.state  = status.runtime_found ? State::Error : State::NotInstalled;
        status.reason = "openvr_dll_failed";
        status.detail = load_error;
        return status;
    }

#ifdef _WIN32
    Session session;
    if (!open_session(*api_, session, status)) {
        return status;
    }

    const std::string expected_binary = utf8(exe_dir_ / "FaceGaze Bridge.exe");
    const std::vector<std::string> manifest_paths = registered_manifest_paths();
    const bool have_config = !manifest_paths.empty();

    // 以磁盘上的 appconfig.json 为准：运行中的 vrserver 可能还缓存着刚移除的旧 app，
    // 只看 IsApplicationInstalled 会把「移除注册」当成没生效、甚至把注册加回来。
    if (have_config && !listed_manifest(manifest_paths, status.manifest_path) &&
        !foreign_manifest(manifest_paths, status.manifest_path)) {
        status.registered  = false;
        status.autolaunch  = false;
        status.binary_path = expected_binary;
        status.state       = State::Off;
        status.reason      = "ok";
        status.ok          = true;
        return status;
    }

    status.registered = session.apps->IsApplicationInstalled(kAppKey);
    status.autolaunch = status.registered && session.apps->GetApplicationAutoLaunch(kAppKey);

    const std::string actual_binary = registered_binary(session.apps);
    status.binary_path = actual_binary.empty() ? expected_binary : actual_binary;

    // 重新安装到新目录后旧 manifest 仍占着 app_key：检测（refresh）和开启自启动时
    // 顺手摘掉它并重新登记当前目录，否则 SteamVR 永远拉起第一次注册的那个 exe。
    bool pending_restart = false;
    const bool may_repair = action != "remove" && action != "disable";
    if (may_repair && !actual_binary.empty() && !same_path(actual_binary, expected_binary)) {
        std::string detail;
        if (!ensure_target_is_current(session, status.manifest_path, expected_binary, detail,
                                      pending_restart)) {
            status.state  = State::Error;
            status.reason = "app_key_conflict";
            status.detail = detail;
            status.ok     = false;
            return status;
        }
        status.binary_path = expected_binary;
        status.registered  = session.apps->IsApplicationInstalled(kAppKey);
        status.autolaunch  = status.registered &&
                             session.apps->GetApplicationAutoLaunch(kAppKey);
        FGB_INFO(kTag, "SteamVR 自启动目标已指向当前目录：%s%s", expected_binary.c_str(),
                 pending_restart ? "（重启 SteamVR 后生效）" : "");
    }

    status.state  = status.autolaunch ? State::On : State::Off;
    status.reason = pending_restart ? "pending_restart" : "ok";
    status.ok     = true;
#endif
    return status;
}

Status Manager::status(std::string_view action) { return probe(action); }

Status Manager::set_autolaunch(bool enabled) {
    Status status = probe(enabled ? "enable" : "disable");
    if (!status.ok) {
        return status;
    }

#ifdef _WIN32
    Session session;
    if (!open_session(*api_, session, status)) {
        return status;
    }

    const std::string manifest = status.manifest_path;
    // probe() 在已注册时会把 status.binary_path 换成 SteamVR 里记录的实际路径；
    // 冲突判断必须用当前安装目录的期望路径，不能拿它自己比自己。
    const std::string expected_binary = utf8(exe_dir_ / "FaceGaze Bridge.exe");
    bool pending_restart = status.reason == "pending_restart";
    EVRApplicationError add = session.apps->AddApplicationManifest(manifest.c_str(), false);
    if (add == VRApplicationError_AppKeyAlreadyExists && enabled) {
        std::string detail;
        bool pending = false;
        if (!ensure_target_is_current(session, manifest, expected_binary, detail, pending)) {
            status.state  = State::Error;
            status.reason = "app_key_conflict";
            status.detail = detail;
            status.ok     = false;
            return status;
        }
        pending_restart = pending_restart || pending;
    } else if (add != VRApplicationError_None &&
               add != VRApplicationError_AppKeyAlreadyExists) {
        status.state  = State::Error;
        status.reason = "manifest_add_failed";
        status.detail = "AddApplicationManifest: " + std::to_string(static_cast<int>(add));
        status.ok     = false;
        return status;
    }

    const EVRApplicationError set = session.apps->SetApplicationAutoLaunch(kAppKey, enabled);
    if (set != VRApplicationError_None) {
        status.state  = State::Error;
        status.reason = "autolaunch_set_failed";
        status.detail = "SetApplicationAutoLaunch: " + std::to_string(static_cast<int>(set));
        status.ok     = false;
        return status;
    }

    // 只有读回一致才算成功：SteamVR 有时会接受调用但不落盘
    const bool readback = session.apps->GetApplicationAutoLaunch(kAppKey);
    if (readback != enabled) {
        status.state  = State::Error;
        status.reason = "autolaunch_not_saved";
        status.detail = "GetApplicationAutoLaunch 读回 false";
        status.ok     = false;
        return status;
    }

    status.registered = current_manifest_listed(manifest);
    status.autolaunch = readback;
    status.state      = readback ? State::On : State::Off;
    status.reason     = pending_restart ? "pending_restart" : "ok";
    status.ok         = status.registered;
    if (!status.ok) {
        status.reason = "manifest_not_registered";
        status.detail = "AddApplicationManifest 返回成功，但 manifest 没有出现在 appconfig.json 里";
    }
#endif
    return status;
}

Status Manager::remove_registration() {
    Status status = probe("remove");
    if (!status.ok) {
        return status;
    }

#ifdef _WIN32
    Session session;
    if (!open_session(*api_, session, status)) {
        return status;
    }

    session.apps->SetApplicationAutoLaunch(kAppKey, false);
    const std::string manifest = status.manifest_path;
    const EVRApplicationError removed =
        session.apps->RemoveApplicationManifest(manifest.c_str());
    if (removed != VRApplicationError_None && removed != VRApplicationError_NoManifest &&
        removed != VRApplicationError_UnknownApplication) {
        status.state  = State::Error;
        status.reason = "manifest_remove_failed";
        status.detail = "RemoveApplicationManifest: " + std::to_string(static_cast<int>(removed));
        status.ok     = false;
        return status;
    }

    // 旧安装目录留下的 manifest 也可能占着同一个 app_key：一并摘掉，
    // 「移除注册」才是真的移除干净。
    drop_stale_manifests(session, manifest);

    // 运行中的 vrserver 可能还缓存着刚移除的 app，以 appconfig.json 为准：
    // 磁盘上没有我们的清单了，下一次启动 SteamVR 就不会再拉起 FGB。
    status.registered = current_manifest_listed(manifest) || foreign_manifest_listed(manifest);
    status.autolaunch = false;
    status.state      = status.registered ? State::Error : State::Off;
    status.reason     = status.registered ? "remove_failed" : "ok";
    status.ok         = !status.registered;
    if (status.registered) {
        status.detail = "app_key 仍被其他 manifest 占用（可能是旧安装目录）";
    }
#endif
    return status;
}

Service::Service(std::filesystem::path exe_dir) : manager_(exe_dir) {
    // 首次连接就要给出路径：页面在用户点「检测」之前也能看到 manifest 在哪
    snapshot_.state         = State::Unknown;
    snapshot_.action        = "refresh";
    snapshot_.manifest_path = utf8(manifest_path_for(exe_dir));
    snapshot_.binary_path   = utf8(exe_dir / "FaceGaze Bridge.exe");
    snapshot_.app_key       = kAppKey;
    worker_                 = std::thread([this] { run(); });
}

Service::~Service() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void Service::request(Op op) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(op);
        busy_ = true;
    }
    cv_.notify_one();
}

bool Service::take(Status& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!has_result_) {
        return false;
    }
    out        = std::move(result_);
    has_result_ = false;
    return true;
}

Status Service::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

bool Service::busy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return busy_;
}

void Service::run() {
    for (;;) {
        Op op;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (stop_ && queue_.empty()) {
                return;
            }
            op = queue_.front();
            queue_.pop_front();
        }

        Status status;
        try {
            switch (op) {
            case Op::Refresh:
                status = manager_.status("refresh");
                break;
            case Op::Enable:
                status = manager_.set_autolaunch(true);
                break;
            case Op::Disable:
                status = manager_.set_autolaunch(false);
                break;
            case Op::Remove:
                status = manager_.remove_registration();
                break;
            }
        } catch (const std::exception& error) {
            status.state  = State::Error;
            status.reason = "exception";
            status.detail = error.what();
            status.ok     = false;
        }

        FGB_INFO(kTag, "SteamVR 操作完成：state=%s reason=%s detail=%s", state_name(status.state),
                 status.reason.c_str(), status.detail.c_str());

        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_   = status;
            result_     = std::move(status);
            has_result_ = true;
            busy_       = !queue_.empty();
        }
    }
}

} // namespace fgb::steamvr
