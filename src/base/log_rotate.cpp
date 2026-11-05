#include "base/log_rotate.hpp"

#include <system_error>

namespace fgb {

bool rotate_log_file(const std::filesystem::path& path, std::uintmax_t max_bytes) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec || size <= max_bytes) {
        return false;
    }

    std::filesystem::path backup = path;
    backup += ".1";
    std::error_code remove_error;
    std::filesystem::remove(backup, remove_error); // 上一份备份留不留都行

    std::error_code rename_error;
    std::filesystem::rename(path, backup, rename_error);
    return !rename_error;
}

} // namespace fgb
