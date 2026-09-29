cmake_minimum_required(VERSION 3.24)
project(fprime_dartwic_plugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(DARTWIC_SOURCE_ROOT "" CACHE PATH "DARTWIC source checkout containing TEMPEST and Engine Protocol")
if(NOT DARTWIC_SOURCE_ROOT AND DEFINED ENV{DARTWIC_SOURCE_ROOT})
    set(DARTWIC_SOURCE_ROOT "$ENV{DARTWIC_SOURCE_ROOT}")
endif()
if(NOT DARTWIC_SOURCE_ROOT AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../DARTWIC/tempest/peer/CMakeLists.txt")
    get_filename_component(DARTWIC_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../DARTWIC" ABSOLUTE)
endif()
if(NOT EXISTS "${DARTWIC_SOURCE_ROOT}/tempest/peer/CMakeLists.txt"
    OR NOT EXISTS "${DARTWIC_SOURCE_ROOT}/sdk/dartwic-engine-protocol/CMakeLists.txt")
    message(FATAL_ERROR
        "Set DARTWIC_SOURCE_ROOT to a compatible DARTWIC source checkout. "
        "The public example plugin does not yet provide standalone TEMPEST::Peer "
        "and DARTWIC::EngineProtocol libraries for this bridge.")
endif()

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json" PLUGIN_MANIFEST_JSON)
string(JSON ENGINE_PLUGIN_ID GET "${PLUGIN_MANIFEST_JSON}" id)
string(JSON CONTAINS_ENGINE_PLUGIN GET "${PLUGIN_MANIFEST_JSON}" contains_engine_plugin)
if(NOT CONTAINS_ENGINE_PLUGIN)
    message(FATAL_ERROR "plugin.json must declare contains_engine_plugin=true")
endif()

add_subdirectory("${DARTWIC_SOURCE_ROOT}/tempest/peer" "${CMAKE_CURRENT_BINARY_DIR}/tempest-peer")
add_subdirectory("${DARTWIC_SOURCE_ROOT}/sdk/dartwic-engine-protocol" "${CMAKE_CURRENT_BINARY_DIR}/engine-protocol")
find_package(nlohmann_json CONFIG REQUIRED)

add_library(fprime_bridge SHARED
    engine/src/FprimeBridgePlugin.cpp
    engine/src/FprimeTransport.cpp
    engine/src/FprimeDictionary.cpp)
target_include_directories(fprime_bridge PRIVATE
    engine/include
    engine/include/sdk
    engine/include/sdk/modules
    engine/include/sdk/plugins
    engine/include/sdk/notifications)
target_link_libraries(fprime_bridge PRIVATE
    TEMPEST::Peer DARTWIC::EngineProtocol nlohmann_json::nlohmann_json)
if(WIN32)
    target_link_libraries(fprime_bridge PRIVATE ws2_32)
endif()

set(ENGINE_PLUGIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/plugin/$<IF:$<CONFIG:Debug>,engine-debug,engine>/${ENGINE_PLUGIN_ID}")
set_target_properties(fprime_bridge PROPERTIES
    OUTPUT_NAME "${ENGINE_PLUGIN_ID}"
    RUNTIME_OUTPUT_DIRECTORY "${ENGINE_PLUGIN_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${ENGINE_PLUGIN_DIR}/bin"
    PDB_OUTPUT_DIRECTORY "${ENGINE_PLUGIN_DIR}/bin")
add_custom_target(copy_engine_plugin ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ENGINE_PLUGIN_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json" "${ENGINE_PLUGIN_DIR}/plugin.json"
    DEPENDS fprime_bridge)

option(FPRIME_BRIDGE_WINDOWS_TEST "Build the native Windows CCSDS bridge test" ON)
set(FPRIME_BRIDGE_TEST_DICTIONARY "" CACHE FILEPATH "Generated F Prime topology dictionary for the native bridge test")
if(WIN32 AND FPRIME_BRIDGE_WINDOWS_TEST)
    enable_testing()
    add_executable(fprime_bridge_windows_test
        test/WindowsBridgeTest.cpp
        engine/src/FprimeTransport.cpp
        engine/src/FprimeDictionary.cpp)
    target_include_directories(fprime_bridge_windows_test PRIVATE engine/include)
    target_link_libraries(fprime_bridge_windows_test PRIVATE
        TEMPEST::Peer DARTWIC::EngineProtocol nlohmann_json::nlohmann_json ws2_32)
    if(FPRIME_BRIDGE_TEST_DICTIONARY)
        add_test(NAME fprime_bridge_windows
            COMMAND fprime_bridge_windows_test "${FPRIME_BRIDGE_TEST_DICTIONARY}")
        set_tests_properties(fprime_bridge_windows PROPERTIES TIMEOUT 20)
    endif()
endif()
