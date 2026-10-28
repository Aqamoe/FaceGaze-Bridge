#include "params/avatar_config.hpp"
#include "params/catalog.hpp"
#include "params/v1_params.hpp"
#include "params/v2_params.hpp"

#include "test_framework.hpp"

#include <cstring>
#include <string>
#include <vector>

using namespace fgb;
using namespace fgb::params;

namespace {

FaceFrame make_frame() {
    FaceFrame frame;
    frame.eyes.left.openness    = 0.5f;
    frame.eyes.right.openness   = 0.5f;
    frame.eyes.left.valid       = true;
    frame.eyes.right.valid      = true;
    return frame;
}

void set(FaceFrame& f, Shape s, float v) { f.shapes[index_of(s)] = v; }

constexpr std::size_t kV2BaseGroup = 0;
constexpr std::size_t kV2CompositeGroup = 1;
constexpr std::size_t kV1Group = 2;

// 在展开后的扁平目录里找某个逻辑名的下标。
// 必须指定组：v2 的 `JawOpen` 与 v1 的 `JawOpen` 是两条不同记录，
// 靠组区分逻辑名（v2 实际叫 `v2/JawOpen`）。
std::size_t flat_index_of(std::size_t group_index, std::string_view name, ParamKind kind,
                          bool& found) {
    std::size_t slot = 0;
    const auto groups_span = groups();
    for (std::size_t g = 0; g < groups_span.size(); ++g) {
        for (const ParamEntry& e : groups_span[g].entries) {
            if (g == group_index && e.name == name && e.kind == kind) {
                found = true;
                return slot;
            }
            ++slot;
        }
    }
    found = false;
    return 0;
}

float value_of(std::string_view name, const EvalContext& ctx, bool& found) {
    for (const Group& g : groups()) {
        for (const ParamEntry& e : g.entries) {
            if (e.name == name) {
                found = true;
                return e.eval(ctx);
            }
        }
    }
    found = false;
    return 0.0f;
}

} // namespace

// ---------------------------------------------------------------------------
// catalog
// ---------------------------------------------------------------------------

FGB_TEST(catalog_sizes) {
    FGB_CHECK(v2_base().size() == kShapeCount);
    FGB_CHECK(v2_composite().size() == 88); // 8 简化 + 80 合成

    // v1 = 37 SRanipal + 91 合成 + 眼追一族
    FGB_CHECK(v1_all().size() > 150);
    FGB_CHECK(v1_all().size() < 220);
}

FGB_TEST(catalog_fits_sink_budget) {
    std::size_t total = 0;
    for (const Group& g : groups()) {
        total += g.entries.size();
    }
    FGB_CHECK(total <= kMaxParamCount);
    FGB_CHECK(total == v2_base().size() + v2_composite().size() + v1_all().size());
}

FGB_TEST(catalog_logical_names_are_unique) {
    // 重名会让路由把两个参数发到同一个地址，症状是"某个表情乱跳"。
    // 比较的是 name_prefix + name（即头像里看到的逻辑名），v2 的 JawOpen
    // 实际叫 v2/JawOpen，与 v1 的 JawOpen 不冲突。
    std::vector<std::pair<std::string, ParamKind>> seen;
    for (const Group& g : groups()) {
        for (const ParamEntry& e : g.entries) {
            std::string logical{g.name_prefix};
            logical += e.name;
            for (const auto& prev : seen) {
                if (prev.first == logical && prev.second == e.kind) {
                    std::fprintf(stderr, "  重复逻辑名: %s\n", logical.c_str());
                    FGB_CHECK(false);
                }
            }
            seen.emplace_back(std::move(logical), e.kind);
        }
    }
    FGB_CHECK(true);
}

// ---------------------------------------------------------------------------
// v1 数学
// ---------------------------------------------------------------------------

FGB_TEST(v1_jawx_is_difference) {
    FaceFrame frame = make_frame();
    set(frame, Shape::JawRight, 0.7f);
    set(frame, Shape::JawLeft, 0.2f);
    const EvalContext ctx{frame};

    bool found = false;
    const float jawx = value_of("JawX", ctx, found);
    FGB_CHECK(found);
    FGB_CHECK(std::fabs(jawx - 0.5f) < 1e-4f);
}

FGB_TEST(v1_eyelid_expanded_is_weighted) {
    FaceFrame frame = make_frame();
    frame.eyes.left.openness = 1.0f;
    set(frame, Shape::EyeWideLeft, 0.0f);
    const EvalContext ctx{frame};

    bool found = false;
    FGB_CHECK(std::fabs(value_of("LeftEyeLidExpanded", ctx, found) - 0.8f) < 1e-4f);

    // 闭眼时 Squeeze 项才有意义：openness 越低，squeeze 越大
    frame.eyes.left.openness = 0.0f;
    set(frame, Shape::EyeSquintLeft, 1.0f);
    const float squeezed = value_of("LeftEyeLidExpandedSqueeze", ctx, found);
    FGB_CHECK(found);
    FGB_CHECK(squeezed < -0.5f);
}

FGB_TEST(v1_tongue_steps_are_contiguous) {
    FaceFrame frame = make_frame();
    const EvalContext ctx{frame};
    bool found = false;

    // TongueOut = 0.25 → step1 = 0.5, step2 = 0 → TongueSteps = -0.5
    set(frame, Shape::TongueOut, 0.25f);
    FGB_CHECK(std::fabs(value_of("TongueSteps", ctx, found) - (-0.5f)) < 1e-4f);

    // TongueOut = 1.0 → step1 = 1, step2 = 1 → 1 - 1 - 1 = -1
    set(frame, Shape::TongueOut, 1.0f);
    FGB_CHECK(std::fabs(value_of("TongueSteps", ctx, found) - (-1.0f)) < 1e-4f);
}

FGB_TEST(v1_sranipal_table_is_dense) {
    // 37 个名字都必须能查到值，且顺序与枚举一致
    FGB_CHECK(kSRanipalCount == 37);
    FaceFrame frame = make_frame();
    const EvalContext ctx{frame};
    for (std::size_t i = 0; i < kSRanipalCount; ++i) {
        const auto s = static_cast<SRanipalShape>(i);
        FGB_CHECK(!sranipal_name(s).empty());
        const float v = sranipal_value(s, ctx);
        FGB_CHECK(v >= -2.0f && v <= 2.0f);
    }
    FGB_CHECK(sranipal_name(SRanipalShape::JawRight) == "JawRight");
    FGB_CHECK(sranipal_name(SRanipalShape::TongueDownLeftMorph) == "TongueDownLeftMorph");
}

// ---------------------------------------------------------------------------
// 地址匹配
// ---------------------------------------------------------------------------

FGB_TEST(matches_tail_prefers_versioned_segment) {
    // v2 逻辑名匹配 FT/v2/<名>
    FGB_CHECK(ParamRouter::matches_tail("/avatar/parameters/FT/v2/EyeLidLeft", "v2/EyeLidLeft"));
    FGB_CHECK(ParamRouter::matches_tail("/avatar/parameters/v2/EyeLidLeft", "v2/EyeLidLeft"));
    FGB_CHECK(
        ParamRouter::matches_tail("/avatar/parameters/EchoFT/v2/MouthPress", "v2/MouthPress"));

    // legacy 逻辑名绝不能匹配 v2 地址，否则两代会抢同一个地址
    FGB_CHECK(!ParamRouter::matches_tail("/avatar/parameters/FT/v2/EyeLidLeft", "EyeLidLeft"));
    FGB_CHECK(!ParamRouter::matches_tail("/avatar/parameters/v2/EyeLidLeft", "EyeLidLeft"));

    // 裸地址仍是 legacy 的合法目标
    FGB_CHECK(ParamRouter::matches_tail("/avatar/parameters/EyeLidLeft", "EyeLidLeft"));

    // 二进制参数有数字后缀，不该被基名匹配
    FGB_CHECK(!ParamRouter::matches_tail("/avatar/parameters/FT/v2/MouthPress1", "v2/MouthPress"));

    FGB_CHECK(!ParamRouter::matches_tail("", "EyeLidLeft"));
    FGB_CHECK(!ParamRouter::matches_tail("/avatar/parameters/", "EyeLidLeft"));
}

// ---------------------------------------------------------------------------
// 头像配置解析
// ---------------------------------------------------------------------------

FGB_TEST(parse_avatar_json_extracts_input_params) {
    const char* json = R"({
      "id": "avtr_test",
      "name": "测试头像",
      "parameters": [
        { "name": "FT/v2/EyeLidLeft",
          "input": { "address": "/avatar/parameters/FT/v2/EyeLidLeft", "type": "Float" },
          "output": { "address": "/avatar/parameters/FT/v2/EyeLidLeft", "type": "Float" } },
        { "name": "FT/v2/EyeWidenToggle",
          "input": { "address": "/avatar/parameters/FT/v2/EyeWidenToggle", "type": "Bool" },
          "output": { "address": "/avatar/parameters/FT/v2/EyeWidenToggle", "type": "Bool" } },
        { "name": "VRCEmote",
          "input": { "address": "/avatar/parameters/VRCEmote", "type": "Int" } }
      ]
    })";

    AvatarConfig cfg;
    FGB_CHECK(parse_avatar_json(json, cfg));
    FGB_CHECK(cfg.id == "avtr_test");
    FGB_CHECK(cfg.name == "测试头像");

    // Int 不参与：OSC 里我们要么写 float 要么写 bool
    FGB_CHECK(cfg.params.size() == 2);
    FGB_CHECK(cfg.params[0].address == "/avatar/parameters/FT/v2/EyeLidLeft");
    FGB_CHECK(cfg.params[0].kind == ParamKind::Float);
    FGB_CHECK(cfg.params[1].kind == ParamKind::Bool);
}

FGB_TEST(parse_avatar_json_rejects_garbage) {
    AvatarConfig cfg;
    FGB_CHECK(!parse_avatar_json("{}", cfg));
    FGB_CHECK(!parse_avatar_json("not json at all", cfg));
}

FGB_TEST(router_binds_declared_addresses) {
    AvatarConfig cfg;
    cfg.params.push_back({"/avatar/parameters/FT/v2/EyeLidLeft", ParamKind::Float});
    cfg.params.push_back({"/avatar/parameters/JawOpen", ParamKind::Float});

    ParamRouter router;
    router.bind(cfg);
    FGB_CHECK(router.bound_to_avatar());
    FGB_CHECK(router.total() > 0);
    FGB_CHECK(router.matched() == 2);

    bool found = false;
    const std::size_t lid = flat_index_of(kV2CompositeGroup, "EyeLidLeft", ParamKind::Float, found);
    FGB_CHECK(found);
    FGB_CHECK(router.address(lid) == "/avatar/parameters/FT/v2/EyeLidLeft");

    // 头像没声明的参数必须留空，否则会往不存在的地址刷包
    const std::size_t smile =
        flat_index_of(kV2CompositeGroup, "MouthSmileLeft", ParamKind::Float, found);
    FGB_CHECK(found);
    FGB_CHECK(router.address(smile).empty());

    // legacy 的 JawOpen 只有裸地址才该被绑定；v2 的 JawOpen 对应 FT/v2/JawOpen
    const std::size_t legacy_jaw = flat_index_of(kV1Group, "JawOpen", ParamKind::Float, found);
    FGB_CHECK(found);
    FGB_CHECK(router.address(legacy_jaw) == "/avatar/parameters/JawOpen");

    const std::size_t v2_jaw = flat_index_of(kV2BaseGroup, "JawOpen", ParamKind::Float, found);
    FGB_CHECK(found);
    FGB_CHECK(router.address(v2_jaw).empty());
}

FGB_TEST(router_fallback_prefix) {
    ParamRouter router;
    router.use_fallback("FT/v2/");
    FGB_CHECK(!router.bound_to_avatar());

    bool found = false;
    const std::size_t lid = flat_index_of(kV2CompositeGroup, "EyeLidLeft", ParamKind::Float, found);
    FGB_CHECK(found);
    FGB_CHECK(router.address(lid) == "/avatar/parameters/FT/v2/EyeLidLeft");

    // v1 组回退时不带前缀
    const std::size_t jaw = flat_index_of(kV1Group, "JawOpen", ParamKind::Float, found);
    FGB_CHECK(found);
    FGB_CHECK(router.address(jaw) == "/avatar/parameters/JawOpen");
}

// ---------------------------------------------------------------------------
// binary 参数
// ---------------------------------------------------------------------------

FGB_TEST(match_binary_tail_extracts_index) {
    int index = 0;
    FGB_CHECK(ParamRouter::match_binary_tail("/avatar/parameters/FT/v2/JawOpen4", "v2/JawOpen",
                                            index));
    FGB_CHECK(index == 4);

    FGB_CHECK(
        ParamRouter::match_binary_tail("/avatar/parameters/v2/MouthPress16", "v2/MouthPress", index));
    FGB_CHECK(index == 16);

    // 没有数字后缀、或者基名对不上，都不算
    FGB_CHECK(!ParamRouter::match_binary_tail("/avatar/parameters/FT/v2/JawOpen", "v2/JawOpen",
                                              index));
    FGB_CHECK(!ParamRouter::match_binary_tail("/avatar/parameters/FT/v2/JawForward4", "v2/JawOpen",
                                              index));

    // legacy 名不能匹配 v2 的二进制参数（与 matches_tail 同一条规则）
    FGB_CHECK(!ParamRouter::match_binary_tail("/avatar/parameters/FT/v2/JawOpen4", "JawOpen",
                                              index));
}

FGB_TEST(binary_bit_quantizes_by_declared_width) {
    // 3 位 = 8 级。0.5 → 4 → 100b：只有 shift=2 置位
    FGB_CHECK(!binary_bit(0.5f, 0, 3, false));
    FGB_CHECK(!binary_bit(0.5f, 1, 3, false));
    FGB_CHECK(binary_bit(0.5f, 2, 3, false));

    // 0.25 → 2 → 010b
    FGB_CHECK(!binary_bit(0.25f, 0, 3, false));
    FGB_CHECK(binary_bit(0.25f, 1, 3, false));
    FGB_CHECK(!binary_bit(0.25f, 2, 3, false));

    // 0 → 全灭
    for (std::uint8_t s = 0; s < 3; ++s) {
        FGB_CHECK(!binary_bit(0.0f, s, 3, false));
    }
}

FGB_TEST(binary_bit_full_scale_sets_every_bit) {
    // 1.0 不能走量化：8 * 1.0 = 8 会落到第 4 位，而头像只声明了 3 位
    for (std::uint8_t s = 0; s < 3; ++s) {
        FGB_CHECK(binary_bit(1.0f, s, 3, false));
        FGB_CHECK(binary_bit(0.999999f, s, 3, false));
    }
}

FGB_TEST(binary_bit_negative_needs_symbol_param) {
    // 没有 <名>Negative 时负值整位归零，而不是取绝对值
    FGB_CHECK(!binary_bit(-0.5f, 2, 3, false));

    // 有符号位时按绝对值编码，符号单独走 Negative
    FGB_CHECK(binary_bit(-0.5f, 2, 3, true));
}

FGB_TEST(router_builds_binary_targets) {
    AvatarConfig cfg;
    cfg.params.push_back({"/avatar/parameters/FT/v2/JawOpen1", ParamKind::Bool});
    cfg.params.push_back({"/avatar/parameters/FT/v2/JawOpen2", ParamKind::Bool});
    cfg.params.push_back({"/avatar/parameters/FT/v2/JawOpen4", ParamKind::Bool});
    cfg.params.push_back({"/avatar/parameters/FT/v2/JawOpenNegative", ParamKind::Bool});
    // 非 2 的幂，VRCFT 会丢弃
    cfg.params.push_back({"/avatar/parameters/FT/v2/JawOpen3", ParamKind::Bool});

    ParamRouter router;
    router.bind(cfg);
    FGB_CHECK(router.binary_param_count() == 1);

    bool found = false;
    const std::size_t jaw = flat_index_of(kV2BaseGroup, "JawOpen", ParamKind::Float, found);
    FGB_CHECK(found);

    // 该参数应产生 3 个二进制位 + 1 个符号位，没有 float 目标
    std::size_t bits = 0;
    std::size_t neg  = 0;
    std::size_t flat = 0;
    for (const auto& t : router.targets()) {
        if (t.entry == nullptr) {
            ++flat;
            continue;
        }
        if (t.address.find("JawOpen") != std::string::npos) {
            if (t.kind == OutputTarget::Kind::BinaryBit) {
                ++bits;
                FGB_CHECK(t.bit_count == 3);
                FGB_CHECK(t.has_negative);
            } else if (t.kind == OutputTarget::Kind::Negative) {
                ++neg;
            }
        }
    }
    FGB_CHECK(bits == 3);
    FGB_CHECK(neg == 1);

    // 主地址仍为空（头像没声明 float 版本）
    FGB_CHECK(router.address(jaw).empty());
}

// ---------------------------------------------------------------------------
// 当前头像识别
// ---------------------------------------------------------------------------

FGB_TEST(parse_avatar_id_from_log_takes_the_last) {
    const char* log =
        "2026.10.29 01:00:00 Debug - [Behaviour] Loading avatar for someone\r\n"
        "2026.10.29 01:00:05 Debug -  Saving Avatar Data:avtr_11111111-1111-1111-1111-111111111111\r\n"
        "2026.10.29 01:05:00 Debug -  Saving Avatar Data:avtr_6ce1b451-cc77-43db-ac3e-eec2f3258ec6\r\n";

    std::string id;
    FGB_CHECK(parse_avatar_id_from_log(log, id));
    // 头像换过之后，只有最后一条代表当下
    FGB_CHECK(id == "avtr_6ce1b451-cc77-43db-ac3e-eec2f3258ec6");
}

FGB_TEST(parse_avatar_id_from_log_rejects_junk) {
    std::string id;
    FGB_CHECK(!parse_avatar_id_from_log("nothing here", id));
    // 标记后面不是合法 ID（长度不够 / 字符非法）
    FGB_CHECK(!parse_avatar_id_from_log("Saving Avatar Data:avtr_xyz", id));
    FGB_CHECK(!parse_avatar_id_from_log("Saving Avatar Data:local:alice", id));
}
