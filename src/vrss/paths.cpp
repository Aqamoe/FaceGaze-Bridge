#include "vrss/paths.hpp"

#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fgb::vrss {

std::filesystem::path data_dir() {
    const char* local = std::getenv("LOCALAPPDATA");
    if (local == nullptr || local[0] == '\0') {
        return {};
    }
    return std::filesystem::path{local} / "FaceGazeBridge" / "vrss";
}

std::filesystem::path run_dir() { return data_dir() / "run"; }

std::filesystem::path contracts_dir() { return data_dir() / "contracts"; }

std::filesystem::path runtime_dir() {
#ifdef _WIN32
    wchar_t buffer[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, 32768);
    if (length > 0 && length < 32768) {
        return std::filesystem::path{buffer}.parent_path() / "runtime" / "vrss";
    }
#endif
    return {};
}

} // namespace fgb::vrss
