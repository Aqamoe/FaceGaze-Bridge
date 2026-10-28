#include "params/avatar_config.hpp"

#include "base/log.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace fgb::params {
namespace {

constexpr char kTag[]     = "avatar";
constexpr char kRoot[]    = "/avatar/parameters/";
constexpr char kOscSub[]  = "AppData/LocalLow/VRChat/VRChat/OSC";

namespace fs = std::filesystem;

// ---- 极简 JSON 扫描 ----
//
// 不引入 JSON 库：这里只关心 parameters[].input.{address,type}，
// 用"定位关键字 + 读取紧随其后的字符串"就够了，而且不会因为未知字段而失败。

std::size_t find_key(std::string_view text, std::string_view key, std::size_t from) noexcept {
    const std::size_t pos = text.find(key, from);
    return pos == std::string_view::npos ? text.size() : pos;
}

bool read_string(std::string_view text, std::size_t& pos, std::string& out) noexcept;

// 找到 key 之后、冒号之后的值字符串。返回是否读到。
bool read_value_after_key(std::string_view text, std::string_view key, std::size_t from,
                          std::string& out, std::size_t& end_pos) noexcept {
    const std::size_t key_pos = text.find(key, from);
    if (key_pos == std::string_view::npos) {
        return false;
    }
    const std::size_t colon = text.find(':', key_pos + key.size());
    if (colon == std::string_view::npos) {
        return false;
    }
    std::size_t cursor = colon + 1;
    return read_string(text, cursor, out) && ((end_pos = cursor), true);
}

// 从 quote 位置开始读一个 JSON 字符串（含转义处理），返回内容并推进 pos
bool read_string(std::string_view text, std::size_t& pos, std::string& out) noexcept {
    const std::size_t begin = text.find('"', pos);
    if (begin == std::string_view::npos) {
        return false;
    }
    out.clear();
    for (std::size_t i = begin + 1; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\\' && i + 1 < text.size()) {
            out.push_back(text[++i]);
            continue;
        }
        if (c == '"') {
            pos = i + 1;
            return true;
        }
        out.push_back(c);
    }
    return false;
}

ParamKind kind_from_type(std::string_view type) noexcept {
    if (type == "Float") {
        return ParamKind::Float;
    }
    if (type == "Bool") {
        return ParamKind::Bool;
    }
    return ParamKind::Float; // Int / String 不参与，由调用方按 kind 过滤
}

bool is_supported_type(std::string_view type) noexcept {
    return type == "Float" || type == "Bool";
}

std::string read_text_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::string data;
    in.seekg(0, std::ios::end);
    const auto size = in.tellg();
    if (size > 0) {
        data.resize(static_cast<std::size_t>(size));
        in.seekg(0, std::ios::beg);
        in.read(data.data(), size);
    }
    // 去掉可能的 UTF-8 BOM
    if (data.size() >= 3 && static_cast<unsigned char>(data[0]) == 0xEF &&
        static_cast<unsigned char>(data[1]) == 0xBB && static_cast<unsigned char>(data[2]) == 0xBF) {
        data.erase(0, 3);
    }
    return data;
}

} // namespace

std::string default_osc_root() {
    const char* profile = std::getenv("USERPROFILE");
    if (profile == nullptr || *profile == '\0') {
        return {};
    }
    std::string root{profile};
    root += '/';
    root += kOscSub;
    return root;
}

bool parse_avatar_json(std::string_view text, AvatarConfig& out) {
    out.params.clear();

    std::size_t pos = 0;
    read_value_after_key(text, "\"id\"", 0, out.id, pos);
    read_value_after_key(text, "\"name\"", 0, out.name, pos);

    pos = find_key(text, "\"parameters\"", 0);
    if (pos >= text.size()) {
        return false;
    }

    // 只取 input 块：output 是同样的地址，重复收集没有意义
    while ((pos = find_key(text, "\"input\"", pos)) < text.size()) {
        const std::size_t next_input = find_key(text, "\"input\"", pos + 1);

        std::string address;
        std::size_t after_address = 0;
        if (!read_value_after_key(text, "\"address\"", pos, address, after_address) ||
            after_address > next_input) {
            break;
        }

        std::string type;
        std::size_t after_type = 0;
        if (read_value_after_key(text, "\"type\"", after_address, type, after_type) &&
            after_type <= next_input && is_supported_type(type) && !address.empty()) {
            out.params.push_back({std::move(address), kind_from_type(type)});
        }
        pos = after_address;
    }
    return !out.params.empty();
}

bool load_avatar_file(const std::string& path, AvatarConfig& out) {
    out = AvatarConfig{};
    out.path = path;
    return parse_avatar_json(read_text_file(fs::path{path}), out);
}

std::vector<AvatarConfig> scan_avatars(const std::string& osc_root, std::size_t limit) {
    std::vector<AvatarConfig> result;
    if (osc_root.empty()) {
        return result;
    }

    std::error_code ec;
    const fs::path root{osc_root};
    if (!fs::is_directory(root, ec)) {
        return result;
    }

    struct Entry {
        fs::path path;
        fs::file_time_type mtime;
    };
    std::vector<Entry> files;

    for (const auto& user_dir : fs::directory_iterator(root, ec)) {
        if (!user_dir.is_directory(ec)) {
            continue;
        }
        const fs::path avatars = user_dir.path() / "Avatars";
        if (!fs::is_directory(avatars, ec)) {
            continue;
        }
        for (const auto& file : fs::directory_iterator(avatars, ec)) {
            if (!file.is_regular_file(ec) || file.path().extension() != ".json") {
                continue;
            }
            files.push_back({file.path(), fs::last_write_time(file.path(), ec)});
        }
    }

    std::sort(files.begin(), files.end(),
              [](const Entry& a, const Entry& b) { return a.mtime > b.mtime; });

    for (const Entry& entry : files) {
        if (limit != 0 && result.size() >= limit) {
            break;
        }
        AvatarConfig cfg;
        cfg.path = entry.path.string();
        if (parse_avatar_json(read_text_file(entry.path), cfg)) {
            result.push_back(std::move(cfg));
        }
    }
    return result;
}

bool parse_avatar_id_from_log(std::string_view text, std::string& id) {
    constexpr std::string_view kMarker = "Saving Avatar Data:";
    constexpr std::size_t kIdLen       = 41; // avtr_ + 36 位 GUID
    std::size_t best = std::string_view::npos;
    for (std::size_t pos = text.find(kMarker); pos != std::string_view::npos;
         pos = text.find(kMarker, pos + 1)) {
        best = pos;
    }
    if (best == std::string_view::npos) {
        return false;
    }
    const std::size_t begin = best + kMarker.size();
    if (begin + kIdLen > text.size()) {
        return false;
    }
    const std::string_view candidate = text.substr(begin, kIdLen);
    if (candidate.substr(0, 5) != "avtr_") {
        return false;
    }
    for (char c : candidate.substr(5)) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || c == '-';
        if (!ok) {
            return false;
        }
    }
    id.assign(candidate);
    return true;
}

std::string current_avatar_id() {
    std::error_code ec;
    const fs::path log_dir = fs::path{default_osc_root()}.parent_path();
    if (!fs::is_directory(log_dir, ec)) {
        return {};
    }

    fs::path newest;
    fs::file_time_type newest_time{};
    for (const auto& entry : fs::directory_iterator(log_dir, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        if (name.rfind("output_log_", 0) != 0 || entry.path().extension() != ".txt") {
            continue;
        }
        const auto mtime = fs::last_write_time(entry.path(), ec);
        if (newest.empty() || mtime > newest_time) {
            newest      = entry.path();
            newest_time = mtime;
        }
    }
    if (newest.empty()) {
        return {};
    }

    // 只读尾部：日志动辄几 MB，而信号永远在最后
    std::ifstream in(newest, std::ios::binary);
    if (!in) {
        return {};
    }
    in.seekg(0, std::ios::end);
    const auto size = static_cast<std::size_t>(in.tellg());
    constexpr std::size_t kTail = 256 * 1024;
    const std::size_t begin     = size > kTail ? size - kTail : 0;
    std::string tail(size - begin, '\0');
    in.seekg(static_cast<std::streamoff>(begin), std::ios::beg);
    in.read(tail.data(), static_cast<std::streamsize>(tail.size()));

    std::string id;
    return parse_avatar_id_from_log(tail, id) ? id : std::string{};
}

bool load_avatar_by_id(const std::string& osc_root, const std::string& id, AvatarConfig& out) {
    if (osc_root.empty() || id.empty()) {
        return false;
    }
    std::error_code ec;
    for (const auto& user_dir : fs::directory_iterator(fs::path{osc_root}, ec)) {
        if (!user_dir.is_directory(ec)) {
            continue;
        }
        const fs::path candidate = user_dir.path() / "Avatars" / (id + ".json");
        if (fs::is_regular_file(candidate, ec)) {
            return load_avatar_file(candidate.string(), out);
        }
    }
    return false;
}

AvatarPick pick_avatar(const std::string& osc_root, std::size_t candidates) {
    AvatarPick best;
    for (AvatarConfig& cfg : scan_avatars(osc_root, candidates)) {
        std::size_t score = 0;
        for (const AvatarParam& p : cfg.params) {
            if (p.address.find("/v2/") != std::string::npos) {
                ++score;
            }
        }
        if (!best.valid || score > best.v2_param_count) {
            best.v2_param_count = score;
            best.config         = std::move(cfg);
            best.valid          = true;
        }
        if (score > 200) {
            break; // 已经明显是面捕头像，不用再看
        }
    }
    return best;
}

bool binary_bit(float value, std::uint8_t shift, std::uint8_t bit_count,
                bool has_negative) noexcept {
    if (value < 0.0f) {
        if (!has_negative) {
            return false;
        }
        value = -value;
    }
    if (value > 0.99999f) {
        return true;
    }
    if (bit_count == 0 || bit_count > 15) {
        return false;
    }
    const int levels    = 1 << bit_count;
    const int quantized = static_cast<int>(value * static_cast<float>(levels));
    return ((quantized >> shift) & 1) == 1;
}

bool ParamRouter::matches_tail(std::string_view address, std::string_view name) noexcept {
    if (name.empty() || address.size() <= name.size()) {
        return false;
    }
    if (address.compare(address.size() - name.size(), name.size(), name) != 0) {
        return false;
    }
    const std::size_t slash = address.size() - name.size() - 1;
    if (address[slash] != '/') {
        return false;
    }
    // 前一段如果是 v<数字>，说明这是另一种版本的同名参数，不能匹配。
    // 例：legacy 的 EyeLidLeft 不得匹配 FT/v2/EyeLidLeft。
    const std::size_t prev_end   = slash;
    const std::size_t prev_slash = slash == 0 ? std::string_view::npos : address.rfind('/', slash - 1);
    const std::size_t prev_begin = prev_slash == std::string_view::npos ? 0 : prev_slash + 1;
    const std::string_view prev  = address.substr(prev_begin, prev_end - prev_begin);
    if (prev.size() >= 2 && prev[0] == 'v') {
        bool digits = true;
        for (std::size_t i = 1; i < prev.size(); ++i) {
            if (prev[i] < '0' || prev[i] > '9') {
                digits = false;
                break;
            }
        }
        if (digits) {
            return false;
        }
    }
    return true;
}

bool ParamRouter::match_binary_tail(std::string_view address, std::string_view name,
                                   int& index) noexcept {
    index = 0;
    if (address.empty() || name.empty()) {
        return false;
    }
    // 从尾部剥掉数字
    std::size_t digits = 0;
    while (digits < address.size()) {
        const char c = address[address.size() - 1 - digits];
        if (c < '0' || c > '9') {
            break;
        }
        ++digits;
    }
    if (digits == 0 || digits == address.size()) {
        return false;
    }
    const std::string_view base = address.substr(0, address.size() - digits);
    if (!matches_tail(base, name)) {
        return false;
    }
    int value = 0;
    for (std::size_t i = address.size() - digits; i < address.size(); ++i) {
        value = value * 10 + (address[i] - '0');
        if (value > 65535) {
            return false; // 明显不是位序号，防止溢出
        }
    }
    index = value;
    return true;
}

void ParamRouter::clear() {
    addresses_.clear();
    targets_.clear();
    matched_       = 0;
    binary_params_ = 0;
    bound_         = false;
}

void ParamRouter::use_fallback(std::string_view v2_prefix) { rebuild(v2_prefix, nullptr); }

void ParamRouter::bind(const AvatarConfig& config) { rebuild("FT/v2/", &config); }

namespace {

// VRCFT 的 GetBinarySteps：只有 2 的幂才是合法位序号，返回值是移位量。
bool binary_shift_of(int index, std::uint8_t& shift) noexcept {
    if (index <= 0) {
        return false;
    }
    int value = 1;
    for (std::uint8_t i = 0; i < 16; ++i) {
        if (value == index) {
            shift = i;
            return true;
        }
        value *= 2;
    }
    return false;
}

} // namespace

void ParamRouter::rebuild(std::string_view v2_prefix, const AvatarConfig* config) {
    addresses_.clear();
    targets_.clear();
    matched_       = 0;
    param_count_   = 0;
    binary_params_ = 0;
    bound_         = config != nullptr;

    for (const Group& group : groups()) {
        for (const ParamEntry& entry : group.entries) {
            std::string primary;
            std::string negative;
            std::vector<OutputTarget> bits;

            if (config == nullptr) {
                primary = kRoot;
                primary += group.fallback_prefix;
                primary += entry.name;
            } else {
                std::string logical;
                logical.reserve(group.name_prefix.size() + entry.name.size());
                logical.append(group.name_prefix);
                logical.append(entry.name);

                // 主参数：同名的 Float / Bool
                for (const AvatarParam& declared : config->params) {
                    if (declared.kind == entry.kind && matches_tail(declared.address, logical)) {
                        primary = declared.address;
                        break;
                    }
                }

                // 符号位：<名>Negative，只可能是 Bool
                std::string negative_name = logical + "Negative";
                for (const AvatarParam& declared : config->params) {
                    if (declared.kind == ParamKind::Bool &&
                        matches_tail(declared.address, negative_name)) {
                        negative = declared.address;
                        break;
                    }
                }

                // 二进制位：<名>1 / <名>2 / <名>4 …，只认 2 的幂
                struct Bit {
                    std::uint8_t shift;
                    std::string address;
                };
                std::vector<Bit> found;
                for (const AvatarParam& declared : config->params) {
                    if (declared.kind != ParamKind::Bool) {
                        continue;
                    }
                    int index = 0;
                    std::uint8_t shift = 0;
                    if (!match_binary_tail(declared.address, logical, index) ||
                        !binary_shift_of(index, shift)) {
                        continue;
                    }
                    bool duplicate = false;
                    for (const Bit& b : found) {
                        if (b.shift == shift) {
                            duplicate = true;
                            break;
                        }
                    }
                    if (!duplicate) {
                        found.push_back({shift, declared.address});
                    }
                }
                std::sort(found.begin(), found.end(),
                          [](const Bit& a, const Bit& b) { return a.shift < b.shift; });

                if (!found.empty()) {
                    ++binary_params_;
                    for (const Bit& b : found) {
                        OutputTarget t;
                        t.address   = b.address;
                        t.kind      = OutputTarget::Kind::BinaryBit;
                        t.entry     = &entry;
                        t.shift     = b.shift;
                        t.bit_count = static_cast<std::uint8_t>(found.size());
                        bits.push_back(std::move(t));
                    }
                }
            }

            addresses_.push_back(primary);
            const bool has_negative = !negative.empty();

            if (!primary.empty()) {
                OutputTarget t;
                t.address = primary;
                t.kind    = entry.kind == ParamKind::Bool ? OutputTarget::Kind::Bool
                                                          : OutputTarget::Kind::Float;
                t.entry        = &entry;
                t.has_negative = has_negative;
                targets_.push_back(std::move(t));
                ++matched_;
            }
            if (!negative.empty()) {
                OutputTarget t;
                t.address = negative;
                t.kind    = OutputTarget::Kind::Negative;
                t.entry   = &entry;
                targets_.push_back(std::move(t));
                ++matched_;
            }
            for (OutputTarget& t : bits) {
                t.has_negative = has_negative;
                targets_.push_back(std::move(t));
            }
        }
    }

    if (config != nullptr) {
        // 统计"不同的参数个数"，别用 matched_（那是"绑定条数"，符号位会重复计入）
        const ParamEntry* last = nullptr;
        for (const OutputTarget& t : targets_) {
            if (t.entry != nullptr && t.entry != last) {
                ++param_count_;
                last = t.entry;
            }
        }
        FGB_INFO(kTag,
                 "已绑定头像「%s」：%zu 个参数（其中 %zu 个用二进制编码），展开成 %zu 条 OSC 目标（%s）",
                 config->name.c_str(), param_count_, binary_params_, targets_.size(),
                 config->path.c_str());
    } else {
        FGB_INFO(kTag, "未发现头像配置，回退到固定前缀 %s%zu 个参数", std::string(v2_prefix).c_str(),
                 addresses_.size());
    }
}

const std::string& ParamRouter::address(std::size_t flat_index) const noexcept {
    static const std::string kEmpty;
    return flat_index < addresses_.size() ? addresses_[flat_index] : kEmpty;
}

} // namespace fgb::params
