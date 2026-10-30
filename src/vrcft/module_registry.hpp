// 模块仓：FaceGaze Bridge 自己管理的一套模块。
//
// 目录约定（全部在 %LOCALAPPDATA%\FaceGazeBridge 下，DLL 不该跟着漫游配置走）：
//
//   modules\<ModuleId>\module.json + <DllFileName>.dll + config.ini
//   modules.ini              ← 启用状态、启动模块、来源、安装时间
//   runtime\vrcft\           ← 可选的本机运行时副本（内置 runtime/ 缺省时用安装版）
//
// 【为什么不再直接加载 %AppData%\VRCFaceTracking\CustomLibs】
// 那是别人的目录：VRCFT 卸载/升级/删模块都会让我们的输入源跟着消失，而且等于在
// 用户没确认的目录里执行代码。现在 VRCFT 目录只作为"可导入候选"，用户点导入后才
// 复制进来；引擎只从 store 加载。
//
// 【is_enabled 与 active 的区别】
// enabled  = 允许被选中/加载（停用不动文件）
// active   = 下次启动加载的那一个（引擎一次只跑一个模块，见 ADR-0015）
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fgb::vrcft {

enum class ModuleKind { vrcft, native };

struct ModuleInfo {
    std::string id;   // ModuleId（VRCFT 用 GUID；无元数据时退化成 dll 文件名）
    std::string name; // ModuleName
    std::string author;
    std::string version;
    std::string description;
    std::string page_url;

    std::filesystem::path directory;
    std::filesystem::path dll;

    ModuleKind kind = ModuleKind::vrcft;
    bool metadata_ok = false; // module.json 读到了
    bool broken      = false; // 有目录但找不到 dll
    bool active      = false; // 下次启动加载的那个
    bool enabled     = true;  // 停用的模块不参与启动选择
    bool loose       = false; // DLL 直接摊在扫描根目录（老布局/导入候选）

    std::string source;       // import / zip / dll / registry / dir
    std::string installed_at; // 展示用，本地时间
    std::uintmax_t bytes = 0;
    std::uint64_t install_order = 0; // 安装顺序：新模块排在列表下面

    // fgb-native.json 的可选字段（ADR-0030 §8）。运行时能力以模块自报为准，
    // 这里只用于加载前的列表/兼容性提示。
    std::uint32_t declared_capabilities = 0; // FGB_CAP_* 位；0 = 清单没写
    std::string role_hint;
    std::string min_host_version;
    bool host_compatible = true;
};

// 启动时加载的模块实例。名字用于展示与 --*-source 匹配；角色绑定用 module_id，
// 不依赖 native[1] / vrcft[2] 这类按顺序生成的名字。
struct ModuleInstance {
    std::string name;
    std::string module_id;
};

struct ModuleRoles {
    std::string expression;
    std::string eyes;
    std::string gaze;
    std::string head;
};

// 模块库条目的元数据。下载包里没有 module.json 时用它补齐（例如 ALVR 只发一个 DLL），
// 这样模块 ID/名字/版本与库条目一致，界面才能识别"已安装"。
struct ModuleMetadataOverride {
    std::string id;
    std::string name;
    std::string author;
    std::string version;
    std::string description;
    std::string page;
    std::string download_url;
};

struct ImportCandidate {
    ModuleInfo info;
    std::string origin; // 候选来源，如 "VRCFT" / "FGB"
};

struct VrcftRuntime {
    std::filesystem::path dir;
    std::filesystem::path exe;
    std::string origin; // bundled / local / VRCFT
};

// 从 module.json 文本解析元数据。文本带不带 BOM 都行。
bool parse_module_metadata(std::string_view text, ModuleInfo& out);

// 按 id 或显示名找（大小写不敏感）。找不到返回 nullptr。
const ModuleInfo* find_module(const std::vector<ModuleInfo>& modules, std::string_view id);

// VRCFT 自己的模块目录：%AppData%\VRCFaceTracking\CustomLibs（只作为导入候选）
std::filesystem::path vrcft_module_root();

// 模块仓与状态文件
std::filesystem::path default_store_dir();
std::filesystem::path default_state_path();

// 找 VRCFT 宿主运行时：内置 <exe>/runtime/vrcft → 本机副本 → 已安装 VRCFT
std::optional<VrcftRuntime> find_vrcft_runtime();

class ModuleRegistry {
public:
    // state/store 为空时用默认路径；传空目录给单测表示不落盘/临时仓
    explicit ModuleRegistry(std::filesystem::path state_path = default_state_path(),
                            std::filesystem::path store_dir = default_store_dir());

    void add_import_root(std::filesystem::path path, std::string origin);

    // 重新扫描仓与导入候选（保留启用/启动选择）
    void rescan();

    const std::vector<ModuleInfo>& modules() const noexcept { return modules_; }
    const std::vector<ImportCandidate>& importable() const noexcept { return importable_; }
    const std::filesystem::path& store_dir() const noexcept { return store_dir_; }
    const std::filesystem::path& state_path() const noexcept { return state_path_; }
    const std::string& active_id() const noexcept { return active_id_; }

    const std::vector<ModuleInstance>& instances() const noexcept { return instances_; }
    const ModuleRoles& roles() const noexcept { return roles_; }

    // 实例/角色都按 module_id 持久化；名字只用于展示。
    bool add_instance(std::string_view name, std::string_view module_id);
    bool remove_instance(std::string_view name);
    bool set_roles(const ModuleRoles& roles);

    const ModuleInfo* find(std::string_view id) const;
    const ImportCandidate* find_importable(std::string_view id) const;

    // 只改内存（自动挑中某个模块时用，不要因为"自动"就改用户配置）。
    void mark_active(std::string_view id);

    // 设为启动时加载的模块；空串 = 不加载。id 未安装或被停用返回 false。
    bool set_active(std::string_view id);

    // 启用/停用。停用当前启动模块会清空 active。
    bool set_enabled(std::string_view id, bool enabled);

    // 把导入候选复制进仓。成功返回 true（error 为空）。
    bool import_module(std::string_view id, std::string& error);

    // 从 .zip / .dll / 目录安装。source 只用于界面展示（默认按扩展名推断）。
    // replace=true 时允许覆盖同 ID 的旧版本（模块库更新用）。
    // metadata 非空且包内没有 module.json 时，用它补齐 ID/名字/版本。
    bool install_path(const std::filesystem::path& path, std::string& error,
                      const std::string& source = {}, bool replace = false,
                      const ModuleMetadataOverride* metadata = nullptr);

    // 删除仓里的模块。文件被占用（正在运行）时返回 false 并给出原因。
    bool remove_module(std::string_view id, std::string& error);

    // 决定这次启动加载哪个：
    //   1. 命令行给的 preferred（显式指定，允许是停用模块）
    //   2. 状态文件里记住的 active（必须仍启用）
    //   3. 只有一个启用模块时用它
    // 都没有则返回 nullptr。
    const ModuleInfo* pick_active(std::string_view preferred) const;

private:
    struct State {
        bool enabled = true;
        std::string source;
        std::string installed_at;
        std::uint64_t order = 0;
    };

    void load_state();
    void save_state() const;
    void apply_state();
    void scan_store();
    void scan_import_roots();
    ModuleInfo scan_directory(const std::filesystem::path& dir, const std::string& origin) const;
    ModuleInfo scan_dll(const std::filesystem::path& dll, const std::string& origin) const;

    bool install_directory(const std::filesystem::path& source_dir, std::string& error,
                           const std::string& source, bool replace,
                           const ModuleMetadataOverride* metadata);
    bool install_loose_dll(const std::filesystem::path& dll, std::string& error,
                           const std::string& source, bool replace,
                           const ModuleMetadataOverride* metadata);
    bool extract_zip(const std::filesystem::path& archive, const std::filesystem::path& dest,
                     std::string& error) const;

    std::vector<ModuleInfo> modules_;
    std::vector<ImportCandidate> importable_;
    std::vector<std::pair<std::filesystem::path, std::string>> import_roots_;
    std::unordered_map<std::string, State> states_;
    std::string active_id_;
    std::vector<ModuleInstance> instances_;
    ModuleRoles roles_;
    std::uint64_t next_order_ = 1;
    std::filesystem::path state_path_;
    std::filesystem::path store_dir_;
};

} // namespace fgb::vrcft
