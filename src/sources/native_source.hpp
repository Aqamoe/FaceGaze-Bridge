// 原生 C ABI 模块（进程内加载）。
//
// 唯一 ABI：v2 属性帧（见 sdk/fgb_module_api.h 与 docs/adr/0034）。线程模型：
// 宿主在引擎线程按 tick 调用 update()，模块在 update/initialize 里同步提交帧；
// 不做跨线程保护。
#pragma once

#include "base/log_gate.hpp"
#include "fgb_module_api.h"
#include "sources/source.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace fgb {

struct NativeSourceConfig {
    std::filesystem::path dll;
    std::string module_id;
    std::string module_name;
    std::string display_name = "native";
    int tick_hz              = 100;
    // 跨进程宿主用它把模块日志转发到共享内存；进程内模式留空。
    std::function<void(int, const char*)> log_sink;
};

class NativeModuleSource final : public ISource {
public:
    explicit NativeModuleSource(NativeSourceConfig config);
    ~NativeModuleSource() override;

    const char* name() const noexcept override { return config_.display_name.c_str(); }
    bool start() override;
    void stop() noexcept override;
    // 进程内重载：卸载 DLL 再重新加载（模块页「重启实例」），不重启引擎。
    bool restart() override;
    bool poll(FaceFrame& out) override;
    SourceStats stats() const noexcept override;
    std::optional<ModuleRuntimeStatus> module_status() const override;

    const std::string& last_error() const noexcept { return last_error_; }

private:
    static void host_log(void* ctx, int level, const char* message);
    static void host_report(void* ctx, int state, const char* detail);
    static FgbFrameSlot* host_acquire(void* ctx);
    static void host_commit(void* ctx, FgbFrameSlot* slot, uint64_t capture_ts_ns);
    static std::int32_t host_shape_index(void* ctx, const char* name);

    void apply_slot(const FgbFrameSlot& slot, std::uint64_t capture_ts_ns);
    // 落一行模块日志（原始文本，不加前缀）
    void write_line(int level, const std::string& line);
    // 落一条模块日志（补 "模块名: " 前缀，跨进程模式下同时写 log_sink）
    void emit_log(int level, const std::string& message);
    // 落一条状态错误（"模块名 报告错误：正文"，沿用既有格式）
    void emit_report(const std::string& text);
    // 把闸门里过期的折叠小结补出来（每 tick 一次，见 ADR-0046）
    void drain_folded_logs(std::uint64_t now_ns);
    // 模块写了自己没声明的能力位时告警一次（不改数据）。
    void check_capabilities(std::uint64_t attributes);
    // 注视数据的两个"看着不像真的"指纹，各告警一次（不改数据）。
    void check_gaze_diagnostics(const FaceFrame& frame);
    void warn_once(bool suspicious, int& frames, bool& warned, const std::string& text);

    NativeSourceConfig config_;
    std::string module_dir_utf8_;
    std::string host_version_utf8_;

    void* library_        = nullptr; // HMODULE
    const FgbModule* api_ = nullptr;
    void* self_           = nullptr;
    FgbHostServices services_{};
    FgbFrameSlot slot_{};
    FaceFrame pending_{};
    bool has_frame_   = false;
    bool initialized_ = false;
    std::string last_error_;
    // 逐帧诊断的连续帧计数与一次性告警（见 native_gaze_parse.hpp）
    int placeholder_frames_   = 0; // 伪造 -Z 方向的指纹
    bool placeholder_warned_  = false;
    int range_frames_         = 0; // 2D 点持续越界（ADR-0033）
    bool range_warned_        = false;
    bool capability_warned_   = false; // 属性与能力位不一致
    // 模块日志与状态错误的去重闸门：上游模块每帧报同一句话时把日志折叠成
    // 首条 + 一条计数，否则 engine.log 能涨到 GB 级（ADR-0046）
    log::RepetitionGate log_gate_;
    log::RepetitionGate report_gate_;

    std::atomic<std::uint64_t> frames_{0};
    std::atomic<bool> alive_{false};
    std::atomic<int> restarts_{0}; // 界面的「重启 N 次」；进程内只有手动重启会加
};

} // namespace fgb
