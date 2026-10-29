// 崩溃转储：至少保证能写出一份可被调试器识别的 minidump，且旧文件会被清理。
#include "base/crash.hpp"

#include "test_framework.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace fgb;

namespace {

namespace fs = std::filesystem;

struct TempDir {
    fs::path path;
    TempDir() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("fgb-crash-" + std::to_string(stamp));
        std::error_code ec;
        fs::create_directories(path, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

} // namespace

FGB_TEST(crash_minidump_writes_recognisable_file) {
    TempDir temp;
    const fs::path path = temp.path / "self.dmp";
    std::string error;
    FGB_CHECK(crash::write_minidump(path, error));
    FGB_CHECK(error.empty());

    std::error_code ec;
    FGB_CHECK(fs::is_regular_file(path, ec));
    FGB_CHECK(fs::file_size(path, ec) > 1024);

    std::ifstream in(path, std::ios::binary);
    char magic[4]{};
    in.read(magic, sizeof(magic));
    FGB_CHECK(magic[0] == 'M' && magic[1] == 'D' && magic[2] == 'M' && magic[3] == 'P');
}

FGB_TEST(crash_prune_keeps_newest_dumps) {
    TempDir temp;
    std::error_code ec;
    for (int i = 0; i < 3; ++i) {
        const fs::path path = temp.path / ("old-" + std::to_string(i) + ".dmp");
        std::ofstream out(path, std::ios::binary);
        out << "MDMP";
        out.close();
        // 让修改时间明确递增，避免同一时钟粒度下排序不稳定
        fs::last_write_time(path,
                            fs::file_time_type::clock::now() + std::chrono::seconds(i), ec);
        fs::path sidecar = path;
        sidecar.replace_extension(".txt");
        std::ofstream(sidecar) << "sidecar\n";
    }

    crash::prune_old_dumps(temp.path, 2);

    int dumps = 0;
    int sidecars = 0;
    for (const auto& entry : fs::directory_iterator(temp.path, ec)) {
        if (entry.path().extension() == ".dmp") {
            ++dumps;
        } else if (entry.path().extension() == ".txt") {
            ++sidecars;
        }
    }
    FGB_CHECK(dumps == 2);
    FGB_CHECK(sidecars == 2);
}
