// 模块发现与选择。
//
// 目录结构和 module.json 都照抄 VRCFT 磁盘上的真实样子（含 BOM、GUID 目录名、
// FileHash: null），因为这一层最怕的就是"按文档猜格式"。
#include "vrcft/module_registry.hpp"

#include "test_framework.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace fs = std::filesystem;
using namespace fgb::vrcft;

namespace {

// 临时目录：每个用例自己一份，跑完删掉
struct TempDir {
    fs::path path;

    TempDir() {
        static int counter = 0;
        path = fs::temp_directory_path() /
               ("fgb-modules-" + std::to_string(++counter) + "-" +
                std::to_string(static_cast<long long>(::rand())));
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write_file(const fs::path& path, std::string_view text) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

// VRCFTPicoModule 的真实元数据（截取关键字段）
constexpr char kPicoMetadata[] = R"({
  "ModuleId": "f3df57d5-1c5b-887d-abb0-be555e14bf09",
  "Version": "0.1.11",
  "AuthorName": "RingLo",
  "ModuleName": "VRCFTPicoModule",
  "ModuleDescription": "face tracking for Pico over PICO Connect",
  "ModulePageUrl": "https://github.com/lonelyicer/VRCFTPicoModule",
  "DllFileName": "VRCFTPicoModule.dll",
  "FileHash": null
})";

} // namespace

FGB_TEST(metadata_parse_real_vrcft_file) {
    ModuleInfo info;
    info.directory = "C:/somewhere/f3df57d5";
    FGB_CHECK(parse_module_metadata(kPicoMetadata, info));
    FGB_CHECK(info.metadata_ok);
    FGB_CHECK(info.id == "f3df57d5-1c5b-887d-abb0-be555e14bf09");
    FGB_CHECK(info.name == "VRCFTPicoModule");
    FGB_CHECK(info.author == "RingLo");
    FGB_CHECK(info.version == "0.1.11");
    FGB_CHECK(info.page_url == "https://github.com/lonelyicer/VRCFTPicoModule");
    FGB_CHECK(info.dll == fs::path{"C:/somewhere/f3df57d5"} / "VRCFTPicoModule.dll");
}

FGB_TEST(metadata_parse_survives_bom_and_garbage) {
    std::string text = "\xEF\xBB\xBF";
    text += R"({"ModuleName":"Bom","ModuleId":"x"})";
    ModuleInfo info;
    // BOM 由调用方（读文件那一步）剥掉；这里先验证裸文本能解
    FGB_CHECK(parse_module_metadata(text.substr(3), info));
    FGB_CHECK(info.name == "Bom");

    ModuleInfo empty;
    FGB_CHECK(!parse_module_metadata("not json at all", empty));
    FGB_CHECK(!empty.metadata_ok);
}

FGB_TEST(registry_scans_vrcft_layout) {
    TempDir root;
    const fs::path module_dir = root.path / "f3df57d5-1c5b-887d-abb0-be555e14bf09";
    write_file(module_dir / "module.json", kPicoMetadata);
    write_file(module_dir / "VRCFTPicoModule.dll", "not a real dll");

    // 第二个模块故意缺 DLL：应该被标成 broken 而不是被静默跳过
    write_file(root.path / "broken-id" / "module.json", R"({"ModuleName":"Broken"})");

    // 老布局：dll 直接摊在根目录
    write_file(root.path / "Legacy.dll", "x");

    ModuleRegistry registry{fs::path{}}; // 不落盘
    registry.add_root(root.path, "TEST");
    registry.rescan();

    FGB_CHECK(registry.modules().size() == 3);

    const ModuleInfo* pico = find_module(registry.modules(), "f3df57d5-1c5b-887d-abb0-be555e14bf09");
    FGB_CHECK(pico != nullptr);
    if (pico != nullptr) {
        FGB_CHECK(pico->name == "VRCFTPicoModule");
        FGB_CHECK(pico->metadata_ok);
        FGB_CHECK(!pico->broken);
        FGB_CHECK(fs::exists(pico->dll));
    }

    // 按名字也能找到（大小写不敏感）
    FGB_CHECK(find_module(registry.modules(), "vrcftpicomodule") == pico);

    const ModuleInfo* broken = find_module(registry.modules(), "broken-id");
    FGB_CHECK(broken != nullptr);
    if (broken != nullptr) {
        FGB_CHECK(broken->broken);
    }

    // 老布局没有 module.json，id 退化成 dll 文件名（不含扩展名）
    FGB_CHECK(find_module(registry.modules(), "legacy") != nullptr);
    FGB_CHECK(find_module(registry.modules(), "") == nullptr);
}

FGB_TEST(registry_active_selection_persists) {
    TempDir root;
    write_file(root.path / "aaa" / "module.json", R"({"ModuleId":"aaa","ModuleName":"A"})");
    write_file(root.path / "aaa" / "A.dll", "x");
    write_file(root.path / "bbb" / "module.json", R"({"ModuleId":"bbb","ModuleName":"B"})");
    write_file(root.path / "bbb" / "B.dll", "x");

    const fs::path ini = root.path / "cfg" / "fgb.ini";
    {
        ModuleRegistry registry{ini};
        registry.add_root(root.path, "TEST");
        registry.rescan();

        // 只有一个模块时自动选中（新装机不该卡在"先手动配置"）
        FGB_CHECK(registry.modules().size() == 2);
        FGB_CHECK(registry.active_id().empty());
        FGB_CHECK(registry.set_active("bbb"));
        FGB_CHECK(registry.active_id() == "bbb");

        const ModuleInfo* active = find_module(registry.modules(), "bbb");
        FGB_CHECK(active != nullptr);
        if (active != nullptr) {
            FGB_CHECK(active->active);
        }
        FGB_CHECK(!registry.set_active("nope")); // 不存在的模块不落盘
        FGB_CHECK(registry.active_id() == "bbb");
    }

    // 重新开一个 registry：选择应该从 ini 里读回来
    {
        ModuleRegistry reopened{ini};
        FGB_CHECK(reopened.active_id() == "bbb");
        reopened.add_root(root.path, "TEST");
        reopened.rescan();
        const ModuleInfo* picked = reopened.pick_active("");
        FGB_CHECK(picked != nullptr);
        if (picked != nullptr) {
            FGB_CHECK(picked->id == "bbb");
        }
        // 命令行优先
        const ModuleInfo* forced = reopened.pick_active("aaa");
        FGB_CHECK(forced != nullptr);
        if (forced != nullptr) {
            FGB_CHECK(forced->id == "aaa");
        }
        // 停用
        FGB_CHECK(reopened.set_active(""));
        FGB_CHECK(reopened.active_id().empty());
    }
}

FGB_TEST(registry_single_module_is_auto_picked) {
    TempDir root;
    write_file(root.path / "only" / "module.json", R"({"ModuleId":"only","ModuleName":"Only"})");
    write_file(root.path / "only" / "Only.dll", "x");

    ModuleRegistry registry{fs::path{}};
    registry.add_root(root.path, "TEST");
    registry.rescan();

    const ModuleInfo* picked = registry.pick_active("");
    FGB_CHECK(picked != nullptr);
    if (picked != nullptr) {
        FGB_CHECK(picked->id == "only");
    }
}
