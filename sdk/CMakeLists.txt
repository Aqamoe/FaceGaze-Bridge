# FaceGaze Bridge 原生模块 SDK：ABI 头 + 文档 + 示例。
# 模块工程可以直接 add_subdirectory(<FaceGaze-Bridge>/sdk) 后链接 fgb::sdk。
cmake_minimum_required(VERSION 3.24)

# SDK 包版本独立于软件版本（ADR-0048）。唯一来源是 fgb_sdk_version.h：
# 它随 sdk/ 目录一起分发，模块作者不跑 CMake 也能读到版本宏。
set(FGB_SDK_VERSION_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/fgb_sdk_version.h")
file(STRINGS "${FGB_SDK_VERSION_HEADER}" _fgb_sdk_version_line
    REGEX "^#define FGB_SDK_VERSION ")
if(NOT _fgb_sdk_version_line)
    message(FATAL_ERROR "sdk/fgb_sdk_version.h 缺少 FGB_SDK_VERSION 定义")
endif()
string(REGEX REPLACE "^#define FGB_SDK_VERSION \"([0-9]+\\.[0-9]+\\.[0-9]+)\".*$" "\\1"
    FGB_SDK_VERSION "${_fgb_sdk_version_line}")
if(NOT FGB_SDK_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "FGB_SDK_VERSION 不是 x.y.z 形式：${_fgb_sdk_version_line}")
endif()

# MAJOR/MINOR/PATCH 与字符串必须一致，否则模块里两种写法会报出互相矛盾的版本。
foreach(_fgb_part MAJOR MINOR PATCH)
    file(STRINGS "${FGB_SDK_VERSION_HEADER}" _fgb_part_line
        REGEX "^#define FGB_SDK_VERSION_${_fgb_part} ")
    if(NOT _fgb_part_line)
        message(FATAL_ERROR "sdk/fgb_sdk_version.h 缺少 FGB_SDK_VERSION_${_fgb_part}")
    endif()
    string(REGEX REPLACE "^.* ([0-9]+).*$" "\\1" _fgb_part_value "${_fgb_part_line}")
    set(FGB_SDK_VERSION_${_fgb_part} "${_fgb_part_value}")
endforeach()
if(NOT "${FGB_SDK_VERSION_MAJOR}.${FGB_SDK_VERSION_MINOR}.${FGB_SDK_VERSION_PATCH}"
        STREQUAL "${FGB_SDK_VERSION}")
    message(FATAL_ERROR
        "fgb_sdk_version.h 的 MAJOR/MINOR/PATCH 与字符串 ${FGB_SDK_VERSION} 不一致")
endif()

# 直接 cmake -S sdk 时作为顶层工程；被 add_subdirectory/FetchContent 引入时不重复 project()
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(FaceGazeBridgeSdk VERSION ${FGB_SDK_VERSION} LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

add_library(fgb_sdk INTERFACE)
add_library(fgb::sdk ALIAS fgb_sdk)
target_include_directories(fgb_sdk INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(fgb_sdk INTERFACE cxx_std_20)

option(FGB_SDK_BUILD_SAMPLES "Build the native module samples" OFF)
if(FGB_SDK_BUILD_SAMPLES)
    add_subdirectory(samples)
endif()
