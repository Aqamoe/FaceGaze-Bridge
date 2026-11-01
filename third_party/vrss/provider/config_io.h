#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <filesystem>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>

// Allow an atomic settings replacement while holding an old read handle.
inline std::map<std::string, std::string> ReadSharedConfig(const std::filesystem::path& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot open config/geometry");
    LARGE_INTEGER size{}; DWORD count{}; std::string text;
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart <= 65536;
    if (ok) {
        text.resize(size_t(size.QuadPart));
        ok = ReadFile(file, text.data(), DWORD(text.size()), &count, nullptr) && count == text.size();
    }
    CloseHandle(file);
    if (!ok) throw std::runtime_error("Cannot read complete config/geometry");
    std::map<std::string, std::string> fields; std::istringstream in(text); std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto equal = line.find('=');
        if (equal != std::string::npos) fields[line.substr(0, equal)] = line.substr(equal + 1);
    }
    return fields;
}
