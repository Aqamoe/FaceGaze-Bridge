// 文件摘要：模块库用它校验下载包。
//
// 只用系统自带的 BCrypt（Windows 7+），不引 OpenSSL：发布包里不需要再多一个
// DLL，也不需要证书/协议栈那一整套。原生模块库要求 SHA-256，VRCFT 老注册表
// 只有 MD5，两个都留着。
#pragma once

#include <filesystem>
#include <string>

namespace fgb::hash {

// 返回小写十六进制；文件读不了或算法不可用时返回空串。
std::string sha256_file(const std::filesystem::path& path);
std::string md5_file(const std::filesystem::path& path);

} // namespace fgb::hash
