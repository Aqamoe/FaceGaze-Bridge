#include "base/json.hpp"

#include <cstdio>
#include <cstdlib>

namespace fgb::json {
namespace {

// 在主串里找 "key" 之后紧跟的冒号，返回其后的第一个非空白字符位置
std::size_t find_value_after(std::string_view text, std::string_view key) {
    std::string needle;
    needle.reserve(key.size() + 2);
    needle.push_back('"');
    needle.append(key);
    needle.push_back('"');

    std::size_t pos = text.find(needle);
    if (pos == std::string_view::npos) {
        return std::string_view::npos;
    }
    pos = text.find(':', pos + needle.size());
    if (pos == std::string_view::npos) {
        return std::string_view::npos;
    }
    ++pos;
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\r' ||
                                 text[pos] == '\n')) {
        ++pos;
    }
    return pos;
}

} // namespace

void append_escaped(std::string& out, std::string_view text) {
    out.push_back('"');
    for (const char c : text) {
        switch (c) {
        case '"':
            out.append("\\\"");
            break;
        case '\\':
            out.append("\\\\");
            break;
        case '\n':
            out.append("\\n");
            break;
        case '\r':
            out.append("\\r");
            break;
        case '\t':
            out.append("\\t");
            break;
        default:
            out.push_back(c);
            break;
        }
    }
    out.push_back('"');
}

void JsonWriter::separate() {
    if (after_key_) {
        after_key_ = false;
        return;
    }
    if (need_comma_) {
        out_.push_back(',');
    }
}

JsonWriter& JsonWriter::obj_begin() {
    separate();
    out_.push_back('{');
    need_comma_ = false;
    return *this;
}

JsonWriter& JsonWriter::obj_end() {
    out_.push_back('}');
    need_comma_ = true;
    return *this;
}

JsonWriter& JsonWriter::arr_begin() {
    separate();
    out_.push_back('[');
    need_comma_ = false;
    return *this;
}

JsonWriter& JsonWriter::arr_end() {
    out_.push_back(']');
    need_comma_ = true;
    return *this;
}

JsonWriter& JsonWriter::key(std::string_view name) {
    separate();
    append_escaped(out_, name);
    out_.push_back(':');
    need_comma_ = false;
    after_key_  = true;
    return *this;
}

JsonWriter& JsonWriter::str(std::string_view value) {
    separate();
    append_escaped(out_, value);
    need_comma_ = true;
    return *this;
}

JsonWriter& JsonWriter::num(double value, int decimals) {
    separate();
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    out_.append(buffer);
    need_comma_ = true;
    return *this;
}

JsonWriter& JsonWriter::integer(long long value) {
    separate();
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%lld", value);
    out_.append(buffer);
    need_comma_ = true;
    return *this;
}

JsonWriter& JsonWriter::boolean(bool value) {
    separate();
    out_.append(value ? "true" : "false");
    need_comma_ = true;
    return *this;
}

bool field_number(std::string_view text, std::string_view key, double& out) {
    const std::size_t pos = find_value_after(text, key);
    if (pos == std::string_view::npos) {
        return false;
    }
    const std::string buffer{text.substr(pos, 32)};
    char* stop         = nullptr;
    const double value = std::strtod(buffer.c_str(), &stop);
    if (stop == buffer.c_str()) {
        return false;
    }
    out = value;
    return true;
}

bool field_bool(std::string_view text, std::string_view key, bool& out) {
    const std::size_t pos = find_value_after(text, key);
    if (pos == std::string_view::npos) {
        return false;
    }
    const std::string_view rest = text.substr(pos);
    if (rest.rfind("true", 0) == 0) {
        out = true;
        return true;
    }
    if (rest.rfind("false", 0) == 0) {
        out = false;
        return true;
    }
    return false;
}

bool field_string(std::string_view text, std::string_view key, std::string& out) {
    const std::size_t pos = find_value_after(text, key);
    // null 也是"没有值"：module.json 里 FileHash 就是 null
    if (pos == std::string_view::npos || pos >= text.size() || text[pos] != '"') {
        return false;
    }

    std::string value;
    std::size_t i = pos + 1;
    while (i < text.size()) {
        const char c = text[i];
        if (c == '\\' && i + 1 < text.size()) {
            switch (text[i + 1]) {
            case 'n':
                value.push_back('\n');
                break;
            case 'r':
                value.push_back('\r');
                break;
            case 't':
                value.push_back('\t');
                break;
            default:
                value.push_back(text[i + 1]); // \" \\ \/ 等
                break;
            }
            i += 2;
            continue;
        }
        if (c == '"') {
            out = std::move(value);
            return true;
        }
        value.push_back(c);
        ++i;
    }
    return false;
}

} // namespace fgb::json
