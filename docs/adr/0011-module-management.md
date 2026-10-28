# ADR-0011 · 模块管理：发现、选择、把失败原因说人话

- 状态：已采纳
- 日期：2026-10-29
- 影响：新增 `vrcft/module_registry`、`base/json`（从 `web/json` 拆出）、`fgb-cli modules`、
  UI「模块」页；`VrcftSource` 增加 `module_error()`

## 问题

M2 的模块兼容层只认一条路径：`--module-dll` 指向某个 DLL，或者"在 `CustomLibs` 下递归找第一个
`.dll`"。到了要和 VRCFT 抢用户的阶段，这是不行的：

- VRCFT 的模块不一定只有一个，也**不一定按目录顺序**排；"第一个 dll"可能是个依赖库
- 模块初始化失败是最常见的投诉，而用户看到的只有 `frames=0`
- 用户换模块得改命令行；界面里没有出口

`docs/02-features.md` §2.1 把这块列为 P0 的第一项，要求：目录扫描、元数据、启用/禁用持久化、
状态显示带可操作的原因、日志按模块过滤。

## 决定

### 1. 目录与元数据：照抄磁盘上的真实布局，不照抄文档

实测 VRCFT（v5）的模块布局是：

```
%AppData%\VRCFaceTracking\CustomLibs\<ModuleId>\
    module.json      ← ModuleId / ModuleName / AuthorName / Version / ModuleDescription /
                        ModulePageUrl / DllFileName / FileHash(null) / IsLocal …
    <DllFileName>.dll
    config.ini       ← 模块自己的配置，我们不碰
```

注意 `module.json` 是**单个**元数据文件（不是"同名 .metadata.json"），且可能带 UTF-8 BOM
（.NET 的写入习惯），`DllFileName` 字段给出真正的入口 DLL。`module_registry` 按这个来，
并且兼容老布局（dll 直接摊在根目录，id 退化成文件名）。

扫描两个根：VRCFT 的 `CustomLibs`、"自家"的 `<exe>/vrcft-libs`。后者让用户可以只给 FGB 装一份
模块，不必和 VRCFT 共用。

### 2. 这一版只有"启动时加载哪个"，没有"多选"

§2.1 的目标形态是"允许同时启用多个模块（A 供眼追、B 供表情）"。但引擎现在只有**一个**
`VrcftSource`、**一个**沙箱 socket、**一个**模块进程。假装支持多选，界面会显示"已启用两个"
而实际只有一个在跑——比不做更糟，而且用户会按这个错误前提去排查。

所以这一版明确只做单选，并且把限制写在模块页上（"引擎一次只加载一个模块"），
多模块并发并入 M4 的多输出工作（要先把 source 的合并层做出来）。

### 3. 选择持久化在 FGB 自己的 `fgb.ini`

`%AppData%\FaceGazeBridge\fgb.ini`：

```ini
[vrcft]
module=f3df57d5-1c5b-887d-abb0-be555e14cf09
```

为什么不读 VRCFT 的 `LocalSettings.json`：那里**没有**模块启用状态（实测键只有
`Calibration` / `Data Filter` / `OSCAddress` / `Unified Correctors` 等），读它等于猜。

优先级：`--module-id` > `fgb.ini` > "磁盘上只有一个模块就用它"。
第三条是为了新装机不卡在"先手动配置"——自动挑中**不写盘**（`mark_active` 与 `set_active` 分开），
只有用户在界面/命令行里明确选择才落盘。

写盘用 `config::IniWriter`，整份重写。这个文件目前只有这一个用途；将来加键必须先读再写，
否则会用默认值盖掉用户手改的内容——这条写在代码注释里，也写在 `module_registry` 的头文件里。

### 4. 状态要带"模块自己说的话"

`VrcftSource` 原来只有 `last_error_`（我们这侧的判断："等待初始化回复超时"）。这句话对用户几乎
没有信息量。模块其实会通过 `EventLog` 包告诉我们真正的原因，例如 PICO 模块初始化失败的真实文本是：

```
System.Net.Sockets.SocketException (10048): 通常每个套接字地址(协议/网络地址/端口)只允许使用一次
```

所以 `log_event()` 里把 level >= 4 的文本压平（换行→空格）、截断到 600 字符存进
`module_error()`，IPC 的 `status.module.error` 优先发它。界面上的效果就是"故障 + 端口被占用"，
而不是"故障 + 未知"。

`status.module.state` 取值：`active`（有帧且在跑）/ `starting`（活着但还没有帧）/ `error`
（有错误文本）/ `stopped`（进程没了）/ `none`（没配模块）/ `external`（`--module-dll` 手工指定）。

### 5. IPC 增补

| 方向 | 消息 | 说明 |
|---|---|---|
| 引擎 → UI | `modules` | 列表 + 当前选择 + 扫描根 + 配置文件路径，连接时与变化时推送 |
| 引擎 → UI | `status.module` | `{id, state, error, restarts, alive}`，跟状态一起 10 Hz |
| UI → 引擎 | `scan_modules` | 重新扫描磁盘 |
| UI → 引擎 | `set_module` `{id}` | 写 `fgb.ini`；空串 = 不加载。重启生效 |

列表只在变化时推一次，运行态跟着 `status` 走——模块列表不常变，没必要 10 Hz 重发几百字节。

### 6. 顺手拆了 `web/json`

`module_registry` 要读 `module.json`，但 `web/json` 属于控制面，`fgb_vrcft` 依赖它会形成
`vrcft → web → engine → vrcft` 的环。所以把 JSON 读写下沉到 `base/json`（`JsonWriter` +
`field_string/number/bool`），同时删掉 `web/json` 里随控制面重写而**不再被使用**的
`HttpRequest` / `parse_request` / `query_*`（连带 5 个测这些死代码的用例）。

## 验证

- `fgb-tests`：**87 个用例通过**，新增 6 个（`module.json` 真实样本解析、BOM、坏 JSON、
  两种目录布局扫描、broken 模块标记、选择持久化 + 命令行优先 + 停用、单模块自动选中）
- `fgb-cli modules` 在本机真实 VRCFT 目录上跑通：
  `VRCFTPicoModule 0.1.11 RingLo 已安装`，配置文件路径正确
- 界面（真跑 `--source vrcft`，PICO 未连接/端口被占）：模块页显示 **故障** +
  `SocketException (10048) … 端口只允许使用一次`，`restarts` 计数增长
  （`tools/probe_ui.py` 现在也会检查模块页卡片数与 `catalog` 一致）
- `fgb-cli modules use <id>` 写入 `fgb.ini`，重开进程后 `pick_active` 能读回

## 遗留

- **多模块并发**（眼追 + 表情各一个模块）没做，见上文第 2 条，并入 M4
- 模块**热切换**没做：改选择要重启引擎。热切换需要可重启的 source + 状态迁移，单独一个 ADR
- 元数据里的图标（`Icon`）、`UsageInstructions` 还没显示；`Downloads/Ratings` 之类与本地无关的字段不读
- 没有"打开模块目录"按钮，也没有按模块过滤日志（日志里已带 `tag`，但模块名没进 tag）
