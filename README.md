# FaceGaze Bridge

> 眼追 / 面捕 / 注视点数据的统一桥接层 —— 把任何来源的追踪数据，送到任何需要它的地方。

FaceGaze Bridge（下称 FGB）是一个用 C++ 重写的追踪数据中间件。它在功能上完整覆盖 [VRCFaceTracking (VRCFT)](https://github.com/benaclejames/VRCFaceTracking) 的能力，并在此之上提供 VRCFT 没有的东西：**多目标输出、注视点数据服务、以及给串流软件/游戏引擎用的低延迟数据通道**。

---

## 一句话定位

VRCFT 解决的问题是「我的面捕硬件怎么驱动 VRChat 头像」。
FGB 要解决的问题是「我的面捕/眼追数据，怎么同时驱动头像、双耳音频、注视点渲染、串流编码和第三方应用」。

---

## 文档索引

| 文档 | 内容 | 读者 |
|---|---|---|
| [docs/01-architecture.md](docs/01-architecture.md) | VRCFT 技术解剖、C++ 重构边界、进程/线程模型、数据模型、模块系统、输出系统 | 架构 / 核心开发 |
| [docs/02-features.md](docs/02-features.md) | 功能规格书：VRCFT 对等功能清单 + 增值功能（注视点渲染 / VRSS / 串流） | 产品 / 开发 |
| [docs/03-roadmap.md](docs/03-roadmap.md) | 里程碑 M0–M6、任务分解、验收标准、工作量估算 | 项目管理 |
| [docs/04-engineering.md](docs/04-engineering.md) | 技术栈、构建、测试、CI、发布、可观测性、许可证合规、风险登记册 | 工程 |

---

## 当前状态

| 阶段 | 状态 |
|---|---|
| 方案设计 | ✅ 完成（本文档集） |
| 技术预研（M0） | ✅ 协议已实测跑通（[ADR-0003](docs/adr/0003-vrcft-ipc-protocol.md)） |
| 核心骨架（M1） | ✅ VMC → OSC 端到端 |
| 模块兼容层（M2） | ✅ **核心跑通**：C++ 直接驱动官方 ModuleProcess，拿到真实 PICO 数据 |
| 参数层（M3） | ✅ v2 + v1 legacy + 地址发现 + binary；**真机验收通过**（[ADR-0006](docs/adr/0006-hardware-acceptance.md)） |
| 录制回放（M4 之一） | ✅ 录原始包 + 回放 + 参数导出 CSV（[ADR-0007](docs/adr/0007-capture-replay.md)） |
| 校准与滤波（M3 剩余） | ✅ 逐形状校准（从两份抓包推导）+ One Euro 滤波（[ADR-0008](docs/adr/0008-calibration-and-filtering.md)） |
| 界面（WebView2） | ✅ 单入口 `FaceGaze Bridge.exe`（窗口自己拉起引擎）+ WebSocket IPC：状态 / 模块 / 参数热力图 / 输出 / 日志 / 设置（[ADR-0010](docs/adr/0010-webview2-ui-and-websocket-ipc.md)、[ADR-0016](docs/adr/0016-single-entry-desktop-app.md)） |
| 界面本地化 | ✅ 简中 / 英 / 日三语，界面内切换并记住选择（[ADR-0012](docs/adr/0012-ui-localization.md)） |
| 模块管理 | ✅ 独立模块仓（导入 / .zip 安装 / 启停 / 删除）+ 模块库一键安装 + 内置精简 VRCFT 运行时；VRCFT 目录只作为导入来源（[ADR-0017](docs/adr/0017-module-store-library-runtime.md)） |
| 多源合并（M4） | ✅ 表情 / 眼追 / 头姿三通道分别选提供者，可重复 `--source`（[ADR-0013](docs/adr/0013-source-merging.md)） |
| 原生 C ABI（M4/M5 基础） | ✅ 进程内 C ABI v1 + 无硬件示例模块（[ADR-0018](docs/adr/0018-native-c-abi-v1.md)） |
| 多输出（M4） | ✅ VRChat OSC + VMC 第二路（`--vmc-out`），可同时驱动 Unity 场景（[ADR-0014](docs/adr/0014-vmc-output-sink.md)）；共享内存 Sink 未做 |

**已经能跑的**（M1 + M2 核心）：

- **VRCFT 模块兼容**：C++ 直接驱动官方 `ModuleProcess.exe`，握手 → Init → 状态同步 →
  100 Hz 拉取 → 432 字节解码，实测从 PICO 模块拿到 1672 帧真实数据
- **v2 参数派生**：协议层 88 个肌肉形状 → 88 个基础参数 + 88 个简化/合成/眼追参数，
  规则逐条对齐 VRCFT（`JawX = JawRight - JawLeft`、`EyeLid = Openness*0.75 + EyeWide*0.25` 等）
- VMC (UDP 39539) 输入 → 统一形状映射 → VRChat OSC 输出
- OSC 编解码：bundle、分片、零分配写入器、容错解析
- 无锁核心：SPSC ring、seqlock 快照
- VRChat 输出端：死区过滤、bundle 聚合、1200 字节主动分包、1 Hz 心跳
- 眼追参数派生（`EyeLeftX/Y`、`EyeLidLeft/Right`），`parameters` / `native` / `both` 三模式
- 界面：`FaceGaze Bridge.exe` 双击即用（窗口拉起隐藏引擎，关窗即停），↔ 引擎走本机 WebSocket，
  10 Hz 推送状态与 365 个参数值，滤波滑杆/重新读取头像/日志实时回传
- 模块管理：自己的模块仓（`%LOCALAPPDATA%\FaceGazeBridge\modules`），本机 VRCFT 模块只作为
  "检测到，是否导入"；支持 .zip/.dll 安装、启用/停用、删除；模块库（registry.vrcft.io，
  与 VRCFT 同源）浏览、搜索、一键安装/更新，下载后校验 MD5
- 独立运行时：发布包带精简的 VRCFT 宿主（`runtime/vrcft`，约 106 MB），
  卸载 VRCFT 后模块仍可加载；界面显示当前用的是内置/本机副本/VRCFT 安装
- 多源合并：多个输入可同时工作（如模块给眼追、VMC 给表情），
  按通道选提供者并有过期回退；状态页能看见每个源在喂哪一路
- 多输出：VRChat OSC 与 VMC 同时输出（`--vmc-out 127.0.0.1:9100`），
  同一张脸可以既驱动 VRChat 模型又驱动 Unity 场景
- 界面三语（简中 / 英 / 日）+ 双击即用的发布包（`cmake --install` 到 `dist/`）
- 112 个单元测试 + 端到端冒烟测试（`tools/smoke_vmc.py`）+ IPC/界面/VMC 输出探针
  （`tools/probe_ipc.py`、`tools/probe_ui.py`）

```bash
# 用 FGB 接管 VRCFT 的模块（需先关闭 VRCFT，模块端口是独占的）
fgb-cli run --source vrcft --vrchat 127.0.0.1:9000 --verbose

# 带界面：拉起 WebView2 窗口（隐含 --panel 8787）
fgb-cli run --source vrcft --ui

# 模块：仓里有什么、本机有什么可导入、运行时来自哪里
fgb-cli modules
fgb-cli modules import-all            # 把本机 VRCFT 模块复制进自己的模块仓
fgb-cli modules install module.zip    # 手动安装 .zip / .dll
fgb-cli modules use VRCFTPicoModule   # 启动模块，重启引擎后生效
```

**还没做的**：

- 头像切换的实时感知（目前启动时读 VRChat 日志确认，换头像需在界面里点"重新读取"）
- 托盘常驻壳（界面已经能单开一个窗口，缺的是"开机自启 + 右键菜单"这一层）
- 界面里还不能改输出目标 / 参数集 / 校准档案，仍需重启
- 多 Sink（共享内存 / OpenXR；VMC 已做）
- 原生 C ABI 模块（设计见 `docs/01-architecture.md` §8.2，尚未实现）
- 与 VRCFT 输出的逐参数对齐（量化"替代"的最后一道关口）

**已验证的关键事实**（M0，详见 [ADR-0003](docs/adr/0003-vrcft-ipc-protocol.md)）：

- 可以脱离 VRCFT 主程序，自带端口驱动官方 `ModuleProcess.exe`
- 数据是**拉模式**：每发一个 `EventUpdate` 才回一个 `ReplyUpdatePacket`
- 数据包 432 字节，`structSize=420` = 16 个眼球/头部 float + 89 个形状 float
- 协议层形状是 **88 个肌肉层形状**（`BrowLowererLeft`、`JawRight`），
  v2 参数（`BrowDownLeft`、`JawX`）是 Core 侧派生出来的另一层

---

## 快速开始

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug

./build/bin/Debug/fgb-cli.exe shapes     # 列出 86 个统一形状
./build/bin/Debug/fgb-cli.exe run --help

# 端到端冒烟：VMC 输入 -> OSC 输出（用 9100，不碰 VRChat 的 9000）
python tools/smoke_vmc.py
```

跑真实链路时：

```bash
fgb-cli run --vmc-port 39539 --vrchat 127.0.0.1:9000 --eye-mode parameters
```

界面的依赖只有 **WebView2 Runtime**（Win10/11 随 Edge 预装）。SDK 在 configure 时从 NuGet 拉一次到
`build/_deps`；内网环境可以自己解压好再 `-DFGB_WEBVIEW2_DIR=<目录>`，或者 `-DFGB_BUILD_UI=OFF` 只编引擎。
页面资源在 `ui/`，构建时复制到 `fgb-cli.exe` 旁边；改 UI 不用重编。

发布包（`cmake --install build/release --prefix dist`）里只有一个入口：
双击 `dist/FaceGaze Bridge.exe`，窗口会自己拉起引擎，关掉窗口即停止追踪。
想用命令行/自动化测试时仍可直接跑 `fgb-cli run --source vmc --panel 8787`。

---

## 关键设计摘要

**1. 三层模块系统，而不是一刀切**

VRCFT 的模块是 .NET 程序集（`ExtTrackingModule` 子类），纯 C++ 进程无法直接加载。FGB 用三层并存解决：

- **A 层 · VRCFT 兼容**：复刻 VRCFT v5 的沙箱 UDP 协议，直接驱动官方 `VRCFaceTracking.ModuleProcess.exe`。现有模块**零改动**可用。
- **B 层 · 原生 C ABI**：新的高性能模块接口，单导出符号、POD 结构、共享内存传帧。给愿意重写模块的开发者。
- **C 层 · 协议直连**：VMC / OSC / LiveLink / ALVR / VD / PICO / OpenXR 眼追。**不用装模块，填个 IP 就能用**。

**2. 核心引擎与 UI 解耦**

`fgb-core` 是无 UI 的 C++ 服务，所有能力通过本地 IPC 暴露。UI 只是第一个消费者，CLI、Unity 插件、OBS 插件、Stream Deck 插件都是同等消费者。

**3. 眼追默认走 avatar 参数，原生通道作为可选模式**

VRCFT 生态的现实是：绝大多数模型只把 `FT/v2/EyeLeftX`、`EyeLidLeft` 这类 **avatar 参数**绑到了表情控制器上。若改走 VRChat 原生的 `/tracking/eye/*`，这些模型的眼睛会完全不动——这不是"更优的路径"，是"更少的模型能用"。

所以 FGB 与 VRCFT 保持行为一致：**眼追和表情都写 avatar 参数**，这是默认路径。`/tracking/eye/*` 作为可切换的**输出模式**，保留给明确知道自己需要它的人（原生眼追头像、需要绕过 Animator 叠加层的场景、参数数量吃紧的极端情况）。两种模式可独立开关，也可以同时开。

**4. 注视点能力是分层的，而且要说清楚边界**

| 层级 | 能力 | 可行性 |
|---|---|---|
| L1 | 注视点数据服务（输出给串流软件 / 第三方应用） | ✅ 确定可做 |
| L2 | OpenXR API Layer + 引擎侧 ETFR | ⚠️ 依赖游戏支持，需逐游戏验证 |
| L3 | D3D/Vulkan 钩子 + VRS 强制注入 | ❌ 与 VRChat EAC 冲突，仅限单机 VR 游戏，作为实验特性 |

详见 [docs/02-features.md](docs/02-features.md#65-注视点渲染能力边界)。

---

## 许可证立场

- VRCFT 为 **Apache-2.0**。FGB 采用 **clean-room 重写 + 协议兼容**策略：只依据公开的 Unified Expressions 规范、VRChat OSC 文档和模块 ABI 行为实现，不搬运其源码。
- 若必须再分发官方二进制（如 `ModuleProcess.exe`），保留其 `LICENSE` 与 `NOTICE`，并在关于页面标注。
- **不使用 "VRCFT" / "VRCFaceTracking" 作为产品名或商标**，仅以「兼容 VRCFT 模块」的事实性描述出现。
- FGB 自身建议 **Apache-2.0**（与生态一致，允许商业闭源集成）。

---

## 仓库结构

```
src/base      日志（含订阅回调）、时钟        零依赖
src/core      数据模型、无锁容器、滤波/校准   零 I/O，纯计算
src/osc       OSC 编解码                     零分配写入器
src/net       UDP / TCP / WebSocket 服务端、WinHTTP 下载
src/sources   输入（VMC / VRCFT 模块 / 原生 C ABI 模块 / 抓包回放）
src/sinks     输出（VRChat / 待接入 VMC、共享内存）
src/engine    合并、快照发布、扇出
src/web       控制面 IPC：WS 协议、状态/参数/日志推送、静态页面
src/ui        FaceGaze Bridge.exe：桌面入口（WebView2 窗口 + 拉起/回收引擎）
src/cli       fgb-cli
ui/           界面资源（index.html / app.js / style.css，无构建步骤）
tests         单元测试 + 金样测试
tools         抓包、冒烟、探针，以及 build_vrcft_runtime.py（出精简运行时）
sdk/          原生模块 ABI 头（只分发这一个头文件）
samples/      native-sine：不需要硬件的原生模块示例
docs          方案、ADR、路线图
```
