cmake_minimum_required(VERSION 3.20)
project(zsharp_android_port_test LANGUAGES C)

add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../../native/core"
                 "${CMAKE_CURRENT_BINARY_DIR}/zsharp-core")

add_library(zsharp_port_test SHARED bridge.c)
target_include_directories(zsharp_port_test PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../../native/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../../native/src")
target_link_libraries(zsharp_port_test PRIVATE zsharp_language_core log)
target_compile_features(zsharp_port_test PRIVATE c_std_17)
