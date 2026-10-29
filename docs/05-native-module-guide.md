# 原生模块开发指南（C ABI v1）

面向想给 FaceGaze Bridge（下称 FGB）写输入模块的开发者。读完这份文档，你应该能：

- 用 C/C++ 编译一个 DLL
- 通过 `fgb_module_query` 暴露身份与回调
- 在 `update()` 里提交表情 / 眼追 / 头姿
- 打包成 `.zip`，在 FGB 的「模块库 → 原生模块」或「模块」页安装并选中

参考实现：[`samples/native-sine`](../samples/native-sine)（无硬件，正弦波输出）。
头文件：[`sdk/fgb_module_api.h`](../sdk/fgb_module_api.h)。

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

- `capabilities` 声明你能提供哪些通道：`FGB_CAP_EXPRESSION / FGB_CAP_EYE / FGB_CAP_HEAD`。
  只声明真正有数据的通道；合并层只认声明。
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

- **v1 是进程内加载**：模块崩溃会带走引擎；需要隔离时后续会上跨进程 ABI v2
- 引擎一次只加载一个模块（原生或 VRCFT 二选一）
- `update` 不要阻塞（读设备建议自带线程 + 非阻塞队列，提交仍回到 `update`）
- ABI 结构只允许在末尾追加字段；任何破坏性改动都会升 `abi_version`
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
