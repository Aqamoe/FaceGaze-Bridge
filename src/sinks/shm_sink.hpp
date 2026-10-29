// 共享内存输出：把同一帧写进命名映射，供 Unity/UE/自研工具零拷贝读取。
//
// 双缓冲 + seqlock：写者只写非活动槽，写完再翻转 active_slot；读者校验
// sequence 没变即可确认没有读到半帧。格式见 sdk/fgb_shared_frame.h。
#pragma once

#include "sinks/sink.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace fgb {

struct ShmSinkConfig {
    std::string name; // 映射名：Local\FGB_Shm_<name>
    std::uint64_t min_interval_ns = 0; // 0 = 每个引擎 tick 都写
};

class ShmSink final : public ISink {
public:
    explicit ShmSink(ShmSinkConfig config);
    ~ShmSink() override;

    const char* name() const noexcept override { return "shm-out"; }
    bool start() override;
    void stop() noexcept override;
    void submit(const FaceFrame& frame, const GazeState& gaze, std::uint64_t now_ns) override;
    SinkStats stats() const noexcept override { return stats_; }

    const std::string& mapping_name() const noexcept { return mapping_name_; }
    const ShmSinkConfig& config() const noexcept { return config_; }

private:
    static std::string sanitize(std::string_view name);
    void write_frame(const FaceFrame& frame, const GazeState& gaze, std::uint64_t now_ns);

    ShmSinkConfig config_;
    std::string mapping_name_;
    void* mapping_ = nullptr; // HANDLE
    void* view_    = nullptr; // FgbSharedBlockV1*
    SinkStats stats_{};
    std::uint64_t last_submit_ns_ = 0;
    std::uint64_t write_sequence_ = 0;
};

} // namespace fgb
