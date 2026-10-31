#include "net/update_check.hpp"

#include "base/json.hpp"
#include "base/json_value.hpp"
#include "net/http_client.hpp"

#include <cctype>
#include <cstdlib>

namespace fgb::update {
namespace {

std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
        text.remove_suffix(1);
    }
    return text;
}

std::string_view next_component(std::string_view& text) noexcept {
    const std::size_t dot = text.find('.');
    std::string_view part = dot == std::string_view::npos ? text : text.substr(0, dot);
    text = dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);
    return part;
}

unsigned long numeric_component(std::string_view part) noexcept {
    // 停在第一个非数字：1-rc1 / 1.2.3+build 都只取前缀
    unsigned long value = 0;
    for (const char c : part) {
        if (c < '0' || c > '9') {
            break;
        }
        value = value * 10 + static_cast<unsigned long>(c - '0');
    }
    return value;
}

bool equals_ci(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

bool ends_with_ci(std::string_view text, std::string_view suffix) noexcept {
    if (text.size() < suffix.size()) {
        return false;
    }
    const std::size_t offset = text.size() - suffix.size();
    for (std::size_t i = 0; i < suffix.size(); ++i) {
        const auto left  = static_cast<unsigned char>(text[offset + i]);
        const auto right = static_cast<unsigned char>(suffix[i]);
        if (std::tolower(left) != std::tolower(right)) {
            return false;
        }
    }
    return true;
}

} // namespace

int compare_versions(std::string_view a, std::string_view b) noexcept {
    a = trim(a);
    b = trim(b);
    if (!a.empty() && (a.front() == 'v' || a.front() == 'V')) {
        a.remove_prefix(1);
    }
    if (!b.empty() && (b.front() == 'v' || b.front() == 'V')) {
        b.remove_prefix(1);
    }

    for (int i = 0; i < 8; ++i) {
        const bool a_empty = a.empty();
        const bool b_empty = b.empty();
        if (a_empty && b_empty) {
            return 0;
        }
        const unsigned long left  = a_empty ? 0UL : numeric_component(next_component(a));
        const unsigned long right = b_empty ? 0UL : numeric_component(next_component(b));
        if (left != right) {
            return left < right ? -1 : 1;
        }
        if (a_empty) {
            a = {};
        }
        if (b_empty) {
            b = {};
        }
    }
    return 0;
}

bool parse_release_json(std::string_view body, ReleaseInfo& out) noexcept {
    out = ReleaseInfo{};
    if (body.empty()) {
        return false;
    }
    // 以前用"一层标量"读，assets[] 读不到；现在唯一需要深结构的地方是资产列表，
    // 用 base/json_value 那棵树，避免再写一套专用解析器。
    json::Value root;
    std::string error;
    if (!json::parse(body, root, error) || !root.is_object()) {
        return false;
    }
    out.tag = root.string_at("tag_name");
    if (out.tag.empty()) {
        return false;
    }
    out.name  = root.string_at("name");
    out.url   = root.string_at("html_url");
    out.notes = root.string_at("body");

    if (const json::Value* assets = root.find("assets"); assets != nullptr && assets->is_array()) {
        out.assets.reserve(assets->array.size());
        for (const json::Value& item : assets->array) {
            if (!item.is_object()) {
                continue;
            }
            ReleaseAsset asset;
            asset.name   = item.string_at("name");
            asset.url    = item.string_at("browser_download_url");
            asset.digest = item.string_at("digest");
            const double size = item.number_at("size", 0.0);
            asset.size = size > 0.0 ? static_cast<std::uint64_t>(size) : 0U;
            // 名称或地址缺一个就没法下，直接跳过而不是留半条记录
            if (asset.name.empty() || asset.url.empty()) {
                continue;
            }
            out.assets.push_back(std::move(asset));
        }
    }
    return true;
}

bool check_latest(std::string_view api_url, ReleaseInfo& out, std::string& error) {
    out = ReleaseInfo{};
    error.clear();
    if (api_url.empty()) {
        error = "更新地址为空";
        return false;
    }
    std::string body;
    // 本机地址（探针）收紧超时并绕过系统代理；公网地址走默认代理设置
    const bool local = api_url.find("127.0.0.1") != std::string_view::npos ||
                       api_url.find("localhost") != std::string_view::npos;
    if (!net::http_get(std::string{api_url}, body, error, net::kDefaultTextLimit,
                       local ? 1500 : 0)) {
        return false;
    }
    if (!parse_release_json(body, out)) {
        error = "发布信息解析失败";
        return false;
    }
    return true;
}

const ReleaseAsset* find_installer_asset(const ReleaseInfo& info) noexcept {
    std::string_view version = trim(info.tag);
    if (!version.empty() && (version.front() == 'v' || version.front() == 'V')) {
        version.remove_prefix(1);
    }
    const std::string preferred = "FaceGazeBridge-Setup-" + std::string{version} + ".exe";

    const ReleaseAsset* fallback = nullptr;
    for (const ReleaseAsset& asset : info.assets) {
        if (!ends_with_ci(asset.name, ".exe")) {
            continue;
        }
        if (asset.name.rfind("FaceGazeBridge-Setup-", 0) != 0) {
            continue;
        }
        if (asset.name == preferred) {
            return &asset;
        }
        if (fallback == nullptr) {
            fallback = &asset;
        }
    }
    return fallback;
}

const ReleaseAsset* find_sums_asset(const ReleaseInfo& info) noexcept {
    for (const ReleaseAsset& asset : info.assets) {
        if (equals_ci(asset.name, "SHA256SUMS.txt")) {
            return &asset;
        }
    }
    return nullptr;
}

std::string digest_hex(std::string_view digest) noexcept {
    const std::string_view prefix = "sha256:";
    if (digest.size() <= prefix.size()) {
        return {};
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(digest[i])) != prefix[i]) {
            return {};
        }
    }
    std::string hex;
    hex.reserve(digest.size() - prefix.size());
    for (std::size_t i = prefix.size(); i < digest.size(); ++i) {
        const auto c = static_cast<unsigned char>(digest[i]);
        if (std::isxdigit(c) == 0) {
            return {};
        }
        hex.push_back(static_cast<char>(std::tolower(c)));
    }
    return hex;
}

bool checksum_for(std::string_view sums, std::string_view filename, std::string& out) noexcept {
    out.clear();
    if (sums.empty() || filename.empty()) {
        return false;
    }
    std::size_t pos = 0;
    while (pos <= sums.size()) {
        const std::size_t end = sums.find('\n', pos);
        const std::string_view line =
            end == std::string_view::npos ? sums.substr(pos) : sums.substr(pos, end - pos);
        pos = end == std::string_view::npos ? sums.size() + 1 : end + 1;

        const std::string_view trimmed = trim(line);
        const std::size_t split        = trimmed.find_first_of(" \t");
        if (split == std::string_view::npos) {
            continue;
        }
        const std::string_view hash = trimmed.substr(0, split);
        if (hash.size() != 64) {
            continue;
        }
        bool hex_only = true;
        for (const char c : hash) {
            if (std::isxdigit(static_cast<unsigned char>(c)) == 0) {
                hex_only = false;
                break;
            }
        }
        if (!hex_only) {
            continue;
        }
        // 星号标记二进制模式（sha256sum -b），文件名部分可能带前导空格或 '*'
        std::string_view name = trim(trimmed.substr(split));
        if (!name.empty() && name.front() == '*') {
            name.remove_prefix(1);
            name = trim(name);
        }
        if (name != filename) {
            continue;
        }
        out.assign(hash.data(), hash.size());
        for (char& c : out) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return true;
    }
    return false;
}

} // namespace fgb::update
