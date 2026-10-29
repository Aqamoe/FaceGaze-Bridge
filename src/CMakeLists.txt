add_library(fgb_base STATIC
    base/log.cpp
    base/clock.cpp
    base/json.cpp
    base/crash.cpp
    base/diagnostics.cpp
    base/process_stats.cpp)
target_include_directories(fgb_base PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fgb_base PUBLIC fgb_options)
if(WIN32)
    target_link_libraries(fgb_base PRIVATE winmm psapi dbghelp)
endif()

add_library(fgb_core STATIC
    core/model/shape.cpp
    core/model/frame.cpp)
target_link_libraries(fgb_core PUBLIC fgb_base)

add_library(fgb_osc STATIC
    osc/encoder.cpp
    osc/decoder.cpp)
target_link_libraries(fgb_osc PUBLIC fgb_base)

add_library(fgb_net STATIC
    net/udp_socket.cpp
    net/tcp_listener.cpp
    net/ws_codec.cpp
    net/ws_server.cpp
    net/http_client.cpp
    net/mdns.cpp)
target_link_libraries(fgb_net PUBLIC fgb_base)
if(WIN32)
    target_link_libraries(fgb_net PRIVATE ws2_32 winhttp)
endif()

# VRCFT 模块兼容层：协议编解码 + 数据转换 + 沙箱服务 + 进程管理 + 模块发现
add_library(fgb_vrcft STATIC
    vrcft/protocol.cpp
    vrcft/update_data.cpp
    vrcft/sandbox_server.cpp
    vrcft/module_host.cpp
    vrcft/module_registry.cpp
    vrcft/module_library.cpp)
target_link_libraries(fgb_vrcft PUBLIC fgb_core fgb_net fgb_config)
if(WIN32)
    target_link_libraries(fgb_vrcft PRIVATE bcrypt)
endif()

# 抓包文件格式。engine 与 params（校准推导要读抓包）都要用
add_library(fgb_capture STATIC
    capture/capture_file.cpp)
target_link_libraries(fgb_capture PUBLIC fgb_base)

# 参数派生层：把协议层的肌肉形状换算成 VRChat 认的 v2 参数，外加 v1 legacy、地址发现与校准
add_library(fgb_params STATIC
    params/catalog.cpp
    params/v2_params.cpp
    params/v1_params.cpp
    params/avatar_config.cpp
    params/oscquery.cpp
    params/preview_tap.cpp
    params/calibration.cpp)
target_link_libraries(fgb_params PUBLIC fgb_core fgb_base fgb_capture fgb_vrcft fgb_net)

# 配置与后处理
add_library(fgb_config STATIC
    config/ini.cpp
    config/outputs.cpp)
target_link_libraries(fgb_config PUBLIC fgb_base)

# SteamVR 启动项：只 vendor openvr.h，运行时从用户已安装的 SteamVR 目录
# 动态加载 openvr_api.dll（不产生再分发义务，见 ADR-0020）。
add_library(fgb_steamvr STATIC
    openvr/steamvr.cpp)
target_include_directories(fgb_steamvr PUBLIC "${CMAKE_SOURCE_DIR}/third_party")
target_link_libraries(fgb_steamvr PUBLIC fgb_base)
if(WIN32)
    target_link_libraries(fgb_steamvr PRIVATE advapi32)
endif()

add_library(fgb_process STATIC
    core/process/one_euro.cpp
    core/process/gaze_estimator.cpp)
target_link_libraries(fgb_process PUBLIC fgb_core)

# VMC 协议的共享部分（blendshape 名字表）：输入和输出都用它，
# 各自维护一份就会出现"能收不能发"。
add_library(fgb_vmc STATIC
    vmc/vmc_map.cpp)
target_link_libraries(fgb_vmc PUBLIC fgb_core)

add_library(fgb_engine STATIC
    engine/source_merger.cpp
    sinks/vrchat_sink.cpp
    sinks/vmc_sink.cpp
    sinks/shm_sink.cpp
    sinks/udp_binary_sink.cpp
    sinks/ws_json_sink.cpp
    sources/vmc_source.cpp
    sources/vrcft_source.cpp
    sources/native_source.cpp
    sources/replay_source.cpp
    engine/engine.cpp)
target_link_libraries(fgb_engine PUBLIC fgb_core fgb_osc fgb_net fgb_vrcft fgb_params fgb_capture
                                      fgb_config fgb_process fgb_vmc)
target_include_directories(fgb_engine PUBLIC "${CMAKE_SOURCE_DIR}/sdk")

# 控制面：WebSocket IPC + 本地静态页面
add_library(fgb_web STATIC
    web/ipc.cpp)
target_link_libraries(fgb_web PUBLIC fgb_engine fgb_net fgb_process fgb_steamvr)
if(WIN32)
    target_link_libraries(fgb_web PRIVATE ole32 comdlg32 shell32)
endif()
target_compile_definitions(fgb_web PRIVATE FGB_VERSION="${PROJECT_VERSION}")

# 版本信息 + 图标：两个 exe 共用一份模板，内部名/原始文件名/说明各自填。
# 图标只给产品入口：fgb-cli 长得和界面入口一样，用户会双击错程序。
function(fgb_add_windows_resources target internal_name original_filename description)
    if(NOT WIN32)
        return()
    endif()
    cmake_parse_arguments(ARG "NO_ICON" "" "" ${ARGN})
    set(FGB_RC_ICON_LINE "")
    if(NOT ARG_NO_ICON)
        set(FGB_RC_ICON_LINE
            "101 ICON \"${CMAKE_SOURCE_DIR}/packaging/icon.ico\"")
    endif()
    set(FGB_RC_INTERNAL_NAME "${internal_name}")
    set(FGB_RC_FILENAME "${original_filename}")
    set(FGB_RC_DESCRIPTION "${description}")
    configure_file("${CMAKE_SOURCE_DIR}/packaging/app.rc.in"
                   "${CMAKE_CURRENT_BINARY_DIR}/res/${target}.rc" @ONLY)
    target_sources(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/res/${target}.rc")
endfunction()

if(FGB_BUILD_CLI)
    add_executable(fgb-cli cli/main.cpp)
    target_link_libraries(fgb-cli PRIVATE fgb_engine fgb_web)
    target_compile_definitions(fgb-cli PRIVATE FGB_VERSION="${PROJECT_VERSION}")
    set_target_properties(fgb-cli PROPERTIES OUTPUT_NAME "fgb-cli")
    fgb_add_windows_resources(fgb-cli "fgb-cli" "fgb-cli.exe"
        "FaceGaze Bridge Engine (command line)" NO_ICON)

    # 页面资源随可执行文件走：fgb-cli 是发页面的那一侧，fgb-ui 只是外壳
    add_custom_command(TARGET fgb-cli POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_SOURCE_DIR}/ui" "$<TARGET_FILE_DIR:fgb-cli>/ui"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/packaging/app.vrmanifest"
                "$<TARGET_FILE_DIR:fgb-cli>/app.vrmanifest"
        COMMENT "复制 UI 资源与 SteamVR manifest → $<TARGET_FILE_DIR:fgb-cli>")
endif()

# 产品入口：FaceGaze Bridge.exe（WebView2 外壳，同时负责把引擎拉起来）。
# sdk 缺失（离线/下载失败）时只跳过这一个目标，不影响引擎
option(FGB_BUILD_UI "Build the FaceGaze Bridge window shell (WebView2)" ON)
if(WIN32 AND FGB_BUILD_UI)
    include(${CMAKE_SOURCE_DIR}/cmake/WebView2.cmake)
    fgb_webview2_target(FGB_WEBVIEW2_TARGET)
    if(FGB_WEBVIEW2_TARGET)
        add_executable(fgb-ui WIN32 ui/main.cpp)
        target_link_libraries(fgb-ui PRIVATE ${FGB_WEBVIEW2_TARGET} fgb_options ws2_32)
        # 用户双击的那个文件就叫产品名；命令行/探针里仍可按 fgb-ui 目标名引用
        set_target_properties(fgb-ui PROPERTIES OUTPUT_NAME "FaceGaze Bridge")
        fgb_add_windows_resources(fgb-ui "fgb-ui" "FaceGaze Bridge.exe" "FaceGaze Bridge")
    endif()
endif()

# 原生 C ABI 示例模块（无硬件也能验证整条链路）
option(FGB_BUILD_SAMPLE_MODULE "Build the native C ABI sample module" ON)
if(WIN32 AND FGB_BUILD_SAMPLE_MODULE)
    add_library(fgb-sample-native MODULE "${CMAKE_SOURCE_DIR}/samples/native-sine/native_sine.cpp")
    target_link_libraries(fgb-sample-native PRIVATE fgb_options)
    target_include_directories(fgb-sample-native PRIVATE "${CMAKE_SOURCE_DIR}/sdk")
    set_target_properties(fgb-sample-native PROPERTIES
        PREFIX ""
        OUTPUT_NAME "fgb-sample-native"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
endif()

# ---- 发布包：用户双击就能跑 ----
#
#   cmake --install build/release --config Release --prefix dist
#
# 目录布局（见 AGENT.md §3.3）：exe 平铺在根，页面在 ui/。
# 唯一入口是「FaceGaze Bridge.exe」——它会自己拉起 fgb-cli，不再需要 .cmd。
if(WIN32)
    if(TARGET fgb-cli)
        install(TARGETS fgb-cli RUNTIME DESTINATION .)
    endif()
    if(TARGET fgb-ui)
        # 静态链接 WebView2Loader，所以 exe 旁边不需要任何 DLL
        install(TARGETS fgb-ui RUNTIME DESTINATION .)
    endif()
    install(DIRECTORY "${CMAKE_SOURCE_DIR}/ui/" DESTINATION ui)
    install(FILES
        "${CMAKE_SOURCE_DIR}/sdk/fgb_module_api.h"
        "${CMAKE_SOURCE_DIR}/sdk/fgb_shared_frame.h"
        "${CMAKE_SOURCE_DIR}/sdk/fgb_udp_frame.h"
        DESTINATION sdk)
    install(FILES "${CMAKE_SOURCE_DIR}/docs/05-native-module-guide.md"
        DESTINATION sdk RENAME NATIVE-MODULE-GUIDE.md)
    install(FILES "${CMAKE_SOURCE_DIR}/docs/06-shared-memory.md"
        DESTINATION sdk RENAME SHARED-MEMORY.md)
    install(FILES "${CMAKE_SOURCE_DIR}/docs/07-streaming-outputs.md"
        DESTINATION sdk RENAME STREAMING-OUTPUTS.md)
    install(FILES "${CMAKE_SOURCE_DIR}/docs/08-gaze-service.md"
        DESTINATION sdk RENAME GAZE-SERVICE.md)
    install(FILES
        "${CMAKE_SOURCE_DIR}/samples/native-sine/native_sine.cpp"
        "${CMAKE_SOURCE_DIR}/samples/native-sine/fgb-native.json"
        "${CMAKE_SOURCE_DIR}/samples/native-sine/CMakeLists.txt"
        DESTINATION sdk/sample-native-sine)
    if(TARGET fgb-sample-native)
        install(TARGETS fgb-sample-native
            RUNTIME DESTINATION sdk/sample-native-sine
            LIBRARY DESTINATION sdk/sample-native-sine)
    endif()
    install(FILES "${CMAKE_SOURCE_DIR}/packaging/README.txt" DESTINATION .)
    install(FILES
        "${CMAKE_SOURCE_DIR}/packaging/app.vrmanifest"
        "${CMAKE_SOURCE_DIR}/NOTICE"
        DESTINATION .)
    install(FILES "${CMAKE_SOURCE_DIR}/third_party/openvr/LICENSE"
        DESTINATION licenses RENAME openvr-LICENSE.txt)
endif()
