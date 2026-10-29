#include "config/ini.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

namespace fgb::config {
namespace {

std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

} // namespace

bool ini_load(const std::string& path, const std::function<void(const IniEntry&)>& on_entry) {
    return ini_load(std::filesystem::path{path}, on_entry);
}

bool ini_load(const std::filesystem::path& path,
              const std::function<void(const IniEntry&)>& on_entry) {
    // 用 path 打开：MSVC 会走宽字符 API，中文目录/文件名不会按 ANSI 丢字
    std::ifstream in(path);
    if (!in) {
        return false;
    }

    std::string section;
    std::string line;
    while (std::getline(in, line)) {
        std::string_view view = trim(line);
        if (view.empty() || view.front() == '#' || view.front() == ';') {
            continue;
        }
        if (view.front() == '[' && view.back() == ']') {
            section.assign(view.substr(1, view.size() - 2));
            continue;
        }
        const std::size_t eq = view.find('=');
        if (eq == std::string_view::npos) {
            continue; // 不认识的行直接忽略，不因为一行写错就整份配置失效
        }
        IniEntry entry;
        entry.section.assign(section);
        entry.key.assign(trim(view.substr(0, eq)));
        entry.value.assign(trim(view.substr(eq + 1)));
        if (on_entry) {
            on_entry(entry);
        }
    }
    return true;
}

bool IniWriter::open(const std::string& path) {
    return open(std::filesystem::path{path});
}

bool IniWriter::open(const std::filesystem::path& path) {
    close();
#ifdef _WIN32
    // _wfopen_s 才能正确处理非 ANSI 的路径
    if (_wfopen_s(&file_, path.wstring().c_str(), L"wb") != 0) {
        file_ = nullptr;
    }
#else
    file_ = std::fopen(path.string().c_str(), "wb");
#endif
    return file_ != nullptr;
}

void IniWriter::close() {
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
}

void IniWriter::section(std::string_view name) {
    if (file_ == nullptr) {
        return;
    }
    std::fprintf(file_, "\n[%.*s]\n", static_cast<int>(name.size()), name.data());
}

void IniWriter::kv(std::string_view key, std::string_view value) {
    if (file_ == nullptr) {
        return;
    }
    std::fprintf(file_, "%.*s = %.*s\n", static_cast<int>(key.size()), key.data(),
                 static_cast<int>(value.size()), value.data());
}

void IniWriter::kv(std::string_view key, double value, int decimals) {
    if (file_ == nullptr) {
        return;
    }
    std::fprintf(file_, "%.*s = %.*f\n", static_cast<int>(key.size()), key.data(), decimals, value);
}

void IniWriter::kv_bool(std::string_view key, bool value) {
    kv(key, std::string_view{value ? "1" : "0"});
}

void IniWriter::comment(std::string_view text) {
    if (file_ == nullptr) {
        return;
    }
    std::fprintf(file_, "# %.*s\n", static_cast<int>(text.size()), text.data());
}

float parse_float(std::string_view text, float fallback) noexcept {
    if (text.empty()) {
        return fallback;
    }
    const std::string buffer{text};
    char* end = nullptr;
    const double value = std::strtod(buffer.c_str(), &end);
    if (end == buffer.c_str()) {
        return fallback;
    }
    return static_cast<float>(value);
}

bool parse_bool(std::string_view text, bool fallback) noexcept {
    if (text == "1" || text == "true" || text == "yes" || text == "on") {
        return true;
    }
    if (text == "0" || text == "false" || text == "no" || text == "off") {
        return false;
    }
    return fallback;
}

} // namespace fgb::config
