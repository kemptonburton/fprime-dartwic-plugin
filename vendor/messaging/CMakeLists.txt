cmake_minimum_required(VERSION 3.20)
project(TEMPESTMessaging VERSION 1.0.0 LANGUAGES CXX)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
find_package(Threads REQUIRED)
add_library(TEMPESTMessaging src/peer.cpp)
add_library(TEMPEST::Messaging ALIAS TEMPESTMessaging)
set_target_properties(TEMPESTMessaging PROPERTIES EXPORT_NAME Messaging POSITION_INDEPENDENT_CODE ON)
target_compile_features(TEMPESTMessaging PUBLIC cxx_std_20)
target_include_directories(TEMPESTMessaging PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(TEMPESTMessaging PUBLIC Threads::Threads)
install(TARGETS TEMPESTMessaging EXPORT TEMPESTMessagingTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT TEMPESTMessagingTargets NAMESPACE TEMPEST:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TEMPESTMessaging)
configure_package_config_file(cmake/TEMPESTMessagingConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/TEMPESTMessagingConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TEMPESTMessaging)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/TEMPESTMessagingConfigVersion.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/TEMPESTMessagingConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/TEMPESTMessagingConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TEMPESTMessaging)
if(PROJECT_IS_TOP_LEVEL)
    include(CTest)
endif()
if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/peer_test.cpp")
    add_executable(TEMPESTMessaging_Test tests/peer_test.cpp)
    target_link_libraries(TEMPESTMessaging_Test PRIVATE TEMPEST::Messaging)
    add_test(NAME TEMPESTMessaging_Test COMMAND TEMPESTMessaging_Test)
    set_tests_properties(TEMPESTMessaging_Test PROPERTIES TIMEOUT 30)
endif()
