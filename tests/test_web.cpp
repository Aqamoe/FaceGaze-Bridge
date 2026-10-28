#include "web/json.hpp"

#include "test_framework.hpp"

#include <cstring>
#include <string>

using namespace fgb::web;

FGB_TEST(json_escapes_special_characters) {
    std::string out;
    json_append_escaped(out, "a\"b\\c\nd");
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

FGB_TEST(http_parse_get_with_query) {
    const char* raw = "GET /api/status?verbose=1&x=2 HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    HttpRequest request;
    FGB_CHECK(parse_request(raw, std::strlen(raw), request));
    FGB_CHECK(request.is_get());
    FGB_CHECK(request.path == "/api/status");
    FGB_CHECK(request.query == "verbose=1&x=2");
    FGB_CHECK(request.body.empty());
}

FGB_TEST(http_parse_post_with_body) {
    const char* raw =
        "POST /api/filter HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 12\r\n\r\n{\"enabled\":1}";
    HttpRequest request;
    FGB_CHECK(parse_request(raw, std::strlen(raw), request));
    FGB_CHECK(request.is_post());
    FGB_CHECK(request.path == "/api/filter");
    FGB_CHECK(request.body == "{\"enabled\":1}");
}

FGB_TEST(http_parse_rejects_garbage) {
    HttpRequest request;
    FGB_CHECK(!parse_request("hello", 5, request));
    FGB_CHECK(!parse_request("", 0, request));
    // 只有方法没有路径
    const char* bad = "GET\r\n\r\n";
    FGB_CHECK(!parse_request(bad, std::strlen(bad), request));
}

FGB_TEST(http_query_and_body_helpers) {
    double number = 0.0;
    bool flag     = false;

    FGB_CHECK(query_number("cutoff=2.5&beta=0.8", "cutoff", number));
    FGB_CHECK(number == 2.5);
    FGB_CHECK(query_bool("enabled=false", "enabled", flag));
    FGB_CHECK(!flag);

    // JSON body 里的值也能读
    const std::string body = R"({"enabled":true,"min_cutoff":1.75})";
    FGB_CHECK(body_bool(body, "enabled", flag));
    FGB_CHECK(flag);
    FGB_CHECK(body_number(body, "min_cutoff", number));
    FGB_CHECK(number == 1.75);

    // 找不到就返回 false，不猜默认值
    FGB_CHECK(!body_number(body, "nope", number));
    FGB_CHECK(!query_number("", "cutoff", number));
}
