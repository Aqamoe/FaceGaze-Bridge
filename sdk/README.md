# FaceGaze Bridge 原生模块 SDK

本目录自包含：ABI 头文件、集成文档和可直接构建的示例。SDK 有**独立的包版本**
（[`fgb_sdk_version.h`](fgb_sdk_version.h)），不跟随软件版本：软件发版不会改它，
只有头文件、文档、示例变化时才 bump（[ADR-0048](../docs/adr/0048-sdk-independent-version.md)）。

## 目录

| 位置 | 内容 |
|---|---|
| [`fgb_module_api.h`](fgb_module_api.h) | 原生输入模块 ABI（v2，唯一需要分发的头） |
| [`fgb_native_host_shm.h`](fgb_native_host_shm.h) | 模块宿主进程的共享内存布局（排查/二次开发） |
| [`fgb_shared_frame.h`](fgb_shared_frame.h) | 共享内存输出布局（Unity/UE/自研工具读取） |
| [`fgb_udp_frame.h`](fgb_udp_frame.h) | UDP 二进制输出布局（固定 656 字节帧） |
| [`fgb_sdk_version.h`](fgb_sdk_version.h) | SDK 包版本（独立于软件版本；需要打印版本时 include 这个） |
| [`docs/native-module-guide.md`](docs/native-module-guide.md) | 原生模块开发指南（从零到可安装的 zip） |
| [`docs/shared-memory.md`](docs/shared-memory.md) | 共享内存读取指南 |
| [`docs/streaming-outputs.md`](docs/streaming-outputs.md) | WebSocket JSON / UDP 二进制协议与读取示例 |
| [`docs/gaze-service.md`](docs/gaze-service.md) | `/api/gaze` 与 WS `gaze` 的字段和边界 |
| [`samples/`](samples) | 四个无硬件的原生模块示例 + Unity UPM 示例包 |
| [`CMakeLists.txt`](CMakeLists.txt) | `fgb::sdk` INTERFACE 目标 |

## 版本

三类版本不要混：

| 版本 | 位置 | 什么时候变 |
|---|---|---|
| 软件 / 宿主版本 | Release 的 tag `v<版本>`、`CMakeLists.txt` | 每次软件发版 |
| **SDK 包版本** | [`fgb_sdk_version.h`](fgb_sdk_version.h) | 只有 ABI 头、文档、示例变化时才 bump |
| ABI 版本 | `fgb_module_api.h` 的 `FGB_MODULE_ABI_V2` | 结构体布局或导出符号变化时才动 |

Release 里的 SDK 压缩包按 **SDK 包版本**命名，所以同一个 `FaceGazeBridge-SDK-1.2.0.zip`
可能出现在软件 1.2.0、1.3.0、1.4.0 三个 Release 上——内容一致，不需要重新下载。

模块清单里的 `MinHostVersion` 比较的是**宿主版本**，不是 SDK 版本。

需要打印版本时单独 include（ABI 头保持单文件可用，不强依赖它）：

```c
#include "fgb_sdk_version.h"
/* FGB_SDK_VERSION -> "1.2.0" */
```

## 在模块工程里引用

SDK 的 ABI 由结构体布局和导出符号定义，**请固定在 tag 上**，不要跟随 `main`：
版本升级时对比 [`docs/native-module-guide.md`](docs/native-module-guide.md) 后再升。
固定的 tag 是**软件版本**；头文件本身的版本看 [`fgb_sdk_version.h`](fgb_sdk_version.h)。

### Git submodule + CMake

```bash
git submodule add https://github.com/Aqamoe/FaceGaze-Bridge.git external/facegaze-bridge
git -C external/facegaze-bridge checkout v1.2.0
```

```cmake
add_subdirectory(external/facegaze-bridge/sdk)
target_link_libraries(my-module PRIVATE fgb::sdk)
```

同步新版本：改 submodule 指向的 tag，然后 `git submodule update --init`。

### CMake FetchContent

```cmake
include(FetchContent)
FetchContent_Declare(facegaze_sdk
    GIT_REPOSITORY https://github.com/Aqamoe/FaceGaze-Bridge.git
    GIT_TAG v1.2.0
    SOURCE_SUBDIR sdk)
FetchContent_MakeAvailable(facegaze_sdk)
target_link_libraries(my-module PRIVATE fgb::sdk)
```

### 不用 CMake

把本目录加入头文件搜索路径（例如 MSVC `/I <sdk>`），`#include "fgb_module_api.h"` 即可。

### 构建示例

```powershell
# 单个示例（在 SDK 根目录执行）
cmake -S samples/native-sine -B build/native-sample -G "Visual Studio 17 2022" -A x64
cmake --build build/native-sample --config Release

# 一次构建全部四个
cmake -S . -B build/sdk -G "Visual Studio 17 2022" -A x64 -DFGB_SDK_BUILD_SAMPLES=ON
cmake --build build/sdk --config Release
```

## 发布包

每个 [Release](https://github.com/Aqamoe/FaceGaze-Bridge/releases) 除安装包外还附带
`FaceGazeBridge-SDK-<SDK 版本>.zip`（版本见 [`fgb_sdk_version.h`](fgb_sdk_version.h)），
内容与本目录一致（ABI 头、docs/、samples/、LICENSE/NOTICE）。**文件名的 SDK 版本与
Release 的软件 tag 可能不同**：只更新软件时 SDK 压缩包沿用同一个版本。解压后的
`FaceGazeBridge-SDK-<SDK 版本>/` 就是 SDK 根目录，可直接 `add_subdirectory`，
也可作为 FetchContent 的源码目录：

```cmake
include(FetchContent)
FetchContent_Declare(facegaze_sdk
    # tag 取软件版本，文件名取 SDK 版本，两者不一定相同
    URL https://github.com/Aqamoe/FaceGaze-Bridge/releases/download/v1.2.0/FaceGazeBridge-SDK-1.2.0.zip
    # 建议固定哈希：取 Release 的 SHA256SUMS.txt
    URL_HASH SHA256=<sha256>)
FetchContent_MakeAvailable(facegaze_sdk)
```

压缩包的哈希随安装包一起写在 Release 的 `SHA256SUMS.txt` 里。

## 许可

Apache-2.0，见仓库根目录 [`LICENSE`](../LICENSE) 与 [`NOTICE`](../NOTICE)。
发布包的 `sdk/` 目录内也各带一份。
