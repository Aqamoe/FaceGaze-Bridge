// 头像参数发现与地址解析。
//
// 【为什么需要这一层】
// VRCFT 文档里的参数名是 `v2/EyeX`，但真实头像里的参数名是 `FT/v2/EyeX`。
// 两者不是笔误，而是两回事：
//
//   catalog 里的名字  →  逻辑参数名（v2/*, v1 裸名）
//   avatar 里声明的名 →  实际 OSC 地址（FT/v2/*、v2/*、EchoFT/v2/* …）
//
// 本机 223 个头像配置的实测分布：FT/v2/ 6534 个参数、v2/ 1315 个、EchoFT/v2/ 384 个。
// **前缀是头像作者定的，没有统一约定**，写死任何一个都只能覆盖一部分头像。
//
// VRChat 把每个加载过的头像的参数表写在：
//   %LocalLow%\VRChat\VRChat\OSC\<用户ID>\Avatars\<头像ID>.json
// 结构为 { id, name, hash, parameters: [{ name, input:{address,type}, output:{...} }] }。
// VRCFT 就是读这些文件（或走 OSCQuery）来把逻辑名映射到真实地址的。
//
// 匹配规则照抄 VRCFT 的 BaseParam 正则：
//   (?<!(v\d+))(/<name>)$  或  ^<name>$
// 取反的那一半很关键——它让 legacy 的 `EyeLidLeft` 不会误匹配到 `FT/v2/EyeLidLeft`，
// 否则 v1 和 v2 会互相抢占同一个地址。
#pragma once

#include "params/catalog.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace fgb::params {

struct AvatarParam {
    std::string address; // 完整 OSC 地址，如 /avatar/parameters/FT/v2/EyeLidLeft
    ParamKind kind;
};

struct AvatarConfig {
    std::string id;
    std::string name;
    std::string path;
    std::vector<AvatarParam> params;
};

// %USERPROFILE%\AppData\LocalLow\VRChat\VRChat\OSC
std::string default_osc_root();

// 解析单个头像配置 JSON。容忍未知字段与多余空白。
bool parse_avatar_json(std::string_view text, AvatarConfig& out);

// 读文件并解析
bool load_avatar_file(const std::string& path, AvatarConfig& out);

// 列出所有 <osc_root>\<usr>\Avatars\*.json，按修改时间从新到旧。
// limit 限制的是"成功解析出的配置数量"，不是读取的文件数。
std::vector<AvatarConfig> scan_avatars(const std::string& osc_root, std::size_t limit = 0);

// 在最近修改的若干配置里挑一个最像"面捕头像"的。
//
// 为什么不能简单取最新：VRChat 会在你每次换头像时写一份配置，最新那份很可能是个
// 根本没有面捕参数的普通模型。判据用"声明了多少个 v2/ 参数"——面捕头像动辄几百个，
// 普通头像通常是 0。
struct AvatarPick {
    AvatarConfig config;
    std::size_t v2_param_count = 0; // 0 表示这份配置不带面捕
    bool valid                 = false;
};

AvatarPick pick_avatar(const std::string& osc_root, std::size_t candidates = 8);

// 一个参数最终要写成的 OSC 目标。
//
// 同一个逻辑参数在不同头像上有三种落地方式，互斥但可以并存：
//   Float      头像声明了 `<名>` 为 Float            → 一条 float
//   Bool       头像声明了 `<名>` 为 Bool             → 一条 T/F
//   BinaryBit  头像声明了 `<名>1`/`<名>2`/`<名>4`…  → 每声明一位一条 T/F
//   Negative   `<名>Negative`，binary 的伴生符号位，取值 = (值 < 0)
struct OutputTarget {
    std::string address;
    enum class Kind : std::uint8_t { Float, Bool, BinaryBit, Negative } kind = Kind::Float;
    const ParamEntry* entry = nullptr; // 取值来源
    std::uint8_t shift      = 0;       // BinaryBit：这一位对应第几次移位
    std::uint8_t bit_count  = 0;       // BinaryBit：头像一共声明了几位（量化级数 = 2^bit_count）
    bool has_negative       = false;   // 该参数是否声明了 <名>Negative
};

// 二进制参数的一位怎么取值。
//
// 规则照抄 VRCFT 的 BinaryBaseParameter.ProcessBinary：
//   1. 负值只有在头像声明了 <名>Negative 时才有意义，否则整位归零（不是取绝对值）
//   2. 级数 = 2^bit_count，量化 = (int)(|v| * 级数)
//   3. |v| > 0.99999 直接全置位——否则 (int)(1.0 * 级数) 会溢出到不存在的位
bool binary_bit(float value, std::uint8_t shift, std::uint8_t bit_count,
                bool has_negative) noexcept;

// 逻辑名 → 真实地址。
class ParamRouter {
public:
    // 没有任何发现信息时的兜底：v2 参数用 prefix，v1 参数用 /avatar/parameters/
    void use_fallback(std::string_view v2_prefix);

    // 用头像声明的参数表绑定真实地址；未声明的参数地址留空（= 不发送）
    void bind(const AvatarConfig& config);

    void clear();

    // 与 catalog::groups() 展开后的扁平索引一一对应
    const std::string& address(std::size_t flat_index) const noexcept;

    std::size_t total() const noexcept { return addresses_.size(); }
    std::size_t matched() const noexcept { return matched_; }
    bool bound_to_avatar() const noexcept { return bound_; }

    // 真正要发出去的 OSC 目标列表（按参数索引升序，同参数的多个目标相邻）
    const std::vector<OutputTarget>& targets() const noexcept { return targets_; }
    std::size_t binary_param_count() const noexcept { return binary_params_; }

    // 供测试：判断某个声明地址是否匹配某个逻辑名
    static bool matches_tail(std::string_view address, std::string_view name) noexcept;

    // 供测试：把 `<名>4` 这样的地址拆成 (基名匹配, 索引)。index = 0 表示没匹配上。
    static bool match_binary_tail(std::string_view address, std::string_view name,
                                  int& index) noexcept;

private:
    void rebuild(std::string_view v2_prefix, const AvatarConfig* config);

    std::vector<std::string> addresses_;
    std::vector<OutputTarget> targets_;
    std::size_t matched_       = 0;
    std::size_t binary_params_ = 0;
    bool bound_                = false;
};

} // namespace fgb::params
