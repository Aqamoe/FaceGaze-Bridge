<div align="right">
  <img src="ui/logo.png" width="96" alt="FaceGaze Bridge">
</div>

# FaceGaze Bridge

[![CI](https://github.com/Aqamoe/FaceGaze-Bridge/actions/workflows/ci.yml/badge.svg)](https://github.com/Aqamoe/FaceGaze-Bridge/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/Aqamoe/FaceGaze-Bridge)](https://github.com/Aqamoe/FaceGaze-Bridge/releases)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)

**把眼追 / 面捕 / 注视点数据，送到任何需要它的地方。**

FaceGaze Bridge（FGB）是一个 Windows 上的开源追踪数据桥接层。它兼容 VRCFT 模块生态，
把表情、眼追和头姿同时输出给 VRChat OSC、VMC、共享内存、WebSocket 和 UDP，
并提供本机注视点 API。核心是 C++，界面是本地 WebView2 页面，不依赖云服务。

> **English:** FaceGaze Bridge is an open-source Windows bridge for eye, face and gaze
> tracking. It loads VRCFT-compatible modules and streams the merged result to VRChat OSC,
> VMC, shared memory, WebSocket and UDP, with a local WebView2 UI and a loopback gaze API.
> Licensed under Apache-2.0. Author: [Aqamoe (光水)](https://github.com/Aqamoe).

## 为什么需要它

VRCFT 解决的是「我的面捕硬件怎么驱动 VRChat 头像」。FGB 解决的是
「同一份追踪数据，怎么同时驱动头像、Unity 场景、串流软件和自研应用」。

- 模块兼容：直接驱动官方 `VRCFaceTracking.ModuleProcess.exe`，现有 .NET 模块零改动可用
- 多目标：一张脸可以同时写 VRChat、VMC、共享内存、WebSocket 和 UDP
- 可集成：原生 C ABI、共享内存布局、JSON/UDP 二进制协议都有文档和示例
- 本地优先：控制面只绑回环，无遥测、无账号、无云端依赖

> FGB 是独立实现，与 VRCFaceTracking 项目没有隶属或背书关系。
> “VRCFaceTracking / VRCFT” 仅用于事实性描述兼容性。

## 特性

| 能力 | 说明 |
|---|---|
| VRCFT 模块兼容 | 自带精简模块宿主运行时，卸载 VRCFT 后仍可加载模块；支持导入本机模块、`.zip` 安装、启停 |
| 原生 C ABI | 单一 ABI（`fgb_module_query_v2`）、POD 属性帧、能力位分通道；附带不需要硬件的示例模块，见 [SDK 开发指南](sdk/docs/native-module-guide.md) |
| 多源合并 | 表情 / 眼睛 / 渲染注视 / 头姿可分别选择提供者，支持 VMC、模块、抓包回放同时输入；可组合多个原生模块，左右眼/左右注视分源，模块可只给 openness 或单眼属性，`--attr-source` 支持逐属性有序源；`--native-host` 可把原生模块放进独立宿主进程，崩溃自动重启 |
| 直连输入 | VMC、ARKit LiveLink（iPhone，UDP 11111）、通用 OSC 映射表（任意发送端） |
| 多路输出 | VRChat OSC、VMC、命名共享内存、WebSocket JSON、UDP 二进制 |
| 注视点服务 | `GET /api/gaze` 与 30 Hz WS 推送：注视方向、扫视/注视状态、聚散、头姿 |
| 头像发现 | OSCQuery（mDNS）自动读取当前头像参数表，换头像自动重绑；可回退本地配置 |
| 校准与滤波 | 88 个形状的 lo/hi、视线 gain/center、3D 预览、One Euro 滤波、命名输出预设 |
| 桌面体验 | 单入口 exe、托盘常驻、关窗继续追踪、可选随 SteamVR 启动、三语界面 |
| 诊断 | tick 延迟/CPU/内存/句柄、日志、崩溃转储、一键诊断包、手动检查更新 |

## 当前状态

最新版本 **1.0.3**。Windows 10/11 x64 上已完成构建、单测、端到端探针、
安装/卸载和真机 PICO 链路验证；发布门禁与未完成项记录在
[docs/11-regression-2026-10-30.md](docs/11-regression-2026-10-30.md)。

已知限制：

- 真机只验证过 PICO + PICO Connect；Quest / iPhone 等模块缺硬件验证
- ARKit LiveLink 直连已实现但未在 iPhone 真机验收（头姿单位/符号为推导值）
- 通用 OSC 输入目前按 `osc_map.ini` 文件配置，界面表格编辑器待做
- 3D 校准预览目前是程序化占位头模，正式美术资产待补
- SteamVR 自启动的注册分支已实现，真实 SteamVR 环境验收待补
- 8 小时长跑、休眠唤醒、设备热插拔仍属于发布前门禁

## 安装

### 安装版（推荐）

从 [Releases](https://github.com/Aqamoe/FaceGaze-Bridge/releases) 下载
`FaceGazeBridge-Setup-<版本>.exe`：

- 用户级安装，**不需要管理员**，默认装到 `%LOCALAPPDATA%\Programs\FaceGazeBridge`
- 创建开始菜单快捷方式，可选桌面图标
- 静默安装：`FaceGazeBridge-Setup-<版本>.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART`
- 卸载：Windows 设置 → 应用 → FaceGaze Bridge；交互式卸载会询问是否删除用户数据

### 绿色版

发布包目录即绿色版，解压到**固定目录**后双击 `FaceGaze Bridge.exe`：

```
FaceGaze Bridge.exe    唯一入口（窗口 + 拉起引擎）
fgb-cli.exe            引擎与命令行工具
fgb-host-native.exe    原生模块跨进程宿主（--native-host，可选）
runtime\vrcft\         VRCFT 模块宿主运行时
ui\                    界面资源
sdk\                   集成文档与示例
NOTICE, LICENSE        许可证与第三方声明
```

移动目录后需要重新注册 SteamVR 启动项。

### 前置条件

- Windows 10 1809+ / Windows 11，x64
- **WebView2 Runtime**（Win10/11 通常随 Edge 预装）
- 用 VRChat OSC 输出时，需要在游戏内 `Options → OSC` 打开开关
- 使用 VRCFT 模块时，先完全退出 VRCFT（模块沙箱端口是独占的）
- VC++ 运行库已随安装包提供，无需另装

### 第一次运行

1. 双击 `FaceGaze Bridge.exe`，窗口会自动拉起引擎
2. 到「模块」页导入本机 VRCFT 模块，或从「模块库」安装一个
3. 到「输出」页确认 VRChat OSC / VMC / 共享内存等目标
4. 关窗只是收进托盘，追踪继续；托盘菜单「退出」才停止引擎

安装包与 SDK 包（`FaceGazeBridge-SDK-<版本>.zip`）的 SHA256 在 Release 的
`SHA256SUMS.txt` 中。

## 快速开始

界面之外，引擎也可以直接用：

```powershell
# 用模块作为输入，输出 VRChat OSC，并启动控制面
fgb-cli run --source vrcft --vrchat 127.0.0.1:9000 --panel 8787

# 纯 VMC 输入（VSeeFace 等），同时再输出一路 VMC 给 Unity
fgb-cli run --source vmc --vmc-out 127.0.0.1:9100

# iPhone Live Link Face 直连，或任意 OSC 发送端（地址→形状映射表）
fgb-cli run --source livelink --panel 8787
fgb-cli run --source osc --osc-map .\osc_map.ini --panel 8787

# 给网页/脚本的 WebSocket JSON，给串流软件的 UDP 二进制
fgb-cli run --source vmc --ws-out 9101 --udp-out 127.0.0.1:9102

# 模块管理
fgb-cli modules
fgb-cli modules import-all
fgb-cli modules install module.zip
fgb-cli modules use VRCFTPicoModule
```

全部参数：`fgb-cli run --help`。

## 输出与集成

| 输出 | 默认 | 协议 / 文档 |
|---|---|---|
| VRChat OSC | 开 | `/avatar/parameters/*`，地址来自当前头像参数表 |
| VMC | 关 | `/VMC/Ext/Blend/Val` + `Apply`，`--vmc-out host:port` |
| 共享内存 | 关 | `Local\FGB_Shm_<name>`，[SDK · 共享内存](sdk/docs/shared-memory.md) |
| WebSocket JSON | 关 | `ws://127.0.0.1:<port>/ws`，[SDK · 流式输出](sdk/docs/streaming-outputs.md) |
| UDP 二进制 | 关 | 固定 656 字节帧，[SDK · 流式输出](sdk/docs/streaming-outputs.md) |
| 注视点 | 开 | `GET /api/gaze` + WS `gaze`，[SDK · 注视服务](sdk/docs/gaze-service.md) |

原生模块接口见 [SDK 开发指南](sdk/docs/native-module-guide.md)，
Unity 读取示例见 [sdk/samples/unity-face-gaze](sdk/samples/unity-face-gaze)。
`sdk/` 目录自包含（头文件 + 文档 + 示例），模块工程可直接按
[SDK README](sdk/README.md) 引用。

## 从源码构建

### 依赖

- Windows 10/11 x64
- CMake 3.24+
- Visual Studio 2022，含「使用 C++ 的桌面开发」和 Windows SDK
- Python 3（可选：抽取 VRCFT 运行时、跑探针）
- Inno Setup 6（可选：生成安装包）

WebView2 SDK 会在 configure 时从 NuGet 下载一次；也可用
`-DFGB_WEBVIEW2_DIR=<已解压目录>` 指向本地副本，或 `-DFGB_BUILD_UI=OFF` 只编引擎。

### 构建与测试

本机与 CI 用同一个 preset（VS2022 x64，警告即错误）：

```powershell
cmake --preset windows-vs2022
cmake --build --preset windows-vs2022 --parallel        # Debug：可运行的 fgb-tests.exe
./build/windows-vs2022/bin/Debug/fgb-tests.exe          # 或 ctest --test-dir build/windows-vs2022 -C Debug --output-on-failure
```

产物在 `build/windows-vs2022/bin/<Debug|Release>/`；出发布包用
`cmake --build build/windows-vs2022 --config Release`。

### 打包

```powershell
# 完整发布：构建 → 单测 → dist → VRCFT 运行时 → VC 运行库 → Setup → 校验和
powershell -ExecutionPolicy Bypass -File packaging\release.ps1

# 同时创建 draft Release（需要 gh 已登录）
powershell -ExecutionPolicy Bypass -File packaging\release.ps1 -Publish
```

`release.ps1` 默认从本机已安装的 VRCFT 抽取运行时；CI 用 `-RuntimeZip` 或
`VRCFT_RUNTIME_URL` 提供运行时包。说明见
[docs/04-engineering.md](docs/04-engineering.md#61-实际发布流水线)。

## 项目结构

```
src/base      日志、时钟、崩溃与诊断
src/core      数据模型、无锁容器、滤波/校准
src/osc       OSC 编解码
src/net       UDP/TCP/WebSocket 服务端、HTTP 下载
src/sources   输入：VMC / ARKit LiveLink / 通用 OSC / VRCFT 模块 / 原生模块 / 回放
src/sinks     输出：VRChat / VMC / 共享内存 / WS / UDP
src/engine    合并、快照发布、扇出
src/web       控制面 IPC 与静态页面
src/ui        FaceGaze Bridge.exe（WebView2 外壳 + 引擎生命周期）
src/cli       fgb-cli
ui/           界面资源（无构建步骤）
sdk/          原生模块 SDK（ABI 头、集成文档、示例，可被模块工程直接引用）
packaging/    安装包、发布脚本、Windows 资源
tools/        冒烟、探针、运行时抽取等辅助脚本
docs/         架构、ADR、路线图、用户手册、安全审查
tests/        单元测试与金样测试
```

## 文档

| 文档 | 内容 |
|---|---|
| [用户手册](docs/09-user-guide.md) | 安装、界面、输出、校准、FAQ、排障 |
| [架构](docs/01-architecture.md) | 进程/线程模型、模块系统、数据流 |
| [功能规格](docs/02-features.md) | 对等功能与增值能力边界 |
| [路线图](docs/03-roadmap.md) | 里程碑、验收标准、发布门禁 |
| [工程手册](docs/04-engineering.md) | 构建、测试、CI、发布、许可证合规 |
| [原生模块](sdk/docs/native-module-guide.md) | C ABI 生命周期与打包 |
| [共享内存](sdk/docs/shared-memory.md) / [流式输出](sdk/docs/streaming-outputs.md) / [注视服务](sdk/docs/gaze-service.md) | 集成协议 |
| [安全审查](docs/10-security-review.md) | 控制面、模块、配置的威胁模型 |
| [ADR](docs/adr/) | 关键决策记录 |

## 安全与隐私

- 不收集遥测，不联网（检查更新需要你手动点击；模块库和模块下载走你选择的来源）
- 控制面默认只绑 `127.0.0.1`，没有账号体系；本机进程都可以调用
- 模块 DLL 是可执行代码，只安装你信任来源的模块
- 诊断包含本机路径与日志，分享前请检查
- 发现安全问题请通过 GitHub 私下联系维护者，避免先公开可利用细节

## 贡献

欢迎 issue 和 pull request。提交前请：

1. 保持 `FGB_WARNINGS_AS_ERRORS=ON` 下构建通过
2. 跑 `ctest`，新增行为补测试
3. 界面文案三语齐全（简中 / 英 / 日）
4. 不提交 `build/`、`dist/`、`.workbuddy/` 等本地产物

## 许可证与致谢

本项目以 **Apache-2.0** 许可发布，见 [LICENSE](LICENSE) 与 [NOTICE](NOTICE)。
第三方组件、再分发的 VRCFT 运行时、.NET 运行时、WebView2 SDK、OpenVR 头文件和
Microsoft VC++ 运行库的说明都在 NOTICE 中；安装包的 `licenses/` 目录附带完整许可证文本。

感谢 [VRCFaceTracking](https://github.com/benaclejames/VRCFaceTracking) 及其贡献者
在模块生态和 Unified Expressions 规范上的工作。

## 作者

**Aqamoe（光水）** · [GitHub](https://github.com/Aqamoe) · 其他贡献者见 [AUTHORS.md](AUTHORS.md)

如果这个项目对你有帮助，欢迎在 GitHub 上点一个 Star。
