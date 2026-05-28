cmake_minimum_required(VERSION 3.16)

project(GChannelManager LANGUAGES CXX)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(GCHANNELMANAGER_BUILD_EXAMPLES "Build the GChannelManager demo executable" OFF)
option(GCHANNELMANAGER_BUILD_TESTS    "Build the GChannelManager unit tests"      OFF)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Network)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Network)

set(GCM_PUBLIC_HEADERS
    include/GChannelManager/GChannelManager_global.h
    include/GChannelManager/GChannelManager.h
    include/GChannelManager/DecodedMessage.h
    include/GChannelManager/RetryPolicy.h
    include/GChannelManager/KeepAliveConfig.h
    include/GChannelManager/ReplyCacheConfig.h
    include/GChannelManager/IMessageCodec.h
    include/GChannelManager/ITransport.h
    include/GChannelManager/GatewayRequest.h
    include/GChannelManager/Gateway.h
    include/GChannelManager/SimpleFrameCodec.h
    include/GChannelManager/TransportConfig.h
)

set(GCM_SOURCES
    src/GChannelManager.cpp
    src/Gateway.cpp
    src/SimpleFrameCodec.cpp
    src/internal/PendingRequests.cpp
    src/internal/PendingRequests.h
    src/internal/KeepAliveMonitor.cpp
    src/internal/KeepAliveMonitor.h
    src/internal/SessionHandshake.cpp
    src/internal/SessionHandshake.h
)

add_library(GChannelManager SHARED
    ${GCM_PUBLIC_HEADERS}
    ${GCM_SOURCES}
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
        Qt${QT_VERSION_MAJOR}::Network
)

target_compile_definitions(GChannelManager PRIVATE GCHANNELMANAGER_LIBRARY)

if(GCHANNELMANAGER_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(GCHANNELMANAGER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
