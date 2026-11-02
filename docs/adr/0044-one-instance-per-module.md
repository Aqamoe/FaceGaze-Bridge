# ADR-0044 · 一个模块只允许一个启动实例，帧率按模块分开看

- 状态：已采纳
- 日期：2026-11-02
- 影响：`vrcft/module_registry`（`add_instance` 增加同模块去重、新增 `find_instance`）、
  `web/ipc`（`add_instance` 的报错文案；`status.sources[]` 增加每源 `fps`）、
  `ui/`（模块卡片按钮、状态页「输入帧率」卡片）、`tools/probe_ui.py`
- 关系：调整 [ADR-0015](0015-multi-instance-modules.md) 的"一个模块可以被多个实例加载"，
  沿用 [ADR-0013](0013-source-merging.md) 的按源报告

## 问题

界面允许把同一个模块添加两次（如 `native:PICO Connect Gaze` 加两路，一路供眼追、
一路供表情）。实际上这两路是同一个 DLL、抢同一份共享内存与 UDP 端口：

- 后起的实例大概率起不来（端口/共享内存冲突），先起的那个也可能被拖坏，
  用户看到的是"两个实例，一个报错"，但界面没有解释为什么
- 两个实例的配置（`config.ini`）是同一份，改一路等于改另一路，与"两路独立"的预期相反
- 状态页的「输入帧率」只有一个总和。两路里一路 30 Hz、一路 0 Hz 时，总和仍然像个
  体面数字，正好掩盖"哪一路不动"

## 决定

### 1. 注册表拒绝同模块的第二个实例

`ModuleRegistry::add_instance()` 除了名字去重，还要求 `module_id` 未被占用
（`find_instance(module_id)` 为空）。IPC 层先查一遍并给出人话报错：

```
「<已有实例名>」已经是这个模块的实例：同一个模块只能添加一次
```

界面同步收敛：模块卡片上已经添加过之后，按钮换成
`已添加为实例：<名字>` 并挂上原因，不再让人点第二下。已有配置文件里若还残留
同模块的多个实例，引擎启动时跳过后面那个并 `WARN`（不悄悄删用户配置），
只是不允许再新增（[ADR-0045](0045-engine-survives-a-failed-source.md)）。

### 2. 帧率按输入源分别统计

`status.sources[]` 每项新增 `fps`（两次状态推送之间的该源帧数增量 ÷ 时间）。
状态页「输入帧率」卡片改成一行一个源：名字 + 该源的 fps，源不新鲜时整行变暗；
不再显示混合总和（`status.fps` 保留给 `/api/status` 与长跑脚本用）。
链路的「输入」一行也只写源名，避免又冒出一个没有参考价值的总和。

## 验证

- `tests/test_modules.cpp`：同名重复、同模块换名重复都必须失败，`find_instance` 能定位
- `tools/probe_ui.py`：`#fps-list .fps-row` 行数 == `status.sources` 长度，首个源 fps > 0
- 实机：加过一个实例后模块卡片显示「已添加为实例」，再发 `add_instance` 得到上面的报错

## 遗留

- 多实例能力仍保留在命令行（`--source vrcft --module-id A --module-id A`）与旧配置文件里，
  将来若要支持"同一模块两路独立配置"，得先解决共享内存/端口的按实例命名。
