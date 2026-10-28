// 回放源：把录制的 ReplyUpdate 原样喂回解码路径。
//
// 设计上的两点坚持：
//   1. 走**同一个** decode_update + apply_to_frame，不写第二条解析路径，
//      否则测的就不是真实链路了
//   2. 按录制时的时间戳节流，能看出真实节奏下的表现（抖动、丢帧、延迟）；
//      speed > 1 可加速跑完，speed = 0 表示"一 tick 尽量多喂"（批量导出用）
#pragma once

#include "capture/capture_file.hpp"
#include "core/cont/seqlock.hpp"
#include "sources/source.hpp"
#include "vrcft/update_data.hpp"

#include <cstdint>
#include <string>

namespace fgb {

struct ReplayConfig {
    std::string path;
    float speed              = 1.0f; // 1.0 = 实时；0 表示不限速
    bool loop                = false;
    float eye_full_scale_deg = 30.0f;
};

class ReplaySource final : public ISource {
public:
    explicit ReplaySource(ReplayConfig config);

    const char* name() const noexcept override { return "replay"; }
    bool start() override;
    void stop() noexcept override;
    bool poll(FaceFrame& out) override;
    SourceStats stats() const noexcept override;

    bool finished() const noexcept { return finished_; }
    std::size_t total_frames() const noexcept { return total_; }

private:
    bool load_next() noexcept;
    std::uint64_t due_ns() const noexcept;

    ReplayConfig config_;
    capture::Reader reader_;
    SourceStats stats_{};

    FaceFrame pending_{};
    SeqLock<FaceFrame> snapshot_{};

    std::vector<std::byte> payload_;
    std::uint64_t next_ts_ = 0;
    bool have_next_        = false;
    bool finished_         = false;
    bool started_          = false;
    std::size_t total_     = 0;
    std::uint64_t t0_      = 0;
};

} // namespace fgb
