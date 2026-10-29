# ADR-0021 · 校准工作台：实时 3D 面部预览与范围映射

- 状态：**提议**（2026-10-29 评估；方向已确认，未实施）
- 日期：2026-10-29
- 影响：`engine` 新增预览取样、`params/calibration` 支持热更新与持久化、
  `web/ipc` 新增校准命令与 30 Hz 预览消息、`ui/` 新增校准页与 WebGL 渲染器、
  `tools/` 新增模型导出与探针、`docs/02-features.md`、`docs/03-roadmap.md`

## 问题

当前校准是"离线推导 + 改档案再试"：

```powershell
fgb-cli run --source vrcft --calibration me.ini
fgb-cli calibrate neutral.fgbrec --express express.fgbrec --out me.ini
```

用户看不到"我调这个 lo/hi，嘴会变成什么样"，也看不出"JawOpen 到底影响脸上哪一块"。
需求是：

1. 新开一个校准页面
2. 渲染一个面部模型，实时把面捕参数画在模型上
3. 鼠标拖动旋转，能看侧面
4. 调节范围映射（如嘴巴）时，模型立即变化
5. 调某个参数时，模型上对应部位高亮

这个需求里**渲染不是最难的**，难的是三件事：把 88 个协议形状映射到一张脸上、
让"原始值 → 校准 → 输出"的链路可解释、以及"写实模型"的资产从哪来。

## 先把目标说清楚

| 目标 | 是不是本页面的职责 |
|---|---|
| 看参数方向、幅度、左右是否对称 | ✅ 核心 |
| 调 lo/hi 时立刻看到表情变化 | ✅ 核心 |
| 选中参数时知道它影响脸上哪一块 | ✅ 核心 |
| 旋转看侧面/下颌/嘴角 | ✅ 核心 |
| 和 VRChat 里自己的头像逐顶点一致 | ❌ 不做；那需要导入用户头像，超出范围 |
| 照片级写实 | ⚠️ v1 不做；见"资产选型" |
| 5 点注视校准 | ❌ 不在本页面；现有校准只覆盖形状与视线偏移 |

一句话：**它是校准的示波器，不是头像预览器。** 这句话决定了资产精度可以低于
VRChat 头像，但映射关系必须诚实。

## 现状与缺口

- 校准档案只在启动时通过 `--calibration` 加载；`CalibrationApplier` 的 profile
  没有运行时更新入口（`set_profile` 存在，但 IPC 没暴露，也没有锁/双缓冲策略）。
- UI 的 `params` 消息是 10 Hz，365 个值，够热力图，不够面部动画（100 ms 台阶肉眼可见）。
- `params` 里前 88 个是**校准 + 滤波之后**的形状值；校准页还需要**校准前**的原始值，
  否则用户不知道 lo/hi 在原始尺度上的位置。
- 没有"默认档案"：界面即使能写档案，下次启动也没人加载它。
- 没有 3D 资源、没有形状→部位/形态的映射表。

## 数据通路（建议）

```
source → sanitize → PreviewTap(raw 快照) → Calibration → OneEuro → Snapshot
                          │                                      │
                          └──────── 30 Hz preview（仅页面打开） ──┘
                                        │
                                        ▼
                                  UI 校准页
```

- **PreviewTap** 是独立 processor，始终挂在校准之前；即使没有校准档案也能拿到原始值。
  它把 `raw[88] + eye + seq` 写进 seqlock 快照，写者永不阻塞引擎；IPC 30 Hz 读。
- **预览用 30 Hz**、单独消息，不复用 10 Hz 的 `params`；只有校准页订阅时才推。
  88×2 个 float 的 JSON 约 1.4 KB/帧，30 Hz 约 42 KB/s，可接受。
- **输出值**取 `engine.latest()`（校准 + 滤波之后），这样模型看到的就是实际发出去的。
- 原始值只用于画"当前值在 lo/hi 之间的位置"，不直接驱动模型。

## 校准档案的运行时更新

### 1. 默认路径与优先级

```
--calibration <path>                              显式指定，最高优先级
%LOCALAPPDATA%\FaceGazeBridge\calibration.ini      默认档案（界面保存到这里）
无                                                 不做校准（但 PreviewTap 仍工作）
```

外壳当前不传 `--calibration`；实现默认档案后，界面保存的映射在下次启动自动生效，
不需要改启动参数。

### 2. 热更新，不重启

为了让界面能随时开始校准，**面板模式下即使没有档案也要挂一个空的
`CalibrationApplier`**（全 passthrough，不改变输出）；否则用户打开校准页时
没有可写的 profile，只能重启引擎。校准档案本身只有 88 个 `(lo, hi, active)` 加一组
视线参数（约 1.5 KB）。控制线程写、引擎线程每帧读，建议：

- 双缓冲 + 原子索引（seqlock 语义）：写者填非活动槽，翻转索引；读者读活动槽，
  前后校验版本号
- 或 copy-on-write：构造新 `shared_ptr<const CalibrationProfile>` 原子交换
- **不要**在 `process()` 里拿 mutex；这是 120 Hz 热路径

### 3. 保存是显式的，生效是实时的

**默认实时生效（2026-10-29 确认）**：拖动滑杆立即改 `CalibrationApplier` 的 profile，
预览和实际输出同时变化；不做"只预览、不生效"的旁路模式（后置为可选增强）。
这样用户看到的反馈就是真实输出，不会出现"预览好看、VRChat 里却不是这样"的偏差。

- 拖动滑杆 → 立即 `set_profile` → 下一帧预览与输出同时变化（这就是"实时"）
- 点「保存」→ 先写临时文件、flush、再原子替换 `calibration.ini`，并保留 `.bak`
- 页面有「未保存」标记；离开页面前用应用内确认框提示
- 「重新加载」丢弃未保存改动；「恢复默认」把所有形状设为 passthrough

INI 格式保持兼容，现有 `fgb-cli calibrate` 与手改档案都不受影响；新增字段只能是
注释或向后兼容的键。

## 形状 → 模型：映射表与模型格式

### 1. 数据驱动，不硬编码

新增 `ui/face/face_map.json`（或随模型资产一起放）：

```json
{
  "model": "head_v1.fgbface",
  "regions": ["jaw", "chin", "lowerLip", "upperLip", "mouthCornerL",
              "mouthCornerR", "cheekL", "cheekR", "browL", "browR",
              "nose", "eyelidL", "eyelidR", "tongue", "neck"],
  "morphs": [
    { "name": "jawOpen",   "shapes": { "JawOpen": 1.0, "JawMandibleRaise": -0.5 } },
    { "name": "smileL",    "shapes": { "MouthCornerPullLeft": 0.7,
                                      "MouthCornerSlantLeft": 0.3 } },
    { "name": "pucker",    "shapes": { "LipPuckerUpperLeft": 0.5,
                                      "LipPuckerLowerRight": 0.5 } }
  ],
  "shape_regions": {
    "JawOpen": ["jaw", "lowerLip", "mouthInterior"],
    "MouthCornerPullLeft": ["mouthCornerL", "cheekL"],
    "BrowInnerUpLeft": ["browL"]
  }
}
```

- 一个模型形态可以由多个形状加权；一个形状也可以影响多个模型形态
- 权重允许为负（例如 `JawMandibleRaise` 抵消 `JawOpen` 的下颌下移）
- 最终权重 clamp 到 0..1；左右分别建 morph，避免"左嘴抽动带动右嘴"
- 不在 `shape_regions` 里的形状 = 模型无法可视化：列表里必须标"仅数值"，
  否则用户调了半天看不到变化，会以为坏了

### 2. 模型格式：自定义紧凑二进制 `.fgbface`

不引 glTF 运行时，也不把 88 组顶点位移塞进 JSON。v1 定义：

```
magic "FGBF" | version u32 | flags u32
vertex_count u32 | index_count u32 | morph_count u16 | region_count u8
positions   f32[3*N]
normals     i8[3*N]          // snorm，省一半
indices     u32[M]
region_mask u32[N]           // 每顶点一个位掩码，支持多区域
morph_names [u8 len + utf8] * K
morph_deltas f16[3*N*K]      // 半精度，视觉足够
可选：UV + PNG 皮肤贴图（不烘焙进二进制，单独放）
```

- 目标预算：≤ 8k 顶点、≤ 24 个 morph、≤ 3 MB（含贴图）
- 导出脚本放 `tools/`（Blender → `.fgbface`），资产源文件不进仓库或放 LFS
- 以后要兼容外部资产（glTF/VRM）再单独评估，不在 v1 范围

### 3. 渲染器：自研 WebGL2，不引 Three.js

| 方案 | 优点 | 缺点 | 结论 |
|---|---|---|---|
| Three.js / Babylon | 成熟、glTF、轨道控制、PBR 一步到位 | 违反"原生 JS、不引第三方库"；包体与供应链负担；为一个页面引入整套引擎 | v1 不用；若要做照片级外部模型，单独 ADR 评估 |
| 自研 WebGL2 | 无依赖、体积小、高亮/区域完全可控、与项目风格一致 | 需要自己写网格/相机/光照/morph，约 600–1000 行 | **采纳** |
| CSS 3D / SVG 2D | 最简单 | 不能真正旋转看侧面，不能表达下颌/嘴唇体积 | 只做降级方案 |
| 外部预览（VMC → VSeeFace / Warudo） | 零开发，用户可能已有 | 不能做部位高亮与范围联动，依赖外部软件 | 作为补充说明，不替代校准页 |

渲染要点：

- 轨道相机：指针拖动 yaw/pitch（pitch 限制 ±80°），滚轮缩放，双击复位；
  预设「正面 / 45° / 侧面」
- 三点布光 + wrap diffuse + 轻微次表面红晕；无环境贴图，避免额外资产
- morph 在 CPU 端算还是 GPU 端算：
  - v1 用 CPU 端（8k 顶点 × 24 morph × 3 分量 ≈ 58 万次乘加/帧），每帧更新动态 VBO，
    实现简单、调试直观
  - 若低端机掉帧，再改为 morph 位移贴图 + 顶点着色器采样
- 只在页面可见时跑 `requestAnimationFrame`；切走就停，不占 GPU/CPU

### 4. 区域高亮

- 每个顶点带 32 位 `region_mask`；选中/悬停某个形状时，把该形状的
  `shape_regions` 并集成一个 `u_highlight_mask`，片元着色器对命中区域叠一层
  自发光/描边色
- 支持多区域（如 `JawOpen` 同时高亮下颌、下唇、口腔）
- 反向也做一点：当前值 > 阈值的形状，其区域给极轻的"活跃"着色，
  让用户一眼看到"脸在动的地方"；默认关，避免干扰
- 2D 降级：WebGL2 不可用时，显示一张 SVG 正面/侧面示意图，区域用同一份
  `shape_regions` 高亮；滑杆与保存仍可用。**不允许因为显卡/远程桌面就整页不可用。**

## 校准页的交互

```
┌ 校准 ───────────────────────────────────────────────────────────┐
│ [实时预览]  已加载 calibration.ini · 2 个形状已校准   [保存] [重载] │
├───────────────────────────────┬─────────────────────────────────┤
│                               │ 搜索 [Jaw________] 只看已校准 ☐  │
│       3D 面部（可拖动旋转）     │ ─────────────────────────────── │
│                               │ JawOpen        原 0.42  出 0.78  │
│       [正面] [45°] [侧面]      │ [lo 0.05]====●========[hi 0.95] │
│                               │ [取当前为 lo] [取当前为 hi] [启用]│
│                               │ MouthCornerPullLeft …            │
│                               │ TongueOut       仅数值（无模型）  │
└───────────────────────────────┴─────────────────────────────────┘
```

- 选中行 → 模型高亮对应部位；滑杆变化 → 下一帧预览更新（≤ 33 ms）
- 每个形状显示：原始值、输出值、lo/hi、是否 active、是否可可视化
- 「取当前为 lo/hi」是最小可用的校准方式：让用户做放松/夸张表情时点一下；
  后续可加"3 秒自动取中位数/95 分位"（复用现有 `derive_profile` 的统计口径）
- 视线单独一小节：gain、center；5 点注视校准仍走后续计划，不混在这个页面
- 所有文案 zh/en/ja 三语；失败/保存/重载都有 toast；禁止 `window.confirm`

## 需要新增的 IPC

| 方向 | 消息 | 说明 |
|---|---|---|
| →UI | `preview`（30 Hz，仅订阅时） | `raw[88]`、`out[88]`、眼睁开度/视线、seq |
| UI→ | `set_preview {enabled}` | 打开/关闭校准页时订阅 |
| →UI | `calibration` | profile 全量：每形状 lo/hi/active、视线参数、路径、dirty |
| UI→ | `set_calibration_shape {name, lo?, hi?, active?}` | 热更新，立即生效 |
| UI→ | `set_calibration_eye {gain_x, gain_y, center_x, center_y, active}` | 视线 |
| UI→ | `capture_calibration {shape?, which:"lo"\|"hi"}` | 用当前原始值写入 |
| UI→ | `save_calibration` / `reload_calibration` / `reset_calibration` | 持久化/丢弃/恢复 |
| →UI | `ack`（沿用） | 每条命令回执 |

`params` 的 10 Hz 不变；校准页的动画只走 `preview`。HTTP 只读接口可加
`/api/preview` 便于探针，但不是必须。

## 性能与资源预算

| 项 | 预算 | 备注 |
|---|---|---|
| 预览消息 | 30 Hz，≤ 50 KB/s | 仅页面打开时 |
| UI 进程内存 | +30 MB 以内 | WebView2 现有预算 < 180 MB |
| UI CPU | 校准页打开时 < 5%（单核） | 8k 顶点 CPU morph |
| 模型资产 | ≤ 3 MB | 含贴图；进 `dist/ui/face/` |
| 引擎热路径 | 零额外开销 | PreviewTap 只做定长拷贝/seqlock |
| 帧率 | 60 fps 为佳，30 fps 可接受 | 只在页面可见时渲染 |

## 明确不做

- 不导入 VRChat 头像（`.vrca` 加密；也不应逆向）
- 不追求照片级写实；v1 用"风格化写实"的通用人头
- 不做完整 ARKit 52 blendshape 兼容层；只做 FGB 需要的 ~24 个 morph
- 不做面部捕捉/摄像头预览；数据只来自引擎
- 不把 3D 预览做成独立窗口或第二进程；它就是一个 UI 页面

## 资产选型：写实度的真实成本

这是本功能最大的不确定性，必须说清楚：

| 路线 | 写实度 | 成本 | 风险 |
|---|---|---|---|
| A · 自研/委托一个中性人头 + ~24 morph | 风格化写实，能看清结构 | 建模/绑定 4–8 人日（外包另计） | 质量依赖作者；艺术返工 |
| B · 现成 ARKit blendshape 资产 | 较高 | 授权与适配 3–10 人日 | 许可常不明确；52 vs 88 映射损失；包体大 |
| C · 用户自带 glTF/VRM | 取决于用户 | 加载器/兼容 10+ 人日 | 格式碎片化；不是校准页的核心价值 |
| D · 程序化低模头（代码生成） | 低（卡通） | 2–3 人日 | 看不出真实嘴型，校准价值打折 |

**选定 A（2026-10-29）**：一张自己拥有版权的中性人头（可用 CC0 扫描头做底模）
+ 手工 morph + 区域标注。它不追求照片级，但能准确表达下颌开合、嘴角、嘴唇、眉毛、
眼睑、脸颊。B/C/D 不作为 v1 路线；如果以后产品确认必须照片级，再单独立项评估 B，
并接受授权与包体代价。

## 分期与工作量（建议）

| 阶段 | 内容 | 人日 |
|---|---|---|
| P1 · 数据链路 | PreviewTap、profile 热更新、默认档案、保存/重载、IPC 命令 | 4–6 |
| P2 · 校准页（无 3D） | 形状列表、原始/输出、lo/hi 滑杆、取当前、i18n | 3–5 |
| P3 · 渲染器 | `.fgbface` 加载、WebGL2、轨道相机、光照、morph、区域高亮、2D 降级 | 5–8 |
| P4 · 资产 | 人头建模/绑定/morph/区域标注/映射表（已确认：自有风格化写实头） | 4–8 |
| P5 · 验证 | 单测（映射完整性、INI 往返、profile 热更新）、探针、性能 | 2–4 |
| **合计** | 不含照片级资产 | **18–31 人日** |

依赖顺序：P1 → P2 可独立交付（先把校准变得可用）；P3 依赖 P4 的模型；
P5 贯穿。**P2 完成就能让用户受益**，3D 是增强而不是前置条件。

## 验证

- [ ] 校准页 1 s 内出现面部；拖动可旋转，侧面能看清下颌/嘴唇；双击复位
- [ ] 选中 `JawOpen`，模型下颌/下唇/口腔高亮；选中 `BrowInnerUpLeft`，左侧眉区高亮
- [ ] 拖动 `JawOpen` 的 hi，模型在 100 ms 内变化，同时 VRChat/VMC 输出也变化
- [ ] 原始值/输出值与 `/api/params` 前 88 项一致（探针断言）
- [ ] 保存后重启引擎仍生效；`--calibration` 显式档案优先于默认档案
- [ ] 没有形状映射的参数显示"仅数值"，不出现"调了没反应"的黑箱
- [ ] WebGL2 不可用时出现 2D 降级，滑杆/保存仍可用，并有明确提示
- [ ] 校准页关闭后不再推 `preview`；其他页面仍 10 Hz；引擎 tick 抖动无变化
- [ ] `restart_engine` 后页面保持原样，预览自动重订阅，3D 模型不重新加载/不白屏
- [ ] 探针通过（页面渲染、数值变化、高亮状态、保存/重载）

## 风险

| 风险 | 缓解 |
|---|---|
| 模型资产做不出"像人"的效果 | 先交付 P1/P2；3D 用风格化写实，不承诺照片级；资产外包前先做原型 |
| 88 形状到 24 morph 的映射失真 | 映射表数据驱动、可调；页面标"仅数值"；用真实抓包做回归 |
| 30 Hz 预览影响引擎 | PreviewTap 只在面板开启时挂；seqlock 读取；预览只在页面可见时推 |
| WebGL2 在远程桌面/低端机不可用 | 2D SVG 降级；启动时检测并提示 |
| profile 热更新的线程安全 | 双缓冲/COW，热路径无锁；单测覆盖并发读写 |
| 范围调错导致头像表情异常 | 保存前可「重载/恢复默认」；`.bak` 备份；实时生效但可一键回退 |

## 已确认（2026-10-29）

1. **模型路线**：自有风格化写实头 + ~24 morph（路线 A）；不做照片级、不导入用户头像。
2. **生效方式**：默认实时影响输出；"只预览、不生效"的旁路后置。
3. **自动取 lo/hi**：仍待定。先做「取当前为 lo/hi」；"录制 3 秒自动取统计值"
   作为 P2 之后的增强（复用 `derive_profile` 的中位数/95 分位口径）。
