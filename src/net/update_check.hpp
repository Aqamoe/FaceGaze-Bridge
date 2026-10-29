// 更新检查：只读 GitHub Releases 的 latest，不下载、不执行。
//
// 产品策略是"提示用户手动下载"（M6.5）：这样没有静默替换 exe 的回滚风险，
// 也不需要维护差分包。版本比较只处理数字段，预发布后缀不参与排序。
#pragma once

#include <string>
#include <string_view>

namespace fgb::update {

inline constexpr const char* kDefaultUpdateUrl =
    "https://api.github.com/repos/Aqamoe/FaceGaze-Bridge/releases/latest";

struct ReleaseInfo {
    std::string tag;   // v0.2.0
    std::string name;  // 发布标题
    std::string url;   // html_url
    std::string notes; // body（可能为空）
};

// -1 / 0 / 1；忽略前导 v、空白和 -rc1 等后缀
int compare_versions(std::string_view a, std::string_view b) noexcept;

// 解析 GitHub release JSON；至少需要 tag_name。url 缺失时返回空串。
bool parse_release_json(std::string_view body, ReleaseInfo& out) noexcept;

// GET + 解析。网络失败时 error 给可显示原因（含 HTTP 状态）。
bool check_latest(std::string_view api_url, ReleaseInfo& out, std::string& error);

} // namespace fgb::update
