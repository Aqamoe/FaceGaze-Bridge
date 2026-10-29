#include "params/oscquery.hpp"

#include "base/log.hpp"
#include "net/http_client.hpp"
#include "net/mdns.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string_view>
#include <vector>

namespace fgb::params {
namespace {

constexpr char kTag[]          = "oscquery";
constexpr char kService[]      = "_oscjson._tcp.local";
constexpr char kParamRoot[]    = "/avatar/parameters/";
constexpr std::size_t kMaxBody = 4u * 1024u * 1024u;

std::string lower_ascii(std::string_view text) {
    std::string out{text};
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

void append_utf8(std::string& out, std::uint32_t code) {
    if (code <= 0x7F) {
        out.push_back(static_cast<char>(code));
    } else if (code <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (code >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
}

int hex_digit(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// OSCQuery 节点只用到 FULL_PATH / TYPE / ACCESS / VALUE / CONTENTS；
// 其余字段一律跳过，避免被 VRChat 以后加字段搞崩。
struct Node {
    std::string full_path;
    std::string type;
    std::string value_string;
    int access      = 0;
    bool has_access = false;
    std::vector<Node> children;
};

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    bool parse(Node& root) {
        skip_ws();
        if (!parse_object(root, 0)) {
            return false;
        }
        skip_ws();
        return pos_ == text_.size();
    }

    const std::string& error() const noexcept { return error_; }

private:
    void skip_ws() noexcept {
        while (pos_ < text_.size() &&
               std::isspace(static_cast<unsigned char>(text_[pos_])) != 0) {
            ++pos_;
        }
    }

    bool fail(std::string_view message) {
        if (error_.empty()) {
            error_.assign(message);
        }
        return false;
    }

    bool expect(char c) {
        skip_ws();
        if (pos_ >= text_.size() || text_[pos_] != c) {
            return fail("JSON 结构不完整");
        }
        ++pos_;
        return true;
    }

    bool parse_string(std::string& out) {
        skip_ws();
        if (pos_ >= text_.size() || text_[pos_] != '"') {
            return fail("期望字符串");
        }
        ++pos_;
        out.clear();
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"') {
                return true;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= text_.size()) {
                return fail("字符串转义不完整");
            }
            const char escape = text_[pos_++];
            switch (escape) {
            case '"':
            case '\\':
            case '/':
                out.push_back(escape);
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u': {
                std::uint32_t code = 0;
                for (int i = 0; i < 4; ++i) {
                    if (pos_ >= text_.size() || hex_digit(text_[pos_]) < 0) {
                        return fail("\\u 转义不完整");
                    }
                    code = (code << 4) | static_cast<std::uint32_t>(hex_digit(text_[pos_++]));
                }
                // 代理对：高代理后面必须跟 \\uDC00-\\uDFFF
                if (code >= 0xD800 && code <= 0xDBFF && pos_ + 6 <= text_.size() &&
                    text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
                    std::uint32_t low = 0;
                    bool ok          = true;
                    for (int i = 0; i < 4; ++i) {
                        const int digit = hex_digit(text_[pos_ + 2 + static_cast<std::size_t>(i)]);
                        if (digit < 0) {
                            ok = false;
                            break;
                        }
                        low = (low << 4) | static_cast<std::uint32_t>(digit);
                    }
                    if (ok && low >= 0xDC00 && low <= 0xDFFF) {
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                        pos_ += 6;
                    }
                }
                append_utf8(out, code);
                break;
            }
            default:
                out.push_back(escape);
                break;
            }
        }
        return fail("字符串没有结束引号");
    }

    bool parse_int(int& out) {
        skip_ws();
        const std::size_t begin = pos_;
        if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) {
            ++pos_;
        }
        while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
            ++pos_;
        }
        if (begin == pos_) {
            return fail("期望整数");
        }
        int value = 0;
        for (std::size_t i = begin; i < pos_; ++i) {
            const char c = text_[i];
            if (c >= '0' && c <= '9') {
                value = value * 10 + (c - '0');
            }
        }
        out = (text_[begin] == '-') ? -value : value;
        return true;
    }

    bool skip_value(int depth) {
        if (depth > 64) {
            return fail("JSON 嵌套过深");
        }
        skip_ws();
        if (pos_ >= text_.size()) {
            return fail("JSON 值缺失");
        }
        const char c = text_[pos_];
        if (c == '"') {
            std::string ignored;
            return parse_string(ignored);
        }
        if (c == '{') {
            ++pos_;
            skip_ws();
            if (pos_ < text_.size() && text_[pos_] == '}') {
                ++pos_;
                return true;
            }
            for (;;) {
                std::string key;
                if (!parse_string(key) || !expect(':')) {
                    return false;
                }
                if (!skip_value(depth + 1)) {
                    return false;
                }
                skip_ws();
                if (pos_ < text_.size() && text_[pos_] == ',') {
                    ++pos_;
                    continue;
                }
                return expect('}');
            }
        }
        if (c == '[') {
            ++pos_;
            skip_ws();
            if (pos_ < text_.size() && text_[pos_] == ']') {
                ++pos_;
                return true;
            }
            for (;;) {
                if (!skip_value(depth + 1)) {
                    return false;
                }
                skip_ws();
                if (pos_ < text_.size() && text_[pos_] == ',') {
                    ++pos_;
                    continue;
                }
                return expect(']');
            }
        }
        // true / false / null / 数字：扫到分隔符即可
        const std::size_t begin = pos_;
        while (pos_ < text_.size()) {
            const char d = text_[pos_];
            if (d == ',' || d == '}' || d == ']' || std::isspace(static_cast<unsigned char>(d)) != 0) {
                break;
            }
            ++pos_;
        }
        return pos_ > begin;
    }

    // 只捕获字符串值（VALUE:["avtr_..."] / VALUE:"..."），其它类型跳过
    bool parse_value_capture(std::string& out) {
        skip_ws();
        if (pos_ < text_.size() && text_[pos_] == '"') {
            return parse_string(out);
        }
        if (pos_ < text_.size() && text_[pos_] == '[') {
            ++pos_;
            skip_ws();
            bool captured = false;
            if (pos_ < text_.size() && text_[pos_] == '"') {
                captured = parse_string(out);
                if (!captured) {
                    return false;
                }
            }
            skip_ws();
            if (captured && pos_ < text_.size() && text_[pos_] == ',') {
                ++pos_;
                skip_ws();
            }
            while (pos_ < text_.size() && text_[pos_] != ']') {
                if (!skip_value(1)) {
                    return false;
                }
                skip_ws();
                if (pos_ < text_.size() && text_[pos_] == ',') {
                    ++pos_;
                    skip_ws();
                }
            }
            return expect(']');
        }
        return skip_value(1);
    }

    bool parse_contents(Node& node, int depth) {
        skip_ws();
        if (pos_ >= text_.size() || text_[pos_] != '{') {
            return skip_value(depth + 1);
        }
        ++pos_;
        skip_ws();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            return true;
        }
        for (;;) {
            std::string key;
            if (!parse_string(key) || !expect(':')) {
                return false;
            }
            Node child;
            if (!parse_object(child, depth + 1)) {
                return false;
            }
            node.children.push_back(std::move(child));
            skip_ws();
            if (pos_ < text_.size() && text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            return expect('}');
        }
    }

    bool parse_object(Node& node, int depth) {
        if (depth > 64) {
            return fail("JSON 嵌套过深");
        }
        if (!expect('{')) {
            return false;
        }
        skip_ws();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            return true;
        }
        for (;;) {
            std::string key;
            if (!parse_string(key)) {
                return false;
            }
            if (!expect(':')) {
                return false;
            }
            if (key == "FULL_PATH") {
                if (!parse_string(node.full_path)) {
                    return false;
                }
            } else if (key == "TYPE") {
                if (!parse_string(node.type)) {
                    return false;
                }
            } else if (key == "ACCESS") {
                if (!parse_int(node.access)) {
                    return false;
                }
                node.has_access = true;
            } else if (key == "VALUE") {
                if (!parse_value_capture(node.value_string)) {
                    return false;
                }
            } else if (key == "CONTENTS") {
                if (!parse_contents(node, depth)) {
                    return false;
                }
            } else if (!skip_value(depth + 1)) {
                return false;
            }
            skip_ws();
            if (pos_ < text_.size() && text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            return expect('}');
        }
    }

    std::string_view text_;
    std::size_t pos_ = 0;
    std::string error_;
};

const Node* find_node(const Node& node, std::string_view path) {
    if (node.full_path == path) {
        return &node;
    }
    for (const Node& child : node.children) {
        if (const Node* found = find_node(child, path)) {
            return found;
        }
    }
    return nullptr;
}

void collect_params(const Node& node, std::vector<AvatarParam>& out) {
    for (const Node& child : node.children) {
        const bool writable = !child.has_access || (child.access & 2) != 0;
        const bool has_address =
            child.full_path.size() > sizeof(kParamRoot) - 1 &&
            child.full_path.compare(0, sizeof(kParamRoot) - 1, kParamRoot) == 0;
        if (has_address && writable) {
            if (child.type == "f") {
                out.push_back({child.full_path, ParamKind::Float});
            } else if (child.type == "T" || child.type == "F") {
                out.push_back({child.full_path, ParamKind::Bool});
            }
        }
        collect_params(child, out);
    }
}

bool is_vrchat_instance(std::string_view instance) {
    const std::string lowered = lower_ascii(instance);
    return lowered.rfind("vrchat", 0) == 0;
}

} // namespace

std::string OscQueryEndpoint::base_url() const {
    const std::string host = address.empty() ? "127.0.0.1" : address;
    return "http://" + host + ":" + std::to_string(port) + "/";
}

bool discover_oscquery_endpoint(OscQueryEndpoint& out, std::string& error, int timeout_ms) {
    std::vector<net::MdnsService> services;
    if (!net::mdns_discover(kService, services, timeout_ms)) {
        error = "mDNS 上没有找到 _oscjson._tcp 服务（VRChat 未运行或 OSC 未启用）";
        return false;
    }

    const net::MdnsService* best = nullptr;
    for (const net::MdnsService& service : services) {
        if (service.port == 0 || !is_vrchat_instance(service.instance)) {
            continue;
        }
        if (best == nullptr) {
            best = &service;
        }
    }
    if (best == nullptr) {
        error = "找到 _oscjson._tcp，但没有 VRChat 实例";
        for (const net::MdnsService& service : services) {
            error += "（";
            error += service.instance;
            error += "）";
        }
        return false;
    }

    out.instance = best->instance;
    out.address  = best->address.empty() ? "127.0.0.1" : best->address;
    out.port     = best->port;
    return true;
}

bool parse_oscquery_tree(std::string_view json, AvatarConfig& out, std::string& error) {
    out = AvatarConfig{};
    error.clear();

    Node root;
    Parser parser{json};
    if (!parser.parse(root)) {
        error = parser.error().empty() ? "OSCQuery JSON 解析失败" : parser.error();
        return false;
    }

    if (const Node* avatar = find_node(root, "/avatar")) {
        for (const Node& child : avatar->children) {
            if (child.full_path == "/avatar/id" || child.full_path == "/avatar/change") {
                if (!child.value_string.empty()) {
                    out.id = child.value_string;
                }
            } else if (child.full_path == "/avatar/name") {
                if (!child.value_string.empty()) {
                    out.name = child.value_string;
                }
            }
        }
    }

    const Node* params = find_node(root, "/avatar/parameters");
    if (params == nullptr) {
        error = "OSCQuery 树里没有 /avatar/parameters（头像可能还没加载）";
        return false;
    }
    // 参数为空也是有效结果：这个头像就是没有可写的 float/bool，绑成空表比回退到
    // 固定前缀更诚实——后者会把参数发到并不存在的地址上。
    collect_params(*params, out.params);
    return true;
}

bool parse_oscquery_avatar_change(std::string_view json, std::string& id) {
    id.clear();
    Node root;
    Parser parser{json};
    if (!parser.parse(root)) {
        return false;
    }
    const Node* change = find_node(root, "/avatar/change");
    if (change == nullptr || change->value_string.empty()) {
        return false;
    }
    id = change->value_string;
    return true;
}

std::string normalize_oscquery_base_url(std::string_view url) {
    if (url.empty()) {
        return {};
    }
    std::string base{url};
    if (base.rfind("http://", 0) != 0 && base.rfind("https://", 0) != 0) {
        base.insert(0, "http://");
    }
    if (base.back() != '/') {
        base.push_back('/');
    }
    return base;
}

bool fetch_oscquery_avatar(const std::string& base_url, AvatarConfig& out, std::string& error,
                           int timeout_ms) {
    if (base_url.empty()) {
        error = "OSCQuery 端点无效";
        return false;
    }
    std::string body;
    if (!net::http_get(base_url, body, error, kMaxBody, timeout_ms)) {
        return false;
    }
    if (!parse_oscquery_tree(body, out, error)) {
        return false;
    }
    out.path = base_url; // 日志里能看出参数表来自哪个端点
    return true;
}

bool fetch_oscquery_avatar_change(const std::string& base_url, std::string& id,
                                  std::string& error, int timeout_ms) {
    if (base_url.empty()) {
        error = "OSCQuery 端点无效";
        return false;
    }
    std::string body;
    if (!net::http_get(base_url + "avatar/change", body, error, 64u * 1024u, timeout_ms)) {
        return false;
    }
    if (!parse_oscquery_avatar_change(body, id)) {
        error = "OSCQuery /avatar/change 没有返回头像 ID";
        return false;
    }
    return true;
}

} // namespace fgb::params
