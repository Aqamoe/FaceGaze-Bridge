# ADR-0047 · 「SteamVR 退出后自动退出」随自启动开关同步打开

- 状态：已采纳
- 日期：2026-11-06
- 影响：`config/outputs`（新增 `[steamvr] exit_with_steamvr`）、`openvr/steamvr_watch`
  （新增 `ExitWatch`）、`web/ipc`（新命令 `set_steamvr_exit_with_steamvr`、自启动开关联动、
  `steamvr` 消息新增 `exit_with_steamvr` 字段）、`cli/main`（退出码 5）、
  `ui/main`（退出码 5 时整体退出）、`ui/`（SteamVR 卡片第二个开关）、`tools/probe_ui.py`
- 关系：配套 [ADR-0020](0020-steamvr-autostart.md)（随 SteamVR 启动）与
  [ADR-0022](0022-tray-shell-and-close-semantics.md)（关窗进托盘）；后者原先写的
  "「跟随 SteamVR 退出」默认不做"改成"默认不做，但提供显式开关"

## 问题

开启「随 SteamVR 启动 FGB」之后，用户摘下头显、关掉 SteamVR，FGB 仍然留在托盘里：
引擎继续占着摄像头/模块，托盘多一个图标，等下次 SteamVR 起来时它又是"已经在跑"的
状态。用户得自己回托盘点「退出」，而这恰好是自启动场景里最容易忘的一步。

## 决定

### 1. 一个独立开关，默认关

`outputs.ini` 新增 `[steamvr] exit_with_steamvr`（默认 `false`）。界面上是「SteamVR
启动项」卡片里的第二个开关：**SteamVR 退出后自动退出 FaceGaze Bridge**。

默认关是因为"手动双击启动 FGB"才是主路径：那种情况下用户关 SteamVR（或者本来就在
用桌面模式）不该把 FGB 一起带走。只有跟随 SteamVR 启动的用户才需要它。

### 2. 打开自启动时同步打开它

`set_steamvr_autolaunch(enabled=true)` 在**读回值为 false（即从关打到开）**时，顺手把
`exit_with_steamvr` 一起写开并落盘，ack 里明说一句。判定用 SteamVR 自己的
`GetApplicationAutoLaunch` 读回值，而不是页面上的复选框——页面只是显示器。

用户之后可以单独把它关掉（"我就是想让 FGB 常驻"）。下次再把自启动开关从关打到开时，
同步会再发生一次。反过来（关自启动）不动它：关自启动只是"别再自动拉我起来"，不是
"我以后都不用了"。

### 3. 判定在引擎，退出在整体

`openvr/steamvr_watch` 的 `ExitWatch` 是纯状态机，喂"此刻 vrserver.exe 在不在"：

- **必须先看见过**：FGB 比 SteamVR 先起来时（手动双击），看不见 vrserver 是正常状态，
  不能判成"它退出了"。
- **去抖 5 秒**：SteamVR 会自己重启 vrserver（换驱动/崩溃恢复），瞬时消失不算退出。
- **只报一次**：调用方没退成也不会每拍重报；开关中途关掉会清空状态，不残留倒计时。

`IpcServer::pump` 每秒查一次 `vrserver.exe`（`base::process_running`，与 VRSS 共用同一份
实现），判定成立时置 `ctx_.quit` 与 `ctx_.steamvr_exit`。引擎以**退出码 5** 结束；外壳
`on_engine_exit` 见到 5 就 `quit_app()`，连托盘图标一起收掉，而不是按"引擎被停了"留在托盘。

不放外壳里做：外壳刻意不碰配置与硬件（ADR-0016/0022），而且托盘形态下页面根本没连着，
判断必须在引擎主循环里照跑。

## 验证

- `tests/test_steamvr.cpp`：未开启静默 / 必须先看见过 / vrserver 重启去抖 / 中途关开关
  清计时 / 休眠后时钟倒退不误判
- `tests/test_outputs.cpp`：新字段默认关、旧 ini（无 `[steamvr]` 段）读回仍是关
- 端到端（Debug 产物，临时 outputs 配置）：`set_steamvr_exit_with_steamvr` 回执 + 落盘；
  用 `ping.exe` 冒充 `vrserver.exe` 起停一次，引擎 5.7 秒后以退出码 5 结束
- `set_steamvr_autolaunch(enabled=true)` 后 `/api/steamvr` 的 `exit_with_steamvr` 变 true
- `tools/probe_ui.py`：`#sv-exit` 存在，勾选状态必须等于 `state.steamvr.exit_with_steamvr`

## 遗留

- 真机验收（SteamVR 运行时）：关 SteamVR → FGB 托盘图标消失、任务管理器无残留进程；
  连点两下 SteamVR 重启不该把 FGB 踢掉（去抖窗口内 vrserver 会回来）。
- 若用户把 FGB 装成"开机自启 + 手动用桌面模式"，需要在设置里自己关掉这个开关。
