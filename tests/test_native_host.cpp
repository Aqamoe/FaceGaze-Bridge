// 跨进程原生模块宿主：真实启动 host 子进程，读共享内存并验证崩溃隔离。
#include "base/clock.hpp"
#include "fgb_module_api.h"
#include "sources/native_host_source.hpp"

#include "test_framework.hpp"

#include <windows.h>

#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using namespace fgb;

namespace {

fs::path exe_dir() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = ::GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return length > 0 ? fs::path{buffer}.parent_path() : fs::current_path();
}

fs::path host_exe() { return exe_dir() / "fgb-host-native.exe"; }
fs::path sine_dll() { return exe_dir() / "fgb-sample-native.dll"; }
fs::path openness_dll() { return exe_dir() / "fgb-sample-openness.dll"; }
fs::path status_cycle_dll() { return exe_dir() / "fgb-test-status-cycle.dll"; }

std::string unique_name(const char* tag) {
    static int counter = 0;
    return std::string{tag} + "-" + std::to_string(::GetTickCount64()) + "-" +
           std::to_string(++counter);
}

NativeHostConfig base_config(const char* tag, const fs::path& dll, const char* id,
                             const char* module_name) {
    NativeHostConfig config;
    config.host_exe    = host_exe();
    config.module_dll  = dll;
    config.log_path    = fs::temp_directory_path() / ("fgb-host-" + unique_name(tag) + ".log");
    config.module_id   = id;
    config.module_name = module_name;
    config.display_name = tag;
    config.shm_name    = unique_name(tag);
    config.tick_hz     = 120;
    return config;
}

} // namespace

FGB_TEST(native_host_process_emits_frames) {
    if (!fs::exists(host_exe()) || !fs::exists(sine_dll())) {
        return;
    }
    NativeHostSource source{base_config("host-sine", sine_dll(),
                                        "com.fgbbridge.sample.sine", "Sine Sample Module")};
    FGB_CHECK(source.start());
    FaceFrame frame;
    bool got = false;
    for (int i = 0; i < 300 && !got; ++i) {
        got = source.poll(frame);
        if (!got) {
            clock::sleep_for_ns(5'000'000);
        }
    }
    FGB_CHECK(got);
    if (got) {
        FGB_CHECK(frame.has_channel(channel::kExpression));
        FGB_CHECK(frame.has_channel(channel::kEyes));
        FGB_CHECK(frame.has_channel(channel::kHead));
        FGB_CHECK(frame.eyes.left.valid);
    }
    const auto status = source.module_status();
    FGB_CHECK(status.has_value());
    if (status.has_value()) {
        FGB_CHECK(status->alive);
        FGB_CHECK(status->frames > 0);
    }
    source.stop();
    FGB_CHECK(!source.module_status()->alive);
}

FGB_TEST(native_host_process_loads_v2_module) {
    if (!fs::exists(host_exe()) || !fs::exists(openness_dll())) {
        return;
    }
    NativeHostSource source{base_config("host-openness", openness_dll(),
                                        "com.fgbbridge.sample.openness",
                                        "Native Openness Sample")};
    FGB_CHECK(source.start());
    FaceFrame frame;
    bool got = false;
    for (int i = 0; i < 300 && !got; ++i) {
        got = source.poll(frame);
        if (!got) {
            clock::sleep_for_ns(5'000'000);
        }
    }
    FGB_CHECK(got);
    if (got) {
        FGB_CHECK(frame.has_channel(channel::kEyes));
        FGB_CHECK(!frame.has_channel(channel::kGaze));
        FGB_CHECK(frame.eyes.any_valid());
        FGB_CHECK(!frame.eyes.left.gazeValid);
    }
    const auto status = source.module_status();
    FGB_CHECK(status.has_value());
    if (status.has_value()) {
        FGB_CHECK((status->capabilities & FGB_CAP_EYES) != 0);
    }
    source.stop();
}

FGB_TEST(native_host_crash_does_not_look_alive) {
    if (!fs::exists(host_exe()) || !fs::exists(sine_dll())) {
        return;
    }
    NativeHostConfig config = base_config("host-crash", sine_dll(),
                                          "com.fgbbridge.sample.sine", "Sine Sample Module");
    config.auto_restart = false;
    NativeHostSource source{config};
    FGB_CHECK(source.start());
    const std::uint32_t pid = source.host_pid();
    FGB_CHECK(pid != 0);
    HANDLE process = ::OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    FGB_CHECK(process != nullptr);
    if (process != nullptr) {
        FGB_CHECK(::TerminateProcess(process, 7) != 0);
        ::CloseHandle(process);
    }
    clock::sleep_for_ns(200'000'000);
    FaceFrame frame;
    FGB_CHECK(!source.poll(frame));
    const auto status = source.module_status();
    FGB_CHECK(status.has_value());
    if (status.has_value()) {
        FGB_CHECK(!status->alive);
        FGB_CHECK(!status->error.empty());
    }
    source.stop();
}

FGB_TEST(native_host_auto_restarts_after_crash) {
    if (!fs::exists(host_exe()) || !fs::exists(sine_dll())) {
        return;
    }
    NativeHostConfig config = base_config("host-restart", sine_dll(),
                                          "com.fgbbridge.sample.sine", "Sine Sample Module");
    config.auto_restart      = true;
    config.max_restarts      = 2;
    config.restart_delay_ns  = 100'000'000ULL;
    NativeHostSource source{config};
    FGB_CHECK(source.start());

    const std::uint32_t pid = source.host_pid();
    HANDLE process = ::OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    FGB_CHECK(process != nullptr);
    if (process != nullptr) {
        ::TerminateProcess(process, 7);
        ::CloseHandle(process);
    }

    bool recovered = false;
    for (int i = 0; i < 300 && !recovered; ++i) {
        FaceFrame frame;
        source.poll(frame);
        const auto status = source.module_status();
        recovered = status.has_value() && status->alive && status->restarts > 0;
        if (!recovered) {
            clock::sleep_for_ns(10'000'000);
        }
    }
    FGB_CHECK(recovered);
    if (recovered) {
        FGB_CHECK(source.host_pid() != pid);
    }
    source.stop();
}

FGB_TEST(native_host_manual_restart_reloads_process) {
    if (!fs::exists(host_exe()) || !fs::exists(sine_dll())) {
        return;
    }
    NativeHostSource source{base_config("host-manual-restart", sine_dll(),
                                        "com.fgbbridge.sample.sine", "Sine Sample Module")};
    FGB_CHECK(source.start());
    const std::uint32_t first_pid = source.host_pid();
    FGB_CHECK(source.restart());
    FGB_CHECK(source.host_pid() != 0);
    FGB_CHECK(source.host_pid() != first_pid);
    // 手动重启也要计数：界面上的「重启 N 次」是用户确认重启生效的唯一反馈
    FGB_CHECK(source.module_status()->restarts == 1);
    FaceFrame frame;
    bool got = false;
    for (int i = 0; i < 200 && !got; ++i) {
        got = source.poll(frame);
        if (!got) {
            clock::sleep_for_ns(5'000'000);
        }
    }
    FGB_CHECK(got);
    source.stop();
}

// 模块报 ERROR 后自愈（再报 ACTIVE）时，宿主必须把共享内存里的旧错误清掉：
// 否则模块明明在正常出数据，模块页却一直显示"故障"，重启实例也像是在走过场。
FGB_TEST(native_host_clears_error_after_module_recovers) {
    if (!fs::exists(host_exe()) || !fs::exists(status_cycle_dll())) {
        return;
    }
    NativeHostSource source{base_config("host-status-cycle", status_cycle_dll(),
                                        "com.fgbbridge.test.statuscycle",
                                        "Status Cycle Test Module")};
    FGB_CHECK(source.start());

    bool saw_error = false;
    bool cleared   = false;
    for (int i = 0; i < 800; ++i) {
        FaceFrame frame;
        source.poll(frame);
        const auto status = source.module_status();
        if (status.has_value()) {
            if (status->error.find("gaze sample stale") != std::string::npos) {
                saw_error = true;
            } else if (saw_error && status->error.empty()) {
                cleared = true;
                break;
            }
        }
        clock::sleep_for_ns(5'000'000);
    }
    FGB_CHECK(saw_error);
    FGB_CHECK(cleared);

    source.stop();
    // 主动停止之后状态是"已停止"，不该继续挂着故障原因
    const auto stopped = source.module_status();
    FGB_CHECK(stopped.has_value());
    if (stopped.has_value()) {
        FGB_CHECK(!stopped->alive);
        FGB_CHECK(stopped->error.empty());
    }
}

FGB_TEST(native_host_can_stop_and_start_for_hot_replace) {
    if (!fs::exists(host_exe()) || !fs::exists(sine_dll())) {
        return;
    }
    NativeHostSource source{base_config("host-stop-start", sine_dll(),
                                        "com.fgbbridge.sample.sine", "Sine Sample Module")};
    FGB_CHECK(source.start());
    source.stop();
    FGB_CHECK(!source.module_status()->alive);
    // 停止后宿主进程退出、DLL 解锁；这里模拟替换完成后的再次启动。
    FGB_CHECK(source.start());
    FGB_CHECK(source.module_status()->alive);
    source.stop();
}

FGB_TEST(native_host_missing_executable_is_reported) {
    NativeHostConfig config;
    config.host_exe    = exe_dir() / "does-not-exist-host.exe";
    config.module_dll  = sine_dll();
    config.module_id   = "x";
    config.module_name = "x";
    config.display_name = "x";
    config.shm_name    = unique_name("missing");
    NativeHostSource source{config};
    FGB_CHECK(!source.start());
    FGB_CHECK(!source.last_error().empty());
}