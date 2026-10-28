// OSC 解码器。只读视图 + 按需取值，不解包到堆上。
#pragma once

#include "osc/message.hpp"

#include <cstddef>
#include <cstring>
#include <span>
#include <string_view>

namespace fgb::osc {

// 一条消息的只读视图。构造时做结构校验，无效则 valid() 为 false。
class MessageView {
public:
    MessageView() noexcept = default;
    explicit MessageView(std::span<const std::byte> packet) noexcept;

    bool valid() const noexcept { return valid_; }
    std::string_view address() const noexcept;
    std::string_view type_tags() const noexcept;
    std::size_t arg_count() const noexcept;

    // 这条消息实际占用多少字节（含尾部对齐）。用于在裸包里连续解析多条消息。
    std::size_t byte_size() const noexcept;

    // 按索引取参数，类型不符返回 false
    bool get_float(std::size_t index, float& out) const noexcept;
    bool get_int(std::size_t index, std::int32_t& out) const noexcept;
    bool get_string(std::size_t index, std::string_view& out) const noexcept;

private:
    // 返回第 index 个参数的字节偏移，失败返回 kInvalid
    std::size_t arg_offset(std::size_t index, char& tag) const noexcept;

    static constexpr std::size_t kInvalid = static_cast<std::size_t>(-1);

    std::span<const std::byte> data_{};
    std::size_t addr_len_ = 0;
    std::size_t tags_off_ = 0;
    std::size_t tags_len_ = 0;
    std::size_t args_off_ = 0;
    bool valid_           = false;
};

// 拆包：单个消息或 bundle，逐条回调。
//
// 裸包内连续多条消息也一并支持——这不符合 OSC 规范，但现实中确实有面捕软件
// 这么发（把几条消息直接拼接后 sendto）。只解析第一条会让 Apply 之类的结束
// 标记永久丢失，故障现场极难定位，所以这里选择容错。
//
// 返回成功解析的消息条数。用模板而非 std::function，避免热路径分配。
template <typename Fn>
std::size_t for_each_message(std::span<const std::byte> packet, Fn&& fn) noexcept {
    if (packet.size() >= kBundleHeaderSize &&
        std::memcmp(packet.data(), "#bundle\0", kBundleMagicLen) == 0) {
        std::size_t pos   = kBundleHeaderSize;
        std::size_t count = 0;
        while (pos + 4 <= packet.size()) {
            const std::uint32_t len = read_be32(packet.data() + pos);
            pos += 4;
            if (len == 0 || pos + len > packet.size()) {
                break;
            }
            MessageView mv{packet.subspan(pos, len)};
            if (mv.valid()) {
                fn(mv);
                ++count;
            }
            pos += pad4(len);
        }
        return count;
    }

    std::size_t pos   = 0;
    std::size_t count = 0;
    while (pos + 8 <= packet.size()) {
        MessageView mv{packet.subspan(pos)};
        if (!mv.valid()) {
            break;
        }
        const std::size_t used = mv.byte_size();
        if (used == 0) {
            break;
        }
        fn(mv);
        ++count;
        pos += used;
    }
    return count;
}

} // namespace fgb::osc
