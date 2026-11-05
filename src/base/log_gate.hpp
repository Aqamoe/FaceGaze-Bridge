// 日志去重闸门（ADR-0046）。
//
// 模块可以在 update() 里每帧报同一句话：真机上有个模块按 100 行/秒刷
// "上报了 N 条无法识别的 OSC 地址"，一晚上把 engine.log 写到 71 MB，
// 其中 95% 是两条重复消息——真问题全被埋在噪音里。
//
// 这里把"同一类消息"折叠成首条 + 一条带计数的结算：既保住"确实出过这件事"，
// 又不让日志变成 GB 级。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fgb::log {

// 判定"同一类"的键：把数字run 抹成 #，其余逐字相同。
// 模块日志里"累计 6101160 条""已 18446744073 秒"这些数字每次都变，
// 不抹平就永远认不出是同一句话。
std::string repetition_key(std::string_view text);

class RepetitionGate {
public:
    // 同一类消息的结算周期：窗口内只落第一条，到期补一条"又折叠了 N 条"
    static constexpr std::uint64_t kDefaultWindowNs = 10'000'000'000ULL;

    struct Emission {
        std::string folded; // 非空：先补一条折叠小结
        std::string line;   // 非空：再落这条原始消息
        bool none() const noexcept { return folded.empty() && line.empty(); }
    };

    explicit RepetitionGate(std::uint64_t window_ns = kDefaultWindowNs) : window_ns_(window_ns) {}

    // 放行第一条；同类消息在窗口内只累加计数。窗口到期时返回"折叠小结 + 当前这条"。
    Emission offer(std::string_view text, std::uint64_t now_ns);

    // 结算已经过窗口的欠账：模块不再说话时，折叠条数也要有个下落（每个 tick 调）。
    std::vector<std::string> drain_expired(std::uint64_t now_ns);

    // 无条件结算（状态恢复、换源）：不计窗口。
    std::vector<std::string> flush();

    void reset();

private:
    // 几个桶：模块交替报两三句话时也能各自折叠，不至于互相顶掉窗口
    static constexpr std::size_t kBucketCount = 8;

    struct Bucket {
        std::string key;
        std::string last;          // 最后一条被折叠的原文，结算时带给用户
        std::uint64_t start_ns = 0; // 本窗口起点
        std::uint32_t folded   = 0; // 本窗口内折叠掉的条数
        bool used              = false;
    };

    Bucket* find(std::string_view key) noexcept;
    // 拿一个空桶；没有空桶就顶掉窗口起点最早的那个（返回它欠下的折叠小结）
    Bucket* slot_for(std::uint64_t now_ns, std::string& evicted_folded);
    static std::string folded_text(const Bucket& bucket);
    bool window_expired(const Bucket& bucket, std::uint64_t now_ns) const noexcept;

    std::uint64_t window_ns_;
    std::array<Bucket, kBucketCount> buckets_{};
};

} // namespace fgb::log
