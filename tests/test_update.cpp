// 更新检查与更新策略的纯逻辑（不联网）：版本比较、release JSON、资产选择、
// 校验和解析、节流时间、update.ini 读写。
#include "net/update_check.hpp"
#include "net/update_manager.hpp"

#include "test_framework.hpp"

#include <filesystem>
#include <string>

using namespace fgb;

namespace {

const char* kReleaseJson = R"({
  "tag_name": "v1.0.5",
  "name": "FaceGaze Bridge 1.0.5",
  "html_url": "https://github.com/Aqamoe/FaceGaze-Bridge/releases/tag/v1.0.5",
  "body": "fix stuff",
  "assets": [
    {"name": "FaceGazeBridge-SDK-1.0.5.zip", "size": 4096,
     "browser_download_url": "https://example.invalid/sdk.zip"},
    {"name": "FaceGazeBridge-Setup-1.0.5.exe", "size": 157286400,
     "digest": "sha256:0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef",
     "browser_download_url": "https://example.invalid/setup.exe"},
    {"name": "SHA256SUMS.txt", "size": 128,
     "browser_download_url": "https://example.invalid/SHA256SUMS.txt"}
  ]
})";

} // namespace

FGB_TEST(update_compare_versions) {
    FGB_CHECK(update::compare_versions("0.1.0", "0.1.0") == 0);
    FGB_CHECK(update::compare_versions("v0.1.0", "0.1.0") == 0);
    FGB_CHECK(update::compare_versions("0.1.1", "0.1.0") > 0);
    FGB_CHECK(update::compare_versions("0.2", "0.1.9") > 0);
    FGB_CHECK(update::compare_versions("1.0.0", "0.9.9") > 0);
    FGB_CHECK(update::compare_versions("0.1.0-rc1", "0.1.0") == 0);
    FGB_CHECK(update::compare_versions("  v0.10.0  ", "0.9.0") > 0);
}

FGB_TEST(update_parse_release_json) {
    const std::string body = R"({
      "tag_name": "v0.2.0",
      "name": "FaceGaze Bridge 0.2.0",
      "html_url": "https://github.com/Aqamoe/FaceGaze-Bridge/releases/tag/v0.2.0",
      "body": "notes\nwith lines",
      "assets": [{"name": "FaceGazeBridge.zip", "size": 123}]
    })";
    update::ReleaseInfo info;
    FGB_CHECK(update::parse_release_json(body, info));
    FGB_CHECK(info.tag == "v0.2.0");
    FGB_CHECK(info.name == "FaceGaze Bridge 0.2.0");
    FGB_CHECK(info.url.find("releases/tag/v0.2.0") != std::string::npos);
    FGB_CHECK(info.notes.find("notes") != std::string::npos);

    FGB_CHECK(!update::parse_release_json("", info));
    FGB_CHECK(!update::parse_release_json(R"({"message":"Not Found"})", info));
}

FGB_TEST(update_parse_assets_and_pick_installer) {
    update::ReleaseInfo info;
    FGB_CHECK(update::parse_release_json(kReleaseJson, info));
    FGB_CHECK(info.assets.size() == 3);

    const update::ReleaseAsset* setup = update::find_installer_asset(info);
    FGB_CHECK(setup != nullptr);
    if (setup != nullptr) {
        FGB_CHECK(setup->name == "FaceGazeBridge-Setup-1.0.5.exe");
        FGB_CHECK(setup->size == 157286400ULL);
        FGB_CHECK(setup->url == "https://example.invalid/setup.exe");
        // digest 统一小写，后面要和 sha256_file 的输出直接比
        FGB_CHECK(update::digest_hex(setup->digest) ==
                  "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    }

    const update::ReleaseAsset* sums = update::find_sums_asset(info);
    FGB_CHECK(sums != nullptr);
    if (sums != nullptr) {
        FGB_CHECK(sums->name == "SHA256SUMS.txt");
    }

    // 没有 Setup 资产：不能自动下载（否则会下到 SDK 包并当成安装器执行）
    update::ReleaseInfo sdk_only;
    FGB_CHECK(update::parse_release_json(R"({"tag_name":"v1.0.5","assets":[
        {"name":"FaceGazeBridge-SDK-1.0.5.zip","browser_download_url":"https://example.invalid/x"}
    ]})", sdk_only));
    FGB_CHECK(update::find_installer_asset(sdk_only) == nullptr);
    FGB_CHECK(update::find_sums_asset(sdk_only) == nullptr);
}

FGB_TEST(update_digest_hex_rejects_other_algorithms) {
    FGB_CHECK(update::digest_hex("sha256:ABC123") == "abc123"); // 统一小写
    FGB_CHECK(update::digest_hex("md5:abc").empty());
    FGB_CHECK(update::digest_hex("").empty());
    FGB_CHECK(update::digest_hex("sha256:").empty());
    FGB_CHECK(update::digest_hex("sha256:zz").empty());
}

FGB_TEST(update_checksum_for_sums_file) {
    const std::string sums =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  FaceGazeBridge-Setup-1.0.5.exe\n"
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb *vrcft-runtime.zip\n";
    std::string hex;
    FGB_CHECK(update::checksum_for(sums, "FaceGazeBridge-Setup-1.0.5.exe", hex));
    FGB_CHECK(hex == std::string(64, 'a'));
    // 星号（sha256sum -b）与前后空白都要能认
    FGB_CHECK(update::checksum_for(sums, "vrcft-runtime.zip", hex));
    FGB_CHECK(hex == std::string(64, 'b'));
    FGB_CHECK(!update::checksum_for(sums, "nope.exe", hex));
    FGB_CHECK(!update::checksum_for("", "x", hex));
}

FGB_TEST(update_next_check_time_throttles) {
    const std::int64_t now = 1'000'000;
    // 成功：24 小时 + 抖动（抖动上限 4 小时）
    FGB_CHECK(update::next_check_time(now, true, 0) == now + 24 * 3600);
    FGB_CHECK(update::next_check_time(now, true, 3600) == now + 25 * 3600);
    FGB_CHECK(update::next_check_time(now, true, 99'999) == now + 28 * 3600);
    // 失败：退避 6 小时，抖动不参与
    FGB_CHECK(update::next_check_time(now, false, 3600) == now + 6 * 3600);
}

FGB_TEST(update_preferences_roundtrip) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "fgb-test-update-prefs";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path path = dir / "update.ini";
    std::filesystem::remove(path, ec);

    update::Preferences saved;
    saved.next_check_at   = 1'700'000'000;
    saved.skipped_version = "1.0.5";
    saved.auto_check      = false;
    saved.staged_version  = "1.0.6";
    saved.staged_path     = "C:\\Users\\x\\AppData\\Local\\FaceGazeBridge\\updates\\setup.exe";
    saved.staged_sha256   = std::string(64, 'c');
    saved.staged_size     = 157286400;
    FGB_CHECK(update::save_preferences(path, saved));

    update::Preferences loaded;
    FGB_CHECK(update::load_preferences(path, loaded));
    FGB_CHECK(loaded.next_check_at == saved.next_check_at);
    FGB_CHECK(loaded.skipped_version == "1.0.5");
    FGB_CHECK(loaded.auto_check == false);
    FGB_CHECK(loaded.staged_version == "1.0.6");
    FGB_CHECK(loaded.staged_path == saved.staged_path);
    FGB_CHECK(loaded.staged_sha256 == saved.staged_sha256);
    FGB_CHECK(loaded.staged_size == saved.staged_size);

    // 文件不存在时不该抛出，也不该给出半截默认值
    update::Preferences missing;
    FGB_CHECK(!update::load_preferences(dir / "nope.ini", missing));
    FGB_CHECK(missing.next_check_at == 0);
    FGB_CHECK(missing.auto_check == true); // 默认开着自动检查

    std::filesystem::remove_all(dir, ec);
}
