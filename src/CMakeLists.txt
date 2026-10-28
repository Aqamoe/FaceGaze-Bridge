add_library(fgb_base STATIC
    base/log.cpp
    base/clock.cpp
    base/json.cpp)
target_include_directories(fgb_base PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fgb_base PUBLIC fgb_options)
if(WIN32)
    target_link_libraries(fgb_base PRIVATE winmm)
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
    net/ws_server.cpp)
target_link_libraries(fgb_net PUBLIC fgb_base)
if(WIN32)
    target_link_libraries(fgb_net PRIVATE ws2_32)
endif()

# VRCFT 模块兼容层：协议编解码 + 数据转换 + 沙箱服务 + 进程管理 + 模块发现
add_library(fgb_vrcft STATIC
    vrcft/protocol.cpp
    vrcft/update_data.cpp
    vrcft/sandbox_server.cpp
    vrcft/module_host.cpp
    vrcft/module_registry.cpp)
target_link_libraries(fgb_vrcft PUBLIC fgb_core fgb_net fgb_config)

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
    params/calibration.cpp)
target_link_libraries(fgb_params PUBLIC fgb_core fgb_base fgb_capture fgb_vrcft)

# 配置与后处理
add_library(fgb_config STATIC
    config/ini.cpp)
target_link_libraries(fgb_config PUBLIC fgb_base)

add_library(fgb_process STATIC
    core/process/one_euro.cpp)
target_link_libraries(fgb_process PUBLIC fgb_core)

add_library(fgb_engine STATIC
    engine/source_merger.cpp
    sinks/vrchat_sink.cpp
    sources/vmc_source.cpp
    sources/vrcft_source.cpp
    sources/replay_source.cpp
    engine/engine.cpp)
target_link_libraries(fgb_engine PUBLIC fgb_core fgb_osc fgb_net fgb_vrcft fgb_params fgb_capture
                                      fgb_config fgb_process)

# 控制面：WebSocket IPC + 本地静态页面
add_library(fgb_web STATIC
    web/ipc.cpp)
target_link_libraries(fgb_web PUBLIC fgb_engine fgb_net fgb_process)
target_compile_definitions(fgb_web PRIVATE FGB_VERSION="${PROJECT_VERSION}")

if(FGB_BUILD_CLI)
    add_executable(fgb-cli cli/main.cpp)
    target_link_libraries(fgb-cli PRIVATE fgb_engine fgb_web)
    target_compile_definitions(fgb-cli PRIVATE FGB_VERSION="${PROJECT_VERSION}")
    set_target_properties(fgb-cli PROPERTIES OUTPUT_NAME "fgb-cli")

    # 页面资源随可执行文件走：fgb-cli 是发页面的那一侧，fgb-ui 只是外壳
    add_custom_command(TARGET fgb-cli POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_SOURCE_DIR}/ui" "$<TARGET_FILE_DIR:fgb-cli>/ui"
        COMMENT "复制 UI 资源 → $<TARGET_FILE_DIR:fgb-cli>/ui")
endif()

# WebView2 外壳。sdk 缺失（离线/下载失败）时只跳过这一个目标，不影响引擎
option(FGB_BUILD_UI "Build fgb-ui (WebView2 shell)" ON)
if(WIN32 AND FGB_BUILD_UI)
    include(${CMAKE_SOURCE_DIR}/cmake/WebView2.cmake)
    fgb_webview2_target(FGB_WEBVIEW2_TARGET)
    if(FGB_WEBVIEW2_TARGET)
        add_executable(fgb-ui WIN32 ui/main.cpp)
        target_link_libraries(fgb-ui PRIVATE ${FGB_WEBVIEW2_TARGET} fgb_options)
        set_target_properties(fgb-ui PROPERTIES OUTPUT_NAME "fgb-ui")
    endif()
endif()

# ---- 发布包：用户双击就能跑 ----
#
#   cmake --install build/release --config Release --prefix dist
#
# 目录布局固定（见 AGENT.md §3.3）：exe 平铺在根，页面在 ui/，
# 两个 .cmd 是入口——引擎需要参数，双击 exe 是起不来的。
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
        "${CMAKE_SOURCE_DIR}/packaging/README.txt"
        "${CMAKE_SOURCE_DIR}/packaging/启动 (VRCFT 模块).cmd"
        "${CMAKE_SOURCE_DIR}/packaging/启动 (VMC).cmd"
        DESTINATION .)
endif()
