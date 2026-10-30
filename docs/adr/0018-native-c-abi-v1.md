# ADR-0018 · 原生 C ABI 模块 v1

- 状态：**已被 [ADR-0034](0034-native-module-v2-only.md) 取代**（v1 ABI 于 2026-10-31 移除，
  本文保留为设计记录；`FgbModuleV1` / `FgbFrameSlotV1` / `fgb_module_query` 等符号已不存在）
- 日期：2026-10-29
- 影响：新增 `sdk/fgb_module_api.h`、`sources/native_source`、`samples/native-sine`；
  `ModuleInfo.kind` 生效；`--source vrcft` 按模块类型分发；`ISource::module_status()` 统一运行态

## 问题

A 层（VRCFT 兼容）必须带一份 .NET 宿主运行时（~106 MB），模块也受 VRCFT 协议与进程模型约束。
`docs/01-architecture.md` §8.2 早就写了原生 C ABI 的纸面设计，但一直没有实现：
新硬件/自研源只能写 .NET 模块，或者退回 VMC 这种粗粒度协议。

## 决定

### 1. v1 进程内加载，ABI 仍然按可跨进程设计

- 模块是一个 DLL，只导出 `fgb_module_query(host_abi_version)`。
- 宿主 `LoadLibraryW` 后校验 `struct_size / abi_version / capabilities`，再调用
  `initialize`；引擎线程按 tick 调 `update`，`stop()` 时调 `teardown` 并卸载。
- **v1 的线程模型是同步的**：`commit` 只能在 `initialize/update` 里调用，模块不要自己起
  写帧线程。宿主不做跨线程保护；需要自驱线程/崩溃隔离时上 v2（共享内存 + 子进程）。
- 帧槽由宿主分配（进程内就是一个结构体指针），模块通过 `acquire/commit` 提交。
  结构按“共享内存兼容”的布局设计，后续跨进程可以复用同一份 ABI。

### 2. 只传 POD，版本靠 `struct_size` 前向兼容

- 字符串用 UTF-8 `const char*`，生命周期由模块保证；跨边界不出现 STL/C++ 对象。
- 所有结构第一个字段是 `struct_size`，第二个是 `abi_version`；新增字段只能追加在末尾。
- 宿主只读 `min(module.struct_size, sizeof(本地结构))`，未知 ABI 版本直接拒绝。
- 形状用**名字查下标**（`host->shape_index("JawOpen")`），模块不硬编码 88 项顺序；
  宿主保证与 Unified Expressions 协议层一致。

### 3. 模块清单 `fgb-native.json`

字段与 VRCFT 的 `module.json` 相同（`ModuleId / ModuleName / AuthorName / Version /
ModuleDescription / DllFileName`）。用途：

- 列出模块不需要先 `LoadLibrary`，避免“打开模块页就执行未知代码”。
- 模块仓据此把 `ModuleInfo.kind` 标成 `native`；界面上显示「原生」而不是 VRCFT。
- 安装/导入/删除沿用 ADR-0017 的模块仓流程；模块库（registry.vrcft.io）仍然只提供
  VRCFT 模块，原生模块不混进那个列表。

### 4. 引擎按模块类型分发

`--source vrcft` 现在的语义是“从模块仓加载当前启动模块”：

- `kind == vrcft` → `VrcftSource` + 内置/本机 VRCFT 运行时（行为不变）。
- `kind == native` → `NativeModuleSource`（进程内 C ABI），**不需要** VRCFT 运行时。
- 两种模块共用模块仓、启用/启动选择、界面状态、合并层；`RunningModule` 持有 `ISource*`，
  运行态统一走 `ISource::module_status()`。

### 5. 示例模块就是 ABI 的可执行文档

`samples/native-sine` 不依赖任何硬件，按正弦波产生表情/眼追/头姿，覆盖：

- 初始化参数、按名字取形状下标、提交帧、日志、状态上报、teardown
- 单测直接加载编译出的 DLL 读帧；引擎端到端 4 秒 480 帧、无 late tick

## 代价与边界

- **进程内模块崩溃会带走引擎**。v1 接受这个代价（与 VMC 输入源同进程类似），
  跨进程沙箱留给 v2。
- 仍然一次只加载一个模块（与 ADR-0015 的引擎形态一致）。
- ABI 是新的公共契约：任何字段改动必须写 ADR、升 `abi_version`，不能悄悄改。
- 模块需要自己处理设备/线程，但帧提交必须回到 `update` 调用栈。

## 验证

- 112 个单测：示例 DLL 加载/读帧/能力位/形状值；模块仓识别原生清单；非模块 DLL 被拒绝。
- 引擎：临时模块仓只放示例模块，`fgb-cli run --source vrcft --panel 8798 --duration 4`
  → `frames=480`，无错误。
- 发布包仍不包含示例模块（只在仓库/构建目录里，避免误把正弦波送去 VRChat）。
