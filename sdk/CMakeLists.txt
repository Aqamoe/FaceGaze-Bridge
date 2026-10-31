# FaceGaze Bridge 原生模块 SDK：ABI 头 + 文档 + 示例。
# 模块工程可以直接 add_subdirectory(<FaceGaze-Bridge>/sdk) 后链接 fgb::sdk。
cmake_minimum_required(VERSION 3.24)

# 直接 cmake -S sdk 时作为顶层工程；被 add_subdirectory/FetchContent 引入时不重复 project()
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(FaceGazeBridgeSdk LANGUAGES CXX)
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
