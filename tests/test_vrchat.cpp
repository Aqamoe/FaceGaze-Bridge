#include "sinks/vrchat_sink.hpp"

#include "test_framework.hpp"

using namespace fgb;

FGB_TEST(eye_mode_parsing) {
    EyeOutputMode mode = EyeOutputMode::both;
    FGB_CHECK(parse_eye_output_mode("parameters", mode));
    FGB_CHECK(mode == EyeOutputMode::parameters);
    FGB_CHECK(parse_eye_output_mode("native", mode));
    FGB_CHECK(mode == EyeOutputMode::native);
    FGB_CHECK(parse_eye_output_mode("both", mode));
    FGB_CHECK(mode == EyeOutputMode::both);
    FGB_CHECK(!parse_eye_output_mode("PARAMETERS", mode));
    FGB_CHECK(!parse_eye_output_mode("", mode));

    FGB_CHECK(std::string_view{to_string(EyeOutputMode::parameters)} == "parameters");
}
