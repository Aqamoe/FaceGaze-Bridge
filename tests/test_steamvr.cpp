#include "openvr/steamvr.hpp"

#include "test_framework.hpp"

#include <filesystem>
#include <string>

using namespace fgb::steamvr;

FGB_TEST(steamvr_parse_openvrpaths_array) {
    const std::string text = R"({
  "config": [ "C:\\Program Files (x86)\\Steam\\config" ],
  "external_drivers": null,
  "jsonid": "vrpathreg",
  "runtime": [ "C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR" ],
  "version": 1
})";
    std::string runtime;
    FGB_CHECK(parse_openvrpaths_runtime(text, runtime));
    FGB_CHECK(runtime == R"(C:\Program Files (x86)\Steam\steamapps\common\SteamVR)");
}

FGB_TEST(steamvr_parse_openvrpaths_tolerates_order_and_string) {
    std::string runtime;
    FGB_CHECK(parse_openvrpaths_runtime(R"({"runtime":"D:/SteamVR"})", runtime));
    FGB_CHECK(runtime == "D:/SteamVR");

    FGB_CHECK(parse_openvrpaths_runtime(R"({"runtime":[]})", runtime) == false);
    FGB_CHECK(parse_openvrpaths_runtime(R"({"nope":"D:/SteamVR"})", runtime) == false);
    FGB_CHECK(parse_openvrpaths_runtime("", runtime) == false);
}

FGB_TEST(steamvr_parse_manifest_paths_from_steam_config) {
    const std::string text = R"({
   "manifest_paths" : [
      "C:\\Program Files (x86)\\Steam\\config\\steamapps.vrmanifest",
      "D:\\code\\FaceGaze-Bridge\\dist\\app.vrmanifest",
      "C:\\Users\\GYSS\\AppData\\Local\\Programs\\FaceGazeBridge\\app.vrmanifest"
   ]
})";
    const auto paths = parse_manifest_paths(text);
    FGB_CHECK(paths.size() == 3);
    FGB_CHECK(paths[1] == R"(D:\code\FaceGaze-Bridge\dist\app.vrmanifest)");
    FGB_CHECK(paths[2] == R"(C:\Users\GYSS\AppData\Local\Programs\FaceGazeBridge\app.vrmanifest)");
    FGB_CHECK(parse_manifest_paths(R"({"applications": []})").empty());
    FGB_CHECK(parse_manifest_paths("").empty());
}

FGB_TEST(steamvr_manifest_json_has_required_fields) {
    const std::string json = build_manifest_json("FaceGaze Bridge.exe", "--tray", kAppKey);
    FGB_CHECK(json.find("\"app_key\": \"com.facegazebridge.desktop\"") != std::string::npos);
    FGB_CHECK(json.find("\"binary_path_windows\": \"FaceGaze Bridge.exe\"") != std::string::npos);
    FGB_CHECK(json.find("\"arguments\": \"--tray\"") != std::string::npos);
    FGB_CHECK(json.find("\"is_dashboard_overlay\": true") != std::string::npos);
    FGB_CHECK(json.find("\"source\": \"builtin\"") != std::string::npos);
}

FGB_TEST(steamvr_paths_match_packaging_layout) {
    const std::filesystem::path dir = "C:/apps/FaceGazeBridge";
    FGB_CHECK(manifest_path_for(dir) == dir / "app.vrmanifest");
    FGB_CHECK(dll_path_for("C:/SteamVR") ==
              std::filesystem::path{"C:/SteamVR/bin/win64/openvr_api.dll"});
}

FGB_TEST(steamvr_classify_init_errors) {
    FGB_CHECK(classify_init_error(0) == State::Off);
    FGB_CHECK(classify_init_error(100) == State::NotInstalled); // InstallationNotFound
    FGB_CHECK(classify_init_error(108) == State::NotRunning);   // HmdNotFound
    FGB_CHECK(classify_init_error(121) == State::NotRunning);   // NoServerForBackgroundApp
    FGB_CHECK(classify_init_error(9999) == State::Error);
    FGB_CHECK(std::string{state_name(State::NotInstalled)} == "not_installed");
    FGB_CHECK(std::string{state_name(State::NotRunning)} == "not_running");
    FGB_CHECK(std::string{state_name(State::On)} == "on");
}
