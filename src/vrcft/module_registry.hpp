// VRCFT 模块的发现与选择。
//
// 目录约定来自 VRCFT 本体（实测，不是文档猜的）：
//
//   %AppData%\VRCFaceTracking\CustomLibs\<ModuleId>\
//       module.json          ← ModuleId / ModuleName / AuthorName / Version / DllFileName …
//       <DllFileName>.dll    ← 真正的模块
//       config.ini           ← 模块自己的配置，不关我们的事
//
// 也兼容"dll 直接摊在根目录里"的老布局（扫描时退化成用文件名当名字）。
//
// 【为什么这里只有"当前模块"而没有"启用/禁用列表"】
// docs/02-features.md §2.1 写的是"允许同时启用多个模块（A 供眼追、B 供表情）"。
// 那是目标形态，但引擎现在一次只跑一个模块进程（一个 VrcftSource、一条沙箱 socket），
// 假装支持多选会让界面显示"已启用两个"，实际只有一个在跑——比不做更糟。
// 所以这一版只做"启动时加载哪个"，并且把限制写在界面上；多模块并发留给 M4。
//
// 选择结果持久化到 %AppData%\FaceGazeBridge\fgb.ini 的 [vrcft] module=，重启生效。
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace fgb::vrcft {

struct ModuleInfo {
    std::string id;   // ModuleId（VRCFT 用 GUID；老布局下退化成 dll 文件名）
    std::string name; // ModuleName
    std::string author;
    std::string version;
    std::string description;
    std::string page_url;

    std::filesystem::path directory;
    std::filesystem::path dll;

    bool metadata_ok = false; // module.json 读到了
    bool broken      = false; // 有目录但找不到 dll
    bool active      = false; // 引擎当前/下次启动加载的那个
};

struct ModuleRoot {
    std::filesystem::path path;
    std::string origin; // 给人看的来源，如 "VRCFT" / "FGB"
};

// 从 module.json 文本解析元数据。文本带不带 BOM 都行。
bool parse_module_metadata(std::string_view text, ModuleInfo& out);

// 按 id 找（大小写不敏感）。找不到返回 nullptr。
const ModuleInfo* find_module(const std::vector<ModuleInfo>& modules, std::string_view id);

// 默认配置路径：%AppData%\FaceGazeBridge\fgb.ini（空 = 取不到 APPDATA）
std::filesystem::path default_config_path();

// VRCFT 自己的模块目录：%AppData%\VRCFaceTracking\CustomLibs
std::filesystem::path vrcft_module_root();

class ModuleRegistry {
public:
    // ini_path 为空时不做持久化（单测、临时运行）
    explicit ModuleRegistry(std::filesystem::path ini_path = default_config_path());

    void add_root(std::filesystem::path path, std::string origin);

    // 重新扫描磁盘。保留当前的 active 选择（模块可能已经不在盘上，那就落空）
    void rescan();

    const std::vector<ModuleInfo>& modules() const noexcept { return modules_; }
    const std::vector<ModuleRoot>& roots() const noexcept { return roots_; }
    const std::string& active_id() const noexcept { return active_id_; }
    const std::filesystem::path& config_path() const noexcept { return ini_path_; }

    // 只改内存（自动挑中某个模块时用，不要因为"自动"就改用户配置）。
    void mark_active(std::string_view id);

    // 设为启动时加载的模块；空串 = 不加载任何模块。id 不存在返回 false。
    // 成功时同时写盘；写盘失败不影响内存里的选择（界面仍能看到）。
    bool set_active(std::string_view id);

    // 决定这次启动加载哪个：
    //   1. 命令行给的 preferred
    //   2. 配置文件里记住的 active
    //   3. 磁盘上只有一个模块时就用它（新装机不必先手动配置）
    // 都没有则返回 nullptr，由调用方打印候选列表。
    const ModuleInfo* pick_active(std::string_view preferred) const;

private:
    void load();
    void save() const;
    void apply_active();

    std::vector<ModuleRoot> roots_;
    std::vector<ModuleInfo> modules_;
    std::string active_id_;
    std::filesystem::path ini_path_;
};

} // namespace fgb::vrcft
