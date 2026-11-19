# ADR-0048 · SDK 包版本独立于软件版本

- 状态：已采纳
- 日期：2026-11-20
- 影响：`sdk/fgb_sdk_version.h`（新增，唯一版本来源）、`sdk/CMakeLists.txt`、
  `packaging/pack_sdk.ps1`、`packaging/release.ps1`、`.github/workflows/release.yml`、
  `sdk/README.md`
- 关系：不改 [ADR-0034](0034-native-module-v2-only.md) 的"只有一套 ABI"；
  `MinHostVersion` 仍然比较宿主版本（[ADR-0035](0035-native-module-registry.md)）

## 问题

SDK 压缩包直接复用软件版本：`release.yml` 从 `CMakeLists.txt` 读 `project(VERSION)`，
强制等于 git tag，再把这个值传给 `pack_sdk.ps1` 拼出 `FaceGazeBridge-SDK-<版本>.zip`。

结果是**软件每次发版，SDK 包名都跟着变**，哪怕 `sdk/` 一个字节都没改：

- 模块作者无法从版本号判断"我需不需要跟进"，只能重新下载、diff 头文件
- 同一个 SDK 内容在不同 Release 上有不同文件名，发布说明里也只能重复同一个版本号
- SDK 包里没有任何版本标识（`install(DIRECTORY sdk/)` 是整目录拷贝，
  `sdk/CMakeLists.txt` 的 `project()` 又故意不带 `VERSION`），只能靠文件名猜

## 决定

### 1. 唯一来源是 `sdk/fgb_sdk_version.h`

版本号写在 SDK 目录内部，随目录一起分发（`add_subdirectory` / FetchContent / 解压 zip
都能读到），模块作者不跑 CMake 也能 `#include` 到：

```c
#define FGB_SDK_VERSION_MAJOR 1
#define FGB_SDK_VERSION_MINOR 2
#define FGB_SDK_VERSION_PATCH 0
#define FGB_SDK_VERSION "1.2.0"
```

`sdk/CMakeLists.txt` 与 `packaging/pack_sdk.ps1` 都解析这个头文件，并且校验
`MAJOR/MINOR/PATCH` 与字符串一致；不另建 `VERSION` 文件或（容易漂移的）CMake 变量。

### 2. 产物命名分开，tag 规则不变

- 安装包：`FaceGazeBridge-Setup-<软件版本>.exe`，仍要求 `v<软件版本>` = tag
- SDK 包：`FaceGazeBridge-SDK-<SDK 版本>.zip`，**不再要求等于 tag**

同一个 `FaceGazeBridge-SDK-1.2.0.zip` 允许出现在后续多个软件 Release 上。

### 3. CI 的三条守卫

`release.yml` 的 "Check version matches tag" 步骤额外做：

1. 从 `sdk/fgb_sdk_version.h` 解析 `SDK_VERSION` 写入 `GITHUB_ENV`
2. 对比上一 tag：`sdk/`（除版本头本身）有改动却**没 bump** SDK 版本 → 直接失败，
   避免"同版本不同内容"
3. 打包前若上一版 Release 已有同名 SDK 资产，直接 `gh release download` 复用，
   保证字节一致（重新压缩会换哈希）；失败时才重新打包

本地 `packaging/release.ps1` 同步支持 `-SdkVersion`，缺省解析同一个头文件。

### 4. 三类版本必须分清

| 版本 | 位置 | 变化时机 |
|---|---|---|
| 软件 / 宿主版本 | `CMakeLists.txt` 的 `project(VERSION)`、tag `v<版本>` | 每次软件发版 |
| SDK 包版本 | `sdk/fgb_sdk_version.h` | 只有 ABI 头、文档、示例变化时 |
| ABI 版本 | `fgb_module_api.h` 的 `FGB_MODULE_ABI_V2` | 结构体布局/导出符号变化时 |

模块清单的 `MinHostVersion` 比较的是**宿主版本**（`module_registry.cpp` 里的
`FGB_VERSION`），不是 SDK 包版本，因此本次不改运行时判断。

## 备选（未采纳）

- **`sdk-v*` 独立 tag + 独立流水线**：能"只发 SDK 不发软件"，但当前痛点是包名乱变，
  不是要单独发版；多一套 tag 反而增加"SDK 版本 ↔ 软件版本"的映射成本。若以后确实
  需要在两次软件发版之间单独更新 SDK，再在现有基础上加一条只跑 `pack_sdk.ps1` 的
  workflow 即可，版本来源不用改。
- **用 `SOVERSION`**：SDK 与所有库都是 `INTERFACE`/`STATIC`，没有共享库版本概念，
  CMake 的 target 版本机制用不上。
- **直接用 ABI 版本当包版本**：ABI v2 只表达二进制契约；文档修正、示例新增同样会
  影响 SDK 使用者，需要比 ABI 更细的粒度。

## 验证

- `cmake -S sdk -B build/sdk-version-check -G "Visual Studio 17 2022" -A x64`：
  `CMakeCache.txt` 里 `CMAKE_PROJECT_VERSION=1.2.0`；改坏任一宏会 `FATAL_ERROR`
- `pack_sdk.ps1` 在模拟 `dist/sdk` 上产出 `FaceGazeBridge-SDK-1.2.0.zip`，
  包内包含 `fgb_sdk_version.h`；传 `-SdkVersion 9.9.9` 会因与包头不一致报错
- 主工程 Debug 构建 + 357 个单测全绿（改的是发布脚本与文档，不涉及运行时）

## 遗留

- 本地 `release.ps1` 不做"复用旧资产"（本地一般就是要重新出包）；用户以 Release
  资产与 `SHA256SUMS.txt` 为准。
- SDK 版本未变时 CI 复用的压缩包里，示例 DLL 来自上一版构建。它只是示例产物，
  源码与头文件与本版一致；等 `sdk/` 有实际改动时会自然 bump SDK 版本并重新构建。
