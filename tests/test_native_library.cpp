// 原生模块库：registry schema v2 是深结构（modules[] 套 author/requires/stats/
// latest/versions/i18n），解析必须保住嵌套与数组顺序——界面靠它们显示版本历史和
// 兼容性。安装前的包内清单核对同样在这里验证。
#include "vrcft/native_library.hpp"

#include "base/hash.hpp"
#include "fgb_module_api.h"
#include "test_framework.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace fgb::vrcft;

namespace {

constexpr char kRegistry[] = R"json({
  "$schema": "registry.schema.json",
  "schemaVersion": 2,
  "generatedAt": "2026-10-31T10:55:37.113Z",
  "generator": "fgb-registry/2.0.0",
  "registry": {
    "title": "FaceGaze Bridge 模块库",
    "description": "原生模块的公开索引",
    "homepage": "https://aqamoe.github.io/fgb-registry/"
  },
  "moduleCount": 3,
  "modules": [
    {
      "id": "com.picogaze.pico-connect-face",
      "name": "PICO Connect Face",
      "author": { "name": "Aqamoe (光水)", "url": "https://github.com/Aqamoe" },
      "summary": "PICO 4 Pro 面捕，不需要 VRCFT。",
      "description": "直接接收 PICO 互联的 UDP 面捕数据。",
      "usage": "1. 改 settings.json；2. 关掉 VRCFT；3. 重启引擎。",
      "homepage": "https://github.com/Aqamoe/FGB-PicoFaceModule",
      "repo": "Aqamoe/FGB-PicoFaceModule",
      "license": "Apache-2.0",
      "icon": "https://cdn.jsdelivr.net/gh/Aqamoe/FGB-PicoFaceModule@main/module/icon.svg",
      "tags": ["pico", "udp"],
      "capabilities": ["expression", "eyes"],
      "defaultRole": "expression",
      "requires": { "abi": 2, "os": ["windows"], "arch": ["x64"] },
      "channel": "stable",
      "featured": true,
      "verified": true,
      "archived": false,
      "stats": { "downloads": 4, "stars": 12 },
      "publishedAt": "2026-10-31T08:56:27Z",
      "updatedAt": "2026-10-31T08:55:30Z",
      "latest": {
        "version": "0.1.0",
        "channel": "stable",
        "publishedAt": "2026-10-31T08:56:27Z",
        "fileName": "pico-connect-face-0.1.0.zip",
        "downloadUrl": "https://github.com/Aqamoe/FGB-PicoFaceModule/releases/download/v0.1.0/pico-connect-face-0.1.0.zip",
        "size": 99131,
        "sha256": "cd7522527cbcd76675a7392d430dabbfdcc1dd33a9b10d74403df195b5f51c43",
        "abiVersion": 2,
        "downloads": 4,
        "notes": "**Full Changelog**",
        "releaseUrl": "https://github.com/Aqamoe/FGB-PicoFaceModule/releases/tag/v0.1.0"
      },
      "versions": [
        {
          "version": "0.1.0",
          "channel": "stable",
          "downloadUrl": "https://github.com/Aqamoe/FGB-PicoFaceModule/releases/download/v0.1.0/pico-connect-face-0.1.0.zip",
          "size": 99131,
          "sha256": "cd7522527cbcd76675a7392d430dabbfdcc1dd33a9b10d74403df195b5f51c43",
          "abiVersion": 2
        },
        {
          "version": "0.0.9",
          "channel": "stable",
          "downloadUrl": "https://github.com/Aqamoe/FGB-PicoFaceModule/releases/download/v0.0.9/pico-connect-face-0.0.9.zip",
          "size": 90000,
          "sha256": "aa7522527cbcd76675a7392d430dabbfdcc1dd33a9b10d74403df195b5f51c43",
          "abiVersion": 2
        }
      ],
      "i18n": {
        "en": {
          "summary": "PICO 4 Pro/Enterprise face tracking straight from PICO Connect.",
          "description": "Reads the UDP stream PICO Connect already sends.",
          "usage": "Set faceTrackingMode to 1, then restart PICO Connect."
        },
        "ja": { "summary": "\u65e5\u672c\u8a9e\u306e\u6982\u8981" }
      }
    },
    {
      "id": "com.example.beta",
      "name": "Beta Module",
      "author": { "name": "Tester" },
      "repo": "tester/beta",
      "summary": "Needs a newer host.",
      "requires": { "abi": 2, "minHostVersion": "99.0.0", "os": ["windows"], "arch": ["x64"] },
      "channel": "beta",
      "verified": false,
      "latest": {
        "version": "1.0.0-beta.1",
        "channel": "beta",
        "downloadUrl": "https://example.com/beta.zip",
        "size": 10,
        "sha256": "bb7522527cbcd76675a7392d430dabbfdcc1dd33a9b10d74403df195b5f51c43",
        "abiVersion": 2
      },
      "versions": [
        {
          "version": "1.0.0-beta.1",
          "channel": "beta",
          "downloadUrl": "https://example.com/beta.zip",
          "size": 10,
          "sha256": "bb7522527cbcd76675a7392d430dabbfdcc1dd33a9b10d74403df195b5f51c43",
          "abiVersion": 2
        }
      ]
    },
    {
      "id": "com.example.insecure",
      "name": "Insecure Module",
      "author": { "name": "Tester" },
      "repo": "tester/insecure",
      "requires": { "abi": 2, "os": ["windows"], "arch": ["x64"] },
      "latest": {
        "version": "1.0.0",
        "channel": "stable",
        "downloadUrl": "http://example.com/insecure.zip",
        "size": 10,
        "sha256": "cc7522527cbcd76675a7392d430dabbfdcc1dd33a9b10d74403df195b5f51c43",
        "abiVersion": 2
      },
      "versions": []
    }
  ]
})json";

std::filesystem::path make_temp_dir(const char* tag) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                      (std::string{"fgb-native-test-"} + tag + "-" +
                                       std::to_string(stamp));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

void write_file(const std::filesystem::path& path, const std::string& body) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(body.data(), static_cast<std::streamsize>(body.size()));
}

} // namespace

FGB_TEST(native_registry_parses_schema_v2) {
    NativeRegistry registry;
    std::string error;
    FGB_CHECK(parse_native_registry(kRegistry, registry, error));
    FGB_CHECK(error.empty());
    FGB_CHECK(registry.title == "FaceGaze Bridge 模块库");
    FGB_CHECK(registry.homepage == "https://aqamoe.github.io/fgb-registry/");
    FGB_CHECK(registry.generated_at == "2026-10-31T10:55:37.113Z");
    // 第三条是 http 下载地址，必须被丢掉（模块是可执行代码）
    FGB_CHECK(registry.modules.size() == 2);
    if (registry.modules.size() != 2) {
        return;
    }

    const NativeEntry& pico = registry.modules[0];
    FGB_CHECK(pico.id == "com.picogaze.pico-connect-face");
    FGB_CHECK(pico.name == "PICO Connect Face");
    FGB_CHECK(pico.author == "Aqamoe (光水)");
    FGB_CHECK(pico.author_url == "https://github.com/Aqamoe");
    FGB_CHECK(pico.tags.size() == 2);
    FGB_CHECK(pico.capabilities.size() == 2);
    FGB_CHECK(pico.default_role == "expression");
    FGB_CHECK(pico.verified);
    FGB_CHECK(pico.featured);
    FGB_CHECK(!pico.archived);
    FGB_CHECK(pico.stats_downloads == 4);
    FGB_CHECK(pico.stats_stars == 12);
    FGB_CHECK(pico.requires_abi == 2);
    FGB_CHECK(pico.requires_os.size() == 1 && pico.requires_os[0] == "windows");
    FGB_CHECK(pico.requires_arch.size() == 1 && pico.requires_arch[0] == "x64");
    FGB_CHECK(pico.latest.version == "0.1.0");
    FGB_CHECK(pico.latest.size == 99131);
    FGB_CHECK(pico.latest.sha256.size() == 64);
    FGB_CHECK(pico.latest.abi_version == 2);
    FGB_CHECK(pico.versions.size() == 2);
    if (pico.versions.size() == 2) {
        FGB_CHECK(pico.versions[0].version == "0.1.0");
        FGB_CHECK(pico.versions[1].version == "0.0.9");
    }
    FGB_CHECK(pico.i18n.size() == 2);
    FGB_CHECK(pico.compatible);
    FGB_CHECK(pico.incompatible_code.empty());

    const NativeEntry& beta = registry.modules[1];
    FGB_CHECK(beta.channel == "beta");
    FGB_CHECK(beta.latest.channel == "beta");
    FGB_CHECK(!beta.verified);
    // minHostVersion 99.0.0 在 1.x 上永远不满足，列表里要直接标"需要更新宿主"
    FGB_CHECK(!beta.compatible);
    FGB_CHECK(beta.incompatible_code == "host_version");
    FGB_CHECK(!beta.incompatible_reason.empty());
}

FGB_TEST(native_registry_rejects_unknown_schema) {
    std::string text{kRegistry};
    const std::string needle = "\"schemaVersion\": 2";
    text.replace(text.find(needle), needle.size(), "\"schemaVersion\": 3");

    NativeRegistry registry;
    std::string error;
    FGB_CHECK(!parse_native_registry(text, registry, error));
    FGB_CHECK(!error.empty());
    FGB_CHECK(registry.modules.empty());

    error.clear();
    FGB_CHECK(!parse_native_registry(R"({"schemaVersion":2})", registry, error));
    FGB_CHECK(!error.empty());

    error.clear();
    FGB_CHECK(!parse_native_registry("not json at all", registry, error));
    FGB_CHECK(!error.empty());
}

FGB_TEST(native_compatibility_gates_abi_os_arch) {
    HostEnvironment env;
    env.abi          = 2;
    env.host_version = "1.0.4";
    env.os           = "windows";
    env.arch         = "x64";

    NativeEntry entry;
    entry.requires_abi = 2;
    entry.requires_os  = {"windows"};
    entry.requires_arch = {"x64"};
    evaluate_compatibility(entry, env);
    FGB_CHECK(entry.compatible);

    entry.requires_abi = 3;
    evaluate_compatibility(entry, env);
    FGB_CHECK(!entry.compatible);
    FGB_CHECK(entry.incompatible_code == "abi");

    entry.requires_abi = 2;
    entry.requires_os  = {"linux"};
    evaluate_compatibility(entry, env);
    FGB_CHECK(!entry.compatible);
    FGB_CHECK(entry.incompatible_code == "os");

    entry.requires_os   = {"windows"};
    entry.requires_arch = {"arm64"};
    evaluate_compatibility(entry, env);
    FGB_CHECK(!entry.compatible);
    FGB_CHECK(entry.incompatible_code == "arch");

    entry.requires_arch   = {"x64"};
    entry.min_host_version = "2.0.0";
    evaluate_compatibility(entry, env);
    FGB_CHECK(!entry.compatible);
    FGB_CHECK(entry.incompatible_code == "host_version");

    // 只写 latest.abiVersion 的条目也要按 ABI 判断
    NativeEntry legacy;
    legacy.latest.abi_version = 1;
    evaluate_compatibility(legacy, env);
    FGB_CHECK(!legacy.compatible);
    FGB_CHECK(legacy.incompatible_code == "abi");
}

FGB_TEST(native_version_selection_prefers_explicit_version) {
    NativeEntry entry;
    entry.latest.version = "2.0.0";
    entry.versions.resize(2);
    entry.versions[0].version = "2.0.0";
    entry.versions[1].version = "1.9.0";

    FGB_CHECK(select_version(entry, "") != nullptr);
    FGB_CHECK(select_version(entry, "")->version == "2.0.0");
    const NativeVersion* old = select_version(entry, "1.9.0");
    FGB_CHECK(old != nullptr);
    if (old != nullptr) {
        FGB_CHECK(old->version == "1.9.0");
    }
    FGB_CHECK(select_version(entry, "0.1.0") == nullptr);
}

FGB_TEST(native_package_manifest_must_match_entry) {
    const std::filesystem::path dir = make_temp_dir("package");
    write_file(dir / "pico.dll", "not a real dll");
    write_file(dir / "fgb-native.json", R"json({
      "ModuleId": "com.picogaze.pico-connect-face",
      "ModuleName": "PICO Connect Face",
      "Version": "0.1.0",
      "DllFileName": "pico.dll",
      "AbiVersion": 2
    })json");

    NativeEntry entry;
    entry.id           = "com.picogaze.pico-connect-face";
    entry.requires_abi = 2;

    ModuleInfo info;
    std::string error;
    FGB_CHECK(verify_native_package(dir, entry, info, error));
    FGB_CHECK(error.empty());
    FGB_CHECK(info.id == entry.id);
    FGB_CHECK(info.abi_version == 2);
    FGB_CHECK(info.dll.filename() == "pico.dll");

    // 包的 ModuleId 与库条目不一致：拒绝（防止挂羊头卖狗肉）
    NativeEntry other = entry;
    other.id          = "com.example.something-else";
    error.clear();
    FGB_CHECK(!verify_native_package(dir, other, info, error));
    FGB_CHECK(!error.empty());

    // ABI 声明不一致：拒绝
    NativeEntry newer = entry;
    newer.requires_abi = 3;
    error.clear();
    FGB_CHECK(!verify_native_package(dir, newer, info, error));
    FGB_CHECK(!error.empty());

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

FGB_TEST(native_package_rejects_broken_manifests) {
    const std::filesystem::path dir = make_temp_dir("broken");
    NativeEntry entry;
    entry.id           = "com.example.pkg";
    entry.requires_abi = 2;

    ModuleInfo info;
    std::string error;
    // 没有清单
    FGB_CHECK(!verify_native_package(dir, entry, info, error));

    // 有清单但没有 AbiVersion
    write_file(dir / "fgb-native.json", R"json({
      "ModuleId": "com.example.pkg",
      "ModuleName": "Pkg",
      "DllFileName": "pkg.dll"
    })json");
    write_file(dir / "pkg.dll", "x");
    error.clear();
    FGB_CHECK(!verify_native_package(dir, entry, info, error));
    FGB_CHECK(error.find("AbiVersion") != std::string::npos);

    // 清单声明了 DLL，包里却没有
    write_file(dir / "fgb-native.json", R"json({
      "ModuleId": "com.example.pkg",
      "ModuleName": "Pkg",
      "DllFileName": "missing.dll",
      "AbiVersion": 2
    })json");
    error.clear();
    FGB_CHECK(!verify_native_package(dir, entry, info, error));

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

FGB_TEST(native_single_dll_package_gets_manifest) {
    // 单个 DLL 资产没有包内清单：宿主必须补一份 fgb-native.json，
    // 否则模块仓会把它当成 VRCFT 模块（走错加载路径）。
    const std::filesystem::path dir = make_temp_dir("manifest");
    NativeEntry entry;
    entry.id             = "com.example.dllonly";
    entry.name           = "DLL only";
    entry.author         = "Someone";
    entry.description    = "desc";
    entry.usage          = "usage";
    entry.homepage       = "https://example.com/dllonly";
    entry.requires_abi   = 2;
    entry.capabilities   = {"expression", "eyes"};
    entry.default_role   = "expression";

    NativeVersion version;
    version.version    = "1.2.3";
    version.abi_version = 2;

    std::string error;
    FGB_CHECK(write_native_package_manifest(dir, entry, version, "dllonly.dll", error));
    FGB_CHECK(error.empty());

    std::ifstream input(dir / "fgb-native.json", std::ios::binary);
    const std::string body{std::istreambuf_iterator<char>(input),
                           std::istreambuf_iterator<char>()};
    ModuleInfo info;
    FGB_CHECK(parse_module_metadata(body, info));
    FGB_CHECK(info.id == entry.id);
    FGB_CHECK(info.name == entry.name);
    FGB_CHECK(info.version == "1.2.3");
    FGB_CHECK(info.abi_version == 2);
    FGB_CHECK(info.dll.filename().string() == "dllonly.dll");
    FGB_CHECK((info.declared_capabilities & FGB_CAP_EXPRESSION) != 0);
    FGB_CHECK((info.declared_capabilities & FGB_CAP_EYES) != 0);
    FGB_CHECK(info.role_hint == "expression");
    FGB_CHECK(info.page_url == entry.homepage);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

FGB_TEST(native_library_hashes_match_reference_values) {
    const std::filesystem::path dir = make_temp_dir("hash");
    write_file(dir / "abc.txt", "abc");

    FGB_CHECK(fgb::hash::sha256_file(dir / "abc.txt") ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    FGB_CHECK(fgb::hash::md5_file(dir / "abc.txt") == "900150983cd24fb0d6963f7d28e17f72");
    FGB_CHECK(fgb::hash::sha256_file(dir / "missing.txt").empty());

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
