// 诊断包：把日志、配置、崩溃转储和一个系统信息文本打成一个 zip。
//
// 不含模块 DLL、不含头像配置；用户主动点「导出」才生成，路径会回给界面。
// 打包用系统自带的 tar.exe（Windows 10 1803+），不引 zip 库。
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fgb::diagnostics {

struct BundleRequest {
    std::filesystem::path output; // .zip；为空则写到默认目录
    std::filesystem::path exe_dir;
    std::string version;
    // 额外文件（相对路径, 内容），例如调用方生成的 status.json
    std::vector<std::pair<std::string, std::string>> extra_files;
};

std::filesystem::path default_bundle_path();

// 成功返回 true；失败时 error 给可显示原因，临时目录会清掉。
bool build_bundle(const BundleRequest& request, std::string& error);

} // namespace fgb::diagnostics
