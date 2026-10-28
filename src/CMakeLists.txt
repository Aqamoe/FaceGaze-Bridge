add_library(fgb_base STATIC
    base/log.cpp
    base/clock.cpp)
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
    net/tcp_listener.cpp)
target_link_libraries(fgb_net PUBLIC fgb_base)
if(WIN32)
    target_link_libraries(fgb_net PRIVATE ws2_32)
endif()

# VRCFT 模块兼容层：协议编解码 + 数据转换 + 沙箱服务 + 进程管理
add_library(fgb_vrcft STATIC
    vrcft/protocol.cpp
    vrcft/update_data.cpp
    vrcft/sandbox_server.cpp
    vrcft/module_host.cpp)
target_link_libraries(fgb_vrcft PUBLIC fgb_core fgb_net)

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

# 本地控制面板：HTTP + 内嵌页面
add_library(fgb_web STATIC
    web/json.cpp
    web/panel.cpp)
target_link_libraries(fgb_web PUBLIC fgb_engine fgb_net fgb_process)

add_library(fgb_engine STATIC
    sinks/vrchat_sink.cpp
    sources/vmc_source.cpp
    sources/vrcft_source.cpp
    sources/replay_source.cpp
    engine/engine.cpp)
target_link_libraries(fgb_engine PUBLIC fgb_core fgb_osc fgb_net fgb_vrcft fgb_params fgb_capture
                                      fgb_config fgb_process)

if(FGB_BUILD_CLI)
    add_executable(fgb-cli cli/main.cpp)
    target_link_libraries(fgb-cli PRIVATE fgb_engine fgb_web)
    target_compile_definitions(fgb-cli PRIVATE FGB_VERSION="${PROJECT_VERSION}")
    set_target_properties(fgb-cli PROPERTIES OUTPUT_NAME "fgb-cli")
endif()
