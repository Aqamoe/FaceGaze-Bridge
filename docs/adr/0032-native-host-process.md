# ADR-0032 · 原生模块跨进程宿主

- 状态：**已实施（阶段 C.2）**
- 日期：2026-10-30
- 影响：新增 `fgb-host-native.exe`、`sdk/fgb_native_host_shm.h`、
  `src/sources/native_host_source.*`、CLI/打包；`NativeModuleSource` 继续作为
  in-proc 路径与宿主内部的模块加载器
- 关系：接 [ADR-0030](0030-native-module-v2-composition.md) §6、
  [ADR-0031](0031-native-module-v2-attribute-fusion.md) 阶段 C

## 问题

v1/v2 原生模块都是进程内加载：模块崩溃、死循环或阻塞会直接拖垮引擎；模块 DLL
被进程占用，更新只能重启整个引擎；厂商模块（PICO 内部 RPC、私有 SDK）稳定性不可控。

需要在**不改变模块 ABI**的前提下，把单个模块放进独立进程，引擎只读共享内存。

## 决定

### 1. 一模块一进程，宿主复用现有加载器

`fgb-host-native.exe` 负责加载一个模块：

```text
fgb-host-native.exe --dll <path> --module-id <id> --module-name <name>
                    --display-name <name> --shm <name> --tick-hz <hz>
```

宿主内部直接复用 `NativeModuleSource`（v1/v2 自动选择、属性帧转换、错误上报都走
同一条已验证路径），所以跨进程不需要第二套 ABI 解析。

引擎侧 `NativeHostSource` 实现 `ISource`：

- `CreateProcessW` 启动宿主，分配 Job Object 并设置
  `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`，引擎退出/崩溃时宿主一起结束；
- 打开 `Local\FGB_NativeHost_<name>` 共享内存，等待宿主播报到 `ACTIVE`；
- 每个 tick 用 seqlock 读取最新帧，转换成 `FaceFrame` 后交给合并层；
- `stop()` 先写 `stop_requested` 让宿主自己 teardown，超时后再终止进程并关闭 Job。

### 2. 共享内存只传稳定 POD

布局在 `sdk/fgb_native_host_shm.h` 固定，不直接暴露引擎内部 `FaceFrame`：

```text
FgbNativeHostBlockV1
  magic / version / struct_size / slot_count
  sequence (seqlock) / active_slot
  state / frames / capabilities
  heartbeat_ns / stop_requested
  error[256]                    # UTF-8，模块/宿主最后一条错误
  slots[2] : FgbNativeHostFrameV1
```

`FgbNativeHostFrameV1` 包含：

- 通道位、capture/publish 时间戳、quality；
- 左右眼：valid、gaze_valid、openness/置信度、瞳孔、逐眼 gaze 方向/置信度/时间戳；
- 渲染注视：逐眼方向、2D 点有效位/坐标、合并方向/置信度/时间戳；
- 头姿、88 形状与有效位。

所有字段只允许末尾追加；`version` 变化时消费者必须拒绝旧布局。

### 3. 新鲜度、崩溃与热更新

- 宿主每 tick 更新 `heartbeat_ns`；引擎超过 2 s 没看到心跳就判定宿主卡死，
  终止进程并标记 `error`。引擎主循环永远不会等宿主。
- 宿主退出：引擎在下一次 `poll` 发现进程已退出，读取共享内存里的最后错误，
  `module_status()` 返回 `alive=false`，其他输入源与输出继续工作。
- **自动重启**：崩溃/心跳超时后，后台线程按 1×/2×/3× 退避重启，默认最多 3 次；
  重启在独立线程里完成，引擎 tick 不会被 `wait_ready` 阻塞。`--no-host-restart`
  可关闭。重启会重新执行模块 initialize，模块内部状态不保留。
- **手动重启/热重载**：模块页实例行有「停止 / 启动 / 重启实例」。热替换 DLL
  的流程是「停止」（宿主退出、文件解锁）→ 替换模块文件 →「启动」；「重启实例」
  是一步完成停止+启动。整个过程不需要重启引擎。
- 模块报 `FGB_STATUS_ERROR` 时宿主继续存活（与 in-proc 行为一致），错误经
  共享内存回传；真正崩溃才退出进程。

### 4. 默认与开关

- 跨进程默认 **关闭**，保持单模块开箱即用的低延迟与兼容性。
- CLI 增加 `--native-host`（所有原生模块走宿主）、`--native-host-exe <路径>` 与
  `--no-host-restart`。
- 界面「模块 → 启动实例」每个实例有「独立进程」开关，写入 `modules.ini` 的
  `[instance_host]`（实例名 = 1/0），重启引擎后生效；只有原生模块受它影响。
- 发布包把 `fgb-host-native.exe` 放在产品 exe 旁边；找不到宿主时原生模块回退
  in-proc 并写警告，不因宿主缺失让输入整体起不来。

### 5. 延迟预算

- 宿主循环与引擎 tick 同频（默认 120 Hz），共享内存 2 槽 seqlock 写读，
  单帧约 1 KB，拷贝成本可忽略；
- 模块日志经共享内存 32 条 ring 回传引擎日志（不需要 socket）；
- 目标端到端增量 < 1 ms（宿主 tick 对齐 + seqlock 读），不引入 socket/UDP；
- 允许丢帧：seqlock 读到写入中时重试，不阻塞；引擎拿“最新已完成帧”。

### 6. 安全边界

- 宿主只接受本机回环共享内存名；名字由引擎清洗，不使用用户任意路径；
- 命令行参数用 `CreateProcessW` 的宽字符路径，避免 ANSI 代码页丢字；
- 宿主 DLL 仍来自模块仓，不改变 ADR-0017 的“只从 store 加载”边界；
- 跨进程不是沙箱：模块仍可访问文件/网络；隔离目标只是崩溃与生命周期。

## 代价与边界

- 多一层进程与共享内存，调试链路更长；首版日志只保留共享内存错误 + 宿主日志文件。
- 模块日志目前不回传 UI；需要时再加 ring 日志。
- 进程启动/停止有百毫秒级成本，不适合每帧切换；只用于启动/重启。
- 首版不自动重启，崩溃后需要用户重启引擎；这是为了避免“自动重启把参数状态悄悄重置”
  的产品歧义。

## 验证计划

- 宿主单测：启动真实 `native-sine` 宿主，读共享内存确认 magic/version、心跳、
  帧序号与形状非零。
- 引擎单测：`NativeHostSource` 启动/取帧/停止；外部杀掉宿主后 `module_status`
  变为错误且引擎 tick 继续。
- CLI：`--source vrcft --native-host` 加载两个原生模块，状态页/日志显示宿主模式。
- 打包：`dist/fgb-host-native.exe` 存在，缺宿主时回退 in-proc 并有明确日志。

## 实施记录（2026-10-30）

- 新增 `sdk/fgb_native_host_shm.h`、`src/native_host/host_main.cpp`、
  `src/sources/native_host_source.*`；`fgb-host-native.exe` 随 CMake/安装包产出。
- CLI：`--native-host`（所有原生模块走宿主）与 `--native-host-exe`；模块页启动
  实例行提供「独立进程」开关（`[instance_host]`），重启后按实例选择宿主；
  找不到宿主时回退进程内并写警告。
- 宿主复用 `NativeModuleSource`，v1/v2 模块都走同一套转换；共享内存传递通道位、
  逐眼/逐通道时间戳、属性级眼睛与渲染注视。
- 验证：当前全量 202 个单测全绿（宿主真实起进程读帧、v2 openness 模块、外部杀进程后
  `module_status` 报错且测试进程继续、缺宿主报错）；`fgb-cli --native-host` +
  SHM 探针实测双模块正常输出；`/WX` 构建通过。
- C.2：宿主日志 ring 回传、崩溃自动重启（默认 3 次退避）、模块页「停止/启动/
  重启实例」（停止后 DLL 解锁，可替换文件再启动）；自动重启在后台线程，
  不阻塞引擎 tick。

## 开放问题

- 自动重启后模块状态被重置；是否需要“保留状态”的重启模式；
- 热替换 DLL 的版本校验（文件名/哈希/ABI）由模块仓还是宿主负责；
- 跨进程宿主的模块 `config.ini` 并发读写（宿主独占写入即可，暂不引入锁服务）。