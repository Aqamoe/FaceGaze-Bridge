# 原生模块开发指南（C ABI v2）

面向想给 FaceGaze Bridge（下称 FGB）写输入模块的开发者。读完这份文档，你应该能：

- 用 C/C++ 编译一个 DLL
- 通过 `fgb_module_query_v2` 暴露身份与回调
- 在 `update()` 里按属性提交表情 / 眼睛 / 注视 / 头姿
- 打包成 `.zip`，在 FGB 的「模块库 → 原生模块」或「模块」页安装并选中

参考实现：[`samples/native-sine`](../samples/native-sine)（全通道：表情 + 眼睛 + 头姿）、
[`samples/native-face`](../samples/native-face)（仅表情）、
[`samples/native-gaze`](../samples/native-gaze)（仅独立注视，含 2D-only 模式）、
[`samples/native-openness`](../samples/native-openness)（只给睁眼度/瞳孔）。
都不需要硬件。头文件：[`sdk/fgb_module_api.h`](../sdk/fgb_module_api.h)。

> **ABI v1 已移除**（[ADR-0034](adr/0034-native-module-v2-only.md)）。
> `FgbFrameSlotV1` / `FgbEyeV1` / `FgbGazeV1` / `fgb_module_query` / `FGB_CAP_EYE` /
> `FGB_GAZE_*` 都不再存在。用旧头编译的 DLL 会加载失败并提示重新编译。
> 迁移对照见 §12。

---

## 1. 前置条件

- Windows x64
- 任意能产出 x64 DLL 的工具链（MSVC / clang-cl / MinGW 皆可；示例用 MSVC + CMake）
- 不需要安装 VRCFT，也不需要 .NET

## 2. 一个模块由什么组成

```
my-module/
  fgb-native.json        ← 身份清单（宿主不加载 DLL 就能列出模块）
  MyModule.dll           ← 实现
  （可选）其它依赖 DLL / config.ini
```

`fgb-native.json` 与 VRCFT 的 `module.json` 字段相同，另加几个可选字段：

```json
{
  "ModuleId": "com.example.mydevice",
  "ModuleName": "My Device Face Tracking",
  "AuthorName": "Your Name",
  "Version": "1.0.0",
  "ModuleDescription": "Face tracking for ...",
  "UsageInstructions": "Install the vendor runtime, then enable eye tracking in its settings.",
  "ModulePageUrl": "https://github.com/you/my-module",
  "DllFileName": "MyModule.dll",

  "AbiVersion": 2,
  "Capabilities": ["expression", "eyes"],
  "RoleHint": "expression",
  "MinHostVersion": "1.1.0"
}
```

| 字段 | 必需 | 说明 |
|---|---|---|
| `ModuleId` | 是 | 反向域名或 GUID；安装后模块仓目录就是它 |
| `DllFileName` | 是 | 必须与实际 DLL 文件名一致 |
| `UsageInstructions` | 可选 | 使用说明。模块卡展开后原样显示，写清"装完还要做什么"（驱动、串流设置、重启顺序） |
| `ModulePageUrl` | 可选 | 模块主页（`https://`）。展开卡片时显示为链接，由系统默认浏览器打开 |
| `AbiVersion` | 建议 | 只接受 `2`。写 1 或更低的模块在列表里直接标成不兼容 |
| `Capabilities` | 建议 | `expression` / `eyes` / `gaze` / `head`；运行时以模块自报为准 |
| `RoleHint` | 可选 | 界面默认建议的角色，不改变实际加载 |
| `MinHostVersion` | 可选 | 高于当前宿主版本时标记为不兼容 |

宿主只在加载前用清单做列表/兼容性提示；加载后一律以 `fgb_module_query_v2`
返回的 `capabilities` 为准，两者不一致会告警。

`ModuleDescription` / `UsageInstructions` / `ModulePageUrl` 也会出现在界面上：
「模块」页和「模块库 → 原生模块」的卡片点开后显示完整描述、使用说明和模块页链接。
没写 `UsageInstructions` 时不显示该段；链接只接受元数据里登记过的 `http(s)` 地址，
由引擎用系统默认浏览器打开，不会让 WebView2 自己导航走。

## 3. ABI 生命周期

宿主只查找一个导出符号：

```c
const FgbModule* fgb_module_query_v2(uint32_t host_abi_version);
```

时序：

```
LoadLibrary
  → fgb_module_query_v2(FGB_MODULE_ABI_V2)   // 版本不匹配返回 NULL
  → initialize(&self, host, params)          // 返回 1 成功
  → update(self)                             // 引擎线程按 tick 调用
  → update(self) ...
  → teardown(self)
FreeLibrary
```

**线程模型**：`update` 在引擎线程同步调用；`commit` 只能在 `initialize` / `update`
的调用栈里执行。不要在模块自己起的线程里写帧（ABI 不做跨线程保护）。
如果模块需要收包/读设备，可以在内部起线程，但提交帧必须回到 `update` 里完成。

模块崩溃会带走引擎——需要崩溃隔离时把实例设成跨进程宿主（§11），
那种模式下模块仍然只写宿主分配的帧槽，代码不用改。

## 4. 最小实现

```cpp
#include "fgb_module_api.h"
#include <cstring>

struct Instance {
    const FgbHostServices* host;
    int32_t jaw_open;
};

static int initialize(void** out, const FgbHostServices* host, const FgbInitParams* params) {
    auto* self      = new Instance();
    self->host      = host;
    self->jaw_open  = host->shape_index(host->ctx, "JawOpen");
    *out            = self;
    (void)params;
    return 1;
}

static void update(void* opaque) {
    auto* self = static_cast<Instance*>(opaque);
    FgbFrameSlot* slot = self->host->acquire_frame_slot(self->host->ctx);
    if (slot == nullptr) {
        return;
    }
    slot->attributes = FGB_ATTR_EXPRESSION;
    if (self->jaw_open >= 0) {
        slot->shapes[self->jaw_open]      = 0.5f;   // 0..1
        slot->shape_valid[self->jaw_open] = 1;
    }
    self->host->commit_frame_slot(self->host->ctx, slot, 0);
}

static void teardown(void* opaque) { delete static_cast<Instance*>(opaque); }

extern "C" __declspec(dllexport)
const FgbModule* fgb_module_query_v2(uint32_t host_abi_version) {
    if (host_abi_version != FGB_MODULE_ABI_V2) {
        return nullptr;
    }
    static const FgbModule api = {
        sizeof(FgbModule), FGB_MODULE_ABI_V2,
        "com.example.mydevice", "My Device", FGB_CAP_EXPRESSION,
        &initialize, &update, &teardown,
    };
    return &api;
}
```

关键点：

- **`attributes` 是本帧的"数据声明"**。只置你真的填了的属性位；没置的字段宿主不会读，
  0 也不会被当成"闭眼""看正前方"。
- **能力位决定这一路进哪个通道**（见 §5 的表）。写了属性却没声明能力位时宿主会告警一次，
  数据仍被采用，但界面/角色选择看不到这一路。
- 形状用 `host->shape_index(ctx, "JawOpen")` 按名字查下标，**不要硬编码 88 项顺序**。
  未知名字返回 `-1`，跳过即可。
- 每帧只填实际有效的形状，并把 `shape_valid[i]` 置 1；不填的保持 0。
- `initialize` 返回 0 会显示在模块卡片上；`report_status(ERROR, ...)` 的文本也会。
- `params->host_version` 是宿主版本（0 = 旧宿主没给），只用于日志/适配提示。

## 5. 能力位与属性位

能力位（`FgbModule::capabilities`，也写进清单的 `Capabilities`）：

| 能力位 | 清单字符串 | 含义 | 进入的通道 |
|---|---|---|---|
| `FGB_CAP_EXPRESSION` | `expression` | 88 个 Unified Expressions 形状 | Expression |
| `FGB_CAP_EYES` | `eyes` | 逐眼属性：openness / pupil / 方向 / 点，可以是任意子集 | Eyes（头像与渲染回退） |
| `FGB_CAP_GAZE` | `gaze` | 独立注视：逐眼/合并方向 + 可选 2D 点 | Gaze（渲染/foveation） |
| `FGB_CAP_HEAD` | `head` | 头姿 | Head |

属性位（`FgbFrameSlot::attributes`）：

| 属性 | 说明 |
|---|---|
| `FGB_ATTR_EXPRESSION` | 88 形状 |
| `FGB_ATTR_LEFT/RIGHT_OPENNESS` | 睁眼度 |
| `FGB_ATTR_LEFT/RIGHT_PUPIL` | 瞳孔 |
| `FGB_ATTR_LEFT/RIGHT_GAZE` | 逐眼方向（头像眼睛方向） |
| `FGB_ATTR_COMBINED_GAZE` | 合并方向（渲染注视） |
| `FGB_ATTR_LEFT/RIGHT_POINT` | 逐眼 2D 注视点 |
| `FGB_ATTR_VERGENCE` | 聚散距离（米） |
| `FGB_ATTR_HEAD` | 头姿 |
| `FGB_ATTR_POINT_VIEWPORT` | 帧级：本帧 2D 点已标定到该眼显示视口（§6.2） |

> **眼睛拆开给是合法的**。只给 openness（PICO 互联那类独立眼动链路）就声明
> `EYES` 并只置 `FGB_ATTR_*_OPENNESS`，不会驱动渲染注视；只给注视点就声明 `GAZE`，
> 不会碰头像的睁眼度。这正是 v2 相对 v1 的核心差别（v1 的 `EYE` 要求整眼原子）。

## 6. 眼睛与注视怎么填

### 6.1 逐眼属性

```c
slot->attributes |= FGB_ATTR_LEFT_OPENNESS | FGB_ATTR_RIGHT_OPENNESS |
                    FGB_ATTR_LEFT_GAZE | FGB_ATTR_RIGHT_GAZE;
slot->left_eye.openness          = 0.7f;      /* 0 闭眼，1 完全睁开 */
slot->left_eye.openness_confidence = 0.9f;
slot->left_eye.gaze_dir[0]     = dx;          /* HMD 局部单位向量，-Z 前 */
slot->left_eye.gaze_dir[1]     = dy;
slot->left_eye.gaze_dir[2]     = dz;
slot->left_eye.gaze_confidence = 0.9f;
slot->left_eye.openness_ts_ns  = device_ts_ns; /* 0 = 用帧的 capture_ts_ns */
```

- 方向会被宿主归一化；全零/NaN 退回 `-Z`，但有效性仍按你置的属性位走，
  所以**不要把"还没算出来"写成方向属性**。
- 逐眼属性可以来自不同时刻：openness/pupil/gaze 各有自己的时间戳。

### 6.2 2D 注视点（ADR-0033）

坐标约定：**该眼"最终看到的那块画面"的归一化坐标**，`x/y ∈ [0,1]`，原点左上，
`+x` 向右、`+y` 向下，畸变前，逐眼独立（不是物理面板，也不是眼动相机图像）。

```c
slot->attributes |= FGB_ATTR_LEFT_POINT | FGB_ATTR_RIGHT_POINT | FGB_ATTR_POINT_VIEWPORT;
slot->left_eye.gaze_point_2d[0]  = u;   /* 该眼视口归一化 */
slot->left_eye.gaze_point_2d[1]  = v;
slot->right_eye.gaze_point_2d[0] = u_r;
slot->right_eye.gaze_point_2d[1] = v_r;
```

| 你有的数据 | 该置的属性 | 宿主给出的结果 |
|---|---|---|
| 逐眼方向 | `LEFT/RIGHT_GAZE`（`COMBINED_GAZE` 可选，缺了由左右眼推） | 渲染注视 `valid=true`，逐眼方向/角度、注视/扫视判定可用 |
| 逐眼 2D 点，**已标定到该眼视口** | `LEFT/RIGHT_POINT \| POINT_VIEWPORT` | `valid=false`（不编方向）；点可用，`point_space="viewport"`，能用于 foveation |
| 逐眼 2D 点，**只有原始坐标** | 只置 `LEFT/RIGHT_POINT` | `valid=false`；点可用但 `point_space="source"`，只能展示/看趋势 |
| 方向 + 点 | 上面的组合 | 方向与点都可用 |

- **映射由模块负责**，宿主不做换算：

  ```c
  u = px / viewport_w;  v = py / viewport_h;             /* 像素 → 视口归一化 */
  u = (x_ndc + 1.0f) * 0.5f;  v = (1.0f - y_ndc) * 0.5f; /* NDC → 视口归一化 */
  ```

- 越界（看向画外、标定误差）不是错误，宿主**保留原值不裁剪**，消费端自己 clamp；
- `NaN`/`Inf` 会把该眼的点标为无效（另一只眼不受影响）；
- 声明了 `POINT_VIEWPORT` 却连续 30 帧明显越界（单轴超出 `[-0.25,1.25]`）时宿主告警一次。

**两条会被宿主点名的一次性告警**（只记日志，不改数据）：

| 指纹 | 含义 |
|---|---|
| 置了方向属性、方向恰好是 `(0,0,-1)`、同时有点 | 模块没有方向模型却伪装了方向；应只置点位属性，否则 `fixation` 永远成立 |
| 置了 `POINT_VIEWPORT` 却持续越界 | 标定或声明错了；没有标定就只置 `LEFT/RIGHT_POINT` |

### 6.3 头姿与聚散

```c
slot->attributes |= FGB_ATTR_HEAD;
slot->head_rotation[0] = qx;  /* x,y,z,w，HMD 局部 */
slot->head_rotation[1] = qy;
slot->head_rotation[2] = qz;
slot->head_rotation[3] = qw;
slot->head_position[0] = px;  /* 米，HMD 局部 */
```

聚散（`FGB_ATTR_VERGENCE` + `vergence_distance_m`）跟着 `EYES` 通道走，只给注视的模块
不需要填它。

## 7. 宿主服务

`FgbHostServices` 提供：

| 服务 | 用途 |
|---|---|
| `log(ctx, level, message)` | 写入引擎日志，`level` 为 `FGB_LOG_*` |
| `report_status(ctx, state, detail)` | 上报启动/活跃/错误/停止；错误会显示在模块卡片 |
| `acquire_frame_slot` | 取得本帧可写槽（宿主已清零） |
| `commit_frame_slot` | 提交本帧；`capture_ts_ns` 可填 0 由宿主补时间 |
| `shape_index(ctx, name)` | Unified Expressions 形状名 → 下标 |

初始化参数 `FgbInitParams`：

- `tick_hz`：引擎当前 tick 频率
- `module_dir`：模块目录（UTF-8），可用于读写自己的 `config.ini`
- `host_version`：宿主版本（UTF-8，可能为 NULL）

## 8. 编译

### 8.1 独立 CMake 工程（推荐）

示例目录自带 [`CMakeLists.txt`](../samples/native-sine/CMakeLists.txt)：

```powershell
cmake -S samples/native-sine -B build/native-sample -G "Visual Studio 17 2022" -A x64
cmake --build build/native-sample --config Release
```

产物在 `build/native-sample/Release/fgb-sample-native.dll`。
把 `fgb-native.json` 与 DLL 放在同一目录，压缩成 zip 即可安装。

### 8.2 直接命令行（MSVC）

```bat
cl /nologo /LD /O2 /EHsc /std:c++20 /I sdk MyModule.cpp /Fe:MyModule.dll
```

注意：

- 必须 **x64**，与 FGB 的位数一致
- 只导出 `fgb_module_query_v2`；C++ 内部随便用 STL，但不要跨 ABI 传 STL 对象
- 如果模块依赖其它 DLL，把它们一起放进 zip

## 9. 打包与安装

zip 根目录直接放清单和 DLL（可再带 config.ini）：

```powershell
Compress-Archive -Path fgb-native.json,MyModule.dll -DestinationPath MyModule.zip
```

安装方式：

1. **界面**：「模块库 → 原生模块 → 安装原生模块 (.zip)」，或「模块」页「从 .zip 安装」
2. **命令行**：`fgb-cli modules install MyModule.zip`

安装后：

1. 到「模块」页把模块「设为启动」
2. 点「重启引擎」
3. 状态页应该看到帧率/输入源，模块卡片会显示运行状态或具体错误

## 10. 多模块组合

| 需求 | 做法 |
|---|---|
| A 面捕 + B 眼睛 | `EYES` 来自 B，`expression` 角色给 A |
| A 面捕 + B 注视点 | 头像用 A，渲染注视用 B 的 `GAZE` |
| A 的 openness + B 的 gaze | B 声明 `GAZE`；`--eye-source A --gaze-source B --avatar-eye-from-gaze`，或直接 `--attr-source left-openness=A --attr-source left-gaze=B` |
| 左眼 A、右眼 B | `--eye-left-source A --eye-right-source B` |
| A 只给 openness、不给 gaze | A 声明 `EYES` 且只置 openness 属性 |

配置方式有两种：

- 界面「模块」页：把模块「添加为实例」，再为四个角色选来源。角色按 `module_id`
  写入 `modules.ini` 的 `[roles]`，实例写入 `[instances]`；改实例列表后重启引擎。
- CLI：重复 `--source vrcft --module-id ...`，或用 `[instances]` 直接启动一批实例：

```powershell
fgb-cli run `
  --source vrcft --module-id com.example.pico-face `
  --source vrcft --module-id com.example.pico-gaze `
  --expression-source "native[1]" --gaze-source "native[2]"
```

`modules.ini` 示例：

```ini
[instances]
face = com.example.pico-face
gaze = com.example.pico-gaze

# 可选：只让这个实例走跨进程宿主
[instance_host]
gaze = 1

[roles]
expression = com.example.pico-face
eyes       = com.example.pico-face
gaze       = com.example.pico-gaze
head       = com.example.pico-face
```

### 10.1 属性级选源

```powershell
# 同一只眼的 openness 来自 A、pupil 来自 B、gaze 来自 C
fgb-cli run --source vrcft --module-id <A> --source vrcft --module-id <B> `
  --source vrcft --module-id <C> `
  --attr-source left-openness="native[1],native[2]" `
  --attr-source left-pupil="native[2]" `
  --attr-source left-gaze="native[3]"

# 列表里多个源都新鲜时，按置信度加权平均（openness/pupil/gaze）
fgb-cli run ... --attr-source left-openness="native[1],native[2]" --attr-fuse
```

- 属性名：`left/right-openness`、`left/right-pupil`、`left/right-gaze`、`vergence`。
  显式列表找不到源时该字段为空，不会回退到列表外。
- `--attr-fuse` 让列表里多个新鲜源按置信度加权平均；默认取第一个可用源。
  vergence 没有置信度，融合模式下仍取第一个。
- 逐属性列表只影响**头像眼睛**；渲染注视继续由 `--gaze-source` /
  `--gaze-left/right-source` 选择。
- 合并结果会逐眼记录来源与捕获时间；左右眼来自不同源时，`Eyes` 通道的整通道
  来源记为未知，诊断以逐眼字段为准。

### 10.2 新鲜度

每个通道独立判过期：表情 500 ms、眼睛 250 ms、注视 100 ms、头姿 500 ms
（CLI 可用 `--expression-stale-ms` / `--eye-stale-ms` / `--gaze-stale-ms` /
`--head-stale-ms` 覆盖）。状态页会显示每个通道的来源与年龄。

头像输出默认只看 `Eyes`；渲染/foveation 用 `Gaze`，没有时回退到眼睛反算。
所以"面捕 A + 注视 B"不会让 B 接管头像眼睛。

## 11. 跨进程宿主

```powershell
fgb-cli run --source vrcft --native-host
# 或显式指定宿主 exe
fgb-cli run --source vrcft --native-host-exe "D:\FGB\fgb-host-native.exe"
```

- 每个原生模块一个 `fgb-host-native.exe` 进程；引擎通过 `Local\FGB_NativeHost_*`
  共享内存读帧，模块崩溃/卡死不会阻塞引擎。
- 崩溃/心跳超时后默认自动重启最多 3 次（退避），不阻塞引擎 tick；
  `--no-host-restart` 可关闭。自动重启会重新 initialize，模块状态不保留。
- 模块页实例行有「停止 / 启动 / 重启实例」：热替换 DLL 时先「停止」（宿主退出、
  文件解锁），替换模块文件后再「启动」，不需要重启整个引擎。
- 宿主模式暂不支持 `--record`；模块日志经共享内存 ring 回传引擎日志，同时写到
  `%LOCALAPPDATA%\FaceGazeBridge\logs\native-host-*.log`。
- 发布包需要 `fgb-host-native.exe` 与主程序放在一起；缺失时自动回退进程内。
  协议布局见 [`sdk/fgb_native_host_shm.h`](../sdk/fgb_native_host_shm.h)。
- 崩溃隔离是**进程级**的，不是沙箱：模块仍以当前用户身份运行，读写文件不改权限。

## 12. 从 v1 迁移

v1 的结构和符号全部删除（[ADR-0034](adr/0034-native-module-v2-only.md)）。对照：

| v1 | v2 |
|---|---|
| `fgb_module_query` | `fgb_module_query_v2` |
| `FgbModuleV1` | `FgbModule` |
| `FgbHostServicesV1` / `FgbInitParamsV1` | `FgbHostServices` / `FgbInitParams` |
| `FgbFrameSlotV1` | `FgbFrameSlot`（末尾多了 `attributes` 和逐属性时间戳） |
| `FGB_CAP_EYE`（整眼原子） | `FGB_CAP_EYES`（逐眼属性，任意子集） |
| `FGB_CAP_GAZE` + `FgbGazeV1` | `FGB_CAP_GAZE` + `FGB_ATTR_*_GAZE` / `FGB_ATTR_*_POINT` |
| `FgbEyeV1.openness` | `FGB_ATTR_*_OPENNESS` + `left_eye.openness` |
| `FgbEyeV1.gaze_dir` | `FGB_ATTR_*_GAZE` + `left_eye.gaze_dir` |
| `FgbEyeV1.valid` | 没有逐眼 valid：该眼的属性位没置就是没数据 |
| `FgbGazeV1.valid` 的 `FGB_GAZE_*` 位 | `FGB_ATTR_*` 位（方向与点各自独立） |
| `FGB_GAZE_POINT_VIEWPORT` | `FGB_ATTR_POINT_VIEWPORT` |
| `slot->has_head_pose` | `FGB_ATTR_HEAD` |
| `slot->has_vergence` | `FGB_ATTR_VERGENCE` |

机械迁移三步：

1. 把导出改成 `fgb_module_query_v2`，结构换名，`FGB_MODULE_ABI_V1` → `FGB_MODULE_ABI_V2`。
2. 在每帧开头按"这帧真的有什么"拼 `attributes`，而不是无条件填所有字段。
3. `EYE` 拆成 `EYES`（头像）+ `GAZE`（渲染）；只给注视的模块只声明 `GAZE`。

不需要改的：形状名查下标、`acquire/commit` 生命周期、日志/状态上报、打包与安装流程。

## 13. 限制与版本演进

- **默认是进程内加载**：模块崩溃会带走引擎。`--native-host` 可让原生模块走
  独立进程（§11）；默认关闭。
- `update` 不要阻塞（读设备建议自带线程 + 非阻塞队列，提交仍回到 `update`）。
- 结构只允许在末尾追加字段；宿主对 `struct_size` 做兼容读取，模块始终用当前 SDK 头编译。
- 帧槽由宿主分配，模块写"可能不存在"的字段前先比较 `slot->struct_size`。
- 原生模块没有在线注册表；通过 `.zip` / `.dll` 分发和安装。

## 14. 常见问题

| 现象 | 原因 |
|---|---|
| 模块列表里没有 | `fgb-native.json` 缺失或 `ModuleId` 为空；或在错误的目录层级 |
| 列表里标「不兼容」 | 清单 `AbiVersion` 不是 2，或 `MinHostVersion` 高于当前宿主 |
| 加载失败“用的是已移除的 ABI v1” | 用旧 SDK 头编译过；换当前 `sdk/fgb_module_api.h` 重新编译 |
| 加载失败“没有导出 fgb_module_query_v2” | 忘记 `extern "C" __declspec(dllexport)`，或编译成了 C++ 修饰名 |
| ABI 版本不兼容 | 模块返回了框架外的 `abi_version`；重新用当前 SDK 头编译 |
| 卡片显示故障 | 看模块 `report_status(ERROR, ...)` / 日志里的具体原因；初始化返回 0 也会显示 |
| 帧率 0 | `update` 没有 `commit`，或 `attributes` 一直是 0 |
| 眼睛不动但表情正常 | 没声明 `FGB_CAP_EYES`，或没置 `FGB_ATTR_*_OPENNESS/GAZE` |
| 注视一直「注视中」、方向恒 `(0,0,-1)` | 置了方向属性却没有方向模型（伪造方向）；只置 `LEFT/RIGHT_POINT` |
| 状态页注视是 `—`、但逐眼有 `2D (x,y)` | 正常：只报点的源没有方向，`valid=false` |
| 逐眼显示的是 `2D~ (x,y)` | 模块没置 `FGB_ATTR_POINT_VIEWPORT`：点在来源自己的空间（原始坐标） |
| 位数错误 | DLL 不是 x64，`LoadLibrary` 会失败并给出错误码 |
