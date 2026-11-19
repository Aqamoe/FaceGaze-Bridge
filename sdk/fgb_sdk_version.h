/*
 * FaceGaze Bridge 原生模块 SDK 的包版本。
 * =====================================
 *
 * 这个版本号**独立于软件版本**（ADR-0048）：软件发版不会自动改它，只有 ABI 头、
 * 集成文档、示例发生变化时才手动 bump。目的是让模块作者看到"SDK 没变"就是真的没变，
 * 而不是每次宿主升级都要重新对一遍头文件。
 *
 * 三类版本不要混：
 *   - 软件/宿主版本：CMakeLists.txt 的 project(VERSION)，也是 git tag `v<版本>`；
 *     模块清单里的 MinHostVersion 比较的是它。
 *   - SDK 包版本：本文件，也就是 Release 里 FaceGazeBridge-SDK-<版本>.zip 的文件名。
 *   - ABI 版本：fgb_module_api.h 的 FGB_MODULE_ABI_V2，二进制契约，改结构体才动。
 *
 * 本文件是 SDK 版本的唯一来源：sdk/CMakeLists.txt 与 packaging/pack_sdk.ps1 都解析它，
 * 不要另建 VERSION 文件或 CMake 变量。
 */
#ifndef FGB_SDK_VERSION_H
#define FGB_SDK_VERSION_H

#define FGB_SDK_VERSION_MAJOR 1
#define FGB_SDK_VERSION_MINOR 2
#define FGB_SDK_VERSION_PATCH 0

/* 必须与上面三个宏一致；打包脚本会校验。 */
#define FGB_SDK_VERSION "1.2.0"

#endif
