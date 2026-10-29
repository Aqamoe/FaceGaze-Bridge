# ADR-0027 · 命名输出预设与快速切换

- 状态：**已实施**（2026-10-30）
- 日期：2026-10-30
- 影响：`src/config/outputs.*`、`src/config/ini.*`、`src/web/ipc.*`、`src/cli/main.cpp`、
  `ui/`（输出页预设区 + 三语文案 + 样式）、`tests/test_outputs.cpp`、
  `tools/probe_output_profiles.py`、`tools/probe_ui.py`
- 关系：补完 roadmap 4.8 的后半；前置是 [ADR-0025](0025-output-settings-persistence.md)

## 问题

ADR-0025 让输出设置能持久化、能在输出页编辑，但只有**一套** `outputs.ini`。
实际使用里至少有三套常见组合：

- VRChat 直播：VRChat OSC + WS JSON（给网页叠层）
- Unity 场景：VMC + 共享内存（给自研客户端）
- 排障：只留 VRChat OSC，关掉所有额外输出

每次切换都要手改表单再重启引擎。用户不会为此保留三份配置。

## 决定

### 1. 每个预设一个 ini，放在独立目录

```
%LOCALAPPDATA%\FaceGazeBridge\outputs-profiles\<清洗后的名字>.ini
```

文件内容与 `outputs.ini` 相同，外加 `[profile] name=<展示名>`：

```ini
# FaceGaze Bridge 输出预设；在界面输出页选择后应用

[profile]
name = VR 直播

[output]
vrchat = 127.0.0.1:9000
...
```

不把多套预设塞进 `outputs.ini` 的多节：

- 清单/删除/备份都是文件操作，不需要解析"第 N 个 profile"；
- 坏掉一个预设只影响那一份，不会让当前 `outputs.ini` 整份失效；
- 导入/导出预设将来就是复制文件。

`outputs.ini` 始终只有"当前生效的一套"，语义不变。

### 2. 文件名清洗，展示名以文件内为准

预设名允许中文/空格，但拒绝路径分隔符、控制字符、`* ? " < > | :`、首尾空白和 `.`/`..`。
文件名由清洗后的名字生成；撞名时追加 `-2`、`-3`。查找同名预设时**只认文件里的
`[profile] name`**，不靠文件名反推。

Windows 上 `std::filesystem::path(std::string)` 按 ANSI 代码页解释，中文名会抛
`std::filesystem_error`。因此：

- 用户文本构造路径统一走 `config::path_from_utf8()`（`MultiByteToWideChar(CP_UTF8)`）；
- INI 层增加 `std::filesystem::path` 重载，写文件用 `_wfopen_s`，读文件用
  `std::ifstream(path)`——两者都走宽字符 API。

这是 4.8 实测时抓到的真 bug：单元测试在"VR 高帧率"上直接 abort，堆栈落在
`std::filesystem::_Convert_narrow_to_wide`。

### 3. 不额外记录"当前预设"

状态里的 `active` 由"当前保存值是否与某个预设逐字段相等"推导：

- 保存后完全一致 → 显示该预设名；
- 用户改了一个字段 → 自动回到「自定义」；
- 重启后仍能正确显示，不需要额外状态文件。

代价是：两个预设内容完全相同时，只会显示排序靠前那个。这比维护一份可能与
`outputs.ini` 不一致的"当前预设名"更可靠。

### 4. 应用 = 写入当前设置 + 重启引擎

`sinks` 在引擎启动时固定，运行时热插拔没做（ADR-0025 已说明理由）。所以"快速切换"
的语义是：

1. `apply_output_profile` 读取预设 → `save_outputs(outputs_path)` → 更新内存保存值；
2. ack 带 `restart_needed=true`；
3. 界面进入已有的 `restart_engine` 流程（外壳原地拉起新引擎）。

界面上的「应用并重启」是一次点击；确认框说明追踪会短暂中断。

### 5. IPC

新增消息：

| 方向 | 消息 | 说明 |
|---|---|---|
| →UI | `outputs_profiles` | `profiles[]`、`active`、`dir`；连接时与每次变更后推送 |
| UI→ | `save_output_profile` | 名称 + 与 `set_outputs` 相同的扁平字段；只写预设，不碰 `outputs.ini` |
| UI→ | `apply_output_profile` | 读取预设并写入 `outputs.ini`，ack `restart_needed` |
| UI→ | `delete_output_profile` | 删除指定展示名的预设文件 |

`set_outputs` 与 `save_output_profile` 共用 `parse_output_patch()`，缺省字段保持原值。
`--outputs-profiles <dir>` 可把预设目录指到临时位置（探针/测试用）。

### 6. 界面

输出页「输出设置」上方新增：

- 预设下拉（含「自定义」）+ 应用/删除按钮；
- 名称输入 + 「保存为预设」；
- 说明：预设只保存输出目标与开关，应用会重启引擎。

删除与应用都走 `ask_confirm`；任何操作都有 toast。三语文案齐全。

## 验证（2026-10-30）

```
单测：4 个新用例（往返/列表/覆盖/非法名/坏文件跳过），总数 151 全过
IPC 探针：保存「探针 预设」→ 列表出现 → 应用 → status.outputs 回读
          ws=9543@45Hz / udp=90Hz / shm=probe-profile / eye=both
          非法名 a/b 被拒绝；删除后列表与目录都为空
UI 探针：输出页预设区存在；保存后列表出现；选中后按钮可用；
          应用弹出带名称的确认框，取消后不进入重启；
          删除走确认框，列表移除
```

## 不采用的做法

| 做法 | 不用的原因 |
|---|---|
| 预设塞进 `outputs.ini` 多节 | 解析/删除/备份都复杂；一份写坏影响当前设置 |
| 用文件名当展示名 | 清洗会丢字符（中文路径/非法字符），展示名必须无损 |
| 额外存"当前预设名" | 会和 `outputs.ini` 漂移；逐字段比较是唯一事实来源 |
| 运行时热插拔 sink | 端口/线程回收成本高，收益不如重启一次（1–2 s） |
| 保存预设时顺带改 `outputs.ini` | 用户可能只想存档，不想立刻改当前设置 |

## 已知限制

- 应用预设一定重启引擎，追踪会短暂中断。
- 预设只含输出设置，不含校准档案、滤波参数、模块选择（它们各有独立的持久化）。
- 内容完全相同的两个预设只显示排序靠前那个为 active。
