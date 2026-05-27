find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Test)

# ----- codec tests -----
add_executable(tst_SimpleFrameCodec tst_SimpleFrameCodec.cpp)
target_link_libraries(tst_SimpleFrameCodec
    PRIVATE
        GChannelManager
        Qt${QT_VERSION_MAJOR}::Test
)
add_test(NAME SimpleFrameCodec COMMAND tst_SimpleFrameCodec)

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
