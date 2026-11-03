# 12 · VRSS 融合实施计划（阶段 2 / 阶段 3）

- 状态：**已确认（v3：术语修正 + D7 一键确认；待实施）**
- 日期：2026-11-01
- 上游：`VRSS-Tuner`（MIT）<https://github.com/challenger0303/VRSS-Tuner>
- 关联：[02-features.md §5](02-features.md#5-p2--注视点渲染)、[ADR-0033](adr/0033-gaze-point-2d-space.md)、[ADR-0016](adr/0016-single-entry-desktop-app.md)、[ADR-0022](adr/0022-tray-shell-and-close-semantics.md)、[ADR-0025](adr/0025-output-settings-persistence.md)
- 新增 ADR：**0040 · VRSS provider 作为可选独立组件**

---

## 0. 目标与边界

### 0.1 目标

1. VRSS 是 FGB 的**重点功能**，拥有**独立侧栏页面「VRSS 可变速率超采样」**，不塞进输出页。
2. 页面提供**一键全流程**：环境自检 → 驱动检查（白名单外自动 DLL 探测）→ 权限/互斥检查 → 启动注视路径 → 启动 VRChat → 验证与微调。
3. 每一步有明确状态（未开始/进行中/通过/失败/需用户操作），**上一步通过才允许下一步**；失败停在该步并给出可操作原因。
4. 驱动不在白名单时，自动做**只读 DLL 探测**并生成实验合同；探测通过后继续，失败则停止。
5. provider 作为**可选、默认关闭、独立进程**随包发布，不进主进程、不进热路径。

### 0.2 术语与能力边界（必须写进 UI 与文档）

**VRSS = Variable Rate Super Sampling（可变速率超采样），不是 foveated rendering（注视点渲染）。**

| 说法 | 正确性 | 说明 |
|---|---|---|
| "注视点抗锯齿" | ✅ | 驱动在注视中心做**更高倍率超采样**，提升中央画质/抗锯齿 |
| "可变速率超采样" | ✅ | VRSS 的正式含义 |
| "注视点渲染" | ❌ | 那是把中央/外围用不同分辨率**渲染**（VRS/MRTR），是另一条技术路线 |
| "省 GPU 算力 10–25%" | ❌ | VRSS 是**画质**特性，通常**增加** GPU 开销；不承诺帧率收益 |
| "所有游戏都能开" | ❌ | 需 NVIDIA 驱动合同、游戏 MSAA、对应应用 profile 允许 VRSS |

页面文案统一使用"注视点抗锯齿 / 可变速率超采样"，不得写"注视点渲染"。

**抗锯齿范围**：1–50%（常用 10–15%，默认 10%）。这是 NVIDIA VRChat profile 里的隐藏
VRSS area 设置（`0x00524254`），FGB 只写这一个字段，不伪造 allow/mode；写入需要
管理员权限，**重启 VRChat 后生效**。

## 0.3 明确不做

| 不做 | 原因 |
|---|---|
| 搬 `VRSS Tuner.exe` 的 GUI、多厂商桥接 | FGB 已有注视源；那些是上游为"没有 FGB"的场景准备的 |
| 把 provider 链接进 `fgb-cli` / 主 exe | 需要管理员令牌、调用未文档化 NVAPI、崩溃域必须隔离 |
| 向游戏进程注入 DLL / 改 EAC | VRSS 走驱动注册，不需要注入 |
| VR 内 9 点标定 | VRSS 要的是每眼视口位置，不是角度 |
| 自动改 NVIDIA 配置 / 自动开 MSAA | 只做只读检查与提示，不伪造 profile |
| 承诺所有驱动/所有头显可用 | 驱动是精确 SHA-256 合同；头显身份需真机验证 |
| 固定 HMD Key 启动（v1） | 那是上游 audited HMD test branch / Steam Frame 的专用路径；标准路线用 Steam 正常启动 |

---

## 1. 前置：阶段 1 必须先有的产物

| # | 产物 | 验收 |
|---|---|---|
| P2.0.1 | `src/sinks/vrss_gaze_sink.{hpp,cpp}`，发 `UdpGazeV1` 到 `127.0.0.1:5556` | 本地 UDP 探针收到 44 字节、`validMask=3`、`qpc` 递增 |
| P2.0.2 | 2D 点 → NDC 换算（`x=2px-1, y=2py-1`，y 符号实测定） | 上下左右四方向区域跟随正确 |
| P2.0.3 | 用现成 VRSS-Tuner provider 真机验证通过 | VRChat 内高质量区跟随双眼 |

> 阶段 1 不在本计划里重复定义；本文只描述"组件化 + 产品化 + 一键流程"。

---

## 2. 总体架构

```
┌──────────────────────────────────────────────────────────────────┐
│ FaceGaze Bridge.exe（外壳/托盘，asInvoker）                        │
│   ├ 现有引擎 Job                                                  │
│   └ 托盘退出时写 stop.request                                      │
├──────────────────────────────────────────────────────────────────┤
│ fgb-cli.exe（引擎，asInvoker）                                     │
│   ├ 写 vrss lease 心跳（控制面每 2s）                              │
│   ├ VrssGazeSink ── 44B UDP ──┐                                  │
│   ├ vrss::Setup      环境自检 + 几何导出 + 驱动白名单/探测          │
│   ├ vrss::Provider   提权启动/停止/lease/status.json              │
│   ├ vrss::Workflow   六步状态机（一键流程，后台线程）               │
│   ├ vrss::Vrchat     进程检测/Steam 启动（AppID 438100）          │
│   └ IPC/UI：独立 VRSS 页面 + 状态推送                              │
└──────────────────────────┬───────────────────────────────────────┘
                           │ 127.0.0.1:5556（单向数据）
┌──────────────────────────▼───────────────────────────────────────┐
│ fgb-vrss-provider.exe（提权，独立进程，不进 Job）                   │
│   ├ 读 geometry.txt（OpenVR 投影 + eyeToHead）                    │
│   ├ 校验 NVIDIA 二进制 SHA-256 与 vtable RVA                       │
│   ├ 注册 gaze provider（按 HMD vendor/product）                    │
│   └ 100Hz 轮询 UDP → NVAPI UpdateGazeData                         │
└──────────────────────────────────────────────────────────────────┘
```

**为什么 provider 不跟引擎同生命周期**：FGB 改输出设置会重启引擎；provider 一重启，NVIDIA 可能拒绝重注册（`-175`），必须重启 VRChat。provider 只在"用户显式停止"或"外壳退出"时结束。

---

## 3. 外部契约（实施依据）

### 3.1 provider 启动参数（FGB 侧固定）

```text
fgb-vrss-provider.exe
  --geometry <run>\geometry.txt
  --config   <data>\vrss\calibration.txt
  --output   <data>\vrss\run
  --source udp
  --port 5556
  --allow-reconnect 1
  --seconds 86400
  --lease    <data>\vrss\lease.txt            ; 新增补丁
  --csv 0                                      ; 新增补丁
  --experimental-contract <path>               ; 新增补丁，仅非白名单驱动
```

- 不传 `--input-route`：上游对 `--source udp` 会解析成空路由（`provider.cpp` L284-L323），走归一化 UDP。
- `calibration.txt` 必须存在，否则 `ReadGain` 抛错；首次运行写默认 2 行 `left_deg=0 / right_deg=0`。
- `--experimental-contract` 覆盖上游的 `VRSS_EXPERIMENTAL_DRIVER_CONTRACT` 环境变量。**必须用参数**：提权进程的环境变量继承不可靠，不能把"能不能启动"押在它上面。

### 3.2 上游补丁（vendor 时随源码维护，写进 `PATCHES.md`）

| 补丁 | 目的 | 改动点 |
|---|---|---|
| `--lease <file>` | 引擎/外壳崩溃时 provider 不长期滞留 | 主循环每 10ms 检查 lease mtime；超过 15s 退出并 unregister |
| `--csv 0` | 避免 90Hz × 24h 的 `gaze.csv` 膨胀 | 仅在开启时打开 CSV |
| `--experimental-contract <path>` | 提权进程确定性加载实验合同 | 参数优先于环境变量 |
| `--driver-audit 1 --contract-out <path>` | 一键探测后直接产出可用合同 | 审计 PASS 后按 7 字段格式写文件 |
| 审计输出加 `driverVersion=` | 非白名单驱动没有 `knownContract` | 从 `nvwgf2umx.dll` 版本资源或 NVAPI 读取 |
| 异常写 `run\error.txt` | 提权进程 stderr 不可见 | `catch` 写文件，带机器可读前缀（`E_DRIVER`/`E_ADMIN`/`E_RECONNECT`...） |

> 这些都是小改动，但必须文档化并在升级上游时重放。

### 3.3 `status.json`（provider 每 50ms 原子写）

已有关键字段（`provider.cpp` L420-L432）：`pid / registered / fresh / consumerCount / queryStatus / sent / submitFailures / sourceSequence / identityMode / deliveryState / locations / xGain / yGain / source / inputRoute / inputPort`。

FGB 只读不改，用作 UI 状态的唯一事实来源。

### 3.4 几何文件

由 `fgb-export-geometry.exe <file>` 生成，格式与上游一致：

```text
format=1
vendor=<EDID vendor 或候选身份>
product=<EDID product 或候选身份>
identitySource=openvr-edid | candidate-auto | candidate-manual | tuner-session
projection0=l,r,t,b
eyeToHead0=<9 floats>
projection1=...
eyeToHead1=...
```

**PICO 串流身份**：PICO Connect 这类虚拟 HMD 可能没有 EDID，导出器先按
`Prop_TrackingSystemName_String` + `SerialNumber` 派生候选身份（`candidate-auto`）。
由于串流会话序列号可能变化，FGB 随后会用 `--common-session-key` 重导一次，改成固定
会话键 `0xFEEE:0x0001`（`identitySource=tuner-session`），这样持久化的环境变量不会失效。
真机验收：失败就报"无法取得头显身份"，不猜。

### 3.5 实验驱动合同

非白名单驱动探测通过后，FGB 生成并保存：

```text
format=1
version=<探测得到的驱动版本>
module=nvapi64.dll | nvapi64_impl.dll
api_sha256=<64 hex>
umd_sha256=<64 hex>
vtable_rva=<0x...>
method_rvas=<8 个 0x... 逗号分隔>
```

- 存储：`%LOCALAPPDATA%\FaceGazeBridge\vrss\contracts\<api前12位>-<umd前12位>.txt`
- 复用：启动前先比对当前 `nvapi64/nvapi64_impl/nvwgf2umx` 哈希；不匹配就作废旧合同并重新探测。
- **探测只读**：只初始化公开 gaze handler、读 vtable 地址、检查方法在可执行段内，**不调用私有注册/提交方法**（`AuditDriverWithoutRegistration`，`provider.cpp` L131-L174）。
- **诚实边界**：探测 PASS 只证明"地址结构仍可读"，不证明私有方法语义正确；UI 必须写"需真机看四方向才能确认渲染生效"。

### 3.6 校准语义（重要）

上游的 `x_gain / y_gain / left_deg / right_deg` **只作用于 OSC 与 BrokenEye 路径**；归一化 UDP 路径直接把来值写进 `fGazeNormalizedLocation`，**不套 gain/offset**（`provider.cpp` L396-L412）。因此：

- 微调必须由 FGB 在发 UDP 前做；
- provider 的 `calibration.txt` 保持中性（`0/0/1/1`），避免两处重复调参；
- UI 只提供一套 gain/offset，文案说明"作用于 FGB 输出"。

---

## 4. 阶段 2：组件化融合

### P2.1 vendor 与许可

| 项 | 内容 |
|---|---|
| 新增目录 | `third_party/vrss-provider/`：`provider.cpp`、`GazeInput.h`、`gaze_state.h`、`gaze_parse.h`、`projection.h`、`private_contract.h`、`driver_identity.h`、`driver_contract_file.h`、`config_io.h`、`HmdIdentity.h`、`hmd_candidate_identity.h`、`OpenVrFrameSource.h`、`export_openvr_geometry.cpp`、`BaballoniaRelay.h`（仅为编译依赖） |
| 上游版本 | 固定 commit hash，写在 `PATCHES.md`；升级前先跑上游测试 |
| 许可 | 保留上游 `LICENSE`（MIT）；`NOTICE` 增加 NVIDIA NVAPI 与 OpenVR（BSD-3）说明；`packaging/` 的 NOTICE 同步 |
| ADR | **ADR-0040**：为什么引第三方源码、为什么独立进程、为什么打补丁、升级策略 |

**验收**：`third_party/vrss-provider/` 能独立编译；`NOTICE` 三处一致。

### P2.2 构建与安装

| 项 | 内容 |
|---|---|
| CMake | 新选项 `FGB_BUILD_VRSS`（Windows 默认 ON）；目标 `fgb-vrss-provider`、`fgb-export-geometry`，**不链接进 `fgb-cli`** |
| 编译参数 | provider 用 `/MT` 静态运行库（与上游一致）；第三方代码单独关 `/WX`，FGB 自己的代码仍 `/W4 /WX` |
| 依赖 | `third_party/nvapi`（头 + `amd64/nvapi64.lib`）、OpenVR 头与 `openvr_api.lib`；不随包分发 NVIDIA 二进制 |
| 安装 | `dist/runtime/vrss/{fgb-vrss-provider.exe, fgb-export-geometry.exe, openvr_api.dll, calibration.txt}` |
| CI | Debug/Release 构建；无 NVIDIA 的 CI 上 `--driver-check-only` 返回特定失败码，不能判为崩溃 |

**验收**：`cmake --install` 后 `dist/runtime/vrss/` 齐全；`dist/fgb-cli.exe` 不新增 VRSS 导入符号。

### P2.3 几何获取

| 项 | 内容 |
|---|---|
| 新增 | `src/vrss/geometry.hpp/cpp`：调用 `fgb-export-geometry.exe` 到 `vrss\run\geometry.txt`，解析 `vendor/product/identitySource/identityFingerprint` |
| 时机 | provider 启动前生成；页面「重新检测」重新生成 |
| 失败 | SteamVR 未运行 / 无 HMD / 无身份 → 明确报错，不启动 provider |
| 缓存 | 不长期缓存；每次启动重导（IPD 与头显可能变） |

**验收**：SteamVR 未运行给出具体原因；有 HMD 时 `vendor/product` 非 0 或 `identitySource=candidate-auto`。

### P2.4 provider 进程控制

| 项 | 内容 |
|---|---|
| 新增 | `src/vrss/provider.hpp/cpp`：`start()/stop()/poll()/snapshot()` |
| 启动 | `ShellExecuteExW(L"runas")`；参数见 §3.1；工作目录 `dist/runtime/vrss` |
| 停止 | 写 `vrss\run\stop.request` 并等待 5s；**不提权杀进程**（非提权进程杀不掉提权进程），超时提示用户手动结束 |
| 单实例 | provider 自带 `Global\VRSS-Tuner-Provider-<vendor>-<product>` 互斥；FGB 另读 `status.json` 的 `pid`，已存活则不重复启动 |
| lease | 引擎控制面每 2s 写 `vrss\lease.txt`（内容为 tick）；provider 15s 未更新即退出并 unregister |
| 冲突 | 检测到 `common_vrss_provider.exe` / `vrss_gaze_provider.exe` → "另一个 VRSS 提供程序在运行" |

**验收**：UAC 取消→`failed`"需要管理员权限"；外壳强杀→15s 内自退并 unregister；引擎重启→provider pid 不变、`registered` 持续。

### P2.5 驱动检查与自动探测

| 项 | 内容 |
|---|---|
| 新增 | `src/vrss/driver.hpp/cpp`：白名单判定、审计输出解析、合同读写、二次校验 |
| 白名单 | 与上游一致：596.02、610.88 的精确 `nvapi64(_impl).dll` + `nvwgf2umx.dll` SHA-256 |
| 第一步 | 直接跑 `--driver-check-only 1`：无合同、只认内建白名单；输出 `DRIVER_CHECK_PASS` |
| 自动探测 | 白名单失败时跑 `--driver-audit 1 --contract-out <path>`；解析 `nvapi64Sha256/nvapi64ImplSha256/nvwgf2umxSha256/vtableRva/methodRvas/methodsInImage/methodsExecutable/DRIVER_AUDIT_PASS` |
| 二次校验 | 带 `--experimental-contract <path>` 再跑 `--driver-check-only 1`，必须 PASS |
| 合同缓存 | 当前三个 DLL 哈希匹配旧合同 → 直接复用；不匹配 → 删旧合同并重新探测 |
| 失败分类 | 无 NVIDIA GPU / 驱动初始化失败 / 哈希读取失败 / vtable 不在 NVAPI 镜像内 / 方法不可执行 / 合同不匹配 |

**验收**：白名单驱动一次通过；非白名单驱动自动探测并落盘合同；人为改合同哈希后校验失败；审计 PASS 但 UI 明确标注"语义未验证"。

### P2.6 观察与错误映射

| 项 | 内容 |
|---|---|
| 状态源 | `status.json` + `pid` 存活 + `mtime` 新鲜度（>2s 视为失联） |
| 错误源 | `run\error.txt`（补丁）、provider 退出码、`status.json` 的 `submitFailures` |
| 映射表 | 驱动不支持 / 未允许 VRSS / VRChat 占用导致重注册失败 / UAC 取消 / 端口被占 / SteamVR 未运行 / 头显身份失败 / 几何无效 / 合同不匹配 |
| 诊断包 | 打包 `vrss\run\status.json`、`error.txt`、`geometry.txt`、`lease` 状态、合同摘要；默认不含 `gaze.csv`（本来也不生成） |

**验收**：每种失败在页面都有可操作的具体原因，不是"失败"。

### P2.7 打包与更新

| 项 | 内容 |
|---|---|
| 安装器 | 随主安装包安装 `runtime/vrss/`；默认关闭，不注册启动项 |
| 更新 | provider 运行时文件被占用：更新前先 `stop`；装完不自动启动 |
| 卸载 | 删除 `runtime/vrss/`；保留 `%LOCALAPPDATA%\FaceGazeBridge\vrss\` 的用户校准、合同与日志，除非用户勾选清理 |
| 体积 | 只含 provider/geometry/openvr，不含 NVAPI 运行时 DLL（系统自带 NVIDIA 驱动） |

### P2.8 测试

| 层级 | 内容 |
|---|---|
| 单测 | 报文布局 `static_assert(sizeof==44)`；参数拼装；审计输出解析；合同序列化/校验；`status.json` 解析；`error.txt` 原因映射；lease 过期判定 |
| 探针 | `tools/probe_vrss_sink.py`：本地 UDP 收包，校验 magic/version/mask/qpc 递增/频率 |
| 离线 | `fgb-vrss-provider --driver-check-only 1` 在无 NVIDIA 环境返回失败码，不误判为崩溃；`--driver-audit` 输出用固定样本做解析单测 |
| 真机 | 见 §8 验收清单 |

---

## 5. 阶段 3：独立页面与一键流程

### P3.1 配置项

页面配置独立于输出页，但仍落在同一份 `outputs.ini`（继续满足 [ADR-0025](adr/0025-output-settings-persistence.md) 的保存/预设语义）：

```text
vrss_enabled   = false       ; 默认关
vrss_port      = 5556        ; 只允许回环
vrss_rate_hz   = 90          ; 30..120
vrss_x_gain    = 0.5         ; 0.1..2；驱动坐标 1 = 一个视口宽，故默认 0.5
vrss_y_gain    = 0.5         ; 0.1..2
vrss_x_offset  = 0.0         ; -1..1（中心偏移，归一化单位）
vrss_y_offset  = 0.0         ; -1..1
vrss_y_invert  = false       ; Y 轴方向由代码统一翻转；仅区域上下颠倒时开
vrss_auto_launch_vrchat = false  ; 一键流程最后一步是否自动启动 VRChat
vrss_auto_start       = false    ; 跟随 SteamVR 自动注册（一轮会话只跑一次）
vrss_persist_identity = true     ; 无 EDID 头显把身份写成用户级环境变量
```

- 校验范围与上游 `ReadGain` 一致；`vrss_port` 只接受 `127.0.0.1`。
- `load_outputs` 缺键用默认值；`save_outputs` 沿用"先读再写 + 原子替换"。
- CLI 增加 `--vrss-out`、`--vrss-rate`（探针用），与现有 `--udp-out` 风格一致。

### P3.2 引擎接线

| 项 | 内容 |
|---|---|
| Sink | `vrss_enabled` 时构造 `VrssGazeSink`，加入 [main.cpp](../src/cli/main.cpp#L1269-L1301) 的 sink 组装段 |
| 开关语义 | VRSS 的开关/端口/帧率/增益通过 `VrssGazeSink::set_config` **实时生效**；`outputs.ini` 只负责持久化。其它既有输出仍按 ADR-0025 重启生效 |
| 启动语义 | 引擎启动时**不自动弹 UAC**；页面读取 provider 状态，按已有状态重建流程步骤 |
| ctx | `IpcContext` 增加 `vrss::Workflow* vrss`（页面与命令共用） |

### P3.3 IPC / HTTP

新增推送消息 `vrss`（2 Hz，连接时补发一次）：

```json
{
  "type": "vrss",
  "enabled": true,
  "state": "off|running|failed|blocked",
  "steps": [
    {"id": "env",     "state": "pass|fail|running|pending|blocked|skipped", "detail": "SteamVR 2.18.4 / PICO"},
    {"id": "driver",  "state": "pass", "detail": "610.88 audited"},
    {"id": "driver_audit", "state": "skipped|pass|fail", "detail": "未使用实验合同"},
    {"id": "admin",   "state": "pass", "detail": "启动时将请求管理员"},
    {"id": "provider","state": "pass", "detail": "pid=1234 registered=true"},
    {"id": "vrchat",  "state": "pass", "detail": "已运行 / consumerCount=1"},
    {"id": "verify",  "state": "pending", "detail": "请在头显确认四个方向"}
  ],
  "provider": {"pid": 1234, "registered": true, "fresh": true, "consumer_count": 1,
               "sent": 458, "submit_failures": 0, "delivery_state": "submitting"},
  "driver": {"supported": true, "version": "610.88", "contract": "audited|experimental|none"},
  "setup": {"steamvr": true, "geometry": true, "identity": "openvr-edid", "admin": true},
  "lease_age_s": 1.2,
  "calibration": {"x_gain": 0.5, "y_gain": 0.5, "x_offset": 0.0, "y_offset": 0.0, "y_invert": false},
  "last_error": ""
}
```

| 命令 | 语义 |
|---|---|
| `vrss_run` | 启动一键流程（从失败/未完成的步骤继续） |
| `vrss_retry_step` | 只重跑指定步骤（`env`/`driver`/`driver_audit`/`admin`/`provider`/`vrchat`） |
| `vrss_cancel` | 取消后台流程；不影响已启动的 provider |
| `vrss_start` | 单独启动 provider（弹 UAC） |
| `vrss_stop` | 写 `stop.request`；置 `vrss-paused`（显式启动前不自动拉起） |
| `vrss_launch_vrchat` | 启动 VRChat（Steam AppID 438100） |
| `set_vrss` | 只改 `vrss.*` 配置字段（含自动启动开关） |

HTTP：`GET /api/vrss`（只读，供 curl 与探针）。

**验收**：`tools/probe_ipc.py` 能拿到 `steps` 全量结构；每条命令有 `ack` 与失败原因。

### P3.4 独立页面（侧栏新增「VRSS」）

位置：侧栏「输出」之后、「日志」之前；页面标题「VRSS 可变速率超采样」，副标题"注视点抗锯齿；实验特性：需要 NVIDIA + SteamVR + MSAA"。

页面结构（无前端框架、内联 SVG、三语文案）：

| 区块 | 内容 |
|---|---|
| 顶部状态条 | `未启用 / 检查中 / 已注册 / 等待 VRChat / 运行中 / 已阻止 / 失败`；右侧主按钮「一键启用」/「停止」 |
| 步骤条 | 6 步竖向 stepper，每步显示状态图标、标题、一行详情、失败时的「重试」 |
| 步骤 1 环境自检 | Windows / NVIDIA GPU / SteamVR 运行 / HMD 连接 / 几何导出 / 身份来源 |
| 步骤 2 驱动检查 | 白名单版本直接 PASS；否则自动探测，显示哈希与 `vtable/method` 结果、合同路径；标注"语义未验证" |
| 步骤 3 权限与互斥 | 管理员令牌、端口 5556 空闲、无其它 provider、VRChat 运行状态 |
| 步骤 4 启动注视路径 | 弹 UAC → 等 `registered=true`；显示 pid 与 `deliveryState` |
| 步骤 5 启动 VRChat | 已运行则跳过；未运行则按钮启动 Steam AppID 438100；等待 `consumerCount>0` |
| 步骤 6 验证与微调 | 四方向勾选（上/下/左/右）、`sent/failures`、X/Y 增益、左/右眼度数偏移、恢复默认 |
| 前提提示 | 一键确认按钮：「我已在 VRChat 开启 MSAA x4，并允许 VRChat 使用 VRSS」；点击即标记步骤 3 通过并进入下一步，未点击则 `blocked`（v1 不自动读 profile） |
| 失败详情 | 每步可展开显示原始 stdout/`error.txt` 摘要 |

交互规则：
- **一键启用**：自动跑 1→3；到需要用户操作的步骤停下并高亮（步骤 3 的一键确认、步骤 4 的 UAC、步骤 5 的启动 VRChat）。
- 任一步 `fail` 后流程停止，不跳到下一步；「重试」只重跑该步。
- 「停止」用 `ask_confirm`；停止后写 `vrss-paused`，一键启用仍可继续。
- 关闭页面不取消后台流程；重新打开按 `vrss` 状态重建。
- 非 NVIDIA 机器：页面可见但显示"不支持"，不做探测。

### P3.5 一键流程状态机

新增 `src/vrss/workflow.hpp/cpp`（后台线程 + 快照，UI 只读）：

```text
IDLE
 └ run → ENV_CHECK
          ├ fail → BLOCKED(step=env)
          └ pass → DRIVER_CHECK
                    ├ whitelist pass → ADMIN_CHECK
                    └ whitelist fail → DRIVER_AUDIT
                          ├ fail → BLOCKED(step=driver_audit)
                          └ pass → DRIVER_RECHECK
                               ├ fail → BLOCKED
                               └ pass → ADMIN_CHECK
 ADMIN_CHECK（端口/互斥/VRChat 状态）
   └ 用户点「我已开启 MSAA 且允许 VRSS」→ CONFIRM_PASS
        └ pass → START_PROVIDER（UAC）
               ├ cancel/fail → BLOCKED(step=provider)
               └ registered → WAIT_VRCHAT
 WAIT_VRCHAT
   ├ 已运行 → VERIFY
   └ 未运行 → 等待用户/自动启动 → VERIFY
 VERIFY（consumerCount>0 且 fresh 后进入运行态）
```

- 状态机在引擎侧，页面关闭不中断；引擎重启后按 provider/consumer 现状重建。
- 任何用户可见步骤都要有"进行中 + 结果"两条反馈，不允许点了没反应（AGENT.md §2.1）。

### P3.6 校准与微调

| 项 | 内容 |
|---|---|
| 应用位置 | Sink 内、发 UDP 前：`ndc = clamp(ndc * gain + offset, -1, 1)`，两眼共用同一组 offset |
| 保存 | 页面内保存到 `outputs.ini`；随预设一起保存/应用 |
| 默认 | 增益 0.5 / 偏移 0 / 翻转开（真机对齐值）；`1.0` 会让区域走两倍距离 |
| 标定 | 不提供 9 点标定；只提供四方向验证 + gain/offset + 恢复默认 |

### P3.7 文档修正与定位

| 文件 | 改动 |
|---|---|
| [02-features.md §5.1/§5.5](02-features.md#L343-L442) | 增加路径 D：NVIDIA VRSS2（驱动级 gaze provider，非注入）；明确 VRSS 是**中央超采样/注视点抗锯齿**，不是注视点渲染、不省 GPU；把"让 VRChat 支持注视点渲染 ❌"改成"✅ 仅 NVIDIA VRSS（抗锯齿）+ MSAA + 驱动合同" |
| [03-roadmap.md](03-roadmap.md) | 增加 VRSS 里程碑与验收项（独立页面、一键流程、驱动探测） |
| `09-user-guide.md` | 新增"VRSS 可变速率超采样（注视点抗锯齿）"章节：六步流程、四方向验证、停止语义、失败排查 |
| `README` / 发布说明 | 能力边界 + 默认关闭 + 实验特性 |
| `docs/adr/0040-...` | 组件边界、补丁清单、驱动合同与探测、升级与回滚 |

### P3.8 可选：自动检查 NVIDIA profile

v1 用步骤 3 的**一键确认按钮**（点击即通过并进入下一步），不读 NVIDIA profile。若要自动化，新增只读 `fgb-vrss-profile-check`（NVAPI DRSSettings 读取 VRChat profile 的 VRSS allow 与 MSAA），不写配置。列为 v2 候选，不阻塞本计划。

---

## 6. 数据面细节（Sink 实现规范）

### 6.1 取数优先级

```text
1) point_viewport && 该眼 point 有效        → x=2*px-1, y=2*py-1（y 符号按 6.2）
2) 该眼方向有效 && 几何可用（阶段 2 可选）   → 眼空间投影（上游 projection.h 同式）
3) 只有一只眼有效                            → 镜像到另一眼（validMask 仍为 3）
4) 双眼都不可用 / 过期 > 50ms                → 不发；状态 waiting-gaze
```

- **不要用 `gaze.valid` 判断 2D-only 源**：PICO 这路方向无效、点有效，必须看 `left_point_valid/right_point_valid + point_viewport`（[gaze.hpp](../src/core/model/gaze.hpp#L36-L45)）。
- **置信度只对方向路径生效**；2D-only 源的 `confidence` 常为 0，不能因此丢弃。

### 6.2 坐标与符号

- NDC：每眼、中心 `(0,0)`、范围 `[-1,1]`（`nvapi.h` L17723）。
- 视口点 `[0,1]` 是左上原点、+y 向下；NVIDIA 的 NDC 是 **+Y 向上**：
  `x_ndc = 2*px - 1`，`y_ndc = 1 - 2*py`。
- 方向型来源用 OpenVR 投影换算（与上游 `ProjectHeadTangent` 同式），结果同样是 +Y 向上。
- 只在**一个**地方翻转；Steam Frame 的 layout 4 翻转是上游特例，FGB 不复制。

### 6.3 频率与新鲜度

- provider 判定 `age > 150ms` 直接丢弃，因此**不能做死区过滤/按变化发送**；按 `vrss_rate_hz` 每帧都发。
- `qpc` 每次发送时直接 `QueryPerformanceCounter`；不要用 `frame.captureTsNs`（可能来自硬件时钟）。
- `sequence` 单调递增；provider 只在 `sequence` 与 `qpc` 都前进时提交（`FrameGaze.h` L9-L13）。
- 单眼镜像保持不超过 150ms；超时停发，不发明"看正前方"的假位置。

---

## 7. 生命周期、权限与 VRChat

| 场景 | 期望行为 |
|---|---|
| 关闭窗口 | 收进托盘；引擎与 provider 都继续 |
| 引擎重启（改输出设置/换源） | provider pid 不变、`registered` 不变；sink 重新开始发包 |
| 托盘「退出」 | 停止引擎 → 写 `stop.request` 等 provider 退出 → 停止 lease → 退出 |
| 外壳崩溃/被结束 | 引擎随之结束，lease 15s 后 provider 自退并 unregister；下次启动检测残留状态 |
| provider 崩溃 | `status.json` 停更 → 页面 `failed`；提供「重新启动」 |
| VRChat 已持有旧连接 | 重注册报 `-175`；页面明确"先正常关闭 VRChat，再启动注视路径，再开 VRChat" |
| UAC 取消 | 步骤 4 `failed`"需要管理员权限"；不自动重试 |
| VRChat 启动 | 有 EDID 的头显用 `steam://rungameid/438100`；无 EDID 的 PICO/串流头显默认走**持久身份**：把同一对 `TEST_VENDOR_ID` / `TEST_PRODUCT_ID` 写进 `HKCU\Environment`，用户重启 Steam 后用普通 Steam 启动即可（ADR-0040 第 9 条）。关闭持久身份时才退回 `start_protected_game.exe` 注入。两种方式缺少时 `consumerCount` 会一直是 0 |

**权限原则**：
- 外壳与引擎始终 `asInvoker`；只有 provider 提权。
- 不使用计划任务/服务常驻；不自动弹 UAC（除用户点「一键启用/启动注视路径」）。

---

## 8. 验收清单（真机）

前提：Windows + NVIDIA、支持的头显、SteamVR、VRChat、MSAA x4。

- [ ] 白名单驱动：步骤 2 一次通过，不弹审计
- [ ] 非白名单驱动：自动审计 → 生成合同 → 二次校验通过；改合同哈希后失败
- [ ] 无 NVIDIA：页面显示"不支持"，不执行私有调用
- [ ] 步骤门控：任一步失败后不进入下一步，重试可恢复
- [ ] PICO 2D 源：`point_space=viewport`，左右眼点各自有效
- [ ] 启动后 `registered=true`、`consumerCount≥1`、`submitFailures=0`
- [ ] 头显内高质量区随上下左右移动，双眼一致
- [ ] 单眼遮挡/失效时不报错、区域不跳到中心
- [ ] 引擎重启后仍 `registered`，区域继续跟随
- [ ] 关闭窗口再打开，步骤状态正确重建
- [ ] 托盘退出后 provider 15s 内消失，驱动注册释放
- [ ] 强杀外壳后 provider 自行退出
- [ ] 页面关闭后一键流程继续；重开页面状态一致

---

## 9. 风险与缓解

| 风险 | 级别 | 缓解 |
|---|---|---|
| 驱动合同随驱动更新失效 | 高 | 自动探测 + 哈希键控缓存；不匹配即重探测；UI 明示"实验合同未验证语义" |
| 探测 PASS 被误解为"一定能用" | 高 | 页面固定文案：探测只查地址结构；必须真机四方向确认 |
| 未文档化 NVAPI vtable | 高 | 独立进程 + 默认关 + 失败不拖累 FGB；补丁最小化 |
| UAC/杀软误报 | 中 | provider 单独 exe、无注入、无计划任务；发布说明解释用途 |
| PICO 串流无 EDID | 中 | `candidate-auto` 真机验证；失败明确报错 |
| provider 文件被占用导致更新失败 | 中 | 更新前强制 stop；失败给具体原因 |
| 自动启动 VRChat 打扰用户 | 中 | 默认不自动；`vrss_auto_launch_vrchat=false`，需显式勾选 |
| 一键流程中途关页面 | 低 | 状态机在引擎侧；页面只读，不承担流程 |
| 用户以为"开了能省 50% GPU" | 中 | 文案写清：VRSS 是**中央抗锯齿/画质**特性，通常增加 GPU 开销，不承诺帧率收益 |

---

## 10. 已确认决策

| # | 决策 | 结论 |
|---|---|---|
| D1 | provider 归属 | **外壳/独立生命周期**，引擎只做数据与控制 |
| D2 | provider 是否随主安装包 | **随包、默认关闭、`runtime/vrss/`** |
| D3 | 是否维护上游补丁 | **是**（lease / csv / 合同参数 / 审计产出 / 错误文件） |
| D4 | 非白名单驱动的自动审计 | **自动执行** |
| D5 | 合同缓存 | **按哈希缓存并复用**，哈希不匹配即重探测 |
| D6 | 一键流程是否自动开 VRChat | **默认不自动**，需显式勾选 |
| D7 | MSAA/VRSS allow 检查 | **步骤 3 一键确认按钮**，点击即进入下一步；v1 不自动读 profile |
| D8 | 页面位置与命名 | 侧栏「VRSS」，标题「VRSS 可变速率超采样」 |
| D9 | 非 `POINT_VIEWPORT` 的 2D 源 | **拒绝启用**并提示修模块 |
| D10 | 校准归属 | **FGB 侧**（UDP 路径 provider 不套 gain） |
| D11 | 无 EDID 头显的身份 | **持久化为用户级环境变量**（固定会话键 `0xFEEE:0x0001`）；Steam 正常启动游戏即可，注入口径降级为兜底 |
| D12 | 是否跟随 SteamVR 自动注册 | **可选开关，默认关**；开启后一轮 SteamVR 会话自动跑一次，取消 UAC 或点停止后本轮不再重试 |

---

## 11. 完成定义（DoD）

- [ ] 阶段 2 产物进入 `dist/runtime/vrss/`，`dist/fgb-cli.exe` 无 VRSS 依赖
- [ ] 独立 VRSS 页面：六步 stepper、一键启用、白名单/自动探测、启动 VRChat、验证与微调
- [ ] 三语文案齐全；无 `window.confirm/alert/prompt`；任何步骤都有进行中/成功/失败反馈
- [ ] 单测 + `tools/probe_vrss_sink.py` + `tools/probe_ipc.py` + `tools/probe_ui.py` 通过
- [ ] 新增 `tools/probe_vrss_flow.py`（mock 审计输出/status.json，验证步骤门控）
- [ ] §8 真机清单全部勾选（至少一台 PICO + 一台白名单驱动 + 一台非白名单驱动探测）
- [ ] `docs/02-features.md`、`03-roadmap.md`、`09-user-guide.md`、ADR-0040、NOTICE 更新
- [ ] Release 包出到 `dist/` 并真的启动过一次
- [ ] 提交 → 推送 → CI 绿
