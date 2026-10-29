// 更新检查的纯逻辑：版本比较与 GitHub release JSON 解析（不联网）。
#include "net/update_check.hpp"

#include "test_framework.hpp"

#include <string>

using namespace fgb;

FGB_TEST(update_compare_versions) {
    FGB_CHECK(update::compare_versions("0.1.0", "0.1.0") == 0);
    FGB_CHECK(update::compare_versions("v0.1.0", "0.1.0") == 0);
    FGB_CHECK(update::compare_versions("0.1.1", "0.1.0") > 0);
    FGB_CHECK(update::compare_versions("0.2", "0.1.9") > 0);
    FGB_CHECK(update::compare_versions("1.0.0", "0.9.9") > 0);
    FGB_CHECK(update::compare_versions("0.1.0-rc1", "0.1.0") == 0);
    FGB_CHECK(update::compare_versions("  v0.10.0  ", "0.9.0") > 0);
}

FGB_TEST(update_parse_release_json) {
    const std::string body = R"({
      "tag_name": "v0.2.0",
      "name": "FaceGaze Bridge 0.2.0",
      "html_url": "https://github.com/Aqamoe/FaceGaze-Bridge/releases/tag/v0.2.0",
      "body": "notes\nwith lines",
      "assets": [{"name": "FaceGazeBridge.zip", "size": 123}]
    })";
    update::ReleaseInfo info;
    FGB_CHECK(update::parse_release_json(body, info));
    FGB_CHECK(info.tag == "v0.2.0");
    FGB_CHECK(info.name == "FaceGaze Bridge 0.2.0");
    FGB_CHECK(info.url.find("releases/tag/v0.2.0") != std::string::npos);
    FGB_CHECK(info.notes.find("notes") != std::string::npos);

    FGB_CHECK(!update::parse_release_json("", info));
    FGB_CHECK(!update::parse_release_json(R"({"message":"Not Found"})", info));
}
