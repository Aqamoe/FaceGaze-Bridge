cmake_minimum_required(VERSION 3.24)

project(FaceGazeBridge
    VERSION 1.0.5
    DESCRIPTION "Eye and face tracking bridge for VRChat and other consumers"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()
if(NOT CMAKE_ARCHIVE_OUTPUT_DIRECTORY)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
endif()

option(FGB_BUILD_TESTS "Build unit tests" ON)
option(FGB_BUILD_CLI "Build fgb-cli" ON)
option(FGB_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

add_library(fgb_options INTERFACE)
target_compile_features(fgb_options INTERFACE cxx_std_20)
# 原生模块 ABI 头（模块作者与宿主共用同一份）
target_include_directories(fgb_options INTERFACE "${CMAKE_SOURCE_DIR}/sdk")

if(MSVC)
    target_compile_options(fgb_options INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
    # C4324: 结构体因 alignas 被填充。这是 SpscRing/SeqLock 刻意的 cache line 对齐，
    #        目的是消除伪共享，不是缺陷。
    # C4819: 源文件包含当前代码页无法表示的字符。源文件统一为 UTF-8 with BOM 且已指定
    #        /utf-8，此警告在系统代码页为 0 的环境（如 GitHub Actions runner）下仍会误报。
    target_compile_options(fgb_options INTERFACE /wd4324 /wd4819)
    target_compile_definitions(fgb_options INTERFACE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        UNICODE
        _UNICODE)
    if(FGB_WARNINGS_AS_ERRORS)
        target_compile_options(fgb_options INTERFACE /WX)
    endif()
else()
    target_compile_options(fgb_options INTERFACE -Wall -Wextra -Wpedantic)
    target_compile_definitions(fgb_options INTERFACE NOMINMAX)
endif()

add_subdirectory(src)

if(FGB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "FaceGaze Bridge ${PROJECT_VERSION}")
message(STATUS "  tests      : ${FGB_BUILD_TESTS}")
message(STATUS "  cli        : ${FGB_BUILD_CLI}")
message(STATUS "  werror     : ${FGB_WARNINGS_AS_ERRORS}")
