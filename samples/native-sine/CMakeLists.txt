cmake_minimum_required(VERSION 3.24)
project(fgb_native_sine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(fgb-sample-native MODULE native_sine.cpp)
target_include_directories(fgb-sample-native PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../sdk")

if(MSVC)
    target_compile_options(fgb-sample-native PRIVATE /W4 /permissive- /utf-8 /EHsc)
endif()

set_target_properties(fgb-sample-native PROPERTIES
    PREFIX ""
    OUTPUT_NAME "fgb-sample-native")
