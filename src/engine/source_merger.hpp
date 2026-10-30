// 多输入源合并。
//
// 为什么需要它：M1 的合并策略是"谁最后 poll 到就用谁"，对单源没问题，但两个源同时喂
// （硬件眼追模块 + 摄像头表情、或者模块 + VMC）时，后 poll 的那个会把前一个整帧盖掉——
// 表现为"表情正常但眼睛不动"，或者反过来，取决于注册顺序。
//
// 这里把一帧拆成四个通道分别选提供者：
//
//   表情 Expression  88 个形状（含 shapeValid 位）
//   眼追 Eyes        openness / pupil / vergence（整眼）
//   头姿 Head        hasHeadPose + 旋转 + 位置
//   注视 Gaze        独立注视方向 / 逐眼 / 可选 2D 点（不碰 openness）
//
// 选择规则（每一路独立）：
//   1. 显式指定（--eye-source / --gaze-source 之类）且该源新鲜且确实提供该通道 → 用它
//   2. 否则按注册顺序取第一个"新鲜且提供该通道"的源
//   3. 都没有 → 该通道清零（invalid / hasHeadPose=false），让下游看到"没数据"
//      而不是继续用已经冻住的值
//
// "新鲜"阈值逐通道不同：面捕 500 ms、眼睛 250 ms、注视 100 ms、头姿 500 ms。
// 注视点用于 foveation 时 500 ms 已经完全不可用，不能和 30 Hz 面捕共用一条线。
#pragma once

#include "sources/source.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace fgb {

struct MergePolicy {
    // 空串 = 按注册顺序自动选
    std::string expression_source;
    std::string eye_source;
    std::string gaze_source;
    std::string head_source;

    std::uint64_t expression_stale_ns = 500'000'000ULL;
    std::uint64_t eye_stale_ns        = 250'000'000ULL;
    std::uint64_t gaze_stale_ns       = 100'000'000ULL;
    std::uint64_t head_stale_ns       = 500'000'000ULL;
};

// 给界面/日志看的单源状态
struct SourceReport {
    std::string name;
    std::uint64_t frames      = 0;
    std::uint64_t age_ns      = 0;
    bool fresh                = false;
    bool provides_expression  = false;
    bool provides_eyes        = false;
    bool provides_gaze        = false;
    bool provides_head        = false;
    bool used_expression      = false;
    bool used_eyes            = false;
    bool used_gaze            = false;
    bool used_head            = false;
};

class SourceMerger {
public:
    void set_policy(const MergePolicy& policy) { policy_ = policy; }
    const MergePolicy& policy() const noexcept { return policy_; }

    // 注册顺序即优先级
    void add(ISource* source);
    std::size_t source_count() const noexcept { return slots_.size(); }

    // 拉一遍所有源并合并。**本 tick 有任何新帧**才返回 true（合并结果写入 out）：
    // 引擎据此决定是否发布新快照，避免同一个值被反复 publish。
    bool poll(std::uint64_t now_ns, FaceFrame& out);

    std::uint64_t frames_in() const noexcept { return frames_in_; }
    std::vector<SourceReport> report(std::uint64_t now_ns) const;

private:
    struct Slot {
        ISource* source          = nullptr;
        FaceFrame last{};
        std::uint64_t updated_ns = 0;
        std::uint64_t frames     = 0;
        bool has_frame           = false;
    };

    bool fresh(const Slot& slot, std::uint64_t now_ns, std::uint64_t stale_ns) const noexcept;
    bool provides_expression(const Slot& slot) const noexcept;
    bool provides_eyes(const Slot& slot) const noexcept;
    bool provides_gaze(const Slot& slot) const noexcept;
    bool provides_head(const Slot& slot) const noexcept;
    std::uint64_t stale_for(int channel) const noexcept;
    // 返回槽位下标，-1 表示没有可用来源
    int pick(int channel, std::uint64_t now_ns) const noexcept;

    MergePolicy policy_;
    std::vector<Slot> slots_;
    std::uint64_t frames_in_ = 0;
};

} // namespace fgb
