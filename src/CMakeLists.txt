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
    net/udp_socket.cpp)
target_link_libraries(fgb_net PUBLIC fgb_base)
if(WIN32)
    target_link_libraries(fgb_net PRIVATE ws2_32)
endif()

add_library(fgb_engine STATIC
    sinks/vrchat_sink.cpp
    sources/vmc_source.cpp
    engine/engine.cpp)
target_link_libraries(fgb_engine PUBLIC fgb_core fgb_osc fgb_net)

if(FGB_BUILD_CLI)
    add_executable(fgb-cli cli/main.cpp)
    target_link_libraries(fgb-cli PRIVATE fgb_engine)
    target_compile_definitions(fgb-cli PRIVATE FGB_VERSION="${PROJECT_VERSION}")
    set_target_properties(fgb-cli PROPERTIES OUTPUT_NAME "fgb-cli")
endif()
