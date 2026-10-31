add_executable(fgb-tests
    test_main.cpp
    test_osc.cpp
    test_ring.cpp
    test_shape.cpp
    test_params.cpp
    test_vrchat.cpp
    test_oscquery.cpp
    test_outputs.cpp
    test_stream_out.cpp
    test_sinks_stress.cpp
    test_update.cpp
    test_proxy.cpp
    test_vrcft_protocol.cpp
    test_v1_params.cpp
    test_capture.cpp
    test_calibration.cpp
    test_gaze.cpp
    test_engine_stats.cpp
    test_crash.cpp
    test_ws.cpp
    test_json.cpp
    test_modules.cpp
    test_library.cpp
    test_native_library.cpp
    test_native_module.cpp
    test_native_host.cpp
    test_shm.cpp
    test_merge.cpp
    test_vmc_out.cpp
    test_osc_input.cpp
    test_livelink.cpp
    test_steamvr.cpp)

target_include_directories(fgb-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(fgb-tests PRIVATE FGB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(fgb-tests PRIVATE fgb_engine fgb_web)
if(TARGET fgb-sample-native)
    add_dependencies(fgb-tests fgb-sample-native)
endif()

# ABI v1 移除后的失败路径夹具：只导出老符号 fgb_module_query 的 DLL。
# 不 include SDK 头（v1 结构已删），见 tests/fixtures/legacy_v1_module.cpp。
add_library(fgb-test-legacy-v1 MODULE fixtures/legacy_v1_module.cpp)
target_link_libraries(fgb-test-legacy-v1 PRIVATE fgb_options)
set_target_properties(fgb-test-legacy-v1 PROPERTIES
    PREFIX ""
    OUTPUT_NAME "fgb-test-legacy-v1"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
add_dependencies(fgb-tests fgb-test-legacy-v1)

# 状态上报夹具：ACTIVE → ERROR → ACTIVE，验证宿主能把"自愈"镜像回引擎。
add_library(fgb-test-status-cycle MODULE fixtures/status_cycle_module.cpp)
target_include_directories(fgb-test-status-cycle PRIVATE "${CMAKE_SOURCE_DIR}/sdk")
target_link_libraries(fgb-test-status-cycle PRIVATE fgb_options)
set_target_properties(fgb-test-status-cycle PROPERTIES
    PREFIX ""
    OUTPUT_NAME "fgb-test-status-cycle"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
add_dependencies(fgb-tests fgb-test-status-cycle)

# C4127: 测试里比较编译期常量（如 kShapeCount == 86）是有意为之——
#        这些断言的作用就是在编译期常量被误改时炸掉。
if(MSVC)
    target_compile_options(fgb-tests PRIVATE /wd4127)
endif()

add_test(NAME fgb-tests COMMAND $<TARGET_FILE:fgb-tests>)

# 故意崩溃的子进程，供 tools/probe_crash.py 验证未处理异常会留下 minidump
add_executable(fgb-crash-child crash_child.cpp)
target_link_libraries(fgb-crash-child PRIVATE fgb_base)
