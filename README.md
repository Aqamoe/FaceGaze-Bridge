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

**已经能跑的**（M1 + M2 核心）：

- **VRCFT 模块兼容**：C++ 直接驱动官方 `ModuleProcess.exe`，握手 → Init → 状态同步 →
  100 Hz 拉取 → 432 字节解码，实测从 PICO 模块拿到 1672 帧真实数据
- **v2 参数派生**：协议层 88 个肌肉形状 → 88 个基础参数 + 71 个合成/简化/眼追参数，
  规则逐条对齐 VRCFT（`JawX = JawRight - JawLeft`、`EyeLid = Openness*0.75 + EyeWide*0.25` 等）
- VMC (UDP 39539) 输入 → 统一形状映射 → VRChat OSC 输出
- OSC 编解码：bundle、分片、零分配写入器、容错解析
- 无锁核心：SPSC ring、seqlock 快照
- VRChat 输出端：死区过滤、bundle 聚合、1200 字节主动分包、1 Hz 心跳
- 眼追参数派生（`EyeLeftX/Y`、`EyeLidLeft/Right`），`parameters` / `native` / `both` 三模式
- 29 个单元测试 + 端到端冒烟测试（`tools/smoke_vmc.py`）

```bash
# 用 FGB 接管 VRCFT 的模块（需先关闭 VRCFT，模块端口是独占的）
fgb-cli run --source vrcft --vrchat 127.0.0.1:9000 --verbose
```

**还没做的**：

- VRCFT .NET 模块兼容层（M2，关键路径上的下一步——协议已验证，见 ADR-0003）
- 头像切换的实时感知（OSCQuery / `/avatar/change`）
- 校准、滤波器、UI（M3）
- 多 Sink、录制回放（M4）

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
src/base      日志、时钟、错误类型            零依赖
src/core      数据模型、无锁容器、滤波/校准   零 I/O，纯计算
src/osc       OSC 编解码                     零分配写入器
src/net       UDP 封装
src/sources   输入（VMC / 待接入 VRCFT 模块）
src/sinks     输出（VRChat / 待接入 VMC、共享内存）
src/engine    合并、快照发布、扇出
tests         单元测试 + 金样测试
tools         抓包与冒烟脚本
docs          方案、ADR、路线图
```
