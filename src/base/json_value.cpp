#include "base/json_value.hpp"

#include <cstdlib>

namespace fgb::json {
namespace {

constexpr int kMaxDepth = 32;

void append_utf8(std::string& out, unsigned code) {
    if (code < 0x80) {
        out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
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

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    bool run(Value& out, std::string& error) {
        skip_ws();
        if (!parse_value(out, 0)) {
            error = fail_.empty() ? "JSON 解析失败" : fail_;
            return false;
        }
        skip_ws();
        if (pos_ != text_.size()) {
            error = "JSON 结尾有多余内容（偏移 " + std::to_string(pos_) + "）";
            return false;
        }
        return true;
    }

private:
    bool fail(const char* what) {
        if (fail_.empty()) {
            fail_ = std::string{what} + "（偏移 " + std::to_string(pos_) + "）";
        }
        return false;
    }

    void skip_ws() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    char peek() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }
    bool eof() const { return pos_ >= text_.size(); }

    bool literal(std::string_view word) {
        if (text_.compare(pos_, word.size(), word) != 0) {
            return false;
        }
        pos_ += word.size();
        return true;
    }

    bool hex4(unsigned& out) {
        if (pos_ + 4 > text_.size()) {
            return false;
        }
        unsigned value = 0;
        for (int i = 0; i < 4; ++i) {
            const char h = text_[pos_++];
            value <<= 4;
            if (h >= '0' && h <= '9') {
                value |= static_cast<unsigned>(h - '0');
            } else if (h >= 'a' && h <= 'f') {
                value |= static_cast<unsigned>(h - 'a' + 10);
            } else if (h >= 'A' && h <= 'F') {
                value |= static_cast<unsigned>(h - 'A' + 10);
            } else {
                return false;
            }
        }
        out = value;
        return true;
    }

    bool parse_string(std::string& out) {
        if (peek() != '"') {
            return fail("这里应该是字符串");
        }
        ++pos_;
        out.clear();
        while (!eof()) {
            const char c = text_[pos_++];
            if (c == '"') {
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return fail("字符串里有未转义的控制字符");
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (eof()) {
                break;
            }
            const char esc = text_[pos_++];
            switch (esc) {
            case '"':
            case '\\':
            case '/':
                out.push_back(esc);
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
                unsigned code = 0;
                if (!hex4(code)) {
                    return fail("\\u 转义不完整");
                }
                if (code >= 0xD800 && code <= 0xDBFF && text_.compare(pos_, 2, "\\u") == 0) {
                    pos_ += 2;
                    unsigned low = 0;
                    if (!hex4(low) || low < 0xDC00 || low > 0xDFFF) {
                        return fail("\\u 代理对不完整");
                    }
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                }
                append_utf8(out, code);
                break;
            }
            default:
                return fail("未知的转义字符");
            }
        }
        return fail("字符串没有结束引号");
    }

    bool parse_number(Value& out) {
        const std::size_t begin = pos_;
        if (peek() == '-') {
            ++pos_;
        }
        while (!eof() && text_[pos_] >= '0' && text_[pos_] <= '9') {
            ++pos_;
        }
        if (peek() == '.') {
            ++pos_;
            while (!eof() && text_[pos_] >= '0' && text_[pos_] <= '9') {
                ++pos_;
            }
        }
        if (peek() == 'e' || peek() == 'E') {
            ++pos_;
            if (peek() == '+' || peek() == '-') {
                ++pos_;
            }
            while (!eof() && text_[pos_] >= '0' && text_[pos_] <= '9') {
                ++pos_;
            }
        }
        if (pos_ == begin) {
            return fail("这里应该是数字");
        }
        const std::string token{text_.substr(begin, pos_ - begin)};
        char* end = nullptr;
        const double value = std::strtod(token.c_str(), &end);
        if (end == nullptr || *end != '\0') {
            return fail("数字格式不合法");
        }
        out.kind   = Value::Type::Number;
        out.number = value;
        return true;
    }

    bool parse_object(Value& out, int depth) {
        ++pos_; // '{'
        out.kind = Value::Type::Object;
        skip_ws();
        if (peek() == '}') {
            ++pos_;
            return true;
        }
        for (;;) {
            skip_ws();
            std::string key;
            if (!parse_string(key)) {
                return false;
            }
            skip_ws();
            if (peek() != ':') {
                return fail("键后面缺少冒号");
            }
            ++pos_;
            skip_ws();
            Value child;
            if (!parse_value(child, depth + 1)) {
                return false;
            }
            out.object.emplace_back(std::move(key), std::move(child));
            skip_ws();
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == '}') {
                ++pos_;
                return true;
            }
            return fail("对象没有结束");
        }
    }

    bool parse_array(Value& out, int depth) {
        ++pos_; // '['
        out.kind = Value::Type::Array;
        skip_ws();
        if (peek() == ']') {
            ++pos_;
            return true;
        }
        for (;;) {
            skip_ws();
            Value child;
            if (!parse_value(child, depth + 1)) {
                return false;
            }
            out.array.push_back(std::move(child));
            skip_ws();
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == ']') {
                ++pos_;
                return true;
            }
            return fail("数组没有结束");
        }
    }

    bool parse_value(Value& out, int depth) {
        if (depth > kMaxDepth) {
            return fail("JSON 嵌套过深");
        }
        if (eof()) {
            return fail("JSON 意外结束");
        }
        switch (peek()) {
        case '{':
            return parse_object(out, depth);
        case '[':
            return parse_array(out, depth);
        case '"':
            if (!parse_string(out.string)) {
                return false;
            }
            out.kind = Value::Type::String;
            return true;
        case 't':
            if (!literal("true")) {
                return fail("无法识别的字面量");
            }
            out.kind    = Value::Type::Bool;
            out.boolean = true;
            return true;
        case 'f':
            if (!literal("false")) {
                return fail("无法识别的字面量");
            }
            out.kind    = Value::Type::Bool;
            out.boolean = false;
            return true;
        case 'n':
            if (!literal("null")) {
                return fail("无法识别的字面量");
            }
            out.kind = Value::Type::Null;
            return true;
        default:
            return parse_number(out);
        }
    }

    std::string_view text_;
    std::size_t pos_ = 0;
    std::string fail_;
};

} // namespace

const Value* Value::find(std::string_view key) const noexcept {
    if (kind != Type::Object) {
        return nullptr;
    }
    for (const auto& [name, value] : object) {
        if (name == key) {
            return &value;
        }
    }
    return nullptr;
}

std::string Value::string_at(std::string_view key, std::string fallback) const {
    const Value* value = find(key);
    if (value == nullptr || !value->is_string()) {
        return fallback;
    }
    return value->string;
}

double Value::number_at(std::string_view key, double fallback) const {
    const Value* value = find(key);
    if (value == nullptr || !value->is_number()) {
        return fallback;
    }
    return value->number;
}

bool Value::bool_at(std::string_view key, bool fallback) const {
    const Value* value = find(key);
    if (value == nullptr || !value->is_bool()) {
        return fallback;
    }
    return value->boolean;
}

std::vector<std::string> Value::string_array_at(std::string_view key) const {
    std::vector<std::string> out;
    const Value* value = find(key);
    if (value == nullptr || !value->is_array()) {
        return out;
    }
    for (const Value& item : value->array) {
        if (item.is_string()) {
            out.push_back(item.string);
        }
    }
    return out;
}

bool parse(std::string_view text, Value& out, std::string& error) {
    out = Value{};
    error.clear();
    // GitHub Pages / jsDelivr 偶尔带 UTF-8 BOM
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.remove_prefix(3);
    }
    Parser parser{text};
    return parser.run(out, error);
}

} // namespace fgb::json
