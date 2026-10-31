// 更新检查：读 GitHub Releases 的 latest，解析出可下载的资产。
//
// 产品策略（[ADR-0036](../../docs/adr/0036-auto-update.md)）：只对安装器装的副本
// 做"静默下载 + 用户确认后安装"；绿色版/开发版只提示。这里只负责"读"与"挑哪个文件"，
// 不下载、不执行——下载与校验在 update_manager。
//
// 版本比较只处理数字段，预发布后缀不参与排序。
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fgb::update {

inline constexpr const char* kDefaultUpdateUrl =
    "https://api.github.com/repos/Aqamoe/FaceGaze-Bridge/releases/latest";

// 发布页上的一项资产。digest 只有较新的上传才有（形如 "sha256:…"），
// 老资产为空——这正是"没有校验和就不自动安装"那条规则的来源。
struct ReleaseAsset {
    std::string name;
    std::string url; // browser_download_url
    std::string digest;
    std::uint64_t size = 0;
};

struct ReleaseInfo {
    std::string tag;   // v0.2.0
    std::string name;  // 发布标题
    std::string url;   // html_url
    std::string notes; // body（可能为空）
    std::vector<ReleaseAsset> assets;
};

// -1 / 0 / 1；忽略前导 v、空白和 -rc1 等后缀
int compare_versions(std::string_view a, std::string_view b) noexcept;

// 解析 GitHub release JSON；至少需要 tag_name。url 缺失时返回空串。
bool parse_release_json(std::string_view body, ReleaseInfo& out) noexcept;

// GET + 解析。网络失败时 error 给可显示原因（含 HTTP 状态）。
bool check_latest(std::string_view api_url, ReleaseInfo& out, std::string& error);

// 安装包资产：名字形如 FaceGazeBridge-Setup-<version>.exe。优先与 tag 版本完全对应的
// 那个；都没有时取第一个 Setup-*.exe（打包脚本改过命名也还能用）。找不到返回 nullptr。
const ReleaseAsset* find_installer_asset(const ReleaseInfo& info) noexcept;

// SHA256SUMS.txt 资产（release.ps1 生成），找不到返回 nullptr
const ReleaseAsset* find_sums_asset(const ReleaseInfo& info) noexcept;

// 从 "sha256:abcd…" 里取出 "abcd…"；不是 sha256 就返回空串
std::string digest_hex(std::string_view digest) noexcept;

// 从 SHA256SUMS.txt 的文本里取某个文件的 sha256。
// 行格式：<64 位十六进制><空格><空格或 *><文件名>；只认第一段是哈希的行。
bool checksum_for(std::string_view sums, std::string_view filename, std::string& out) noexcept;

} // namespace fgb::update
