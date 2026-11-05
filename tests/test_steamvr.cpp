#include "openvr/steamvr.hpp"
#include "openvr/steamvr_watch.hpp"

#include "test_framework.hpp"

#include <cstdint>
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

// 没打开开关时，SteamVR 退出多少次都不该有动作
FGB_TEST(steamvr_exit_watch_stays_quiet_when_disabled) {
    ExitWatch watch;
    std::uint64_t now = 1000;
    for (int i = 0; i < 20; ++i, now += 1000) {
        FGB_CHECK(!watch.observe(false, false, now));
    }
}

// 用户先开 FGB、SteamVR 后起来：没看见过 vrserver 就不能判"它退出了"
FGB_TEST(steamvr_exit_watch_needs_a_sighting) {
    ExitWatch watch;
    FGB_CHECK(!watch.observe(true, false, 0));
    FGB_CHECK(!watch.saw_steamvr());
    FGB_CHECK(!watch.observe(true, false, 60000));
    FGB_CHECK(!watch.saw_steamvr());

    FGB_CHECK(!watch.observe(true, true, 61000));
    FGB_CHECK(watch.saw_steamvr());
}

// 关 SteamVR 要连续消失到超时才算退出，中途回来（vrserver 自己重启）就重新计时
FGB_TEST(steamvr_exit_watch_debounces_vrserver_restart) {
    ExitWatch watch;
    FGB_CHECK(!watch.observe(true, true, 0));

    // 消失 3 秒后又回来：仍在去抖窗口内，不算退出
    FGB_CHECK(!watch.observe(true, false, 1000));
    FGB_CHECK(!watch.observe(true, false, 3000));
    FGB_CHECK(!watch.observe(true, true, 4000));

    // 再消失，必须从这一刻重新数满 kGoneTimeoutMs
    FGB_CHECK(!watch.observe(true, false, 5000));
    FGB_CHECK(!watch.observe(true, false, 5000 + ExitWatch::kGoneTimeoutMs - 1));
    FGB_CHECK(watch.observe(true, false, 5000 + ExitWatch::kGoneTimeoutMs));
    // 只报一次：引擎没退成也不会每拍都报
    FGB_CHECK(!watch.observe(true, false, 5000 + ExitWatch::kGoneTimeoutMs + 5000));

    watch.reset();
    FGB_CHECK(!watch.saw_steamvr());
    FGB_CHECK(!watch.observe(true, false, 100000)); // 复位后要重新看见过
}

// 开关中途关掉再打开：不能沿用上次的倒计时（否则一开就立刻退出）
FGB_TEST(steamvr_exit_watch_disable_clears_countdown) {
    ExitWatch watch;
    FGB_CHECK(!watch.observe(true, true, 0));
    FGB_CHECK(!watch.observe(true, false, 1000));
    FGB_CHECK(!watch.observe(false, false, 2000));
    FGB_CHECK(!watch.observe(true, false, 3000));
    FGB_CHECK(!watch.observe(true, false, 3000 + ExitWatch::kGoneTimeoutMs));
}

// 休眠恢复后 now_ms 可能跳变。跳变要重新计时，不能当成"消失很久了"直接退出
FGB_TEST(steamvr_exit_watch_handles_clock_jump_backwards) {
    ExitWatch watch;
    FGB_CHECK(!watch.observe(true, true, 100000));
    FGB_CHECK(!watch.observe(true, false, 101000));
    FGB_CHECK(!watch.observe(true, false, 500));                              // 时间倒退
    FGB_CHECK(!watch.observe(true, false, 500 + ExitWatch::kGoneTimeoutMs - 1)); // 重新计时中
    FGB_CHECK(watch.observe(true, false, 500 + ExitWatch::kGoneTimeoutMs));
}
