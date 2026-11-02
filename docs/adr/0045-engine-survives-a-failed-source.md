# ADR-0045 · 一路输入源起不来，引擎继续跑

- 状态：已采纳
- 日期：2026-11-02
- 影响：`engine/engine`（`start()` 不再因输入源失败而返回 false）、
  `cli/main`（启动时跳过同一模块的重复实例）、`docs/09-user-guide.md`
- 关系：配合 [ADR-0044](0044-one-instance-per-module.md)（一个模块一个实例）；
  沿用 [ADR-0015](0015-multi-instance-modules.md) 的按实例状态上报

## 问题

有人给同一个模块加了第二个实例（独立进程），重启引擎后引擎直接退出，界面跟着没了，
连"哪一路坏了"都看不到。日志里的链条是：

```
ERROR engine: source 'native:iFacialMocap-2' 启动失败：无法绑定 UDP 49983，WSA 10048
（引擎进程随即退出，桌面外壳改用 --source vmc 重试，模块全部消失）
```

根因不是那个模块，而是 `Engine::start()` 的失败语义：

```cpp
for (auto& source : sources_) {
    if (!source->start()) return false;   // ← 一路失败，整个引擎失败
}
```

`main` 收到 false 就 `return 1`。而**引擎同时是界面宿主**（`/` 与 `/ws` 都由它提供），
一退出用户就失去了唯一的诊断入口和修复入口。模块是第三方代码，端口被占、共享内存
冲突、ABI 不匹配都是常态，不该按"致命错误"处理。

## 决定

### 1. 输入源启动失败只记日志，不中断启动

`Engine::start()` 遍历输入源时，失败的那一路：

- 打一条 `ERROR`，带上模块自报的原因，并注明"其余输入继续运行"
- 留在 `sources_` 与合并器的槽位里（它的 `poll()` 在未启动时本来就返回 false），
  于是状态页的「输入帧率」「输入源」仍然有它一行（0 帧、变暗）
- `status.modules[]` 里状态是 `error` 并带错误文本；界面可以在原因消除后点「启动」，
  走 `Engine::start_source()` 原地恢复，不用重启引擎

输出端（sink）失败仍然算致命：那是我们自己的代码与配置，静默降级会让人以为"发出去了"。

### 2. 老配置里的重复实例在启动时跳过

`[instances]` 里残留的同一模块第二个实例，在 `cli/main.cpp` 的实例循环里按 `module_id`
去重并 `WARN` 跳过（不删用户配置）。它本来就必然抢不到端口，留着只会每次启动刷一条
错误，还会让「运行中的实例」少一路却显示不出原因。

## 验证

- `tests/test_merge.cpp::engine_start_survives_a_source_that_fails_to_start`：
  一个源 `start()` 返回 false、另一个正常，`engine.start()` 必须为 true，
  正常那一路的帧照样进 `latest()`，失败那一路在 `source_report()` 里是 0 帧
- 端到端（临时 `LOCALAPPDATA` + 手工 `modules.ini`）：
  `sine-1`/`sine-2` 指向同一模块 + 一个只导出 ABI v1 的 DLL 作为必失败源 →
  日志 `实例 sine-2 与前面的实例是同一个模块，已跳过`、
  `source 'native:legacy' 启动失败…（其余输入继续运行）`，引擎进程 12 秒后仍在，
  `/api/status` 返回 `native:sine-1 frames=863` 与 `native:legacy state=error`
- 修复前的同一份配置会让引擎在启动阶段以退出码 1 结束（真机 `engine.log` 已有记录）

## 遗留

- 引擎因**输出端**失败退出时，界面同样会跟着消失。是否也改成"降级继续跑"需要单独
  决定：静默不发送比直接失败更难排查，暂时保持现状。
- 桌面外壳的"启动失败 → 改用 VMC 重试"逻辑没有改动；本 ADR 让它在模块出问题时不再被触发。
