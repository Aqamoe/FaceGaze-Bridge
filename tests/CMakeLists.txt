add_executable(fgb-tests
    test_main.cpp
    test_osc.cpp
    test_ring.cpp
    test_shape.cpp
    test_params.cpp
    test_vrchat.cpp
    test_vrcft_protocol.cpp
    test_v1_params.cpp
    test_capture.cpp
    test_calibration.cpp
    test_ws.cpp
    test_json.cpp
    test_modules.cpp
    test_library.cpp
    test_native_module.cpp
    test_shm.cpp
    test_merge.cpp
    test_vmc_out.cpp
    test_steamvr.cpp)

target_include_directories(fgb-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(fgb-tests PRIVATE FGB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(fgb-tests PRIVATE fgb_engine fgb_web)
if(TARGET fgb-sample-native)
    add_dependencies(fgb-tests fgb-sample-native)
endif()

# C4127: 测试里比较编译期常量（如 kShapeCount == 86）是有意为之——
#        这些断言的作用就是在编译期常量被误改时炸掉。
if(MSVC)
    target_compile_options(fgb-tests PRIVATE /wd4127)
endif()

add_test(NAME fgb-tests COMMAND $<TARGET_FILE:fgb-tests>)
