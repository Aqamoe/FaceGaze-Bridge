# 原生模块开发指南（C ABI v1）

面向想给 FaceGaze Bridge（下称 FGB）写输入模块的开发者。读完这份文档，你应该能：

- 用 C/C++ 编译一个 DLL
- 通过 `fgb_module_query` 暴露身份与回调
- 在 `update()` 里提交表情 / 眼追 / 独立注视 / 头姿
- 打包成 `.zip`，在 FGB 的「模块库 → 原生模块」或「模块」页安装并选中

参考实现：[`samples/native-sine`](../samples/native-sine)（完整通道）、
[`samples/native-face`](../samples/native-face)（仅表情）、
[`samples/native-gaze`](../samples/native-gaze)（仅独立注视）、
[`samples/native-openness`](../samples/native-openness)（ABI v2，仅 openness/pupil）；
都不需要硬件。头文件：[`sdk/fgb_module_api.h`](../sdk/fgb_module_api.h)。

> **能力粒度（先读这一段）**
> `FGB_CAP_EYE` 是**整块原子能力**：声明了它，就必须同时给出 gaze、openness 和 pupil。
> 只提供注视点、不提供睁眼度的模块（例如 PICO 互联的独立眼动链路）**不要**声明
> `EYE`，否则头像会因为 openness 缺失而闭眼；这类模块应声明 `FGB_CAP_GAZE` 并填
> `FgbGazeV1`。`EYE` 与 `GAZE` 可以同时声明。v1.1 已经支持多模块按角色组合
> （面捕 / 眼睛 / 注视 / 头姿），见 §11；属性级组合（A 的 openness + B 的 gaze、
> 左右眼分源）仍是 v2 方向。

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

`fgb-native.json` 与 VRCFT 的 `module.json` 字段相同：

```json
{
  "ModuleId": "com.example.mydevice",
  "ModuleName": "My Device Face Tracking",
  "AuthorName": "Your Name",
  "Version": "1.0.0",
  "ModuleDescription": "Face tracking for ...",
  "ModulePageUrl": "https://github.com/you/my-module",
  "DllFileName": "MyModule.dll"
}
```

`ModuleId` 建议用反向域名或 GUID，安装后模块仓目录就是它。
`DllFileName` 必须与实际 DLL 文件名一致。

## 3. ABI 生命周期

宿主只查找一个导出符号：

```c
const FgbModuleV1* fgb_module_query(uint32_t host_abi_version);
```

时序：

```
LoadLibrary
  → fgb_module_query(FGB_MODULE_ABI_V1)      // 版本不匹配返回 NULL
  → initialize(&self, host, params)          // 返回 1 成功
  → update(self)                              // 引擎线程按 tick 调用
  → update(self) ...
  → teardown(self)
FreeLibrary
```

**线程模型（v1）**：`update` 在引擎线程同步调用；`commit` 只能在 `initialize` / `update`
的调用栈里执行。不要在模块自己起的线程里写帧（v1 不做跨线程保护）。
如果模块需要收包/读设备，可以在内部起线程，但提交帧必须回到 `update` 里完成。

## 4. 最小实现

```cpp
#include "fgb_module_api.h"
#include <cstring>

struct Instance {
    const FgbHostServicesV1* host;
    int32_t jaw_open;
};

static int initialize(void** out, const FgbHostServicesV1* host, const FgbInitParamsV1*) {
    auto* self = new Instance();
    self->host = host;
    self->jaw_open = host->shape_index(host->ctx, "JawOpen");
    *out = self;
    return 1;
}

static void update(void* opaque) {
    auto* self = static_cast<Instance*>(opaque);
    FgbFrameSlotV1* slot = self->host->acquire_frame_slot(self->host->ctx);
    if (slot == nullptr) {
        return;
    }
    if (self->jaw_open >= 0) {
        slot->shapes[self->jaw_open] = 0.5f;      // 0..1
        slot->shape_valid[self->jaw_open] = 1;
    }
    self->host->commit_frame_slot(self->host->ctx, slot, 0);
}

static void teardown(void* opaque) { delete static_cast<Instance*>(opaque); }

extern "C" __declspec(dllexport)
const FgbModuleV1* fgb_module_query(uint32_t host_abi_version) {
    if (host_abi_version != FGB_MODULE_ABI_V1) {
        return nullptr;
    }
    static const FgbModuleV1 api = {
        sizeof(FgbModuleV1), FGB_MODULE_ABI_V1,
        "com.example.mydevice", "My Device", FGB_CAP_EXPRESSION,
        &initialize, &update, &teardown,
    };
    return &api;
}
```

关键点：

- `capabilities` 声明你能提供哪些通道：`FGB_CAP_EXPRESSION / FGB_CAP_EYE /
  FGB_CAP_GAZE / FGB_CAP_HEAD`；ABI v2 属性模块用 `FGB_CAP_EYE_ATTRIBUTES`。
  只声明真正有数据的通道；合并层只认声明。
- `FGB_CAP_EYE` 是 v1 的整眼能力（gaze + openness + pupil）。**只给注视点**的模块不能借用它，
  应声明 `FGB_CAP_GAZE` 并写 `slot->gaze`（见 §11.2）。
- 只给 openness/pupil、或只给单眼数据的模块应使用 ABI v2 的
  `FGB_CAP_EYE_ATTRIBUTES` + `FgbFrameSlotV2`（见 §11.5）。
- 眼追填 `slot->left_eye/right_eye`；独立注视填 `slot->gaze`；头姿填
  `has_head_pose/head_rotation/head_position`。
- 形状用 `host->shape_index(ctx, "JawOpen")` 按名字查下标，**不要硬编码 88 项顺序**。
  未知名字返回 `-1`，跳过即可。
- 每帧只填实际有效的形状，并把 `shape_valid[i]` 置 1；不填的保持 0。
- 眼追填 `slot->left_eye/right_eye` 的 `valid/openness/gaze_dir`（-Z 为前，右手系），
  头姿填 `has_head_pose/head_rotation/head_position`。

完整示例见 [`samples/native-sine/native_sine.cpp`](../samples/native-sine/native_sine.cpp)，
它还演示了眼追、头姿、日志和状态上报。

## 5. 宿主服务

`FgbHostServicesV1` 提供：

| 服务 | 用途 |
|---|---|
| `log(ctx, level, message)` | 写入引擎日志，`level` 为 `FGB_LOG_*` |
| `report_status(ctx, state, detail)` | 上报启动/活跃/错误/停止；错误会显示在模块卡片 |
| `acquire_frame_slot` | 取得本帧可写槽（宿主已清零） |
| `commit_frame_slot` | 提交本帧；`capture_ts_ns` 可填 0 由宿主补时间 |
| `shape_index(ctx, name)` | Unified Expressions 形状名 → 下标 |

初始化参数 `FgbInitParamsV1`：

- `tick_hz`：引擎当前 tick 频率
- `module_dir`：模块目录（UTF-8），可用于读写自己的 `config.ini`

## 6. 编译

### 6.1 独立 CMake 工程（推荐）

示例目录自带 [`CMakeLists.txt`](../samples/native-sine/CMakeLists.txt)：

```powershell
cmake -S samples/native-sine -B build/native-sample -G "Visual Studio 17 2022" -A x64
cmake --build build/native-sample --config Release
```

产物在 `build/native-sample/Release/fgb-sample-native.dll`。
把 `fgb-native.json` 与 DLL 放在同一目录，压缩成 zip 即可安装。

### 6.2 直接命令行（MSVC）

```bat
cl /nologo /LD /O2 /EHsc /std:c++20 /I sdk MyModule.cpp /Fe:MyModule.dll
```

注意：

- 必须 **x64**，与 FGB 的位数一致
- 只导出 `fgb_module_query`；C++ 内部随便用 STL，但不要跨 ABI 传 STL 对象
- 如果模块依赖其它 DLL，把它们一起放进 zip

## 7. 打包与安装

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

## 8. 无硬件调试

用示例模块验证整条链路：

- 构建 `fgb-sample-native.dll`
- 把它和 `samples/native-sine/fgb-native.json` 放进 zip 安装
- 选中并重启，状态页帧率应 > 0，参数热力图出现正弦变化

引擎日志里原生模块的 tag 是 `native`；模块通过 `host->log` 输出的内容也会带上模块名。

## 9. 限制与版本演进

- **默认是进程内加载**：模块崩溃会带走引擎。`--native-host` 可让原生模块走
  `fgb-host-native.exe` 独立进程（V2 阶段 C MVP），崩溃不再影响引擎；默认关闭
- 界面可以配置多个启动实例并按“表情 / 眼睛 / 注视 / 头姿”选来源；CLI 还支持
  左右眼/左右注视分源与 `--avatar-eye-from-gaze`。ABI v2 属性帧支持只给
  openness/pupil 或单眼数据。见
  [ADR-0030](adr/0030-native-module-v2-composition.md)、
  [ADR-0031](adr/0031-native-module-v2-attribute-fusion.md)
- `update` 不要阻塞（读设备建议自带线程 + 非阻塞队列，提交仍回到 `update`）
- v1 结构只允许在末尾追加字段；v2 是独立的 `FgbFrameSlotV2`，通过
  `fgb_module_query_v2` 选择，两者并存
- 宿主对 `struct_size` 做兼容读取；请始终用当前 SDK 头编译
- 原生模块没有在线注册表；通过 `.zip` / `.dll` 分发和安装

## 10. 常见问题

| 现象 | 原因 |
|---|---|
| 模块列表里没有 | `fgb-native.json` 缺失或 `ModuleId` 为空；或在错误的目录层级 |
| 加载失败“没有导出 fgb_module_query” | 忘记 `extern "C" __declspec(dllexport)`，或编译成了 C++ 修饰名 |
| ABI 版本不兼容 | 用了旧/新 SDK 头；重新用 `sdk/fgb_module_api.h` 编译 |
| 卡片显示故障 | 看模块 `report_status(ERROR, ...)` / 日志里的具体原因；初始化返回 0 也会显示 |
| 帧率 0 | `update` 没有 `commit`，或 `shape_valid` 全 0（表情通道不会启用） |
| 位数错误 | DLL 不是 x64，`LoadLibrary` 会失败并给出错误码 |

## 11. 多模块与注视通道（ADR-0030 v1.1，已实现）

### 11.1 能表达的组合

| 需求 | 能否做到 | 做法 |
|---|---|---|
| A 面捕 + B 整眼 | ✅ | A 供 Expression，B 供 Eyes |
| A 完整 + B 注视点 | ✅ | 头像用 A，渲染注视用 B 的 `GAZE` |
| A 面捕 + B 整眼 + C 注视 | ✅ | 三路各取所需 |
| A 的 openness + B 的 gaze | ✅ | v1 模块用 `--eye-source A --gaze-source B --avatar-eye-from-gaze`；v2 模块可直接只给 openness |
| 左眼 A、右眼 B | ✅（CLI） | `--eye-left-source A --eye-right-source B` |
| A 只给 openness、不给 gaze | ✅（ABI v2） | 声明 `FGB_CAP_EYE_ATTRIBUTES`，只填 openness 属性位 |

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

# 可选：只让这个实例走跨进程宿主（原生模块）
[instance_host]
gaze = 1

[roles]
expression = com.example.pico-face
eyes       = com.example.pico-face
gaze       = com.example.pico-gaze
head       = com.example.pico-face
```

### 11.2 写一个注视模块

声明 `FGB_CAP_GAZE`，不要声明 `EYE`；在 `update()` 里填 `slot->gaze`。它位于
`FgbFrameSlotV1` 末尾，旧宿主不会分配这段内存，所以写之前必须检查 `struct_size`：

```c
const size_t needed = offsetof(FgbFrameSlotV1, gaze) + sizeof(FgbGazeV1);
if (slot->struct_size >= needed) {
    slot->gaze.valid = FGB_GAZE_LEFT | FGB_GAZE_RIGHT |
                       FGB_GAZE_COMBINED | FGB_GAZE_POINT_2D;
    slot->gaze.left_dir[0] = dx;   /* HMD 局部单位向量，-Z 前 */
    slot->gaze.left_dir[1] = dy;
    slot->gaze.left_dir[2] = dz;
    /* right_dir / combined_dir 同上 */
    slot->gaze.left_point_2d[0]  = x; /* 暂定 [0,1]，左上原点 */
    slot->gaze.left_point_2d[1]  = y;
    slot->gaze.confidence = 0.9f;
    slot->gaze.capture_ts_ns = device_ts_ns; /* 0 = 用帧时间戳 */
}
```

- 方向会被宿主归一化；非法/NaN 会被丢弃。
- `FGB_GAZE_POINT_2D` 只表示点字段可用，是否按眼由 `LEFT/RIGHT` 位决定。
- 2D 点约定尚未与串流/OpenXR 消费端对齐，暂时只做透传。
- 合并层每个通道独立选源、独立新鲜度：表情 500 ms、眼睛 250 ms、注视 100 ms
  （CLI 可用 `--*-stale-ms` 覆盖）。状态页会显示每个通道的来源与年龄。
- 头像输出默认只看 `Eyes`；渲染/foveation 用 `Gaze`，没有时回退到眼睛反算。
  所以“面捕 A + 注视 B”不会让 B 接管头像眼睛。

### 11.3 模块清单的可选字段

```json
{
  "AbiVersion": 1,
  "Capabilities": ["expression", "gaze"],
  "RoleHint": "gaze",
  "MinHostVersion": "1.0.2"
}
```

宿主只在加载前用它们做列表/兼容性提示；加载后以 `fgb_module_query` 的
`capabilities` 为准。旧清单不写这些字段也照常工作。`MinHostVersion` 高于当前
宿主版本时，界面会标记为不兼容。

### 11.4 属性级眼睛合成（V2 阶段 A，v1 输入）

V2 阶段 A 让头像眼睛可以拆开取值，但默认行为仍是 v1.1：

```powershell
# A 提供 openness/pupil，B 提供注视方向；头像眼睛方向改用 B，睁眼度仍来自 A
fgb-cli run --source vrcft --module-id <A> --source vrcft --module-id <B> `
  --eye-source "native[1]" --gaze-source "native[2]" --avatar-eye-from-gaze

# 左眼来自 A、右眼来自 B（两个都必须是 EYE 模块）
fgb-cli run --source vrcft --module-id <A> --source vrcft --module-id <B> `
  --eye-left-source "native[1]" --eye-right-source "native[2]"

# 渲染注视也按左右分源
fgb-cli run ... --gaze-left-source "native[1]" --gaze-right-source "native[2]"

# B.2：同一只眼的 openness 来自 A、pupil 来自 B、gaze 来自 C。
# 列表按优先级取第一个新鲜且提供该属性的源；未列出的属性回退整眼策略。
fgb-cli run --source vrcft --module-id <A> --source vrcft --module-id <B> `
  --source vrcft --module-id <C> `
  --attr-source left-openness="native[1],native[2]" `
  --attr-source left-pupil="native[2]" `
  --attr-source left-gaze="native[3]"
```

- 不传这些参数时，左右眼来自同一个 `EYE` 源，头像眼睛不会被 `Gaze` 通道改写。
- `--avatar-eye-from-gaze` 只替换方向；openness/pupil 仍来自 `EYE` 源。
- `--attr-source` 的属性名：`left/right-openness`、`left/right-pupil`、
  `left/right-gaze`、`vergence`。显式列表找不到源时该字段为空，不会回退到列表外。
- 逐属性列表只影响**头像眼睛**；渲染注视继续由 `--gaze-source` /
  `--gaze-left/right-source` 选择。
- 合并结果会逐眼记录来源与捕获时间；左右眼来自不同源时，`Eyes` 通道的整通道
  来源记为未知，诊断以逐眼字段为准。
- 这仍是 V2 阶段 A：模块必须用 v1 ABI 的整眼 `FgbEyeV1`。真正“只给 openness”
  或“只给单眼”的模块要等阶段 B 的 `FgbFrameSlotV2` 属性掩码。

### 11.5 ABI v2：属性帧

只提供部分眼睛属性的模块使用 `FgbFrameSlotV2`，不要伪造 `FgbEyeV1`：

```c
const FgbModuleV2* fgb_module_query_v2(uint32_t host_abi_version) {
    if (host_abi_version != FGB_MODULE_ABI_V2) return NULL;
    return &kModule; // capabilities = FGB_CAP_EYE_ATTRIBUTES
}
```

`update()` 里按属性填写：

```c
FgbFrameSlotV2* slot = host->acquire_frame_slot(host->ctx);
if (slot->struct_size < sizeof(FgbFrameSlotV2)) return;

slot->attributes = FGB_ATTR_LEFT_OPENNESS | FGB_ATTR_RIGHT_OPENNESS |
                   FGB_ATTR_LEFT_PUPIL | FGB_ATTR_RIGHT_PUPIL;
slot->left_eye.openness  = 0.7f;
slot->right_eye.openness = 0.6f;
slot->left_eye.openness_ts_ns = device_ts_ns; /* 0 = 帧时间戳 */
slot->capture_ts_ns = device_ts_ns;
host->commit_frame_slot(host->ctx, slot, 0);
```

属性位（`FgbFrameSlotV2::attributes`）：

| 属性 | 说明 |
|---|---|
| `FGB_ATTR_EXPRESSION` | 88 形状 |
| `FGB_ATTR_LEFT/RIGHT_OPENNESS` | 睁眼度 |
| `FGB_ATTR_LEFT/RIGHT_PUPIL` | 瞳孔 |
| `FGB_ATTR_LEFT/RIGHT_GAZE` | 逐眼方向 |
| `FGB_ATTR_COMBINED_GAZE` | 合并方向 |
| `FGB_ATTR_LEFT/RIGHT_POINT` | 2D 点 |
| `FGB_ATTR_VERGENCE` | 聚散距离 |
| `FGB_ATTR_HEAD` | 头姿 |

- 宿主优先查 `fgb_module_query_v2`；没有或返回空时回退 v1 `fgb_module_query`。
- v2 模块的 `capabilities` 用 `FGB_CAP_EYE_ATTRIBUTES` 表示“按属性提供眼睛”；
  仍可同时声明 `EXPRESSION / GAZE / HEAD`。
- 只给 openness 的模块不会产生渲染注视；宿主用 `EyeState.gazeValid` 区分
  “真的给了方向”和“默认正前方”。只给 gaze 的模块不会驱动头像 openness。
- 参考实现：[`samples/native-openness`](../samples/native-openness)。

### 11.6 跨进程宿主（V2 阶段 C）

```powershell
fgb-cli run --source vrcft --native-host
# 或显式指定宿主 exe
fgb-cli run --source vrcft --native-host-exe "D:\FGB\fgb-host-native.exe"
```

- 每个原生模块一个 `fgb-host-native.exe` 进程；引擎通过 `Local\FGB_NativeHost_*`
  共享内存读帧，模块崩溃/卡死不会阻塞引擎。
- 崩溃/心跳超时后默认自动重启最多 3 次（退避），不阻塞引擎 tick；
  `--no-host-restart` 可关闭。自动重启会重新 initialize，模块状态不保留。
- 模块页实例行的「重启实例」会停宿主再拉起；DLL 在宿主退出期间解锁，
  替换文件后点它即可加载新 DLL，不需要重启整个引擎。
- 宿主模式暂不支持 `--record`；模块日志经共享内存 ring 回传引擎日志，同时写到
  `%LOCALAPPDATA%\FaceGazeBridge\logs\native-host-*.log`。
- 发布包需要 `fgb-host-native.exe` 与主程序放在一起；缺失时自动回退进程内。
  协议布局见 [`sdk/fgb_native_host_shm.h`](../sdk/fgb_native_host_shm.h)。

### 11.7 PICO 类设备怎么拆

| 模块 | 能力 | 数据来源 |
|---|---|---|
| `pico-connect-face` | `EXPRESSION` | PICO 互联面捕协议 |
| `pico-connect-gaze` | `GAZE`（可选 openness/pupil） | PICO 互联内部 RPC / OpenXR 眼动 |
| `pico-full`（可选） | `EXPRESSION \| EYE \| GAZE \| HEAD` | 头显 APK |

这样面捕模块和注视模块可以独立更新、独立故障；PICO 互联接口变化时只换
`pico-connect-gaze`，不影响面捕。详细设计与开放问题见
[ADR-0030](adr/0030-native-module-v2-composition.md)。
