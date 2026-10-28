#include "osc/decoder.hpp"
#include "osc/encoder.hpp"

#include "test_framework.hpp"

#include <array>
#include <cstring>

using namespace fgb;

namespace {

std::array<std::byte, 2048> g_buffer{};

} // namespace

FGB_TEST(osc_single_float_roundtrip) {
    osc::BundleWriter writer{g_buffer};
    const float values[3] = {0.25f, -1.5f, 123.456f};
    FGB_CHECK(writer.write_floats("/avatar/parameters/FT/v2/JawOpen", values));
    FGB_CHECK(!writer.empty());

    std::size_t count = 0;
    osc::for_each_message(writer.data(), [&](const osc::MessageView& msg) {
        ++count;
        FGB_CHECK(msg.address() == "/avatar/parameters/FT/v2/JawOpen");
        FGB_CHECK(msg.arg_count() == 3);

        float got = 0.0f;
        FGB_CHECK(msg.get_float(0, got));
        FGB_CHECK_NEAR(got, 0.25, 1e-6);
        FGB_CHECK(msg.get_float(1, got));
        FGB_CHECK_NEAR(got, -1.5, 1e-6);
        FGB_CHECK(msg.get_float(2, got));
        FGB_CHECK_NEAR(got, 123.456, 1e-4);
    });
    FGB_CHECK(count == 1);
}

FGB_TEST(osc_bundle_contains_all_messages) {
    osc::BundleWriter writer{g_buffer};
    FGB_CHECK(writer.begin_bundle());

    for (int i = 0; i < 5; ++i) {
        const float v[1] = {static_cast<float>(i) * 0.2f};
        FGB_CHECK(writer.write_floats("/a/b", v));
    }
    FGB_CHECK(!writer.empty());

    int seen = 0;
    float sum  = 0.0f;
    osc::for_each_message(writer.data(), [&](const osc::MessageView& msg) {
        ++seen;
        float v = 0.0f;
        if (msg.get_float(0, v)) {
            sum += v;
        }
    });
    FGB_CHECK(seen == 5);
    FGB_CHECK_NEAR(sum, 0.0f + 0.2f + 0.4f + 0.6f + 0.8f, 1e-5);
}

FGB_TEST(osc_string_argument) {
    osc::BundleWriter writer{g_buffer};
    FGB_CHECK(writer.begin_message("/VMC/Ext/Blend/Val", ",sf"));
    FGB_CHECK(writer.add_string("jawOpen"));
    FGB_CHECK(writer.add_float(0.75f));
    FGB_CHECK(writer.end_message());

    bool matched = false;
    osc::for_each_message(writer.data(), [&](const osc::MessageView& msg) {
        std::string_view name;
        float value = 0.0f;
        if (msg.get_string(0, name) && msg.get_float(1, value)) {
            matched = (name == "jawOpen") && std::fabs(value - 0.75f) < 1e-6f;
        }
    });
    FGB_CHECK(matched);
}

FGB_TEST(osc_overflow_is_reported) {
    std::array<std::byte, 32> tiny{};
    osc::BundleWriter writer{tiny};
    const float v[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    FGB_CHECK(!writer.write_floats("/overflow/test/address", v));
    FGB_CHECK(writer.overflowed());
}

FGB_TEST(osc_rejects_malformed_packets) {
    osc::MessageView empty{};
    FGB_CHECK(!empty.valid());

    const std::array<std::byte, 4> garbage = {std::byte{'x'}, std::byte{'y'}, std::byte{0}, std::byte{0}};
    osc::MessageView bad{garbage};
    FGB_CHECK(!bad.valid());

    // 有地址但类型标签缺失
    const char raw[] = "/abc\0\0\0\0";
    osc::MessageView no_tags{std::span<const std::byte>{reinterpret_cast<const std::byte*>(raw), sizeof(raw) - 1}};
    FGB_CHECK(!no_tags.valid());
}

FGB_TEST(osc_tolerates_concatenated_messages) {
    // 不符合规范但现实中确实存在：多条消息裸拼后一次 sendto。
    // 只解第一条会让 Apply 之类的结束标记永久丢失，所以必须能连续解析。
    std::array<std::byte, 256> first{};
    std::array<std::byte, 256> second{};

    osc::BundleWriter w1{first};
    const float v1[1] = {1.5f};
    FGB_CHECK(w1.write_floats("/one", v1));

    osc::BundleWriter w2{second};
    const float v2[1] = {2.5f};
    FGB_CHECK(w2.write_floats("/two", v2));

    std::array<std::byte, 512> joined{};
    std::memcpy(joined.data(), w1.data().data(), w1.size());
    std::memcpy(joined.data() + w1.size(), w2.data().data(), w2.size());

    int count = 0;
    float sum = 0.0f;
    const std::span<const std::byte> view{joined.data(), w1.size() + w2.size()};
    osc::for_each_message(view, [&](const osc::MessageView& msg) {
        ++count;
        float v = 0.0f;
        if (msg.get_float(0, v)) {
            sum += v;
        }
    });
    FGB_CHECK(count == 2);
    FGB_CHECK_NEAR(sum, 4.0f, 1e-5);
}

FGB_TEST(osc_type_mismatch_returns_false) {
    osc::BundleWriter writer{g_buffer};
    FGB_CHECK(writer.begin_message("/test", ",i"));
    FGB_CHECK(writer.add_int(42));
    FGB_CHECK(writer.end_message());

    osc::for_each_message(writer.data(), [&](const osc::MessageView& msg) {
        float as_float = 0.0f;
        FGB_CHECK(!msg.get_float(0, as_float));

        std::int32_t as_int = 0;
        FGB_CHECK(msg.get_int(0, as_int));
        FGB_CHECK(as_int == 42);
    });
}
