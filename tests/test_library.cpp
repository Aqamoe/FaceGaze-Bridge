// 模块库 JSON：registry.vrcft.io 返回的是"数组套一层对象"，描述里可能带括号、
// 引号、Unicode 转义。这个解析器只服务这一种形状，但字符串处理必须是对的。
#include "vrcft/module_library.hpp"

#include "test_framework.hpp"

#include <string>
#include <vector>

using namespace fgb::vrcft;

FGB_TEST(library_json_parses_real_shape) {
    const char* text = R"([
      {"AuthorName":"Lorow","DllFileName":"ETVR.dll",
       "DownloadUrl":"https://example.com/ETVR.dll","Downloads":12293,
       "LastUpdated":"2024-02-19T19:48:18Z","ModuleDescription":"Translates {data} for \"eye\" tracking, with a comma, and a brace }",
       "ModuleId":"9058ac35-c6be-4e8b-8e3b-a16b00aa5488","ModuleName":"ETVR Tracking Module",
       "ModulePageUrl":"https://github.com/lorow/ETVRTrackingModule","Rating":4.43,"Ratings":56,
       "UsageInstructions":"See the page","Version":"0.0.5","FileHash":null,
       "Nested":{"a":[1,2,{"b":"}"}]}},
      {"ModuleId":"bad","ModuleName":"No URL"},
      {"ModuleId":"ja","ModuleName":"\u65e5\u672c\u8a9e","DownloadUrl":"http://127.0.0.1:9/x.zip"}
    ])";

    std::vector<LibraryEntry> entries;
    std::string error;
    FGB_CHECK(parse_registry_json(text, entries, error));
    FGB_CHECK(entries.size() == 2);
    if (entries.size() == 2) {
        FGB_CHECK(entries[0].id == "9058ac35-c6be-4e8b-8e3b-a16b00aa5488");
        FGB_CHECK(entries[0].name == "ETVR Tracking Module");
        FGB_CHECK(entries[0].downloads == 12293);
        FGB_CHECK(entries[0].ratings == 56);
        FGB_CHECK(entries[0].rating > 4.42 && entries[0].rating < 4.44);
        FGB_CHECK(entries[0].description.find("\"eye\"") != std::string::npos);
        FGB_CHECK(entries[0].description.find("brace }") != std::string::npos);
        FGB_CHECK(entries[0].usage == "See the page");
        FGB_CHECK(entries[0].page == "https://github.com/lorow/ETVRTrackingModule");
        FGB_CHECK(entries[0].file_hash.empty());
        FGB_CHECK(entries[1].name == "日本語");
    }
}

FGB_TEST(library_json_rejects_bad_input) {
    std::vector<LibraryEntry> entries;
    std::string error;
    FGB_CHECK(!parse_registry_json(R"({"not":"an array"})", entries, error));
    FGB_CHECK(!error.empty());

    const char* bad = R"json([{"ModuleId":"x","DownloadUrl":"ftp://nope"}])json";
    FGB_CHECK(parse_registry_json(bad, entries, error));
    FGB_CHECK(entries.empty()); // 非 http(s) 的下载地址被丢弃
}
