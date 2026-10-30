// 原生模块跨进程宿主：加载一个模块，把 FaceFrame 写进共享内存。
//
// 它不是沙箱；目标只是让模块崩溃/卡死时不带走引擎，并让 DLL 在宿主退出后解锁。
// 模块 ABI 的解析完全复用 NativeModuleSource，避免出现第二套 v1/v2 兼容逻辑。
#include "base/clock.hpp"
#include "base/log.hpp"
#include "fgb_native_host_shm.h"
#include "sources/native_source.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>

namespace {

struct Options {
    std::filesystem::path dll;
    std::string module_id;
    std::string module_name;
    std::string display_name;
    std::wstring shm_name;
    std::filesystem::path log_path;
    int tick_hz = 100;
};

std::string to_utf8(std::wstring_view text) {
#ifdef _WIN32
    if (text.empty()) {
        return {};
    }
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size,
                          nullptr, nullptr);
    return out;
#else
    return std::string{text.begin(), text.end()};
#endif
}

std::wstring mapping_name(const std::wstring& shm) {
    return L"Local\\FGB_NativeHost_" + shm;
}

void set_state(FgbNativeHostBlockV1* block, std::uint32_t state) {
    std::atomic_ref<std::uint32_t>(block->state).store(state, std::memory_order_release);
}

void set_error(FgbNativeHostBlockV1* block, std::string_view text) {
    if (block == nullptr) {
        return;
    }
    const std::size_t copy = text.size() < FGB_HOST_SHM_ERROR_MAX - 1 ? text.size()
                                                                      : FGB_HOST_SHM_ERROR_MAX - 1;
    std::memset(block->error, 0, sizeof(block->error));
    if (copy != 0) {
        std::memcpy(block->error, text.data(), copy);
    }
}

void heartbeat(FgbNativeHostBlockV1* block, std::uint64_t now_ns) {
    std::atomic_ref<std::uint64_t>(block->heartbeat_ns).store(now_ns, std::memory_order_release);
}

void write_log(FgbNativeHostBlockV1* block, int level, const char* text) {
    if (block == nullptr || text == nullptr) {
        return;
    }
    std::atomic_ref<std::uint32_t> head(block->log_head);
    const std::uint32_t sequence = head.load(std::memory_order_relaxed) + 1;
    FgbNativeHostLogV1& entry =
        block->logs[(sequence - 1) % FGB_HOST_SHM_LOG_ENTRIES];
    entry.sequence = sequence;
    entry.level    = static_cast<std::uint32_t>(level);
    std::memset(entry.text, 0, sizeof(entry.text));
    std::strncpy(entry.text, text, sizeof(entry.text) - 1);
    head.store(sequence, std::memory_order_release);
}

bool stop_requested(const FgbNativeHostBlockV1* block) {
    return std::atomic_ref<std::uint32_t>(const_cast<FgbNativeHostBlockV1*>(block)->stop_requested)
               .load(std::memory_order_acquire) != 0;
}

void copy_eye(const fgb::EyeState& in, FgbNativeHostEyeV1& out) {
    out.valid                 = in.valid ? 1u : 0u;
    out.gaze_valid            = in.gazeValid ? 1u : 0u;
    out.openness              = in.openness;
    out.openness_confidence   = in.opennessConf;
    out.pupil_diameter_mm     = in.pupilDiameterMm;
    out.pupil_dilation        = in.pupilDilation;
    out.gaze_dir[0]           = in.gazeDirLocal.x;
    out.gaze_dir[1]           = in.gazeDirLocal.y;
    out.gaze_dir[2]           = in.gazeDirLocal.z;
    out.gaze_confidence       = in.gazeConf;
    out.openness_ts_ns        = in.opennessTsNs;
    out.pupil_ts_ns           = in.pupilTsNs;
    out.gaze_ts_ns            = in.gazeTsNs;
}

void copy_gaze(const fgb::GazeSample& in, FgbNativeHostGazeSampleV1& out) {
    out.valid       = in.valid ? 1u : 0u;
    out.point_valid = in.pointValid ? 1u : 0u;
    out.dir[0]      = in.dirLocal.x;
    out.dir[1]      = in.dirLocal.y;
    out.dir[2]      = in.dirLocal.z;
    out.point2d[0]  = in.point2d.x;
    out.point2d[1]  = in.point2d.y;
    out.confidence  = in.confidence;
    out.capture_ts_ns = in.captureTsNs;
}

void write_frame(FgbNativeHostBlockV1* block, const fgb::FaceFrame& frame, std::uint64_t now_ns) {
    std::atomic_ref<std::uint64_t> sequence(block->sequence);
    std::atomic_ref<std::uint32_t> active(block->active_slot);
    sequence.fetch_add(1, std::memory_order_relaxed); // 奇数：写入中

    const std::uint32_t slot = 1u - active.load(std::memory_order_acquire);
    FgbNativeHostFrameV1& out = block->slots[slot];

    out.sequence      = sequence.load(std::memory_order_relaxed);
    out.capture_ts_ns = frame.captureTsNs;
    out.publish_ts_ns = frame.publishTsNs != 0 ? frame.publishTsNs : now_ns;
    out.channels      = frame.channels;
    out.has_head_pose = frame.hasHeadPose ? 1u : 0u;
    out.quality       = frame.qualityScore;

    out.expression_ts_ns =
        frame.channelStamps[fgb::channel::index(fgb::channel::kExpression)].captureTsNs;
    out.eyes_ts_ns = frame.channelStamps[fgb::channel::index(fgb::channel::kEyes)].captureTsNs;
    out.gaze_ts_ns = frame.channelStamps[fgb::channel::index(fgb::channel::kGaze)].captureTsNs;
    out.head_ts_ns = frame.channelStamps[fgb::channel::index(fgb::channel::kHead)].captureTsNs;

    copy_eye(frame.eyes.left, out.left_eye);
    copy_eye(frame.eyes.right, out.right_eye);

    out.has_vergence       = frame.eyes.hasVergence ? 1u : 0u;
    out.vergence_distance_m = frame.eyes.vergenceDistanceM;
    out.vergence_ts_ns     = frame.eyes.vergenceTsNs;

    copy_gaze(frame.gaze.left, out.gaze_left);
    copy_gaze(frame.gaze.right, out.gaze_right);
    out.combined_gaze_valid = frame.gaze.combinedValid ? 1u : 0u;
    out.points_valid        = (frame.gaze.pointsValid ? FGB_HOST_POINTS_VALID : 0u) |
                              (frame.gaze.pointViewport ? FGB_HOST_POINTS_VIEWPORT : 0u);
    out.combined_gaze_dir[0] = frame.gaze.combinedDirLocal.x;
    out.combined_gaze_dir[1] = frame.gaze.combinedDirLocal.y;
    out.combined_gaze_dir[2] = frame.gaze.combinedDirLocal.z;
    out.combined_gaze_confidence = frame.gaze.confidence;
    out.combined_gaze_ts_ns      = frame.gaze.captureTsNs;

    out.head_rotation[0] = frame.headRotLocal.x;
    out.head_rotation[1] = frame.headRotLocal.y;
    out.head_rotation[2] = frame.headRotLocal.z;
    out.head_rotation[3] = frame.headRotLocal.w;
    out.head_position[0] = frame.headPosLocal.x;
    out.head_position[1] = frame.headPosLocal.y;
    out.head_position[2] = frame.headPosLocal.z;

    std::memcpy(out.shapes, frame.shapes.data(), sizeof(out.shapes));
    std::memcpy(out.shape_valid, frame.shapeValid.data(), sizeof(out.shape_valid));

    sequence.fetch_add(1, std::memory_order_release); // 偶数：写完
    active.store(slot, std::memory_order_release);
    std::atomic_ref<std::uint32_t>(block->frames).fetch_add(1, std::memory_order_relaxed);
    heartbeat(block, now_ns);
}

bool same_options(const Options& options) {
    return !options.dll.empty() && !options.shm_name.empty();
}

int run(const Options& options) {
#ifdef _WIN32
    if (options.log_path.empty()) {
        return 3;
    }
    std::error_code ec;
    if (!options.log_path.parent_path().empty()) {
        std::filesystem::create_directories(options.log_path.parent_path(), ec);
    }
    FILE* log_file = nullptr;
    _wfreopen_s(&log_file, options.log_path.wstring().c_str(), L"ab", stderr);
    _wfreopen_s(&log_file, options.log_path.wstring().c_str(), L"ab", stdout);
#endif

    fgb::log::set_level(fgb::log::Level::info);
    const std::wstring map_name = mapping_name(options.shm_name);
    HANDLE mapping = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                          sizeof(FgbNativeHostBlockV1), map_name.c_str());
    if (mapping == nullptr) {
        FGB_ERROR("host", "创建共享内存失败：%ls（错误码 %lu）", map_name.c_str(),
                  static_cast<unsigned long>(::GetLastError()));
        return 1;
    }
    auto* block = static_cast<FgbNativeHostBlockV1*>(
        ::MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(FgbNativeHostBlockV1)));
    if (block == nullptr) {
        FGB_ERROR("host", "映射共享内存失败：%ls", map_name.c_str());
        ::CloseHandle(mapping);
        return 1;
    }

    std::memset(block, 0, sizeof(*block));
    block->magic       = FGB_HOST_SHM_MAGIC;
    block->version     = FGB_HOST_SHM_VERSION;
    block->struct_size = sizeof(FgbNativeHostBlockV1);
    block->slot_count  = FGB_HOST_SHM_SLOTS;
    set_state(block, FGB_HOST_STATE_STARTING);
    heartbeat(block, fgb::clock::now_ns());

    fgb::NativeSourceConfig config;
    config.dll          = options.dll;
    config.module_id    = options.module_id;
    config.module_name  = options.module_name;
    config.display_name = options.display_name;
    config.tick_hz      = options.tick_hz;
    config.log_sink     = [block](int level, const char* message) {
        write_log(block, level, message);
    };

    fgb::NativeModuleSource source{config};
    if (!source.start()) {
        set_error(block, source.last_error());
        set_state(block, FGB_HOST_STATE_ERROR);
        heartbeat(block, fgb::clock::now_ns());
        FGB_ERROR("host", "模块启动失败：%s", source.last_error().c_str());
        // 让引擎来得及打开映射并读到错误。
        for (int i = 0; i < 100 && !stop_requested(block); ++i) {
            ::Sleep(100);
        }
        ::UnmapViewOfFile(block);
        ::CloseHandle(mapping);
        return 1;
    }

    const auto initial = source.module_status();
    block->capabilities = initial.has_value() ? initial->capabilities : 0;
    set_state(block, FGB_HOST_STATE_ACTIVE);
    set_error(block, initial.has_value() ? initial->error : std::string{});
    heartbeat(block, fgb::clock::now_ns());
    FGB_INFO("host", "模块已就绪：%s (%s)", options.module_name.c_str(),
             options.module_id.c_str());

    const int hz = options.tick_hz > 0 ? options.tick_hz : 100;
    const std::uint64_t period = 1'000'000'000ULL / static_cast<std::uint64_t>(hz);
    std::uint64_t next = fgb::clock::now_ns() + period;

    while (!stop_requested(block)) {
        const std::uint64_t now = fgb::clock::now_ns();
        fgb::FaceFrame frame;
        if (source.poll(frame)) {
            write_frame(block, frame, now);
        }

        const auto status = source.module_status();
        if (status.has_value()) {
            if (!status->alive) {
                set_error(block, "模块已停止");
                set_state(block, FGB_HOST_STATE_STOPPED);
                break;
            }
            // 每拍镜像模块的当前状态，而不是"只在有错时写"：模块报 ERROR 后恢复
            // （report_status(ACTIVE)）必须能把共享内存里的旧错误清掉，否则界面
            // 会一直显示故障，重启实例也救不回来。空串在这里是有意义的。
            set_error(block, status->error);
        }
        heartbeat(block, now);

        fgb::clock::sleep_until_ns(next);
        next += period;
        if (next < fgb::clock::now_ns()) {
            next = fgb::clock::now_ns() + period;
        }
    }

    set_state(block, FGB_HOST_STATE_STOPPED);
    heartbeat(block, fgb::clock::now_ns());
    source.stop();
    ::UnmapViewOfFile(block);
    ::CloseHandle(mapping);
    return 0;
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view arg = argv[i];
        auto value = [&]() -> std::wstring_view {
            if (i + 1 >= argc) {
                return {};
            }
            return argv[++i];
        };
        if (arg == L"--dll") {
            options.dll = value();
        } else if (arg == L"--module-id") {
            options.module_id = to_utf8(value());
        } else if (arg == L"--module-name") {
            options.module_name = to_utf8(value());
        } else if (arg == L"--display-name") {
            options.display_name = to_utf8(value());
        } else if (arg == L"--shm") {
            options.shm_name = value();
        } else if (arg == L"--log") {
            options.log_path = value();
        } else if (arg == L"--tick-hz") {
            options.tick_hz = _wtoi(std::wstring{value()}.c_str());
        } else {
            std::fputs("未知参数：", stderr);
            std::fputs(to_utf8(arg).c_str(), stderr);
            std::fputc('\n', stderr);
            return 2;
        }
    }
    if (!same_options(options) || options.log_path.empty()) {
        std::fprintf(stderr,
                     "用法: fgb-host-native --dll <path> --module-id <id> --module-name <name>\n"
                     "       --display-name <name> --shm <name> --tick-hz <hz> --log <path>\n");
        return 2;
    }
    return run(options);
}
#else
int main() {
    std::fprintf(stderr, "fgb-host-native 仅支持 Windows\n");
    return 1;
}
#endif