// OSC 编码器。单遍写入，零分配，溢出即失败（不做静默截断）。
#pragma once

#include "osc/message.hpp"

#include <cstddef>
#include <span>
#include <string_view>

namespace fgb::osc {

class BundleWriter {
public:
    explicit BundleWriter(std::span<std::byte> buffer) noexcept;

    // 开始一个 bundle。timetag 为 0 表示"立即"。
    // 不开 bundle 也可以直接写单条消息。
    bool begin_bundle(std::uint64_t timetag = 0) noexcept;

    bool begin_message(std::string_view address, std::string_view type_tags) noexcept;
    bool add_float(float value) noexcept;
    bool add_int(std::int32_t value) noexcept;
    bool add_string(std::string_view value) noexcept;
    bool end_message() noexcept;

    // 便捷：一条消息携带 N 个 float（N <= 63）
    bool write_floats(std::string_view address, std::span<const float> values) noexcept;

    std::span<const std::byte> data() const noexcept { return buffer_.first(size_); }
    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }
    bool overflowed() const noexcept { return overflow_; }

    void reset() noexcept;

private:
    bool put(const void* src, std::size_t n) noexcept;
    bool put_string_raw(std::string_view s) noexcept;

    static constexpr std::size_t kNoOpenMessage = static_cast<std::size_t>(-1);

    std::span<std::byte> buffer_;
    std::size_t size_        = 0;
    std::size_t msg_len_pos_ = kNoOpenMessage;
    std::size_t msg_start_   = 0;
    bool in_bundle_          = false;
    bool overflow_           = false;
};

} // namespace fgb::osc
