# ADR-0015 · 模块页按实例显示，而不是只显示"第一个"

- 状态：已采纳
- 日期：2026-10-29
- 影响：`IpcContext` 从单个 `vrcft` 指针改为 `running_modules` 列表；`status.module` → `status.modules[]`；
  `modules` 消息的条目增加 `running` / `instance`；模块页新增「运行中的实例」；`tools/probe_ui.py` 增加断言

## 问题

ADR-0011 的模块页是按"引擎只跑一个模块"设计的：`status.module` 只有一份状态，
页面把"磁盘上有什么"和"正在跑什么"混在一张卡片里。

M4 的合并层让"两个 vrcft 实例各加载一个模块"成为可配置的形态（`--source vrcft --module-id A
--source vrcft --module-id B`，或两个不同模块分别供眼追与表情）。此时旧界面会骗人：

- 第二路的故障、重启次数完全看不到
- 卡片上的状态取自第一个实例，用户会以为"那个模块"就是全貌
- 注册表里的"启动时加载"是单选，和"实际跑着两个"是两条不同的信息，混在一起更乱

## 决定

### 1. IPC 按实例给状态

`status.modules[]`，每项：

```json
{"name":"vrcft[1]","module_id":"f3df…","module_name":"VRCFTPicoModule",
 "state":"error","error":"…10048…","restarts":2,"alive":true}
```

- `name` 与 `--eye-source` / `--expression-source` 用的是同一个标识（`vrcft[1]`），
  用户在界面看到的和写在命令行上的对得上
- `state` 的取值与之前一致（active / starting / error / stopped / external），
  由**该实例**的 `VrcftSource` 决定（有错误文本优先算故障）
- 原来的 `status.module` 被移除：只留一份状态等于默认多实例不存在

`modules` 消息（磁盘列表）里每个条目额外给 `running` 与 `instance`：
卡片上就能看出"这个模块被 vrcft[2] 加载着"。运行态与磁盘状态**分开显示**，
不把两者压成一个 chip——它们可能不一致（用户刚改了"启动时加载"，还没重启）。

### 2. 界面分两块

- 顶部「运行中的实例」：实例名 / 模块名 / 状态 chip / 重启次数与错误摘要
- 下方「已安装的模块」：磁盘上有什么、选启动时加载哪个

这样"我现在跑的是哪几个、各自健康吗"和"我下次想加载哪个"是两个可以分开回答的问题。

### 3. 仍然只是单选注册表

`ModuleRegistry` 里的 `module=` 依旧是"下次启动加载的那一个"。多实例要靠命令行显式重复
`--source vrcft --module-id …`。按实例的启动配置（`vrcft[1]=A, vrcft[2]=B` 写进 fgb.ini）
留到后面——没有真实双模块硬件的情况下，配置格式定下来也是猜的。

## 验证

- 实跑两个实例（本机只有一个 PICO 模块，两个实例都加载它）：
  `status.modules` 返回 `vrcft[1]`（error，带 `SocketException (10048)`）与
  `vrcft[2]`（先 starting、后 error），两路互不覆盖
- 界面「运行中的实例」两行分别显示 chip 与错误摘要（截图核对），顶部 pill 显示
  `输入 · vrcft[1]+vrcft[2]`
- `tools/probe_ui.py` 新增断言：运行实例行数 == `status.modules` 长度；
  未改动的 100 个单测仍全绿（这一层没有纯逻辑可单测，验证靠探针与实跑）

## 遗留

- `--module-id` 与 `--source vrcft` 的配对靠出现顺序，写错了不会报错（会拿错模块）。
  以后改成 `--module vrcft[1]=<id>` 这种显式绑定。
- 模块热切换仍然没有：改"启动时加载"要重启引擎。
- 每实例的独立配置（眼追开/关、眼追刻度）还没做，现在共用全局参数。
