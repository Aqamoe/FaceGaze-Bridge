#include "web/json.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace fgb::web {
namespace {

// 在主串里找 "key" 之后紧跟的冒号，返回其后的位置
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
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t')) {
        ++pos;
    }
    return pos;
}

} // namespace

void json_append_escaped(std::string& out, std::string_view text) {
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
    json_append_escaped(out_, name);
    out_.push_back(':');
    need_comma_ = false;
    after_key_  = true;
    return *this;
}

JsonWriter& JsonWriter::str(std::string_view value) {
    separate();
    json_append_escaped(out_, value);
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

bool parse_request(const char* data, std::size_t size, HttpRequest& out) {
    if (data == nullptr || size == 0) {
        return false;
    }
    const std::string_view text{data, size};

    const std::size_t line_end = text.find("\r\n");
    if (line_end == std::string_view::npos) {
        return false;
    }
    const std::string_view request_line = text.substr(0, line_end);
    const std::size_t sp1               = request_line.find(' ');
    if (sp1 == std::string_view::npos) {
        return false;
    }
    const std::size_t sp2 = request_line.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos) {
        return false;
    }

    out.method.assign(request_line.substr(0, sp1));
    const std::string_view target = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
    const std::size_t q           = target.find('?');
    if (q == std::string_view::npos) {
        out.path.assign(target);
        out.query.clear();
    } else {
        out.path.assign(target.substr(0, q));
        out.query.assign(target.substr(q + 1));
    }

    const std::size_t header_end = text.find("\r\n\r\n");
    if (header_end != std::string_view::npos) {
        out.body.assign(text.substr(header_end + 4));
    } else {
        out.body.clear();
    }
    return !out.method.empty() && !out.path.empty();
}

bool query_number(std::string_view query, std::string_view key, double& out) {
    if (query.empty()) {
        return false;
    }
    std::size_t pos = query.find(key);
    if (pos == std::string_view::npos) {
        return false;
    }
    pos = query.find('=', pos + key.size());
    if (pos == std::string_view::npos) {
        return false;
    }
    ++pos;
    const std::size_t end = query.find('&', pos);
    const std::string buffer{query.substr(pos, end == std::string_view::npos ? end : end - pos)};
    if (buffer.empty()) {
        return false;
    }
    char* stop = nullptr;
    const double value = std::strtod(buffer.c_str(), &stop);
    if (stop == buffer.c_str()) {
        return false;
    }
    out = value;
    return true;
}

bool query_bool(std::string_view query, std::string_view key, bool& out) {
    std::size_t pos = query.find(key);
    if (pos == std::string_view::npos) {
        return false;
    }
    pos = query.find('=', pos + key.size());
    if (pos == std::string_view::npos) {
        return false;
    }
    ++pos;
    const std::size_t end = query.find('&', pos);
    const std::string_view value = query.substr(pos, end == std::string_view::npos ? end : end - pos);
    if (value == "1" || value == "true") {
        out = true;
        return true;
    }
    if (value == "0" || value == "false") {
        out = false;
        return true;
    }
    return false;
}

bool body_number(std::string_view body, std::string_view key, double& out) {
    const std::size_t pos = find_value_after(body, key);
    if (pos == std::string_view::npos) {
        return false;
    }
    const std::string buffer{body.substr(pos, 32)};
    char* stop         = nullptr;
    const double value = std::strtod(buffer.c_str(), &stop);
    if (stop == buffer.c_str()) {
        return false;
    }
    out = value;
    return true;
}

bool body_string(std::string_view body, std::string_view key, std::string& out) {
    const std::size_t pos = find_value_after(body, key);
    if (pos == std::string_view::npos || pos >= body.size() || body[pos] != '"') {
        return false;
    }
    std::string value;
    std::size_t i = pos + 1;
    while (i < body.size()) {
        const char c = body[i];
        if (c == '\\' && i + 1 < body.size()) {
            switch (body[i + 1]) {
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
                value.push_back(body[i + 1]); // \" \\ \/ 等
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

bool body_bool(std::string_view body, std::string_view key, bool& out) {
    const std::size_t pos = find_value_after(body, key);
    if (pos == std::string_view::npos) {
        return false;
    }
    const std::string_view rest = body.substr(pos);
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

} // namespace fgb::web
