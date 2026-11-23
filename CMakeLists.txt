cmake_minimum_required(VERSION 3.16)
project(mea_g1_control LANGUAGES CXX)

option(MEA_ENABLE_G1 "Build the Unitree SDK2 backend" OFF)

find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp)
if(NOT NLOHMANN_JSON_INCLUDE_DIR)
    message(FATAL_ERROR "Could not find nlohmann/json.hpp. Install nlohmann-json3-dev or set NLOHMANN_JSON_INCLUDE_DIR.")
endif()

add_library(mea_control STATIC src/controller.cpp src/mea_json.cpp)
target_compile_features(mea_control PUBLIC cxx_std_17)
target_include_directories(mea_control PUBLIC src PRIVATE "${NLOHMANN_JSON_INCLUDE_DIR}")

add_executable(mea_g1 src/main.cpp)
target_link_libraries(mea_g1 PRIVATE mea_control)

if(MEA_ENABLE_G1)
    find_package(unitree_sdk2 CONFIG REQUIRED)
    target_sources(mea_g1 PRIVATE src/g1_backend.cpp)
    target_compile_definitions(mea_g1 PRIVATE MEA_ENABLE_G1)
    target_link_libraries(mea_g1 PRIVATE unitree_sdk2)
endif()

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 3.7 REQUIRED COMPONENTS Interpreter)
    find_package(Threads REQUIRED)
    add_executable(controller_test tests/controller_test.cpp)
    target_link_libraries(controller_test PRIVATE mea_control Threads::Threads)
    add_test(NAME live_controller COMMAND controller_test)
    add_test(NAME jsonl_replay COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/replay.py" $<TARGET_FILE:mea_g1>
        "${CMAKE_CURRENT_SOURCE_DIR}/examples/mock_windows.jsonl")
    set_tests_properties(live_controller jsonl_replay PROPERTIES TIMEOUT 10)
endif()
