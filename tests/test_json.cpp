#include "base/json.hpp"

#include "test_framework.hpp"

#include <string>
#include <vector>

using namespace fgb::json;

FGB_TEST(json_escapes_special_characters) {
    std::string out;
    append_escaped(out, "a\"b\\c\nd");
    FGB_CHECK(out == "\"a\\\"b\\\\c\\nd\"");
}

FGB_TEST(json_writer_builds_nested_object) {
    std::string out;
    JsonWriter writer{out};
    writer.obj_begin();
    writer.key("ok").boolean(true);
    writer.key("count").integer(3);
    writer.key("ratio").num(0.5, 2);
    writer.key("items").arr_begin().num(1.0, 1).num(2.0, 1).arr_end();
    writer.obj_end();

    const std::string expected = R"({"ok":true,"count":3,"ratio":0.50,"items":[1.0,2.0]})";
    FGB_CHECK(out == expected);
}

FGB_TEST(json_writer_escapes_keys_and_values) {
    std::string out;
    JsonWriter writer{out};
    writer.obj_begin();
    writer.key("a\"b").str("x\\y");
    writer.obj_end();
    // 注意这里必须用普通字面量手工转义：内容里同时有引号和反斜杠，
    // 裸字符串 R"(...)" 会在 ")" 处提前结束
    const std::string expected = "{\"a\\\"b\":\"x\\\\y\"}";
    FGB_CHECK(out == expected);
}

FGB_TEST(json_field_string_array_reads_capabilities) {
    const std::string text = R"({
  "Capabilities": ["expression", "gaze"],
  "RoleHint": "gaze"
})";
    std::vector<std::string> values;
    FGB_CHECK(field_string_array(text, "Capabilities", values));
    FGB_CHECK(values.size() == 2);
    if (values.size() == 2) {
        FGB_CHECK(values[0] == "expression");
        FGB_CHECK(values[1] == "gaze");
    }
    FGB_CHECK(!field_string_array(text, "Missing", values));
}

FGB_TEST(json_field_readers) {
    // 故意带上缩进、换行、以及 null：VRCFT 的 module.json 就长这样
    const std::string text = R"({
  "ModuleName": "VRCFTPicoModule",
  "Version": "0.1.11",
  "FileHash": null,
  "Downloads": 0,
  "IsLocal": true
})";

    std::string name;
    FGB_CHECK(field_string(text, "ModuleName", name));
    FGB_CHECK(name == "VRCFTPicoModule");

    double downloads = 42.0;
    FGB_CHECK(field_number(text, "Downloads", downloads));
    FGB_CHECK(downloads == 0.0);

    bool local = false;
    FGB_CHECK(field_bool(text, "IsLocal", local));
    FGB_CHECK(local);

    // null 不算字符串值；找不到的键也不能猜
    FGB_CHECK(!field_string(text, "FileHash", name));
    FGB_CHECK(!field_string(text, "Nope", name));
    FGB_CHECK(!field_number(text, "ModuleName", downloads));
}
