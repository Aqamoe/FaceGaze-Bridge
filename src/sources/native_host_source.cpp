#include "sources/native_host_source.hpp"

#include "base/clock.hpp"
#include "base/log.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string_view>

namespace fgb {
namespace {

constexpr char kTag[] = "native-host";
constexpr std::uint64_t kHostHeartbeatTimeoutNs = 2'000'000'000ULL;

std::string sanitize(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (const char c : name) {
        const auto uc = static_cast<unsigned char>(c);
        const bool ok = (uc >= '0' && uc <= '9') || (uc >= 'a' && uc <= 'z') ||
                        (uc >= 'A' && uc <= 'Z') || c == '.' || c == '-' || c == '_';
        out.push_back(ok ? c : '_');
    }
    return out.empty() ? "default" : out;
}

#ifdef _WIN32
std::wstring widen(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(),
                          size);
    return out;
}

std::wstring quote(const std::wstring& text) { return L"\"" + text + L"\""; }

std::string block_error_from(const FgbNativeHostBlockV1* block) {
    if (block == nullptr) {
        return {};
    }
    const char* begin = block->error;
    const char* end   = std::find(begin, begin + FGB_HOST_SHM_ERROR_MAX, '\0');
    return std::string{begin, end};
}
#endif

void copy_eye(const FgbNativeHostEyeV1& in, EyeState& out) {
    out.valid            = in.valid != 0;
    out.gazeValid        = in.gaze_valid != 0;
    out.openness         = in.openness;
    out.opennessConf     = in.openness_confidence;
    out.pupilDiameterMm  = in.pupil_diameter_mm;
    out.pupilDilation    = in.pupil_dilation;
    out.gazeDirLocal     = Vec3{in.gaze_dir[0], in.gaze_dir[1], in.gaze_dir[2]};
    out.gazeConf         = in.gaze_confidence;
    out.opennessTsNs     = in.openness_ts_ns;
    out.pupilTsNs        = in.pupil_ts_ns;
    out.gazeTsNs         = in.gaze_ts_ns;
    out.gazePitchYawDeg  = dir_to_pitch_yaw_deg(out.gazeDirLocal);
}

void copy_gaze(const FgbNativeHostGazeSampleV1& in, GazeSample& out) {
    out.valid       = in.valid != 0;
    out.pointValid  = in.point_valid != 0;
    out.dirLocal    = Vec3{in.dir[0], in.dir[1], in.dir[2]};
    out.point2d     = Vec2{in.point2d[0], in.point2d[1]};
    out.confidence  = in.confidence;
    out.captureTsNs = in.capture_ts_ns;
}

FaceFrame to_face(const FgbNativeHostFrameV1& in) {
    FaceFrame frame;
    frame.captureTsNs = in.capture_ts_ns;
    frame.publishTsNs = in.publish_ts_ns;
    frame.channels    = static_cast<std::uint8_t>(in.channels);
    frame.qualityScore = in.quality;

    copy_eye(in.left_eye, frame.eyes.left);
    copy_eye(in.right_eye, frame.eyes.right);
    frame.eyes.hasVergence       = in.has_vergence != 0;
    frame.eyes.vergenceDistanceM = in.vergence_distance_m;
    frame.eyes.vergenceTsNs      = in.vergence_ts_ns;

    copy_gaze(in.gaze_left, frame.gaze.left);
    copy_gaze(in.gaze_right, frame.gaze.right);
    frame.gaze.combinedValid   = in.combined_gaze_valid != 0;
    frame.gaze.combinedDirLocal =
        Vec3{in.combined_gaze_dir[0], in.combined_gaze_dir[1], in.combined_gaze_dir[2]};
    frame.gaze.confidence  = in.combined_gaze_confidence;
    frame.gaze.captureTsNs = in.combined_gaze_ts_ns;
    frame.gaze.pointsValid = in.points_valid != 0;

    frame.hasHeadPose  = in.has_head_pose != 0;
    frame.headRotLocal = Quat{in.head_rotation[0], in.head_rotation[1], in.head_rotation[2],
                              in.head_rotation[3]};
    frame.headPosLocal =
        Vec3{in.head_position[0], in.head_position[1], in.head_position[2]};

    std::memcpy(frame.shapes.data(), in.shapes, sizeof(in.shapes));
    std::memcpy(frame.shapeValid.data(), in.shape_valid, sizeof(in.shape_valid));

    frame.channelStamps[channel::index(channel::kExpression)].captureTsNs = in.expression_ts_ns;
    frame.channelStamps[channel::index(channel::kEyes)].captureTsNs       = in.eyes_ts_ns;
    frame.channelStamps[channel::index(channel::kGaze)].captureTsNs       = in.gaze_ts_ns;
    frame.channelStamps[channel::index(channel::kHead)].captureTsNs       = in.head_ts_ns;

    frame.sanitize();
    return frame;
}

} // namespace

NativeHostSource::NativeHostSource(NativeHostConfig config)
    : config_(std::move(config)) {
    mapping_name_ = "Local\\FGB_NativeHost_" +
                    sanitize(config_.shm_name.empty() ? config_.display_name : config_.shm_name);
}

NativeHostSource::~NativeHostSource() { stop(); }

bool NativeHostSource::start() {
    if (started_) {
        return true;
    }
    if (restart_thread_.joinable()) {
        restart_thread_.join();
    }
    stopping_.store(false, std::memory_order_release);
    restarting_.store(false, std::memory_order_release);
    restarts_.store(0, std::memory_order_release);
    last_error_.clear();
    exited_ = false;
    if (!spawn()) {
        return false;
    }
    if (!open_block()) {
        stop();
        return false;
    }
    if (!wait_ready()) {
        stop();
        return false;
    }
    started_ = true;
    last_log_sequence_ = 0;
    FGB_INFO(kTag, "%s 宿主已就绪（pid=%lu）", config_.display_name.c_str(),
             static_cast<unsigned long>(pid_));
    return true;
}

bool NativeHostSource::spawn() {
#ifdef _WIN32
    std::error_code ec;
    if (!std::filesystem::exists(config_.host_exe, ec)) {
        last_error_ = "找不到原生模块宿主：" + config_.host_exe.string();
        return false;
    }
    if (!std::filesystem::exists(config_.module_dll, ec)) {
        last_error_ = "找不到模块 DLL：" + config_.module_dll.string();
        return false;
    }

    job_ = ::CreateJobObjectW(nullptr, nullptr);
    if (job_ == nullptr) {
        last_error_ = "创建 Job Object 失败";
        return false;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    ::SetInformationJobObject(static_cast<HANDLE>(job_), JobObjectExtendedLimitInformation,
                              &limits, sizeof(limits));

    std::filesystem::path log_path = config_.log_path;
    if (log_path.empty()) {
        log_path = std::filesystem::temp_directory_path(ec) /
                   ("fgb-host-" + sanitize(config_.shm_name) + ".log");
    }
    constexpr std::string_view kMappingPrefix = "Local\\FGB_NativeHost_";
    const std::string shm = mapping_name_.substr(kMappingPrefix.size());
    std::wstring command = quote(config_.host_exe.wstring()) + L" --dll " +
                           quote(config_.module_dll.wstring()) + L" --module-id " +
                           quote(widen(config_.module_id)) + L" --module-name " +
                           quote(widen(config_.module_name)) + L" --display-name " +
                           quote(widen(config_.display_name)) + L" --shm " +
                           quote(widen(shm)) +
                           L" --tick-hz " + std::to_wstring(std::max(1, config_.tick_hz)) +
                           L" --log " + quote(log_path.wstring());

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (::CreateProcessW(config_.host_exe.wstring().c_str(), command.data(), nullptr, nullptr,
                         FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) == 0) {
        last_error_ = "启动原生模块宿主失败（错误码 " +
                      std::to_string(static_cast<unsigned long>(::GetLastError())) + "）";
        close_handles();
        return false;
    }
    ::CloseHandle(process.hThread);
    process_ = process.hProcess;
    pid_     = process.dwProcessId;
    if (!::AssignProcessToJobObject(static_cast<HANDLE>(job_), static_cast<HANDLE>(process_))) {
        FGB_WARN(kTag, "无法把宿主加入 Job Object（错误码 %lu）",
                 static_cast<unsigned long>(::GetLastError()));
    }
    FGB_INFO(kTag, "%s 启动宿主：pid=%lu", config_.display_name.c_str(),
             static_cast<unsigned long>(pid_));
    return true;
#else
    last_error_ = "当前平台不支持原生模块宿主";
    return false;
#endif
}

bool NativeHostSource::open_block() {
#ifdef _WIN32
    for (int attempt = 0; attempt < 500; ++attempt) {
        if (!process_alive()) {
            mark_process_exit();
            return false;
        }
        const std::wstring name(mapping_name_.begin(), mapping_name_.end());
        mapping_ = ::OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name.c_str());
        if (mapping_ != nullptr) {
            // 写 stop_requested 需要可写视图；读帧仍走 seqlock，不会与宿主写槽冲突。
            view_ = ::MapViewOfFile(static_cast<HANDLE>(mapping_), FILE_MAP_READ | FILE_MAP_WRITE,
                                    0, 0, sizeof(FgbNativeHostBlockV1));
            if (view_ == nullptr) {
                last_error_ = "映射宿主共享内存失败";
                return false;
            }
            auto* block = static_cast<const FgbNativeHostBlockV1*>(view_);
            if (block->magic != FGB_HOST_SHM_MAGIC || block->version != FGB_HOST_SHM_VERSION ||
                block->struct_size != sizeof(FgbNativeHostBlockV1)) {
                last_error_ = "宿主共享内存 magic/version 不匹配";
                return false;
            }
            return true;
        }
        ::Sleep(10);
    }
    last_error_ = "等待宿主创建共享内存超时";
    return false;
#else
    return false;
#endif
}

bool NativeHostSource::wait_ready() {
#ifdef _WIN32
    for (int attempt = 0; attempt < 500; ++attempt) {
        if (view_ == nullptr) {
            return false;
        }
        auto* block = static_cast<FgbNativeHostBlockV1*>(view_);
        const auto state =
            std::atomic_ref<std::uint32_t>(block->state).load(std::memory_order_acquire);
        if (state == FGB_HOST_STATE_ACTIVE) {
            capabilities_    = block->capabilities;
            last_heartbeat_ns_ =
                std::atomic_ref<std::uint64_t>(block->heartbeat_ns).load(std::memory_order_acquire);
            return true;
        }
        if (state == FGB_HOST_STATE_ERROR) {
            last_error_ = block_error_from(block);
            if (last_error_.empty()) {
                last_error_ = "宿主报告模块初始化失败";
            }
            return false;
        }
        if (!process_alive()) {
            mark_process_exit();
            return false;
        }
        ::Sleep(10);
    }
    last_error_ = "等待宿主就绪超时";
    return false;
#else
    return false;
#endif
}

void NativeHostSource::close_handles() noexcept {
#ifdef _WIN32
    if (view_ != nullptr) {
        ::UnmapViewOfFile(view_);
        view_ = nullptr;
    }
    if (mapping_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(mapping_));
        mapping_ = nullptr;
    }
    if (process_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(process_));
        process_ = nullptr;
    }
    if (job_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(job_));
        job_ = nullptr;
    }
#else
    view_    = nullptr;
    mapping_ = nullptr;
    process_ = nullptr;
    job_     = nullptr;
#endif
    pid_ = 0;
}

bool NativeHostSource::process_alive() const noexcept {
#ifdef _WIN32
    if (process_ == nullptr) {
        return false;
    }
    return ::WaitForSingleObject(static_cast<HANDLE>(process_), 0) == WAIT_TIMEOUT;
#else
    return false;
#endif
}

void NativeHostSource::mark_process_exit() {
    if (exited_) {
        return;
    }
    exited_ = true;
    unsigned long code = 0;
#ifdef _WIN32
    if (process_ != nullptr) {
        ::GetExitCodeProcess(static_cast<HANDLE>(process_), &code);
    }
#endif
    const std::string detail = view_ != nullptr ? block_error_from(
                                                    static_cast<const FgbNativeHostBlockV1*>(view_))
                                                : std::string{};
    last_error_ = "原生模块宿主已退出（代码 " + std::to_string(code) + "）";
    if (!detail.empty()) {
        last_error_ += "：" + detail;
    }
    FGB_WARN(kTag, "%s：%s", config_.display_name.c_str(), last_error_.c_str());
}

std::string NativeHostSource::block_error() const {
#ifdef _WIN32
    return view_ != nullptr ? block_error_from(static_cast<const FgbNativeHostBlockV1*>(view_))
                            : std::string{};
#else
    return {};
#endif
}

bool NativeHostSource::read_frame(FaceFrame& out) {
#ifdef _WIN32
    if (view_ == nullptr) {
        return false;
    }
    auto* block = static_cast<FgbNativeHostBlockV1*>(view_);
    std::atomic_ref<std::uint64_t> sequence(block->sequence);
    std::atomic_ref<std::uint32_t> active(block->active_slot);
    for (int attempt = 0; attempt < 4; ++attempt) {
        const std::uint64_t seq1 = sequence.load(std::memory_order_acquire);
        if ((seq1 & 1u) != 0) {
            continue;
        }
        const std::uint32_t slot = active.load(std::memory_order_acquire);
        if (slot >= FGB_HOST_SHM_SLOTS) {
            continue;
        }
        FgbNativeHostFrameV1 copy = block->slots[slot];
        const std::uint64_t seq2  = sequence.load(std::memory_order_acquire);
        if (seq1 != seq2 || (seq2 & 1u) != 0) {
            continue;
        }
        if (copy.sequence == 0 || copy.sequence == last_sequence_) {
            return false;
        }
        last_sequence_ = copy.sequence;
        out            = to_face(copy);
        frames_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
#else
    (void)out;
    return false;
#endif
}

bool NativeHostSource::poll(FaceFrame& out) {
    if (restarting_.load(std::memory_order_acquire)) {
        return false;
    }
    if (restart_thread_.joinable()) {
        restart_thread_.join();
    }
    if (!started_ || view_ == nullptr) {
        return false;
    }
    if (exited_) {
        schedule_restart();
        return false;
    }
    if (!process_alive()) {
        mark_process_exit();
        schedule_restart();
        return false;
    }
    auto* block = static_cast<FgbNativeHostBlockV1*>(view_);
    const std::uint64_t now = clock::now_ns();
    const std::uint64_t heartbeat =
        std::atomic_ref<std::uint64_t>(block->heartbeat_ns).load(std::memory_order_acquire);
    if (heartbeat != 0 && now > heartbeat &&
        now - heartbeat > kHostHeartbeatTimeoutNs) {
#ifdef _WIN32
        ::TerminateProcess(static_cast<HANDLE>(process_), 1);
        ::WaitForSingleObject(static_cast<HANDLE>(process_), 1000);
#endif
        mark_process_exit();
        last_error_ = "原生模块宿主心跳超时（模块可能卡死），已终止";
        schedule_restart();
        return false;
    }
    if (read_frame(out)) {
        drain_logs();
        return true;
    }
    drain_logs();
    return false;
}

bool NativeHostSource::restart() {
    stopping_.store(true, std::memory_order_release);
    if (restart_thread_.joinable()) {
        restart_thread_.join();
    }
    stop_host();
    stopping_.store(false, std::memory_order_release);
    started_ = false;
    exited_  = false;
    restarting_.store(false, std::memory_order_release);
    restarts_.store(0, std::memory_order_release);
    last_error_.clear();
    last_sequence_     = 0;
    last_log_sequence_ = 0;
    return start();
}

void NativeHostSource::schedule_restart() {
    if (!config_.auto_restart || stopping_.load(std::memory_order_acquire) ||
        restarting_.load(std::memory_order_acquire)) {
        return;
    }
    if (restarts_.load(std::memory_order_acquire) >= config_.max_restarts) {
        return;
    }
    if (restart_thread_.joinable()) {
        restart_thread_.join();
    }
    restarting_.store(true, std::memory_order_release);
    restart_thread_ = std::thread([this] { restart_worker(); });
}

void NativeHostSource::restart_worker() {
    const int max_restarts = std::max(0, config_.max_restarts);
    while (!stopping_.load(std::memory_order_acquire) &&
           restarts_.load(std::memory_order_acquire) < max_restarts) {
        const int attempt = restarts_.load(std::memory_order_acquire) + 1;
        const std::uint64_t delay =
            config_.restart_delay_ns * static_cast<std::uint64_t>(std::min(attempt, 3));
        const std::uint64_t deadline = clock::now_ns() + delay;
        while (!stopping_.load(std::memory_order_acquire) && clock::now_ns() < deadline) {
            clock::sleep_for_ns(50'000'000);
        }
        if (stopping_.load(std::memory_order_acquire)) {
            break;
        }

        stop_host();
        exited_    = false;
        last_error_.clear();
        last_sequence_     = 0;
        last_log_sequence_ = 0;
        if (spawn() && open_block() && wait_ready()) {
            started_ = true;
            restarts_.fetch_add(1, std::memory_order_release);
            FGB_INFO(kTag, "%s 已自动重启（第 %d 次）", config_.display_name.c_str(), attempt);
            restarting_.store(false, std::memory_order_release);
            return;
        }
        close_handles();
        restarts_.fetch_add(1, std::memory_order_release);
        last_error_ = "自动重启失败：" + last_error_;
        FGB_WARN(kTag, "%s", last_error_.c_str());
    }
    started_ = false;
    restarting_.store(false, std::memory_order_release);
}

void NativeHostSource::drain_logs() {
#ifdef _WIN32
    if (view_ == nullptr) {
        return;
    }
    auto* block = static_cast<FgbNativeHostBlockV1*>(view_);
    const std::uint32_t head =
        std::atomic_ref<std::uint32_t>(block->log_head).load(std::memory_order_acquire);
    if (head == 0 || head <= last_log_sequence_) {
        return;
    }
    const std::uint32_t first =
        std::max(last_log_sequence_ + 1, head > FGB_HOST_SHM_LOG_ENTRIES
                                             ? head - FGB_HOST_SHM_LOG_ENTRIES + 1
                                             : 1u);
    for (std::uint32_t seq = first; seq <= head; ++seq) {
        const FgbNativeHostLogV1& entry =
            block->logs[(seq - 1) % FGB_HOST_SHM_LOG_ENTRIES];
        if (entry.sequence != seq) {
            continue;
        }
        const char* end = std::find(entry.text, entry.text + FGB_HOST_SHM_LOG_TEXT, '\0');
        const std::string message =
            std::string{entry.text, end} + " [" + config_.display_name + "]";
        switch (entry.level) {
        case 0:
            FGB_TRACE(kTag, "%s", message.c_str());
            break;
        case 1:
            FGB_DEBUG(kTag, "%s", message.c_str());
            break;
        case 3:
            FGB_WARN(kTag, "%s", message.c_str());
            break;
        case 4:
            FGB_ERROR(kTag, "%s", message.c_str());
            break;
        default:
            FGB_INFO(kTag, "%s", message.c_str());
            break;
        }
    }
    last_log_sequence_ = head;
#endif
}

SourceStats NativeHostSource::stats() const noexcept {
    SourceStats stats;
    stats.frames = frames_.load(std::memory_order_relaxed);
    return stats;
}

std::optional<ModuleRuntimeStatus> NativeHostSource::module_status() const {
    ModuleRuntimeStatus status;
    const bool restarting = restarting_.load(std::memory_order_acquire);
    status.alive        = !restarting && started_ && !exited_ && process_alive();
    status.frames       = frames_.load(std::memory_order_relaxed);
    status.restarts     = restarts_.load(std::memory_order_acquire);
    status.capabilities = capabilities_;
    if (restarting) {
        status.error = "宿主正在重启";
    } else if (view_ != nullptr) {
        const std::string detail = block_error();
        status.error = !last_error_.empty() ? last_error_ : detail;
    } else {
        status.error = last_error_;
    }
    return status;
}

void NativeHostSource::stop_host() noexcept {
#ifdef _WIN32
    if (view_ != nullptr) {
        auto* block = static_cast<FgbNativeHostBlockV1*>(view_);
        std::atomic_ref<std::uint32_t>(block->stop_requested)
            .store(1, std::memory_order_release);
    }
    if (process_ != nullptr) {
        const DWORD result = ::WaitForSingleObject(static_cast<HANDLE>(process_), 2000);
        if (result == WAIT_TIMEOUT) {
            ::TerminateProcess(static_cast<HANDLE>(process_), 1);
            ::WaitForSingleObject(static_cast<HANDLE>(process_), 1000);
        }
    }
#endif
    close_handles();
    started_ = false;
}

void NativeHostSource::stop() noexcept {
    stopping_.store(true, std::memory_order_release);
    if (restart_thread_.joinable() &&
        restart_thread_.get_id() != std::this_thread::get_id()) {
        restart_thread_.join();
    }
    stop_host();
    restarting_.store(false, std::memory_order_release);
    started_ = false;
}

} // namespace fgb