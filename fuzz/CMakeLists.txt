# Fuzz targets — built only when GCHANNELMANAGER_BUILD_FUZZERS=ON, and only
# under Clang (libFuzzer ships with LLVM). Off by default, so the normal build
# stays Qt-Core-only and dependency-free.
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR
        "GCHANNELMANAGER_BUILD_FUZZERS requires Clang (libFuzzer). "
        "Re-run with -DCMAKE_CXX_COMPILER=clang++ or turn the option OFF.")
endif()

set(GCM_FUZZ_FLAGS -fsanitize=fuzzer,address,undefined -g -O1)

# Compile the codec translation unit directly into the target (instead of
# linking the shared library) so the sanitizers instrument SimpleFrameCodec.cpp
# itself — that is where the parser logic under test lives.
add_executable(fuzz_codec
    fuzz_codec.cpp
    ${CMAKE_SOURCE_DIR}/src/SimpleFrameCodec.cpp
)
target_include_directories(fuzz_codec PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(fuzz_codec PRIVATE Qt${QT_VERSION_MAJOR}::Core)
target_compile_options(fuzz_codec PRIVATE ${GCM_FUZZ_FLAGS})
target_link_options(fuzz_codec PRIVATE ${GCM_FUZZ_FLAGS})
