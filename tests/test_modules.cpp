// 模块仓：安装、导入、启停、删除、zip 安全。
//
// 目录结构和 module.json 都照抄 VRCFT 磁盘上的真实样子（含 BOM、GUID 目录名、
// FileHash: null），因为这一层最怕的就是"按文档猜格式"。
#include "vrcft/module_registry.hpp"

#include "test_framework.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using namespace fgb::vrcft;

namespace {

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

std::string metadata_for(std::string_view id, std::string_view name, std::string_view dll,
                         std::string_view version = "1.0.0") {
    std::string json = R"({"ModuleId":")";
    json.append(id);
    json.append(R"(","ModuleName":")");
    json.append(name);
    json.append(R"(","AuthorName":"Tester","Version":")");
    json.append(version);
    json.append(R"(","DllFileName":")");
    json.append(dll);
    json.append(R"(","FileHash":null})");
    return json;
}

void make_module_dir(const fs::path& dir, std::string_view id, std::string_view name,
                     std::string_view version = "1.0.0") {
    write_file(dir / "module.json", metadata_for(id, name, "Mod.dll", version));
    write_file(dir / "Mod.dll", "not a real dll");
    write_file(dir / "config.ini", "[cfg]\nx=1\n");
}

std::uint32_t crc32(std::string_view data) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const char ch : data) {
        crc ^= static_cast<std::uint8_t>(ch);
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

void put16(std::string& out, std::uint16_t value) {
    out.push_back(static_cast<char>(value & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
}

void put32(std::string& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
    }
}

struct ZipEntry {
    std::string name;
    std::string data;
};

// 只写 stored（不压缩）条目：测试不需要压缩，tar 两种都认
void write_zip(const fs::path& path, const std::vector<ZipEntry>& entries) {
    std::string body;
    struct Central {
        std::string name;
        std::uint32_t crc;
        std::uint32_t size;
        std::uint32_t offset;
    };
    std::vector<Central> central;

    for (const ZipEntry& entry : entries) {
        Central info{entry.name, crc32(entry.data), static_cast<std::uint32_t>(entry.data.size()),
                     static_cast<std::uint32_t>(body.size())};
        put32(body, 0x04034b50u);
        put16(body, 20);
        put16(body, 0);
        put16(body, 0);
        put16(body, 0);
        put16(body, 0);
        put32(body, info.crc);
        put32(body, info.size);
        put32(body, info.size);
        put16(body, static_cast<std::uint16_t>(entry.name.size()));
        put16(body, 0);
        body.append(entry.name);
        body.append(entry.data);
        central.push_back(std::move(info));
    }

    const std::uint32_t central_offset = static_cast<std::uint32_t>(body.size());
    for (const Central& info : central) {
        put32(body, 0x02014b50u);
        put16(body, 20);
        put16(body, 20);
        put16(body, 0);
        put16(body, 0);
        put16(body, 0);
        put16(body, 0);
        put32(body, info.crc);
        put32(body, info.size);
        put32(body, info.size);
        put16(body, static_cast<std::uint16_t>(info.name.size()));
        put16(body, 0);
        put16(body, 0);
        put16(body, 0);
        put16(body, 0);
        put32(body, 0);
        put32(body, info.offset);
        body.append(info.name);
    }
    const std::uint32_t central_size = static_cast<std::uint32_t>(body.size()) - central_offset;
    put32(body, 0x06054b50u);
    put16(body, 0);
    put16(body, 0);
    put16(body, static_cast<std::uint16_t>(central.size()));
    put16(body, static_cast<std::uint16_t>(central.size()));
    put32(body, central_size);
    put32(body, central_offset);
    put16(body, 0);

    std::ofstream out(path, std::ios::binary);
    out.write(body.data(), static_cast<std::streamsize>(body.size()));
}

std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

} // namespace

FGB_TEST(metadata_parse_real_vrcft_file) {
    ModuleInfo info;
    info.directory = "C:/somewhere/f3df57d5";
    FGB_CHECK(parse_module_metadata(
        R"({"ModuleId":"f3df57d5-1c5b-887d-abb0-be555e14bf09","Version":"0.1.11",
            "AuthorName":"RingLo","ModuleName":"VRCFTPicoModule",
            "ModulePageUrl":"https://github.com/lonelyicer/VRCFTPicoModule",
            "DllFileName":"VRCFTPicoModule.dll","FileHash":null})",
        info));
    FGB_CHECK(info.metadata_ok);
    FGB_CHECK(info.id == "f3df57d5-1c5b-887d-abb0-be555e14bf09");
    FGB_CHECK(info.name == "VRCFTPicoModule");
    FGB_CHECK(info.author == "RingLo");
    FGB_CHECK(info.version == "0.1.11");
    FGB_CHECK(info.dll == fs::path{"C:/somewhere/f3df57d5"} / "VRCFTPicoModule.dll");
}

FGB_TEST(install_directory_and_state_persist) {
    TempDir root;
    const fs::path src = root.path / "source" / "pico";
    make_module_dir(src, "pico-id", "PicoModule");

    const fs::path store = root.path / "store";
    const fs::path state = root.path / "modules.ini";
    {
        ModuleRegistry registry{state, store};
        std::string error;
        FGB_CHECK(registry.install_path(src, error, "test"));
        FGB_CHECK(error.empty());
        FGB_CHECK(registry.modules().size() == 1);
        const ModuleInfo* module = registry.find("pico-id");
        FGB_CHECK(module != nullptr);
        if (module != nullptr) {
            FGB_CHECK(module->enabled);
            FGB_CHECK(module->active); // 只有一个模块时自动成为启动模块
            FGB_CHECK(module->source == "test");
            FGB_CHECK(fs::exists(module->dll));
            FGB_CHECK(module->bytes > 0);
        }
        FGB_CHECK(fs::exists(store / "pico-id" / "module.json"));
    }
    {
        ModuleRegistry reopened{state, store};
        reopened.rescan();
        FGB_CHECK(reopened.modules().size() == 1);
        FGB_CHECK(reopened.active_id() == "pico-id");
        const ModuleInfo* picked = reopened.pick_active("");
        FGB_CHECK(picked != nullptr);
    }
}

FGB_TEST(install_loose_dll_synthesizes_metadata) {
    TempDir root;
    const fs::path dll = root.path / "LooseModule.dll";
    write_file(dll, "not a real dll");

    const fs::path store = root.path / "store";
    ModuleRegistry registry{root.path / "modules.ini", store};
    std::string error;
    FGB_CHECK(registry.install_path(dll, error, "dll"));
    const ModuleInfo* module = registry.find("LooseModule");
    FGB_CHECK(module != nullptr);
    if (module != nullptr) {
        FGB_CHECK(module->name == "LooseModule");
        FGB_CHECK(fs::exists(module->directory / "module.json"));
    }
}

FGB_TEST(import_candidates_are_not_loaded_until_imported) {
    TempDir root;
    const fs::path vrcft_root = root.path / "CustomLibs";
    make_module_dir(vrcft_root / "pico-id", "pico-id", "PicoModule");
    write_file(vrcft_root / "Legacy.dll", "x");

    const fs::path store = root.path / "store";
    ModuleRegistry registry{root.path / "modules.ini", store};
    registry.add_import_root(vrcft_root, "VRCFT");
    registry.rescan();

    // 候选不等于已安装：引擎这边一个模块都没有
    FGB_CHECK(registry.modules().empty());
    FGB_CHECK(registry.importable().size() == 2);
    FGB_CHECK(registry.find_importable("pico-id") != nullptr);
    FGB_CHECK(registry.find_importable("Legacy") != nullptr);

    std::string error;
    FGB_CHECK(registry.import_module("pico-id", error));
    FGB_CHECK(registry.modules().size() == 1);
    FGB_CHECK(registry.find("pico-id") != nullptr);
    FGB_CHECK(registry.importable().size() == 1);

    FGB_CHECK(registry.import_module("Legacy", error));
    FGB_CHECK(registry.modules().size() == 2);
    const ModuleInfo* legacy = registry.find("Legacy");
    FGB_CHECK(legacy != nullptr);
    if (legacy != nullptr) {
        FGB_CHECK(legacy->source == "import");
        FGB_CHECK(fs::exists(legacy->directory / "module.json"));
    }
    FGB_CHECK(registry.importable().empty());
}

FGB_TEST(enable_disable_and_active) {
    TempDir root;
    make_module_dir(root.path / "a", "aaa", "A");
    make_module_dir(root.path / "b", "bbb", "B");
    const fs::path store = root.path / "store";
    ModuleRegistry registry{root.path / "modules.ini", store};
    std::string error;
    FGB_CHECK(registry.install_path(root.path / "a", error));
    FGB_CHECK(registry.install_path(root.path / "b", error));

    FGB_CHECK(registry.set_active("bbb"));
    FGB_CHECK(registry.active_id() == "bbb");

    // 停用当前启动模块会清空选择，并且不能再被选中
    FGB_CHECK(registry.set_enabled("bbb", false));
    FGB_CHECK(registry.active_id().empty());
    FGB_CHECK(!registry.set_active("bbb"));

    const ModuleInfo* b = registry.find("bbb");
    FGB_CHECK(b != nullptr);
    if (b != nullptr) {
        FGB_CHECK(!b->enabled);
    }

    // 只剩 A 启用时自动回落到 A
    const ModuleInfo* picked = registry.pick_active("");
    FGB_CHECK(picked != nullptr);
    if (picked != nullptr) {
        FGB_CHECK(picked->id == "aaa");
    }

    FGB_CHECK(registry.set_enabled("bbb", true));
    FGB_CHECK(registry.set_active("bbb"));
}

FGB_TEST(remove_module_clears_state) {
    TempDir root;
    make_module_dir(root.path / "a", "aaa", "A");
    const fs::path store = root.path / "store";
    const fs::path state = root.path / "modules.ini";
    {
        ModuleRegistry registry{state, store};
        std::string error;
        FGB_CHECK(registry.install_path(root.path / "a", error));
        FGB_CHECK(registry.remove_module("aaa", error));
        FGB_CHECK(registry.modules().empty());
        FGB_CHECK(registry.active_id().empty());
        FGB_CHECK(!fs::exists(store / "aaa"));
    }
    ModuleRegistry reopened{state, store};
    reopened.rescan();
    FGB_CHECK(reopened.modules().empty());
    FGB_CHECK(reopened.active_id().empty());
}

FGB_TEST(install_zip_with_root_folder) {
    TempDir root;
    const fs::path zip = root.path / "module.zip";
    write_zip(zip, {{"Pico/module.json", metadata_for("pico-id", "PicoModule", "Mod.dll")},
                    {"Pico/Mod.dll", "dll"},
                    {"Pico/config.ini", "[x]\n"}});

    ModuleRegistry registry{root.path / "modules.ini", root.path / "store"};
    std::string error;
    FGB_CHECK(registry.install_path(zip, error, "zip"));
    const ModuleInfo* module = registry.find("pico-id");
    FGB_CHECK(module != nullptr);
    if (module != nullptr) {
        FGB_CHECK(fs::exists(module->directory / "config.ini"));
        FGB_CHECK(module->source == "zip");
    }
}

FGB_TEST(install_zip_rejects_path_traversal) {
    TempDir root;
    const fs::path zip = root.path / "evil.zip";
    write_zip(zip, {{"../evil.txt", "bad"}, {"module.json", metadata_for("ok", "Ok", "Ok.dll")},
                    {"Ok.dll", "dll"}});

    ModuleRegistry registry{root.path / "modules.ini", root.path / "store"};
    std::string error;
    FGB_CHECK(!registry.install_path(zip, error, "zip"));
    FGB_CHECK(!error.empty());
    FGB_CHECK(!fs::exists(root.path / "evil.txt"));
    FGB_CHECK(!fs::exists(root.path / "store" / "evil.txt"));
    FGB_CHECK(registry.modules().empty());
}

FGB_TEST(install_rejects_duplicate_and_invalid_id) {
    TempDir root;
    make_module_dir(root.path / "a", "aaa", "A");
    ModuleRegistry registry{root.path / "modules.ini", root.path / "store"};
    std::string error;
    FGB_CHECK(registry.install_path(root.path / "a", error));
    FGB_CHECK(!registry.install_path(root.path / "a", error)); // 重复 id
    FGB_CHECK(error.find("已安装") != std::string::npos);

    const fs::path bad = root.path / "bad";
    write_file(bad / "module.json", metadata_for("a/b", "Bad", "Bad.dll"));
    write_file(bad / "Bad.dll", "dll");
    FGB_CHECK(!registry.install_path(bad, error));
    FGB_CHECK(!error.empty());
}

FGB_TEST(install_replace_keeps_enabled_state) {
    TempDir root;
    const fs::path src = root.path / "src";
    make_module_dir(src, "aaa", "A", "1.0.0");
    ModuleRegistry registry{root.path / "modules.ini", root.path / "store"};
    std::string error;
    FGB_CHECK(registry.install_path(src, error));
    FGB_CHECK(registry.set_enabled("aaa", false));
    FGB_CHECK(registry.find("aaa") != nullptr);
    FGB_CHECK(!registry.find("aaa")->enabled);

    // 同一 id 的新版本：replace=true 覆盖，启用状态保留
    write_file(src / "module.json", metadata_for("aaa", "A", "Mod.dll", "2.0.0"));
    FGB_CHECK(registry.install_path(src, error, "registry", true));
    const ModuleInfo* module = registry.find("aaa");
    FGB_CHECK(module != nullptr);
    if (module != nullptr) {
        FGB_CHECK(module->version == "2.0.0");
        FGB_CHECK(!module->enabled);
    }
    FGB_CHECK(registry.modules().size() == 1);
}

FGB_TEST(zip_without_metadata_uses_registry_override) {
    TempDir root;
    const fs::path zip = root.path / "alvr.zip";
    // ALVR 的发布包只有 DLL，没有 module.json
    write_zip(zip, {{"ALVRModule.dll", "dll"}});

    ModuleMetadataOverride metadata;
    metadata.id      = "699750bf-e217-4bd8-8039-3d5f97ee80ba";
    metadata.name    = "ALVR Module";
    metadata.author  = "zmerp";
    metadata.version = "1.3.0";

    ModuleRegistry registry{root.path / "modules.ini", root.path / "store"};
    std::string error;
    FGB_CHECK(registry.install_path(zip, error, "registry", false, &metadata));
    const ModuleInfo* module = registry.find(metadata.id);
    FGB_CHECK(module != nullptr);
    if (module != nullptr) {
        FGB_CHECK(module->name == "ALVR Module");
        FGB_CHECK(module->version == "1.3.0");
        FGB_CHECK(module->directory.filename().string() == metadata.id);
        FGB_CHECK(fs::exists(module->directory / "module.json"));
        FGB_CHECK(module->dll.filename().string() == "ALVRModule.dll");
    }
}

FGB_TEST(zip_metadata_wins_over_registry_version) {
    TempDir root;
    const fs::path zip = root.path / "cymple.zip";
    // 包里的 module.json 比模块库条目新（1.5.2 vs 1.0），应保留包内版本
    write_zip(zip, {{"module.json", metadata_for("cymple-id", "cymple", "Cymple.dll", "1.5.2")},
                    {"Cymple.dll", "dll"}});

    ModuleMetadataOverride metadata;
    metadata.id      = "cymple-id";
    metadata.name    = "cymple";
    metadata.version = "1.0";

    ModuleRegistry registry{root.path / "modules.ini", root.path / "store"};
    std::string error;
    FGB_CHECK(registry.install_path(zip, error, "registry", false, &metadata));
    const ModuleInfo* module = registry.find("cymple-id");
    FGB_CHECK(module != nullptr);
    if (module != nullptr) {
        FGB_CHECK(module->version == "1.5.2");
    }
}

FGB_TEST(modules_keep_installation_order) {
    TempDir root;
    make_module_dir(root.path / "zebra", "zebra-id", "Zebra");
    make_module_dir(root.path / "alpha", "alpha-id", "Alpha");
    const fs::path store = root.path / "store";
    const fs::path state = root.path / "modules.ini";
    {
        ModuleRegistry registry{state, store};
        std::string error;
        FGB_CHECK(registry.install_path(root.path / "zebra", error));
        FGB_CHECK(registry.install_path(root.path / "alpha", error));
        FGB_CHECK(registry.modules().size() == 2);
        // 新安装的排下面，不按名字重排
        FGB_CHECK(registry.modules()[0].id == "zebra-id");
        FGB_CHECK(registry.modules()[1].id == "alpha-id");
    }
    ModuleRegistry reopened{state, store};
    reopened.rescan();
    FGB_CHECK(reopened.modules().size() == 2);
    FGB_CHECK(reopened.modules()[0].id == "zebra-id");
    FGB_CHECK(reopened.modules()[1].id == "alpha-id");
}
