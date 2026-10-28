add_executable(fgb-tests
    test_main.cpp
    test_osc.cpp
    test_ring.cpp
    test_shape.cpp
    test_vrchat.cpp)

target_include_directories(fgb-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fgb-tests PRIVATE fgb_engine)

add_test(NAME fgb-tests COMMAND fgb-tests)
