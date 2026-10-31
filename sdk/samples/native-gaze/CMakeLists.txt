cmake_minimum_required(VERSION 3.24)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(fgb_native_gaze LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

add_library(fgb-sample-gaze MODULE native_gaze.cpp)
if(TARGET fgb::sdk)
    target_link_libraries(fgb-sample-gaze PRIVATE fgb::sdk)
else()
    target_include_directories(fgb-sample-gaze PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../..")
    target_compile_features(fgb-sample-gaze PRIVATE cxx_std_20)
endif()

if(MSVC)
    target_compile_options(fgb-sample-gaze PRIVATE /W4 /permissive- /utf-8 /EHsc)
endif()

set_target_properties(fgb-sample-gaze PROPERTIES
    PREFIX ""
    OUTPUT_NAME "fgb-sample-gaze")
