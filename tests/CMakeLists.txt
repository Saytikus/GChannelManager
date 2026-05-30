find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Test)

# ----- codec tests -----
add_executable(tst_SimpleFrameCodec tst_SimpleFrameCodec.cpp)
target_link_libraries(tst_SimpleFrameCodec
    PRIVATE
        GChannelManager
        Qt${QT_VERSION_MAJOR}::Test
)
add_test(NAME SimpleFrameCodec COMMAND tst_SimpleFrameCodec)

# ----- codec property tests (seeded, deterministic) -----
add_executable(tst_CodecProperties tst_CodecProperties.cpp)
target_link_libraries(tst_CodecProperties
    PRIVATE
        GChannelManager
        Qt${QT_VERSION_MAJOR}::Test
)
add_test(NAME CodecProperties COMMAND tst_CodecProperties)

# ----- gateway tests -----
add_executable(tst_Gateway
    tst_Gateway.cpp
    FakeTransport.h
)
target_link_libraries(tst_Gateway
    PRIVATE
        GChannelManager
        Qt${QT_VERSION_MAJOR}::Test
)
add_test(NAME Gateway COMMAND tst_Gateway)

# ----- integration tests (two gateways over a linked in-memory transport) -----
add_executable(tst_Integration tst_Integration.cpp)
target_link_libraries(tst_Integration
    PRIVATE
        GChannelManager
        Qt${QT_VERSION_MAJOR}::Test
)
add_test(NAME Integration COMMAND tst_Integration)
