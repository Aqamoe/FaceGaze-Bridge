# ADR-0049 · 命名校准存档与多套校准切换

- 状态：**已实施**（2026-11-27）
- 影响：`src/params/calibration.*`、`src/web/ipc.*`、`src/cli/main.cpp`、
  `ui/`（校准页工具条存档下拉 + 带输入框的确认弹窗 + 三语文案）、`tests/test_calibration.cpp`、
  `tools/probe_calibration_ui.py`、`tools/probe_calibration_profiles.py`
- 关系：与 [ADR-0027](0027-named-output-profiles.md)（命名输出预设）同构；
  依赖 [ADR-0021](0021-calibration-workbench-3d-preview.md) 的实时热加载与
  `save_profile_atomic()` 的原子写入

## 问题

校准只有一份 `calibration.ini`。实际使用里同一个人往往需要多套：

- 不同头显/不同模块的 lo/hi 与曲线不一样；
- 同一个头显，"直播用"和"日常用"的眼睑增益、视线增益不同；
- 调新参数前想留一份"确认可用"的底稿。

现在只能手工复制 `calibration.ini` 再改 `--calibration` 路径重启，用户不会为此保留多份文件。

## 决定

### 1. 一个存档一个 ini，放在独立目录

```
%LOCALAPPDATA%\FaceGazeBridge\calibration-profiles\<清洗后的名字>.ini
```

文件内容与 `calibration.ini` 完全相同，外加 `[profile] name=<展示名>`：

```ini
# FaceGaze Bridge 校准档案

[profile]
name = 夜间 直播

[eye]
active = 1
...
```

不把多套存档塞进 `calibration.ini` 的多节：清单/删除/备份都是文件操作，
坏掉一个存档也不会影响当前生效的档案。这一点直接沿用 ADR-0027 的理由。

### 2. 文件名清洗，展示名以文件内为准

名称规则与输出预设共用 `config::validate_profile_name()`：允许中文/空格，
拒绝路径分隔符、控制字符、`* ? " < > | :`、首尾空白与 `.`/`..`。
文件名由清洗后的名字生成，撞名追加 `-2`、`-3`；查找同名存档只认 `[profile] name`。

旧档案（没有 `[profile]` 段）在文件名兜底时也走 UTF-8 转宽字符；Windows 上
`std::filesystem::path(std::string)` 按 ANSI 解释，中文名会抛异常（ADR-0027 踩过）。
因此 `save_profile` / `load_profile` / `save_profile_atomic` 增加
`std::filesystem::path` 重载，存档读写一律走它。

### 3. 「当前档案」本身就是一个存档槽

界面上不再单独占一张卡片，而是在校准页顶部工具条放**一个下拉**：

```
当前档案（calibration.ini）
  夜间 直播
  默认
────────
新建：默认参数…
新建：当前参数的新存档…
────────
删除当前存档…
```

- 选中「当前档案」= 编辑 `calibration.ini`；选中命名存档 = 编辑那份文件；
- 切换选项即**应用**：读入内存并写成启动档案（`calibration.ini`），有未保存改动先确认；
- 「保存」写回**当前选中的存档**，同时写 `calibration.ini`，两边始终一致；
- 「恢复默认」只改内存（全部透传），落盘还要点保存，和以前一样；
- 新建默认参数 = 全透传；新建当前参数 = 复制内存里这套。新建即选中并立刻生效；
- 新建**不允许重名**：要改一份存档请选中它再保存，避免一次手滑覆盖旧档；
- 删除只删存档文件，当前生效的校准不受影响。

### 4. 应用/切换要写盘，不重启引擎

1. 读存档 → `save_profile_atomic(calibration.ini)`；
2. `CalibrationApplier::set_profile()` 立刻生效（校准本来就支持热更新）；
3. 写盘失败就保持现状并报错，不出现"界面换了、重启又回去了"。

写盘保留 `.bak`：应用一份存档会覆盖当前档案，但上一份还能从 `.bak` 拿回来。

### 5. IPC / 文件

| 方向 | 消息 | 说明 |
|---|---|---|
| →UI | `calibration_profiles` | `available`、`dir`、`profiles[]`、`paths[]`（同序，界面显示目标文件） |
| UI→ | `save_calibration`（可选 `profile` 名） | 有 `profile` 时先写该存档再写 `calibration.ini`；没有就是原来的"保存当前档案" |
| UI→ | `create_calibration_profile` | `name` + `source=default\|current`；新建并立刻应用，重名拒绝 |
| UI→ | `apply_calibration_profile` | 读取存档并写入 `calibration.ini`，热加载 |
| UI→ | `delete_calibration_profile` | 删除指定展示名的存档文件 |
| UI→ | `save_calibration_profile` | 直接写当前内存为存档（探针/脚本用，界面已改走 `save_calibration`） |

`--calibration-profiles <目录>` 可把存档目录指到临时位置（探针/测试用），
默认 `params::default_calibration_archive_dir()`。

### 6. 列表里不记"当前存档"

`outputs_profiles` 有 `active`（按保存值逐字段比较推导）。校准不做这件事：

- 88 条滑杆每次拖动都会让"当前等于某个存档"失效，为了一个高亮字段
  每次校准改动都重读目录里所有存档，收益与成本不成比例；
- 选中状态只属于本页会话，表示"保存/重新加载的目标"，不是服务端真值；
- 应用/保存已经同时写 `calibration.ini`，重启后仍是这一套，
  不需要靠高亮来确认"现在用的是哪份"。

## 验证

```
单测：存档往返（含中文名）/ 列表 / 同名覆盖留 .bak / 非法名 / 坏文件跳过
UI 探针：工具条里出现存档下拉（没有独立卡片）；当前档案/两个存档/两个新建动作都在；
          有未保存改动时切换弹确认，取消退回原选择、不发光；确认后发出带名字的 apply，
          路径行跟着换成选中存档的路径
IPC 探针：新建默认 → 形状全透传；重名新建被拒；改形状后保存到存档 → 恢复默认 →
          应用回读 lo/hi；新建当前参数副本 → 应用后数值一致；非法名；删除
```

## 不采用的做法

| 做法 | 不用的原因 |
|---|---|
| 存档塞进 `calibration.ini` 多节 | 解析/删除/备份都复杂；一份写坏影响当前档案 |
| 用文件名当展示名 | 清洗会丢字符，展示名必须无损 |
| 单独一张"存档"卡片 | 存档是整份档案的操作，和搜索/保存/重载同一条工具条更顺；少占一屏 |
| 新建时允许重名覆盖 | 一次手滑就抹掉旧存档；改内容应该"选中再保存" |
| 应用存档要求重启引擎 | 校准本来就支持热加载，重启只会白断追踪 |
| 服务端记录"当前存档名" | 会和实时滑杆改动漂移；换成逐字段比较高亮又要每次重读全部存档 |

## 已知限制

- 应用/切换存档会覆盖当前 `calibration.ini`（上一版在 `.bak`），没有"只试听不保存"的模式。
- 内容完全相同但未激活形状数值不同的两份存档，会被逐字段比较当成同一份；
  界面本就不显示未激活形状的数值，不影响使用。
- 存档不含注视圆盘标定（`gaze.ini`）、滤波与输出设置。
- 删除动作在下拉里，必须先选中一个命名存档才可用；「当前档案」不能删除（它永远是启动档案）。
