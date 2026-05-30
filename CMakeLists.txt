cmake_minimum_required(VERSION 3.16)

project(GChannelManager VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(GCHANNELMANAGER_BUILD_EXAMPLES "Build the GChannelManager demo executable" OFF)
option(GCHANNELMANAGER_BUILD_TESTS    "Build the GChannelManager unit tests"      OFF)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core)

set(GCM_PUBLIC_HEADERS
    include/GChannelManager/GChannelManager_global.h
    include/GChannelManager/DecodedMessage.h
    include/GChannelManager/RetryPolicy.h
    include/GChannelManager/KeepAliveConfig.h
    include/GChannelManager/ReplyCacheConfig.h
    include/GChannelManager/IMessageCodec.h
    include/GChannelManager/ITransport.h
    include/GChannelManager/GatewayRequest.h
    include/GChannelManager/GatewayStats.h
    include/GChannelManager/Gateway.h
    include/GChannelManager/SimpleFrameCodec.h
    include/GChannelManager/TransportConfig.h
)

set(GCM_SOURCES
    src/Gateway.cpp
    src/SimpleFrameCodec.cpp
    src/internal/PendingRequests.cpp
    src/internal/PendingRequests.h
    src/internal/KeepAliveMonitor.cpp
    src/internal/KeepAliveMonitor.h
    src/internal/SessionHandshake.cpp
    src/internal/SessionHandshake.h
    src/internal/TimerMs.h
)

add_library(GChannelManager SHARED
    ${GCM_PUBLIC_HEADERS}
    ${GCM_SOURCES}
)

set_target_properties(GChannelManager PROPERTIES
    VERSION   ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

target_include_directories(GChannelManager
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(GChannelManager
    PUBLIC
        Qt${QT_VERSION_MAJOR}::Core
)

target_compile_definitions(GChannelManager PRIVATE GCHANNELMANAGER_LIBRARY)

# ---------------------------------------------------------------------
#  Install / export — lets downstreams consume the library via
#  find_package(GChannelManager) and link GChannelManager::GChannelManager.
# ---------------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS GChannelManager
    EXPORT GChannelManagerTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY include/GChannelManager
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h"
)

install(EXPORT GChannelManagerTargets
    FILE      GChannelManagerTargets.cmake
    NAMESPACE GChannelManager::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/GChannelManager
)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/GChannelManagerConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/GChannelManagerConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/GChannelManager
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/GChannelManagerConfigVersion.cmake
    VERSION       ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/GChannelManagerConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/GChannelManagerConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/GChannelManager
)

if(GCHANNELMANAGER_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(GCHANNELMANAGER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
