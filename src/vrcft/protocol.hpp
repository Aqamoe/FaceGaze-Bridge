// VRCFaceTracking 模块 IPC 协议。
//
// 规格来源：docs/adr/0003-vrcft-ipc-protocol.md（已在真实进程上验证）
//
// 传输：UDP 回环。FGB 作为 server 绑定端口，ModuleProcess.exe 作为 client
// 主动连上来。所有整数均为小端。
//
// 分工提醒：
//   本文件只管"字节 <-> 结构"，不做任何业务判断，也不碰 socket。
//   这样协议层可以脱离网络单测，出问题时能立刻二分出是"解析错"还是"时序错"。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace fgb::vrcft {

inline constexpr std::array<std::byte, 4> kMagic = {
    std::byte{0xAF}, std::byte{0xEC}, std::byte{0x00}, std::byte{0x8D}};

// 握手挑战字。Server 收到后必须校验，并把原包回发作为 ACK。
inline constexpr std::array<std::byte, 5> kChallenge = {
    std::byte{0xDE}, std::byte{0xC1}, std::byte{0xCA}, std::byte{0x12}, std::byte{0xD0}};

enum class PacketType : std::uint32_t {
    Handshake          = 1,
    SplitPacketChunk   = 2,

    MetadataUpdate     = 100,
    LipUpdate          = 101,
    EyeUpdate          = 102,

    EventGetSupported  = 200,
    EventInit          = 201,
    EventTeardown      = 202,
    EventUpdate        = 203,
    EventUpdateStatus  = 204,
    EventLog           = 205,

    ReplyGetSupported  = 300,
    ReplyInit          = 301,
    ReplyUpdate        = 302,
    ReplyTeardown      = 303,

    DebugStreamFrame   = 1000,
};

enum class ModuleState : std::int32_t {
    Uninitialized = -1,
    Idle          = 0,
    Active        = 1,
};

inline constexpr std::size_t kHeaderSize = 8; // magic(4) + type(4)

// 接收缓冲必须容得下最大的包。实测 ReplyInit 带内嵌图标时可达 8457 字节，
// 若缓冲不足，Windows 的 recvfrom 会返回 WSAEMSGSIZE 并丢弃整包——
// 表现为"握手成功但初始化回复永远等不到"，极难定位。
inline constexpr std::size_t kMaxPacketSize = 65536;

const char* to_string(PacketType type) noexcept;

struct PacketHeader {
    PacketType type{};
    std::span<const std::byte> body; // 指向包头之后的载荷
};

// 校验 magic 并切出 type / body。
bool parse_header(std::span<const std::byte> packet, PacketHeader& out) noexcept;

// ---- 编码（写入调用方提供的缓冲，返回实际字节数，0 表示失败）----

// 无载荷包：EventGetSupported / EventTeardown / EventUpdate
std::size_t write_simple(PacketType type, std::span<std::byte> buffer) noexcept;

// bit0 = a, bit1 = b。用于 EventInit / ReplyGetSupported 的载荷。
std::size_t write_flags(PacketType type, bool a, bool b, std::span<std::byte> buffer) noexcept;

// 模块状态同步。**Init 成功后必须发一次 Active，否则模块 Update() 空转。**
std::size_t write_status(ModuleState state, bool using_eye, bool using_expression,
                         std::span<std::byte> buffer) noexcept;

// ---- 解码 ----

struct HandshakeInfo {
    bool challenge_ok = false;
    std::int32_t pid  = 0;
    std::string_view module_path;
};

bool parse_handshake(std::span<const std::byte> packet, HandshakeInfo& out) noexcept;

struct ReplySupported {
    bool eye        = false;
    bool expression = false;
};

bool parse_reply_supported(std::span<const std::byte> packet, ReplySupported& out) noexcept;

struct ReplyInitInfo {
    bool eye_success        = false;
    bool expression_success = false;
    std::string_view module_name;
    std::int32_t icon_count = 0;
};

bool parse_reply_init(std::span<const std::byte> packet, ReplyInitInfo& out) noexcept;

struct EventLogEntry {
    std::int32_t level = 2; // 2=Info 3=Warn 4=Error
    std::string_view text;
};

bool parse_event_log(std::span<const std::byte> packet, EventLogEntry& out) noexcept;

} // namespace fgb::vrcft
