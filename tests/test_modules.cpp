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

#ifdef _WIN32
#include <windows.h>
#endif

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

// 原生模块的清单叫 fgb-native.json，字段与 module.json 相同（ADR-0030 §8）
std::string native_metadata_for(std::string_view id, std::string_view name, std::string_view dll,
                                std::string_view version) {
    std::string json = R"({"ModuleId":")";
    json.append(id);
    json.append(R"(","ModuleName":")");
    json.append(name);
    json.append(R"(","AuthorName":"Tester","Version":")");
    json.append(version);
    json.append(R"(","DllFileName":")");
    json.append(dll);
    json.append(R"(","AbiVersion":2,"Capabilities":["expression"]})");
    return json;
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

// 模块库安装时会把库条目的使用说明/模块页写进 module.json；重新扫描要能读回来，
// 否则展开卡片只剩描述，"使用说明"永远为空。
FGB_TEST(metadata_parse_usage_and_page) {
    ModuleInfo info;
    FGB_CHECK(parse_module_metadata(
        R"({"ModuleId":"com.example.mod","ModuleName":"Example","Version":"1.0.0",
            "ModuleDescription":"desc","UsageInstructions":"Enable eye tracking first.",
            "ModulePageUrl":"https://example.com/mod","DllFileName":"Example.dll"})",
        info));
    FGB_CHECK(info.usage == "Enable eye tracking first.");
    FGB_CHECK(info.page_url == "https://example.com/mod");
}

FGB_TEST(metadata_parse_module_icon) {
    ModuleInfo info;
    FGB_CHECK(parse_module_metadata(
        R"({"ModuleId":"com.example.mod","ModuleName":"Example",
            "ModuleIcon":"art/icon.svg","DllFileName":"Example.dll"})",
        info));
    FGB_CHECK(info.icon_name == "art/icon.svg");
}

// 清单指定的图标要跟着模块目录走；没写 ModuleIcon 时认识根目录的 icon.*。
FGB_TEST(install_keeps_declared_and_default_icons) {
    TempDir root;
    const fs::path declared = root.path / "declared";
    write_file(declared / "module.json",
               R"({"ModuleId":"icon-declared","ModuleName":"Declared",
                   "DllFileName":"Mod.dll","ModuleIcon":"art/icon.svg"})");
    write_file(declared / "Mod.dll", "not a real dll");
    write_file(declared / "art/icon.svg", "<svg/>");

    const fs::path implicit = root.path / "implicit";
    make_module_dir(implicit, "icon-implicit", "Implicit");
    write_file(implicit / "icon.png", "png-bytes");

    ModuleRegistry registry{root.path / "modules.ini", root.path / "store"};
    std::string error;
    FGB_CHECK(registry.install_path(declared, error, "test"));
    FGB_CHECK(registry.install_path(implicit, error, "test"));

    const ModuleInfo* first = registry.find("icon-declared");
    FGB_CHECK(first != nullptr);
    if (first != nullptr) {
        FGB_CHECK(!first->icon.empty());
        FGB_CHECK(fs::exists(first->icon));
    }
    const ModuleInfo* second = registry.find("icon-implicit");
    FGB_CHECK(second != nullptr);
    if (second != nullptr) {
        FGB_CHECK(!second->icon.empty());
        FGB_CHECK(fs::exists(second->icon));
    }
}

// ModuleIcon 指到模块目录外（../）时必须拒绝，哪怕文件真的存在。
// PE 图标资源 → ICO：用带产品图标的界面 exe 验证头部与目录项。
// FGB_BUILD_UI=OFF 时没有这个 exe，跳过（不是失败）。
FGB_TEST(dll_icon_extracts_embedded_icon) {
#ifdef _WIN32
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = ::GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    const fs::path exe =
        length > 0 ? fs::path{buffer}.parent_path() / "FaceGaze Bridge.exe" : fs::path{};
    if (exe.empty() || !fs::exists(exe)) {
        FGB_CHECK(true);
        return;
    }
    std::string ico;
    std::string mime;
    FGB_CHECK(read_dll_icon(exe, ico, mime));
    FGB_CHECK(mime == "image/x-icon");
    FGB_CHECK(ico.size() > 22);
    if (ico.size() > 22) {
        const auto byte = [&ico](std::size_t index) {
            return static_cast<unsigned>(static_cast<unsigned char>(ico[index]));
        };
        const unsigned count = byte(4) | (byte(5) << 8);
        FGB_CHECK(byte(0) == 0 && byte(1) == 0);
        FGB_CHECK(byte(2) == 1 && byte(3) == 0); // ICO type
        FGB_CHECK(count >= 1 && count <= 64);
        FGB_CHECK(ico.size() >= 6u + 16u * count);
    }
#else
    FGB_CHECK(true);
#endif
}

// .NET 程序集把 logo 当 PNG 托管资源嵌在 PE 里；扫描时要取最大且尺寸合理的那个。
FGB_TEST(embedded_png_extraction) {
    std::ifstream input(fs::path{FGB_SOURCE_DIR} / "ui" / "logo.png", std::ios::binary);
    const std::string data{std::istreambuf_iterator<char>(input),
                           std::istreambuf_iterator<char>()};
    FGB_CHECK(!data.empty());

    std::string png;
    FGB_CHECK(find_embedded_png(data, png));
    FGB_CHECK(png.size() == data.size());
    FGB_CHECK(png.rfind("\x89PNG\r\n\x1a\n", 0) == 0);

    // 把 IHDR 宽度改成 10000：尺寸不合理，应被跳过
    std::string huge = data;
    huge[16] = 0;
    huge[17] = 0;
    huge[18] = 0x27;
    huge[19] = 0x10;
    std::string none;
    FGB_CHECK(!find_embedded_png(huge, none));
}

FGB_TEST(icon_outside_module_dir_is_rejected) {
    TempDir root;
    const fs::path src = root.path / "src";
    write_file(src / "module.json",
               R"({"ModuleId":"icon-outside","ModuleName":"Outside",
                   "DllFileName":"Mod.dll","ModuleIcon":"../secret.png"})");
    write_file(src / "Mod.dll", "not a real dll");

    ModuleRegistry registry{root.path / "modules.ini", root.path / "store"};
    std::string error;
    FGB_CHECK(registry.install_path(src, error, "test"));
    write_file(root.path / "store" / "secret.png", "png-bytes");
    registry.rescan();

    const ModuleInfo* module = registry.find("icon-outside");
    FGB_CHECK(module != nullptr);
    if (module != nullptr) {
        FGB_CHECK(module->icon.empty());
    }
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

// 模块库只发 DLL 时，条目里的使用说明/模块页必须跟着兜底元数据落盘，
// 之后重新扫描才能让「模块」页展开卡片显示详情。
FGB_TEST(install_loose_dll_keeps_library_usage) {
    TempDir root;
    const fs::path dll = root.path / "LibOnly.dll";
    write_file(dll, "not a real dll");

    ModuleMetadataOverride metadata;
    metadata.id          = "lib-only";
    metadata.name        = "Lib Only";
    metadata.author      = "Tester";
    metadata.version     = "2.1.0";
    metadata.description = "desc";
    metadata.usage       = "Follow the module page to finish setup.";
    metadata.page        = "https://example.com/lib-only";

    ModuleRegistry registry{root.path / "modules.ini", root.path / "store"};
    std::string error;
    FGB_CHECK(registry.install_path(dll, error, "registry", true, &metadata));
    const ModuleInfo* module = registry.find("lib-only");
    FGB_CHECK(module != nullptr);
    if (module != nullptr) {
        FGB_CHECK(module->usage == "Follow the module page to finish setup.");
        FGB_CHECK(module->page_url == "https://example.com/lib-only");
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

FGB_TEST(module_manifest_min_host_version_is_checked) {
    TempDir root;
    const fs::path dir = root.path / "store" / "future";
    write_file(dir / "module.json",
               R"({"ModuleId":"future-id","ModuleName":"Future","DllFileName":"Mod.dll",
                   "Capabilities":["eye","gaze"],"RoleHint":"gaze",
                   "MinHostVersion":"99.0.0"})");
    write_file(dir / "Mod.dll", "not a real dll");

    ModuleRegistry registry{root.path / "modules.ini", root.path / "store"};
    registry.rescan();
    const ModuleInfo* module = registry.find("future-id");
    FGB_CHECK(module != nullptr);
    if (module != nullptr) {
        FGB_CHECK(module->declared_capabilities == (0x1u | 0x8u));
        FGB_CHECK(module->role_hint == "gaze");
        FGB_CHECK(module->min_host_version == "99.0.0");
        FGB_CHECK(!module->host_compatible);
    }
}

FGB_TEST(module_instances_and_roles_persist) {
    TempDir root;
    make_module_dir(root.path / "face", "face-id", "Face Module");
    make_module_dir(root.path / "gaze", "gaze-id", "Gaze Module");
    const fs::path store = root.path / "store";
    const fs::path state = root.path / "modules.ini";

    {
        ModuleRegistry registry{state, store};
        std::string error;
        FGB_CHECK(registry.install_path(root.path / "face", error));
        FGB_CHECK(registry.install_path(root.path / "gaze", error));

        FGB_CHECK(registry.add_instance("face", "face-id", true));
        FGB_CHECK(registry.add_instance("gaze", "gaze-id"));
        FGB_CHECK(!registry.add_instance("face", "gaze-id")); // 名字重复
        FGB_CHECK(!registry.add_instance("bad", "missing-id"));

        ModuleRoles roles;
        roles.expression = "face-id";
        roles.eyes       = "face-id";
        roles.gaze       = "gaze-id";
        roles.head       = "face-id";
        FGB_CHECK(registry.set_roles(roles));
    }

    ModuleRegistry reopened{state, store};
    reopened.rescan();
    FGB_CHECK(reopened.instances().size() == 2);
    if (reopened.instances().size() == 2) {
        FGB_CHECK(reopened.instances()[0].name == "face");
        FGB_CHECK(reopened.instances()[0].module_id == "face-id");
        FGB_CHECK(reopened.instances()[0].host_process);
        FGB_CHECK(reopened.instances()[1].name == "gaze");
        FGB_CHECK(reopened.instances()[1].module_id == "gaze-id");
        FGB_CHECK(!reopened.instances()[1].host_process);
    }
    FGB_CHECK(reopened.set_instance_host("gaze", true));
    {
        ModuleRegistry host_reopened{state, store};
        host_reopened.rescan();
        FGB_CHECK(host_reopened.instances().size() == 2);
        FGB_CHECK(host_reopened.instances()[0].host_process);
        FGB_CHECK(host_reopened.instances()[1].host_process);
    }
    FGB_CHECK(reopened.roles().expression == "face-id");
    FGB_CHECK(reopened.roles().eyes == "face-id");
    FGB_CHECK(reopened.roles().gaze == "gaze-id");
    FGB_CHECK(reopened.roles().head == "face-id");

    FGB_CHECK(reopened.remove_instance("gaze"));
    FGB_CHECK(!reopened.remove_instance("gaze"));
    ModuleRegistry after_remove{state, store};
    after_remove.rescan();
    FGB_CHECK(after_remove.instances().size() == 1);
}

// 原生模块没有在线注册表，升级只能靠"再装一次新包"。同 ID 的新包必须覆盖旧版本，
// 而不是报"已安装相同 ID 的模块"；同时模块自己写的 config.ini 不能丢。
FGB_TEST(native_module_upgrade_replaces_in_place) {
    TempDir root;
    const fs::path store = root.path / "store";
    const fs::path state = root.path / "modules.ini";

    const fs::path first = root.path / "native-1.0.0.zip";
    write_zip(first, {{"Mod.dll", "v1 dll"},
                      {"fgb-native.json",
                       native_metadata_for("com.test.native", "Native", "Mod.dll", "1.0.0")}});

    ModuleRegistry registry{state, store};
    std::string error;
    InstallOutcome outcome;
    FGB_CHECK(registry.install_path(first, error, "zip", true, nullptr, &outcome));
    FGB_CHECK(!outcome.upgraded);
    FGB_CHECK(outcome.version == "1.0.0");
    FGB_CHECK(outcome.previous_version.empty());

    const ModuleInfo* module = registry.find("com.test.native");
    FGB_CHECK(module != nullptr);
    if (module == nullptr) {
        return;
    }
    FGB_CHECK(module->kind == ModuleKind::native);
    const fs::path dir = module->directory;
    // 模块运行时会自己写配置；升级不能把它清掉
    write_file(dir / "config.ini", "[cfg]\nthreshold=7\n");

    // 新版本换成带版本号的外层目录，install 仍然要认成同一个模块
    const fs::path second = root.path / "native-1.1.0.zip";
    write_zip(second, {{"native-1.1.0/Mod.dll", "v2 dll"},
                       {"native-1.1.0/fgb-native.json",
                        native_metadata_for("com.test.native", "Native", "Mod.dll", "1.1.0")}});

    outcome = InstallOutcome{};
    FGB_CHECK(registry.install_path(second, error, "zip", true, nullptr, &outcome));
    FGB_CHECK(error.empty());
    FGB_CHECK(outcome.upgraded);
    FGB_CHECK(outcome.id == "com.test.native");
    FGB_CHECK(outcome.previous_version == "1.0.0");
    FGB_CHECK(outcome.version == "1.1.0");

    FGB_CHECK(registry.modules().size() == 1);
    const ModuleInfo* upgraded = registry.find("com.test.native");
    FGB_CHECK(upgraded != nullptr);
    if (upgraded != nullptr) {
        FGB_CHECK(upgraded->version == "1.1.0");
        FGB_CHECK(upgraded->directory == dir); // 目录不换名，状态/实例绑定不受影响
        FGB_CHECK(upgraded->kind == ModuleKind::native);
        FGB_CHECK(read_text(upgraded->dll) == "v2 dll");
        FGB_CHECK(fs::exists(upgraded->directory / "config.ini"));
        FGB_CHECK(read_text(upgraded->directory / "config.ini").find("threshold=7") !=
                  std::string::npos);
    }
    FGB_CHECK(registry.active_id() == "com.test.native");

    // 升级过程中用的临时目录不能留在仓里
    std::size_t dirs = 0;
    for (const auto& entry : fs::directory_iterator(store)) {
        if (entry.is_directory()) {
            ++dirs;
        }
    }
    FGB_CHECK(dirs == 1);

    // 不带 replace 的程序化安装仍然拒绝覆盖，并把已装版本写进错误
    FGB_CHECK(!registry.install_path(second, error, "zip"));
    FGB_CHECK(error.find("已安装") != std::string::npos);
    FGB_CHECK(error.find("1.1.0") != std::string::npos);

    // 重新打开也要读到升级后的版本与配置（状态文件没有被升级流程破坏）
    ModuleRegistry reopened{state, store};
    reopened.rescan();
    const ModuleInfo* persisted = reopened.find("com.test.native");
    FGB_CHECK(persisted != nullptr);
    if (persisted != nullptr) {
        FGB_CHECK(persisted->version == "1.1.0");
        FGB_CHECK(fs::exists(persisted->directory / "config.ini"));
    }
}

// VRCFT 模块同样按 ID 升级：模块库"更新"、CLI install 都依赖这条路径
FGB_TEST(vrcft_module_upgrade_replaces_dll_and_metadata) {
    TempDir root;
    const fs::path store = root.path / "store";
    ModuleRegistry registry{root.path / "modules.ini", store};
    std::string error;

    const fs::path v1 = root.path / "pico-0.1.11";
    write_file(v1 / "module.json", metadata_for("pico-id", "Pico", "Pico.dll", "0.1.11"));
    write_file(v1 / "Pico.dll", "v1 dll");
    FGB_CHECK(registry.install_path(v1, error, "test"));

    const fs::path v2 = root.path / "pico-0.1.12";
    write_file(v2 / "module.json", metadata_for("pico-id", "Pico", "Pico.dll", "0.1.12"));
    write_file(v2 / "Pico.dll", "v2 dll");

    InstallOutcome outcome;
    FGB_CHECK(registry.install_path(v2, error, "test", true, nullptr, &outcome));
    FGB_CHECK(outcome.upgraded);
    FGB_CHECK(outcome.previous_version == "0.1.11");
    const ModuleInfo* module = registry.find("pico-id");
    FGB_CHECK(module != nullptr);
    if (module != nullptr) {
        FGB_CHECK(module->version == "0.1.12");
        FGB_CHECK(module->kind == ModuleKind::vrcft);
        FGB_CHECK(read_text(module->dll) == "v2 dll");
        FGB_CHECK(read_text(module->directory / "module.json").find("0.1.12") !=
                  std::string::npos);
    }
    FGB_CHECK(registry.modules().size() == 1);
}
