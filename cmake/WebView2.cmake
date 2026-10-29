# WebView2 运行时与 SDK 是两回事：
#   - 运行时（Evergreen）Win10/11 自带，`fgb-ui` 不需要安装任何东西
#   - SDK 只用于编译：头文件 + WebView2LoaderStatic.lib
#
# SDK 不在 vcpkg 里，也不想让用户先装 NuGet，所以这里在 configure 时从 NuGet 拉一份
# 到 build/_deps 下（只下一次）。内网/离线环境可以预先解压好，用 -DFGB_WEBVIEW2_DIR=...
# 指过去。
#
# 用静态版 Loader：省掉"exe 旁边必须躺一个 WebView2Loader.dll"这个发布期陷阱，
# 单文件就能跑。

set(FGB_WEBVIEW2_VERSION "1.0.4314.35" CACHE STRING "Microsoft.Web.WebView2 SDK 版本")
set(FGB_WEBVIEW2_DIR "" CACHE PATH "已解压的 WebView2 SDK 根目录（含 build/native）")

function(fgb_webview2_target out_target)
    set(${out_target} "" PARENT_SCOPE)

    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|ARM64|arm64)$")
            set(arch "arm64")
        else()
            set(arch "x64")
        endif()
    else()
        set(arch "x86")
    endif()

    if(FGB_WEBVIEW2_DIR)
        set(root "${FGB_WEBVIEW2_DIR}")
    else()
        set(root "${CMAKE_BINARY_DIR}/_deps/webview2/sdk")
        if(NOT EXISTS "${root}/build/native/include/WebView2.h")
            set(archive "${CMAKE_BINARY_DIR}/_deps/webview2/webview2-sdk.nupkg")
            file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/_deps/webview2")
            message(STATUS "下载 WebView2 SDK ${FGB_WEBVIEW2_VERSION}")
            file(DOWNLOAD
                "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/${FGB_WEBVIEW2_VERSION}/microsoft.web.webview2.${FGB_WEBVIEW2_VERSION}.nupkg"
                "${archive}"
                STATUS download_status
                TIMEOUT 180)
            list(GET download_status 0 download_code)
            if(NOT download_code EQUAL 0)
                list(GET download_status 1 download_message)
                message(WARNING "WebView2 SDK 下载失败：${download_message}；跳过 fgb-ui")
                return()
            endif()
            file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${root}")
        endif()
    endif()

    # 供安装规则复制 SDK 的 LICENSE/NOTICE（发布包 licenses/ 里要带）
    set(FGB_WEBVIEW2_SDK_DIR "${root}" PARENT_SCOPE)

    set(include_dir "${root}/build/native/include")
    set(loader "${root}/build/native/${arch}/WebView2LoaderStatic.lib")
    if(NOT EXISTS "${include_dir}/WebView2.h" OR NOT EXISTS "${loader}")
        message(WARNING "WebView2 SDK 结构不符合预期（${root}）；跳过 fgb-ui")
        return()
    endif()

    add_library(webview2 INTERFACE)
    target_include_directories(webview2 INTERFACE "${include_dir}")
    target_link_libraries(webview2 INTERFACE
        "${loader}"
        version shlwapi advapi32 ole32 oleaut32 uuid user32 gdi32 shell32)
    set(${out_target} webview2 PARENT_SCOPE)
endfunction()
