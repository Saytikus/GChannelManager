add_executable(GChannelManagerDemo demo_peer.cpp)

target_link_libraries(GChannelManagerDemo
    PRIVATE
        GChannelManager
        Qt${QT_VERSION_MAJOR}::Core
)
