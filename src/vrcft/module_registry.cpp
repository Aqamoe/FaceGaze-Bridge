#include "vrcft/module_registry.hpp"

#include "base/json.hpp"
#include "base/log.hpp"
#include "config/ini.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <system_error>

namespace fgb::vrcft {
namespace {

constexpr char kTag[] = "modules";

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    // VRCFT 的 json 有时候带 BOM（.NET 的 UTF8 写入习惯），不去掉会让首键匹配不上
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
    return text;
}

std::filesystem::path first_dll(const std::filesystem::path& directory) {
    std::error_code ec;
    std::filesystem::path best;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        if (entry.path().extension() != ".dll") {
            continue;
        }
        // 多个 dll 时优先挑跟目录同名 / 名字最长的（依赖库通常叫 *Core.dll 之类）
        if (best.empty() || entry.path().filename().string().size() > best.filename().string().size()) {
            best = entry.path();
        }
    }
    return best;
}

std::string lower(std::string_view text) {
    std::string out{text};
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

} // namespace

bool parse_module_metadata(std::string_view text, ModuleInfo& out) {
    bool any = false;
    std::string value;

    if (fgb::json::field_string(text, "ModuleId", value)) {
        out.id = value;
        any    = true;
    }
    if (fgb::json::field_string(text, "ModuleName", value)) {
        out.name = value;
        any      = true;
    }
    if (fgb::json::field_string(text, "AuthorName", value)) {
        out.author = value;
    }
    if (fgb::json::field_string(text, "Version", value)) {
        out.version = value;
    }
    if (fgb::json::field_string(text, "ModuleDescription", value)) {
        out.description = value;
    }
    if (fgb::json::field_string(text, "ModulePageUrl", value)) {
        out.page_url = value;
    }
    if (fgb::json::field_string(text, "DllFileName", value) && !value.empty()) {
        out.dll = out.directory / value;
        any     = true;
    }

    out.metadata_ok = any;
    return any;
}

const ModuleInfo* find_module(const std::vector<ModuleInfo>& modules, std::string_view id) {
    if (id.empty()) {
        return nullptr;
    }
    const std::string needle = lower(id);
    for (const ModuleInfo& module : modules) {
        if (lower(module.id) == needle || lower(module.name) == needle) {
            return &module;
        }
    }
    return nullptr;
}

std::filesystem::path default_config_path() {
    const char* appdata = std::getenv("APPDATA");
    if (appdata == nullptr || appdata[0] == '\0') {
        return {};
    }
    return std::filesystem::path{appdata} / "FaceGazeBridge" / "fgb.ini";
}

std::filesystem::path vrcft_module_root() {
    const char* appdata = std::getenv("APPDATA");
    if (appdata == nullptr || appdata[0] == '\0') {
        return {};
    }
    return std::filesystem::path{appdata} / "VRCFaceTracking" / "CustomLibs";
}

ModuleRegistry::ModuleRegistry(std::filesystem::path ini_path) : ini_path_(std::move(ini_path)) {
    load();
}

void ModuleRegistry::add_root(std::filesystem::path path, std::string origin) {
    if (path.empty()) {
        return;
    }
    roots_.push_back(ModuleRoot{std::move(path), std::move(origin)});
}

void ModuleRegistry::load() {
    if (ini_path_.empty()) {
        return;
    }
    config::ini_load(ini_path_.string(), [this](const config::IniEntry& entry) {
        if (entry.section == "vrcft" && entry.key == "module") {
            active_id_ = entry.value;
        }
    });
}

void ModuleRegistry::save() const {
    if (ini_path_.empty()) {
        return;
    }
    std::error_code ec;
    const auto parent = ini_path_.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }

    config::IniWriter writer;
    if (!writer.open(ini_path_.string())) {
        FGB_WARN(kTag, "配置写不进去：%s", ini_path_.string().c_str());
        return;
    }
    // 这个文件目前只放模块选择。以后加别的键时记得先读再写，
    // 否则会用默认值盖掉用户手改的内容。
    writer.comment("FaceGaze Bridge 配置（自动生成，可手改）");
    writer.section("vrcft");
    writer.kv("module", active_id_);
    writer.close();
}

void ModuleRegistry::apply_active() {
    for (ModuleInfo& module : modules_) {
        module.active = !active_id_.empty() && module.id == active_id_;
    }
}

void ModuleRegistry::rescan() {
    modules_.clear();

    for (const ModuleRoot& root : roots_) {
        std::error_code ec;
        if (!std::filesystem::is_directory(root.path, ec)) {
            continue;
        }

        for (const auto& entry : std::filesystem::directory_iterator(root.path, ec)) {
            if (ec) {
                break;
            }

            if (entry.is_directory()) {
                ModuleInfo info;
                info.directory = entry.path();
                info.id        = entry.path().filename().string();

                const std::string metadata = read_file(entry.path() / "module.json");
                if (!metadata.empty()) {
                    parse_module_metadata(metadata, info);
                }
                if (info.id.empty()) {
                    info.id = entry.path().filename().string();
                }
                if (info.dll.empty() || !std::filesystem::exists(info.dll, ec)) {
                    info.dll = first_dll(entry.path());
                }
                info.broken = info.dll.empty();
                if (info.name.empty()) {
                    info.name = info.id;
                }
                modules_.push_back(std::move(info));
            } else if (entry.is_regular_file() && entry.path().extension() == ".dll") {
                // 老布局：dll 直接放在根目录
                ModuleInfo info;
                info.directory = root.path;
                info.dll       = entry.path();
                info.id        = entry.path().stem().string();
                info.name      = info.id;
                modules_.push_back(std::move(info));
            }
        }
    }

    // 名字排序：结果稳定，界面不会每次刷新都跳位置
    std::sort(modules_.begin(), modules_.end(), [](const ModuleInfo& a, const ModuleInfo& b) {
        return lower(a.name) < lower(b.name);
    });

    apply_active();
    FGB_DEBUG(kTag, "扫描到 %zu 个模块", modules_.size());
}

void ModuleRegistry::mark_active(std::string_view id) {
    active_id_.clear();
    if (!id.empty()) {
        const ModuleInfo* module = find_module(modules_, id);
        active_id_ = module != nullptr ? module->id : std::string{id};
    }
    apply_active();
}

bool ModuleRegistry::set_active(std::string_view id) {
    if (!id.empty() && find_module(modules_, id) == nullptr) {
        return false;
    }
    mark_active(id);
    save();
    return true;
}

const ModuleInfo* ModuleRegistry::pick_active(std::string_view preferred) const {
    if (!preferred.empty()) {
        return find_module(modules_, preferred);
    }
    if (!active_id_.empty()) {
        return find_module(modules_, active_id_);
    }
    if (modules_.size() == 1) {
        return &modules_.front();
    }
    return nullptr;
}

} // namespace fgb::vrcft
