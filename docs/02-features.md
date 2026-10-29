# 02 · 功能规格书

> 本文定义 FGB 要做什么、做到什么程度、以及明确不做什么。
> 优先级：**P0 = v1.0 必须** / **P1 = v1.x** / **P2 = 有明确价值但可延后** / **P3 = 实验性**

---

## 1. 功能全景

| 域 | 功能 | 优先级 |
|---|---|---|
| **输入** | VRCFT .NET 模块兼容（零改动加载） | P0 |
| | 原生 C ABI 模块 | P1 |
| | VMC 输入 / OSC 通用输入 / ARKit LiveLink | P0 |
| | OpenXR 眼追（`XR_EXT_eye_gaze_interaction`） | P1 |
| | ALVR / Virtual Desktop / PICO Connect 私有协议 | P2 |
| | 摄像头眼追（MediaPipe / OpenSeeFace） | P2 |
| | 多源融合（硬件眼追 + 摄像头表情） | P2 |
| **处理** | Unified Expressions v2 全量形状（约 120 项） | P0 |
| | v1 legacy 参数派生 | P0 |
| | 简化参数（Simplified） | P0 |
| | 滤波链（One Euro / 限速 / 死区） | P0 |
| | 三层校准（中立位 / 注视偏移 / 漂移补偿） | P0 |
| | 校准工作台：3D 预览 / 范围映射 / 部位高亮 | P2（规划，[ADR-0021](adr/0021-calibration-workbench-3d-preview.md)） |
| | 注视估计（聚散度 / 扫视 / 注视点检测） | P1 |
| | 眨眼分类（瞬时/长闭/单眼） | P1 |
| **输出** | OSC → VRChat avatar 参数（表情 + 眼追，默认） | P0 |
| | OSC → VRChat 原生眼追 `/tracking/eye/*`（可切换模式） | P1 |
| | OSCQuery 发现 + 参数相关性裁剪 | P0 |
| | VRChat 自动配置（含备份/还原） | P0 |
| | VMC 输出 | P0 |
| | 共享内存输出（Unity/UE 插件） | P1 |
| | WebSocket JSON 输出 | P1 |
| | UDP 二进制（低延迟，给串流软件） | P2 |
| | 多 Sink 并发 | P0 |
| **管理** | 模块安装 / 启用 / 禁用 / 排序 | P0 |
| | 模块注册表（在线浏览安装） | P1 |
| | 随 SteamVR 启动（OpenVR 自启动注册） | P2（规划，[ADR-0020](adr/0020-steamvr-autostart.md)） |
| | 实时参数监视矩阵 | P0 |
| | 日志查看与导出 | P0 |
| | 配置预设（Profile） | P1 |
| | 自动更新 | P1 |
| **诊断** | 端到端延迟测量 | P1 |
| | 录制 / 回放（`.fgbcap`） | P1 |
| | 丢帧与健康度统计 | P0 |
| **增值** | 注视点数据服务（对外 API） | P1 |
| | OpenXR API Layer（眼追注入 + ETFR 宿主） | P2 |
| | D3D VRS 注入（实验） | P3 |
| | 眼动输入（注视光标 / 停留点击） | P3 |
| | 远程传输（LAN/WebRTC，多机） | P3 |

---

## 2. P0 · VRCFT 对等能力

这一节是"不做到就是残废"的清单。任何一项缺失，用户都会立刻发现"不如 VRCFT"。

### 2.1 模块管理

| 需求 | 细节 |
|---|---|
| 目录扫描 | `%AppData%\VRCFaceTracking\CustomLibs\*.dll`（可切换到 FGB 自己的 `vrcft-libs/`） |
| 元数据 | 读同名 `metadata.json`（模块名、作者、图标、主页） |
| 能力探测 | 通过握手协议拿到 `(SupportsEye, SupportsExpression)` |
| 启用/禁用 | 持久化到配置，重启后保持 |
| 优先级 | 用户可拖拽排序；眼追源和表情源可分别指定 |
| 状态显示 | `未初始化 / 初始化中 / 活跃 / 故障 / 不兼容`，带详细原因 |
| 日志 | 模块日志经 IPC 回传到主日志，可按模块过滤 |
| 并发 | 允许同时启用多个模块（如 A 供眼追、B 供表情） |

**关键体验点**：模块初始化失败是最常见的用户投诉。UI 必须显示**可操作的原因**（"未检测到设备"、"SDK 版本不匹配"、"需要管理员权限"），而不是一个红点。

### 2.2 参数输出

**v2 统一参数（约 120 项）** —— 完整对齐 VRCFT Unified Expressions v2：
```
FT/v2/EyeLidLeft .. EyeSquintRight
FT/v2/BrowDownLeft .. BrowOuterUpRight
FT/v2/CheekPuffLeft .. CheekSquintRight
FT/v2/JawOpen, JawX, JawZ, JawClench
FT/v2/MouthClosed .. MouthTightenerRight
FT/v2/LipPucker* .. LipSuck*
FT/v2/NoseSneerLeft/Right, NasalDilation*, NasalConstrict*
FT/v2/Tongue*（9 项）
FT/v2/NeckFlexLeft/Right, SoftPalateClose, ThroatSwallow
```
（完整名称以官方 Unified Expressions v2 规范为准，实现时以数据驱动方式生成，不硬编码在代码里。）

**眼追参数（默认路径）** —— `FT/v2/EyeLeftX` / `EyeLeftY` / `EyeRightX` / `EyeRightY`（-1..1 归一化视线方向）、`EyeLidLeft` / `EyeLidRight`（0..1，1 = 闭眼）、`PupilDiameterLeft` / `PupilDiameterRight`。

这是现实中**绝大多数模型唯一的眼追消费方式**。VRCFT 也走这条路，FGB 必须逐值对齐——包括 v1 的符号反转（v1 `EyeLid` 1 = 睁眼）。

VRChat 原生眼追（`/tracking/eye/*`）不是默认路径，见 §2.3。

**v1 legacy 参数** —— 由 v2 派生，保证 2022–2023 年的老头像继续工作。派生规则表需要与 VRCFT 逐条对齐并回归测试。

**简化参数（Simplified）** —— `EyeX/EyeY`、`BrowUp`、`MouthSmileLeft` 等聚合参数，供只做了简化绑定的头像使用。

**精度编码** —— float 直发；对只支持 bool 的场景，提供 binary 编码（`Param1/2/4/8...` 位分解）。

### 2.3 VRChat 集成

| 能力 | 细节 | 风险控制 |
|---|---|---|
| OSCQuery 发现 | mDNS `_oscjson._tcp` + HTTP `:9001` 拉参数列表 | 失败则回退到"全量发送" |
| 参数裁剪 | 只发当前 avatar 实际存在的参数 | 未开 OSCQuery 时用本地 avatar 配置 JSON |
| 自动启用 OSC | 改写 VRChat `config.json` | **改前备份 + 单独还原按钮 + 明确告知** |
| 眼追输出模式 | 默认 `parameters`（写 avatar 参数）；可切 `native` / `both` | 切换时明确提示"未适配原生眼追的模型眼睛会不动" |
| 原生眼追（可选） | `/tracking/eye/LeftRightPitchYaw` + `EyesClosedAmount` | 10 s 超时 → 心跳保活 |
| 发送节流 | 死区 + bundle 聚合 + 1 Hz 全量心跳 | 可配置 |

### 2.4 校准

| 能力 | 细节 |
|---|---|
| 中立位训练 | 采集放松状态基线，后续相对偏移 |
| 5 点注视校准 | 屏幕四角 + 中心，最小二乘拟合 2×3 仿射 + 二次项 |
| 残差验证 | 复测 3 点，残差 > 3° 提示重做 |
| 漂移补偿 | 后台检测零点漂移，缓慢修正（时间常数约 60 s） |
| 按源独立 | 每个 source 独立校准数据（硬件换了不用重做） |
| 过期提醒 | 校准数据 > 30 天或换了设备时提示 |

**已实现的部分**是逐形状范围映射（对放松 / 夸张两份抓包取中位数与 95 分位）
和视线 gain/center；5 点注视校准与漂移补偿仍是计划项。

**规划增强（P2，[ADR-0021](adr/0021-calibration-workbench-3d-preview.md)）**：

- 档案热更新与持久化：界面改 lo/hi 立即作用于输出，显式保存到
  `%LOCALAPPDATA%\FaceGazeBridge\calibration.ini`，下次启动自动加载
- 校准工作台：3D 面部预览 + 鼠标旋转 + 实时参数 + 选中部位高亮，
  让"调范围"从盲调变成看得见
- 明确的边界：它是校准的示波器，不是头像预览器；不承诺与 VRChat 头像逐顶点一致，
  v1 也不做照片级写实（详见 §6.5）

### 2.5 UI

参考 VRCFT 的信息架构，但补上 VRCFT 缺的"一屏看懂"：

```
┌─ 状态 ─────────────────────────────────────┐
│  眼追  ● Quest Pro (VD)      延迟 18ms     │
│  表情  ● Quest Pro (VD)      延迟 21ms     │
│  输出  ● VRChat :9000  42/130 参数 ↑      │
│        ○ VMC :39539                       │
├─ 参数 ─────────────────────────────────────┤
│  [实时热力图：120 格，颜色 = 当前值]        │
│  悬停显示名称/数值/更新时间                 │
├─ 模块 ─────────────────────────────────────┤
│  已装 5 / 启用 2 / 故障 0    [浏览注册表]   │
├─ 输出 ─────────────────────────────────────┤
│  [VRChat] [VMC] [共享内存] [WebSocket] ... │
├─ 诊断 ─────────────────────────────────────┤
│  延迟分布  P50 12ms  P99 28ms              │
│  [开始录制] [导出诊断包]                    │
└────────────────────────────────────────────┘
```

**参数热力图**是重点：VRCFT 的参数页是列表，120 个数字滚动查找很难用。改成网格热力图 + 悬停详情，一眼能看出"哪个形状在乱跳"。

### 2.6 输入协议

P0 只做三个，覆盖 80% 场景：

1. **VMC**（UDP 39539）：`/VMC/Ext/Blend/Val` + `/VMC/Ext/Blend/Apply`，外加 `/VMC/Ext/Root/Pos`（头部位姿）
2. **OSC 通用**：用户配置 `地址 → 形状` 映射（表格编辑器），支持缩放/偏移/反相
3. **ARKit LiveLink**（UDP 11111）：52 blendshape + 头部姿态，iPhone 用户的主路径

---

## 3. P1 · 多目标输出

### 3.1 场景

同一份数据同时驱动：

- VRChat（头像）
- VMC → Warudo / VSeeFace（直播画面）
- 共享内存 → Unity 项目（虚拟形象预览 / 研究）
- WebSocket → 网页仪表盘
- UDP → 串流软件（注视点编码）

**这不是"顺手加的"，而是 FGB 与 VRCFT 的根本分野。**

### 3.2 共享内存布局

给 Unity/UE 插件用的零拷贝通道：

```c
// fgb_shm.h — 单生产者单消费者，双缓冲
typedef struct {
    uint32_t magic;          // 'FGB1'
    uint32_t abi_version;
    uint32_t slot_size;
    uint32_t slot_count;     // 2
    volatile uint32_t write_index;
    volatile uint64_t sequence;
    uint8_t  slots[2][/* slot_size */];
} FgbShmHeaderV1;

typedef struct {
    uint64_t capture_ts_ns;
    uint64_t publish_ts_ns;
    uint64_t sequence;
    // 眼球
    float gaze_dir_local[2][3];    // [left,right][xyz]
    float openness[2];
    float pupil_diameter_mm[2];
    uint8_t eye_valid[2];
    // 头部位姿
    float head_quat[4];            // xyzw
    float head_pos[3];
    // 形状
    uint16_t shape_count;
    float    shapes[/* shape_count */];
} FgbFrameV1;
```

读者侧伪代码：
```csharp
// Unity C# 侧
var seq = header->sequence;
if ((seq & 1) != 0) return;              // 写入中
var idx = header->write_index;
Marshal.Copy(...);                        // 读 slots[idx]
if (header->sequence != seq) return;      // 期间被改写，丢弃
```

### 3.3 WebSocket JSON

```json
{
  "t": 1284733,
  "seq": 88421,
  "src": "questpro-vd",
  "eye": {
    "left":  { "open": 0.92, "gaze": [0.02, 0.11, -0.99], "pupil": 4.8, "ok": 1 },
    "right": { "open": 0.90, "gaze": [0.01, 0.10, -0.99], "pupil": 4.7, "ok": 1 },
    "vergence": 1.24
  },
  "head": { "q": [0, 0.03, 0, 0.999], "p": [0, 0.05, 0] },
  "shapes": { "JawOpen": 0.31, "MouthSmileLeft": 0.44, "BrowInnerUp": 0.12 }
}
```
默认只发活跃形状（> 0.01），可配置为全量。

---

## 4. P1 · 注视点数据服务

这是"第三方注视点"最确定能落地的部分：**不碰渲染，只做数据**。

### 4.1 对外 API

```http
GET /api/gaze
{
  "ts_ns": 1284733000,
  "origin_local": [0, 0, 0],
  "dir_local": [0.02, 0.11, -0.99],
  "origin_world": [1.20, 1.62, -0.40],
  "target_world": [1.22, 1.65, -3.10],
  "vergence_distance": 2.74,
  "confidence": 0.93,
  "fixation": true,
  "saccade_vel_deg_s": 3.2,
  "head_pose": { "q": [0,0.03,0,0.999], "p": [0,0.05,0] }
}
```

三个消费者：

1. **串流软件**：把眼追喂给 VD / Steam Link / ALVR 的注视点编码（见 §5.2）
2. **研究/开发**：Python/Unity 直接读，做注意力分析、UI 评估
3. **无障碍**：gaze cursor、dwell click

### 4.2 为什么这个能力有市场

目前的现实是：

- Quest Pro / PICO 4 Pro / Galaxy XR 有眼追，但**只有厂商自己的串流软件能用**
- Steam Frame 有眼追（2 个内摄像头），但只暴露 `XR_EXT_eye_gaze_interaction` 的合并视线，**没有面捕**
- 廉价头显 + 摄像头眼追方案，**没有任何软件能把数据喂给串流层**

FGB 站在中间：统一采集 → 统一校准 → 统一分发。这正是"Bridge"的含义。

---

## 5. P2 · 注视点渲染

这里必须把话说清楚，否则会做出一个没人能用的功能。

### 5.1 三种实现路径的工程现实

| 路径 | 原理 | 可行性 | 收益 | 风险 |
|---|---|---|---|---|
| **A. OpenXR API Layer** | 作为 layer 插在游戏与 runtime 之间，拦截 `xrEndFrame`/`xrCreateSwapchain` | ✅ 高 | 中（依赖游戏） | 低（不改进程内存） |
| **B. D3D/Vulkan 钩子 + VRS** | 注入目标进程，拦截 `ExecuteCommandLists`，每帧注入 shading rate image | ⚠️ 中 | 高（若游戏吃 VRS） | **高**（反作弊拦截、崩溃、兼容性） |
| **C. 引擎内 SDK** | 提供 Unity/UE 插件，游戏自己调 | ✅ 高 | 最高 | 无（但要开发者接入） |

### 5.2 路径 A：OpenXR API Layer（推荐首选）

Layer 能做三件事，价值递增：

**A1 · 眼追注入（最容易，收益立竿见影）**

游戏调 `xrGetActionStatePose` 查 `XR_EXT_eye_gaze_interaction` 时，若 runtime 不提供（多数 PCVR 串流没有），layer 伪造一个可用结果，数据来自 FGB 的共享内存。

效果：**原本不支持眼追的游戏，突然能用了**。适用于任何用 Unity XR / UE OpenXR EyeTracker 的游戏。

```cpp
// 拦截链路
XRAPI_ATTR XrResult XRAPI_CALL fgb_xrGetActionStatePose(
    XrSession session, const XrActionStateGetInfo* info, XrActionStatePose* state)
{
    auto r = next_xrGetActionStatePose(session, info, state);
    if (XR_SUCCEEDED(r) && is_eye_gaze_action(info->action)) {
        auto frame = fgb_shm_read();           // 从 FGB 读最新帧
        if (frame.valid && frame.age_ms < 30) {
            state->isActive = XR_TRUE;         // 关键：伪装可用
            write_gaze_pose(frame);            // 覆盖 pose
        }
    }
    return r;
}
```

**A2 · 注视点渲染委托（收益中等）**

若游戏支持 `XR_VARJO_foveated_rendering` / `XR_VARJO_quad_views`（UE/Unity 的 Varjo 插件支持），layer 可以声称 runtime 支持该扩展，让游戏**自己**渲染 foveal + peripheral 双区视图，layer 负责正确合成。

优点：完全合规，不改游戏渲染逻辑；缺点是支持该扩展的游戏很少（主要是 Varjo 生态和部分 UE 项目）。

**A3 · 合成期 late-latching（收益小但通用）**

在 `xrEndFrame` 时根据最新注视点做重投影微调。收益有限（主要是降低运动到光子延迟），实现复杂，放最后。

### 5.3 路径 B：VRS 注入（务必理解其限制）

**先纠正一个常见误解**：VRS 只能**降低**着色率，不能提高。

```
D3D12_SHADING_RATE  ∈ {1x1, 1x2, 2x1, 2x2, 2x4, 4x2, 4x4}
                      ^^^^ 最精细 = 1 像素 1 个着色样本
```

所以"VRSS"（Variable Rate **Super**sampling）的**中央超采样**部分，第三方无法复刻——那需要游戏以更高分辨率渲染中央区域（多分辨率渲染 MRTR），是引擎侧的能力，NVIDIA 是在驱动层做的。

第三方能做的只有**外围降采样（foveated shading）**：
- 收益：像素着色开销最多降 ~40%（实际 15–25%，因为顶点/几何/带宽不变）
- 观感损失：外围模糊，但人眼本来就看不清

**VRSS 现状**：NVIDIA 2020 年随 Turing 推出，2021 年 VRSS 2 加入 Tobii 眼追（仅 HP Reverb G2 Omnicept），此后基本停止推进，现在 RTX 驱动里已不是主推特性。**它本质上是被平台方放弃的技术路线，不应该成为 FGB 的主攻方向。**

**实现要点（若做）**：
```cpp
// DX12 Tier 2: 每帧生成 shading rate image
// 依赖：device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6) 返回 Tier2
// 注入点：ID3D12GraphicsCommandList4::RSSetShadingRateImage
void inject_frame(ID3D12GraphicsCommandList* cl, const GazeState& g) {
    update_sri_cpu(g);                                  // 按注视点生成 foveal 区域
    cl->CopyBufferRegion(sri_upload, 0, sri_cpu, 0, sri_size);
    cl->ResourceBarrier(...);                           // → COPY_DEST → SHADING_RATE_SOURCE
    cl->RSSetShadingRateImage(sri_resource);
}
```
每帧一次 8 KB 上传，开销可忽略；真正的成本是注入框架（MinHook/Detours）和兼容性维护。

**结论**：路径 B 作为 P3 实验特性，明确标注"仅单机 VR 游戏，**不适用于 VRChat**（EAC 会拦截 DLL 注入）"。

### 5.4 路径 C：串流侧注视点编码（真正的性价比之选）

如果能控制编码环节，注视点带来的带宽节省远大于 VRS 带来的算力节省。

VD 从 1.34.16（2026-02）起支持 eye-tracked foveated streaming；Steam Link 也已支持。它们的共同前提是：**runtime 侧能拿到眼追数据**。

FGB 的机会：
1. 提供标准化 UDP 眼追输出，让串流软件可以直接订阅（需要与 VD/ALVR 作者合作或逆向）
2. 对**没有原生眼追的头显**（如 Quest 3 + 摄像头眼追），提供"虚拟眼追源"

这条路的价值最大，但**依赖外部集成**，不能作为 v1.0 承诺。

### 5.5 能力边界声明（对外文档必须写清楚）

| 说法 | 是否成立 |
|---|---|
| "给不支持眼追的游戏加眼追" | ✅ 成立（OpenXR Layer 注入） |
| "让支持的引擎做注视点渲染" | ✅ 成立（委托 `XR_VARJO_foveated_rendering`） |
| "给任意游戏强制开注视点渲染" | ⚠️ 仅 D3D11/12/Vulkan 游戏，且反作弊环境下不可用 |
| "复刻 NVIDIA VRSS（中央超采样）" | ❌ 不成立，驱动级能力 |
| "省 50% GPU 算力" | ❌ 夸张，现实 10–25%，且只在像素受限场景 |
| "让 VRChat 支持注视点渲染" | ❌ 不成立（EAC；且 VRChat 本身无眼追输入） |

**这份表格要放进产品页和 FAQ。** 过度承诺是这类工具最常见的死法。

---

## 6. P1/P2 · 其他增值功能

### 6.1 录制 / 回放（建议提前到 P0 之后立刻做）

`.fgbcap` 格式：帧序列 + 元数据，**可回放注入引擎**。

价值不在于"用户能录视频"，而在于：
- **回归测试**：录一段真实硬件数据，之后每次改滤波器/映射都用它跑，对比输出差异
- **Bug 复现**：用户上传 10 秒录制，就能复现"某参数抽搐"
- **调参**：离线反复试滤波器参数，不用真人一直做表情

格式设计：
```
[header]  magic "FGBcap\0\0" | version | created_ts | source_meta(json_len+json)
[frame]*  len(u32) | FaceFrame(序列化) | raw_payload_len | raw_payload
[footer]  frame_count | checksum
```

### 6.2 眼动输入

- 注视光标（gaze cursor）：在 VR 里画一个点
- 停留点击（dwell click）：注视某点 > 800 ms 触发
- 眨眼点击：长闭眼 > 400 ms

用途：无障碍、VR 手部不便场景、演示。**注意**：VRChat 里这属于"辅助输入"，不构成作弊，但要在文档里说明用途边界。

### 6.3 远程传输

把追踪数据从 A 机发到 B 机（VRChat 跑在另一台机器 / 云端串流）。

- 局域网：UDP + 简单序列号，延迟 < 2 ms
- 广域网：WebRTC DataChannel，延迟 20–80 ms
- 安全：预共享密钥 + 可选 TLS

### 6.4 双耳音频（远期）

用头部位姿 + 耳廓参数做 HRTF 个性化。这是独立产品级别的工程量（需要 SOFA 数据集、卷积引擎），**不建议 v2.0 前碰**。

### 6.5 校准工作台（P2，规划）

> 详细评估与取舍见 [ADR-0021](adr/0021-calibration-workbench-3d-preview.md)。

校准目前只能改档案再重启，用户看不到"调这个范围会让嘴变成什么样"。校准工作台要解决
的是**可解释性**，不是渲染精度：

| 能力 | 细节 |
|---|---|
| 新「校准」页 | 左侧 3D 视图，右侧 88 个形状的范围编辑；三语文案 |
| 实时预览 | 30 Hz 推送校准前原始值 + 校准后输出值；只在校准页打开时推 |
| 旋转查看 | 鼠标拖动 yaw/pitch，滚轮缩放，双击复位；预设正面 / 45° / 侧面 |
| 范围映射 | 每形状 lo/hi 滑杆 + 「取当前为 lo/hi」；拖动立即生效 |
| 部位高亮 | 选中/悬停某形状时，模型上对应区域（下颌、下唇、左眉…）高亮 |
| 映射状态 | 没有模型可视化的形状标"仅数值"，避免"调了没反应"的黑箱 |
| 保存/重载 | 显式保存 + `.bak`；未保存标记；恢复默认 |
| 降级 | WebGL2 不可用时用 SVG 正面/侧面示意图 + 同样的高亮与滑杆 |

**能力边界（对外要写清楚）**：

| 说法 | 是否成立 |
|---|---|
| "调范围能立刻看到表情变化" | ✅ 成立 |
| "选中参数能知道影响脸上哪块" | ✅ 成立 |
| "能旋转看侧面" | ✅ 成立 |
| "预览和我的 VRChat 头像完全一致" | ❌ 不成立（不导入用户头像） |
| "照片级写实人脸" | ❌ v1 不承诺（风格化写实；照片级需单独立项与资产授权） |

技术约束：不引 Three.js/前端框架（项目规则），自研 WebGL2 小渲染器 + 紧凑
`.fgbface` 模型 + 数据驱动映射表；模型资产 ≤ 3 MB，只在页面可见时渲染。

**已确认（2026-10-29）**：模型走自有风格化写实头 + ~24 morph；调参默认实时影响输出；
"只预览、不生效"的旁路后置。

### 6.6 随 SteamVR 启动（P2，规划）

> 详细评估与取舍见 [ADR-0020](adr/0020-steamvr-autostart.md)。

用官方 OpenVR `IVRApplications` 注册一个 dashboard overlay 清单
（`app.vrmanifest`），再设置 `SetApplicationAutoLaunch`。设置页提供一个显式开关，
并显示四种主要状态（另有错误态）：未安装 SteamVR / SteamVR 未运行 / 已注册未启用 / 已启用。

| 说法 | 是否成立 |
|---|---|
| "SteamVR 启动时自动拉起 FGB" | ✅ 成立（需 SteamVR 正在运行时设置） |
| "能随时关掉自启动" | ✅ 成立（读回校验） |
| "注册后 FGB 会自己启动 SteamVR" | ❌ 不成立（`VRApplication_Background` 语义） |
| "会出现在 Steam 游戏库里" | ❌ 不成立（需要 Steamworks AppID，本项目不通过 Steam 分发） |
| "在头显里提供一个 FGB 面板" | ❌ 不成立（只借用 dashboard overlay 的自启动机制） |

约束：绝不直接改 SteamVR 配置/注册表，绝不自动注册。**上线前置是托盘常驻壳**：
SteamVR 启动后 FGB 只进托盘、不弹窗口；托盘完成前不开放这个功能。引入托盘后，
"关闭窗口 = 停引擎"（ADR-0016）需要改成"关闭窗口 = 收进托盘，托盘退出 = 停引擎"。

---

## 7. 非功能需求

| 类别 | 指标 |
|---|---|
| **启动时间** | 冷启动到首个 OSC 包 < 2.5 s |
| **CPU** | 空闲 < 0.3%（单核）；全速运行 < 2% |
| **内存** | 常驻 < 180 MB（含 WebView2） |
| **延迟** | 软件侧端到端 < 2 ms（见架构 §12） |
| **抖动** | 输出间隔 P99 偏差 < 1.5 ms |
| **稳定性** | 连续运行 8 小时无内存增长 > 5% |
| **兼容性** | Windows 10 19041+ / 11；x64 |
| **无障碍** | 全键盘可操作；支持 200% DPI 缩放 |
| **多语言** | 简中 / 英 / 日（VRChat 主要用户群） |
| **隐私** | 摄像头画面**不出进程**；遥测默认关闭且明示 |
| **3D 预览（规划）** | 校准页 60 fps 目标 / 30 fps 可接受；预览流量 ≤ 50 KB/s；模型 ≤ 3 MB |
| **桌面集成（规划）** | 未注册/未启动 SteamVR 时不影响冷启动与追踪；注册失败给具体原因 |
| **卸载** | 不留残留注册表项与配置文件（可选保留） |

---

## 8. 明确不做

范围控制比功能列表更重要：

| 不做 | 原因 |
|---|---|
| 硬件驱动 / 固件 | 不是我们的战场，厂商 SDK 足够 |
| 头像制作 / 参数绑定工具 | Unity 生态的活 |
| 串流协议栈（编码/传输） | 与 VD/ALVR 竞争毫无胜算，做上游数据即可 |
| 修改 VRChat 客户端 | 违反 ToS，EAC 拦截 |
| 云端数据处理 | 延迟不可接受 + 隐私风险 |
| 通用动作捕捉（全身） | 是另一个产品 |
| 移动端 App | 输入端已有成熟方案（LiveLink/MeowFace） |
| 照片级真人预览 / 导入 VRChat 头像 | VRChat 头像文件加密且授权复杂；预览页的职责是校准反馈，不是头像展示 |
| 注册为 Steam 库应用 / 直接改 SteamVR 配置 | 需要 Steamworks AppID；未公开配置格式不可靠（见 ADR-0020） |
| VR 内 dashboard 面板 | 只借用 overlay 的自启动机制，不做面板 UI |

---

## 9. 功能验收的总标准

一句话：**VRCFT 能跑的场景，FGB 必须能跑，且延迟不高于它、稳定性不低于它。**

具体化为三条硬指标：

1. VRCFT 官方注册表里排名前 10 的模块，FGB 全部能加载并正常出数据
2. VRChat 官方 VRCFT 测试头像（含 v1 与 v2 参数），表情与眼动在视觉上与 VRCFT 无差异
3. 连续 8 小时挂机，输出中断次数 = 0
