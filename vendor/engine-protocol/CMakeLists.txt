cmake_minimum_required(VERSION 3.20)
project(DARTWICEngineProtocol VERSION 2.0.0 LANGUAGES CXX)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
if(NOT TARGET TEMPEST::Messaging)
    find_package(TEMPESTMessaging CONFIG QUIET)
    if(NOT TARGET TEMPEST::Messaging)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/messaging/CMakeLists.txt")
            add_subdirectory(vendor/messaging ${CMAKE_CURRENT_BINARY_DIR}/messaging)
        else()
            add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../tempest/messaging ${CMAKE_CURRENT_BINARY_DIR}/messaging)
        endif()
    endif()
endif()
add_library(DARTWICEngineProtocol src/engine_contract.cpp src/engine_protocol.cpp)
add_library(DARTWIC::EngineProtocol ALIAS DARTWICEngineProtocol)
set_target_properties(DARTWICEngineProtocol PROPERTIES EXPORT_NAME EngineProtocol POSITION_INDEPENDENT_CODE ON)
target_compile_features(DARTWICEngineProtocol PUBLIC cxx_std_20)
target_include_directories(DARTWICEngineProtocol PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(DARTWICEngineProtocol PUBLIC TEMPEST::Messaging)
install(TARGETS DARTWICEngineProtocol EXPORT DARTWICEngineProtocolTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT DARTWICEngineProtocolTargets NAMESPACE DARTWIC:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/DARTWICEngineProtocol)
configure_package_config_file(cmake/DARTWICEngineProtocolConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/DARTWICEngineProtocolConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/DARTWICEngineProtocol)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/DARTWICEngineProtocolConfigVersion.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/DARTWICEngineProtocolConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/DARTWICEngineProtocolConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/DARTWICEngineProtocol)
