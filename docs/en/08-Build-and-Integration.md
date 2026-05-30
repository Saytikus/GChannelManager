# Build and integration

> 🌐 **English** | [Русский](../ru/08-Сборка-и-интеграция.md)

## Requirements

| Component | Minimum |
|---|---|
| CMake | 3.16 |
| Compiler | C++20 (GCC 10+, Clang 11+, MSVC 19.29+) |
| Qt | 6 (Core); Qt5 supported via `find_package(QT NAMES Qt6 Qt5)` |
| Build tool | Ninja / Make / MSBuild |
| Qt Test | only for `GCHANNELMANAGER_BUILD_TESTS=ON` |

## CMake options

```cmake
option(GCHANNELMANAGER_BUILD_EXAMPLES "Build the GChannelManager demo executable" OFF)
option(GCHANNELMANAGER_BUILD_TESTS    "Build the GChannelManager unit tests"      OFF)
```

| Option | Default | Effect |
|---|---|---|
| `GCHANNELMANAGER_BUILD_EXAMPLES` | `OFF` | Builds `examples/GChannelManagerDemo` — a loopback demo with losses and retries |
| `GCHANNELMANAGER_BUILD_TESTS` | `OFF` | Builds `tests/tst_SimpleFrameCodec` and `tests/tst_Gateway`, registers them with `ctest` |

## Building from the command line

```sh
# a clean development build with tests and the demo
cmake -S . -B build/Desktop-Debug \
    -DCMAKE_BUILD_TYPE=Debug \
    -DGCHANNELMANAGER_BUILD_TESTS=ON \
    -DGCHANNELMANAGER_BUILD_EXAMPLES=ON

cmake --build build/Desktop-Debug

# run the tests
( cd build/Desktop-Debug && LD_LIBRARY_PATH="$PWD" ctest --output-on-failure )
```

## Building via Qt Creator

`CMakeLists.txt` is discovered by the standard `Open Project…`. By default Qt Creator places the build in `build/Desktop-Debug/`. The `GCHANNELMANAGER_BUILD_*` options are available in `Projects → Build → Initial CMake Configuration` or via the `Initial Parameters` dialog.

## Using it as a dependency

### Option 1: `add_subdirectory` (mono-repo, vendored)

The simplest way: drop `GChannelManager` into a subdirectory and link it.

```cmake
# in your CMakeLists.txt
add_subdirectory(third_party/GChannelManager)

add_executable(myapp src/main.cpp)
target_link_libraries(myapp PRIVATE GChannelManager)
```

The library's `target_include_directories` has a `PUBLIC` scope, so the headers are available automatically:

```cpp
#include <GChannelManager/Gateway.h>
#include <GChannelManager/SimpleFrameCodec.h>
```

### Option 2: `find_package` (installed package)

The library ships install/export rules, so you can install it and consume it with `find_package`:

```cmake
find_package(GChannelManager REQUIRED)

add_executable(myapp src/main.cpp)
target_link_libraries(myapp PRIVATE GChannelManager::GChannelManager)
```

See [Installing](#installing) for how to produce the package.

## Artifact layout

After building you get:

| Artifact | Where |
|---|---|
| `libGChannelManager.so` (`.dll`/`.dylib`) | `build/Desktop-Debug/` |
| `GChannelManagerDemo` (if `BUILD_EXAMPLES=ON`) | `build/Desktop-Debug/examples/` |
| `tst_SimpleFrameCodec`, `tst_Gateway` (if `BUILD_TESTS=ON`) | `build/Desktop-Debug/tests/` |

## The `GCHANNELMANAGER_EXPORT` macro

```cpp
// include/GChannelManager/GChannelManager_global.h
#if defined(GCHANNELMANAGER_LIBRARY)
#define GCHANNELMANAGER_EXPORT Q_DECL_EXPORT
#else
#define GCHANNELMANAGER_EXPORT Q_DECL_IMPORT
#endif
```

All exported classes are marked with this macro: `Gateway`, `GatewayRequest`, `ITransport`, `SimpleFrameCodec`. When building the `.so` itself, `GCHANNELMANAGER_LIBRARY` is defined (via `target_compile_definitions(... PRIVATE ...)`); when included from an external project, the macro resolves to `Q_DECL_IMPORT`.

> [!WARNING] Windows
> On MSVC symbols are **not** exported by default. If you add a new public class — be sure to put `GCHANNELMANAGER_EXPORT` before its name, otherwise it will not be visible from the application using the DLL.

## A minimal consumer `CMakeLists.txt`

```cmake
cmake_minimum_required(VERSION 3.16)
project(MyApp LANGUAGES CXX)

set(CMAKE_AUTOMOC ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core)

add_subdirectory(external/GChannelManager)

add_executable(myapp src/main.cpp)
target_link_libraries(myapp PRIVATE
    GChannelManager
    Qt${QT_VERSION_MAJOR}::Core
)
```

## Installing

The library installs the versioned `.so`, the public headers, and a CMake
package (targets + config + version files), so a downstream `find_package`
works out of the box:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix /your/prefix
```

This produces, under the prefix:

| Artifact | Location |
|---|---|
| `libGChannelManager.so.0.1.0` (+ `.so.0`, `.so` symlinks) | `lib/` |
| public headers | `include/GChannelManager/` |
| `GChannelManagerConfig.cmake`, `…ConfigVersion.cmake`, `…Targets.cmake` | `lib/cmake/GChannelManager/` |

Consumers then point CMake at the prefix (`-DCMAKE_PREFIX_PATH=/your/prefix`)
and use `find_package(GChannelManager REQUIRED)` as shown above. The exported
target is `GChannelManager::GChannelManager`; its config re-finds `Qt::Core` as
a dependency. The package version follows the project version
(`SameMajorVersion` compatibility).
