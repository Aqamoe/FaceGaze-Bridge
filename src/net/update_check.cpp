#include "net/update_check.hpp"

#include "base/json.hpp"
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
    json::field_string(body, "tag_name", out.tag);
    if (out.tag.empty()) {
        return false;
    }
    json::field_string(body, "name", out.name);
    json::field_string(body, "html_url", out.url);
    json::field_string(body, "body", out.notes);
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

} // namespace fgb::update
