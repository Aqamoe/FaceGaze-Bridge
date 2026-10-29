# ADR-0020 · 随 SteamVR 启动（OpenVR 注册）

- 状态：**提议**（2026-10-29 评估；方向已确认，未实施；上线前置：托盘常驻壳）
- 日期：2026-10-29
- 影响：`app.vrmanifest`（新增，随包）、`src/openvr/`（新增）、`src/web/ipc`（新命令）、
  `src/ui/main.cpp`（托盘启动参数与单实例）、设置页「SteamVR」卡片、安装/卸载清理、
  `docs/02-features.md`、`docs/03-roadmap.md`

## 问题

用户戴上头显后希望 FaceGaze Bridge 已经就绪，而不是每次先去找快捷方式、双击、
再戴上头显。需求可以写成一句话：**SteamVR 起来，FGB 也起来；用户能随时撤销。**

这个需求有两个容易走偏的方向：

1. 直接改 SteamVR 的配置文件/注册表，看起来"能开机自启"，但格式未公开，SteamVR
   更新、校验或用户重装都会覆盖，出问题时还会被当成"这软件改坏了我的 SteamVR"。
2. 把 FGB 注册成 Steam 库里的 VR 应用。那需要 Steamworks AppID；本项目不通过
   Steam 分发，也不应该为了一个自启动去上架。

## 可选路径

| 方案 | 做法 | 结论 |
|---|---|---|
| A · 直接改配置 | 写 `steamvr.vrsettings` / `steamapps.vrmanifest` / 注册表 | **拒绝**：未公开格式，容易写坏用户配置，无法跨版本保证 |
| B · OpenVR `IVRApplications` | `AddApplicationManifest` + `SetApplicationAutoLaunch` | **采纳**：官方 API，VRCFT 与多个 SteamVR 工具的同款做法 |
| C · 注册为 Steam 库应用 | 需要 Steamworks AppID 与上架流程 | **不做**：超出本项目分发方式 |
| D · `VRApplication_Scene` 抢占场景 | 以场景应用身份启动 | **拒绝**：会和用户正在玩的 VR 游戏抢场景 |

## 决定（建议）

### 1. 随包提供 `app.vrmanifest`，与入口 exe 同目录

```json
{
  "source": "builtin",
  "applications": [
    {
      "app_key": "com.facegazebridge.desktop",
      "launch_type": "binary",
      "binary_path_windows": "FaceGaze Bridge.exe",
      "arguments": "--tray",
      "is_dashboard_overlay": true,
      "strings": {
        "en_us": {
          "name": "FaceGaze Bridge",
          "description": "Starts FaceGaze Bridge with SteamVR."
        },
        "zh_cn": {
          "name": "FaceGaze Bridge",
          "description": "随 SteamVR 启动 FaceGaze Bridge。"
        }
      }
    }
  ]
}
```

`binary_path_windows` 相对 manifest 所在目录解析；manifest 必须与
`FaceGaze Bridge.exe` 放在一起，移动安装目录后要重新注册。

### 2. 为什么 `is_dashboard_overlay` 必须是 `true`

`openvr.h` 对 `SetApplicationAutoLaunch` 的注释是：

> Sets the application auto-launch flag. This is only valid for applications which
> return true for `VRApplicationProperty_IsDashboardOverlay_Bool`.

也就是说，**普通后台程序没有"随 SteamVR 启动"这个开关**；能自启动的只有
dashboard overlay。VRCFT 的 `app.vrmanifest` 也是同样的写法（`is_dashboard_overlay: true`）。

FGB **不实现 VR 内面板**，只是借用这个官方机制；用户不会在头显里看到 FGB 的
dashboard 页面。这是需要写进用户手册的事实，避免"注册了却找不到界面"的误解。

### 3. 用 `VRApplication_Background` 初始化

`openvr.h` 对该类型的说明：

> Application should not start SteamVR if it's not already running, and should not
> keep it running if everything else quits.

这正好是"跟随 SteamVR"的语义：

- SteamVR 没起来时，注册/查询/开关都不可用；**不能让 FGB 反向拉起整个 SteamVR**
- SteamVR 退出后，FGB 不负责保活；是否跟着退出见 §6
- 初始化成功后，通过 `IVRApplications` 完成注册与开关

### 4. 状态机与用户操作

**OpenVR 调用放在引擎（`fgb-cli`）里，不放在外壳。** 外壳的职责仍然只有窗口与
引擎生命周期（[ADR-0016](0016-single-entry-desktop-app.md)）；设置页的开关通过
WebSocket 发给引擎，由引擎的 `src/openvr/` 小模块完成注册/读写，再把结果回给 UI。
这样命令行模式、探针、未来的托盘都共用同一套控制面，不需要外壳理解 OpenVR。

设置页新增一张「SteamVR」卡片，只暴露一个开关与一行状态：

| 状态 | 判定 | 界面 |
|---|---|---|
| `not_installed` | 找不到 `openvrpaths.vrpath` 或 `openvr_api.dll` | 开关禁用；说明"未检测到 SteamVR" |
| `not_running` | 运行库在，但 `VR_Init(Background)` 失败 | 开关禁用；说明"请先启动 SteamVR，再回来设置"并给「重试」 |
| `off` | 初始化成功，`GetApplicationAutoLaunch` 为 false | 可打开 |
| `on` | 初始化成功，`GetApplicationAutoLaunch` 为 true | 可关闭 |
| `error` | 注册/读回失败 | 显示具体原因与「重新注册」 |

关键规则：

- **绝不自动注册**。首次启动不写任何 SteamVR 状态；用户明确打开开关才注册。
- 打开：`AddApplicationManifest(绝对路径, false)` →
  `SetApplicationAutoLaunch(app_key, true)` → 再 `GetApplicationAutoLaunch` 读回，
  只有读回为 true 才提示成功。
- 关闭：`SetApplicationAutoLaunch(app_key, false)`；manifest 本身保留（程序还在磁盘上），
  避免下次打开时又要重新 Add。
- 「移除注册」：先关自启动，再 `RemoveApplicationManifest`。用于换目录、卸载前清理。
- 每次失败都要给可操作的原因（SteamVR 没开、manifest 路径不对、app_key 冲突），
  不能只给红点。这是项目已有的界面规则。

### 5. 自启动拉起桌面入口，进入托盘（托盘是上线前置）

manifest 指向 `FaceGaze Bridge.exe --tray`，理由：

- 维持 [ADR-0016](0016-single-entry-desktop-app.md) 的单入口与 Job 生命周期：
  窗口/托盘在，引擎在；从托盘「退出」才停引擎
- 外壳是 GUI 子系统，不会闪黑窗口；`fgb-cli` 是控制台程序，被 SteamVR 直接拉起
  会出现黑框
- 第二次双击/自启动时如果已有引擎，外壳走 `/healthz` 附着，不重复起引擎

**上线前置：托盘常驻壳（用户 2026-10-29 决定）**。在托盘完成之前，不发布 SteamVR
自启动——不能用一个"最小化到任务栏的窗口"冒充后台驻留，否则用户关闭窗口就停了追踪，
或者多出一个不知道干什么的窗口。

托盘版本需要明确两件事（实现时补 ADR，可能要修订 ADR-0016 的关窗语义）：

1. `--tray` 启动：SteamVR 拉起 FGB 时只显示托盘图标，不弹窗口，引擎照常工作
2. 关闭窗口的行为：建议"关闭窗口 = 收进托盘，托盘菜单「退出」= 停引擎并退出"；
   这会改变 ADR-0016 的"关窗即停"，必须显式记录

自启动前还要补一个**单实例守卫**：已有 FGB 窗口/托盘时，把现有窗口带到前台并退出，
不再开第二个实例。否则每次 SteamVR 启动都会多出一个托盘图标或窗口。

### 6. 是否跟随 SteamVR 退出

**v1 不做自动退出**。理由：

- 面捕模块的数据不依赖 SteamVR 场景；SteamVR 退出后继续输出并不危险
- 自动退出需要监听 OpenVR 事件或轮询 runtime 状态，边界情况多（SteamVR 重启、
  睡眠唤醒、用户只想关头显）
- 用户仍可用界面「停止引擎」或托盘菜单「退出」

后续可加 `--exit-with-steamvr`：初始化 OpenVR Background，收到
`VREvent_Quit` / runtime 不可用时退出。作为独立开关，默认关。

### 7. 不写注册表、不改 Steam 配置、不自动上架

所有持久化都由 OpenVR/SteamVR 自己完成。FGB 只调用官方接口，不直接编辑
`steamvr.vrsettings`、`steamapps.vrmanifest` 或注册表。

## 依赖与许可证

- 只 vendor `openvr.h`（BSD-3-Clause，Valve）。它是纯头文件，体积可忽略。
- `openvr_api.dll` **不随包分发**：运行时从用户已安装的 SteamVR 目录加载
  （优先读 `%LOCALAPPDATA%\openvr\openvrpaths.vrpath` 的 `runtime` 字段，
  再回退到 Steam 安装路径），因此不产生再分发义务。
- 新增依赖需要 ADR（项目规则 §1.8）：本 ADR 即是该例外。许可证说明进
  `THIRD_PARTY_NOTICES.md`（若还没有，则随实现补）。

## 实现落点（预估 2–4 人日，不含托盘本身）

| 工作 | 内容 | 人日 |
|---|---|---|
| 运行库定位 | 解析 `openvrpaths.vrpath`、回退 Steam 安装路径、加载 DLL | 0.5–1 |
| OpenVR 小封装 | `VR_Init(Background)`、manifest 注册/移除、开关读写、错误码转人话 | 0.5–1 |
| 外壳 | `--tray` 启动参数、单实例守卫（依赖托盘） | 0.5 |
| 设置页 | SteamVR 卡片、状态机、i18n（zh/en/ja）、失败提示 | 0.5–1 |
| 打包/验证 | manifest 随包、安装/卸载说明、手动验收与探针 | 0.5–1 |

托盘常驻壳单独估算 2–4 人日（托盘图标、菜单、收进托盘、退出、单实例），
不包含在上表内；它是本功能的上线前置，见 `docs/03-roadmap.md` M7.0。

## 验证

- [ ] 未装 SteamVR：开关禁用，文案说明原因，不崩溃
- [ ] SteamVR 未运行：开关禁用，提示"先启动 SteamVR"；启动后可重试
- [ ] 打开开关后，`GetApplicationAutoLaunch` 读回 true；SteamVR 重启后 FGB 进入托盘、不弹窗口
- [ ] 关闭开关后，SteamVR 重启不再拉起 FGB
- [ ] 「移除注册」后，SteamVR 的启动项里看不到 FGB
- [ ] 已有 FGB 实例时，自启动不会产生第二个窗口/托盘图标，而是恢复现有实例
- [ ] 移动安装目录后，旧路径失效时能提示"重新注册"
- [ ] 托盘未完成时，设置页不显示/禁用 SteamVR 自启动入口（不发布半成品）
- [ ] CI 不依赖 SteamVR：只有纯逻辑单测（manifest 生成、路径解析、错误码映射）

## 代价与风险

- **必须 SteamVR 在运行才能设置**。这是 OpenVR Background 的语义决定的，
  不是实现偷懒；界面要把这一点说清楚。
- **dashboard overlay 的语义借用**：FGB 不是 VR 面板，注册列表里可能会显示为
  overlay 应用；VRCFT 也如此，用户可接受，但文档要解释。
- **无 CI 覆盖**：真实注册只能在装 SteamVR 的机器上验收；CI 只测纯逻辑。
- **卸载残留**：程序被直接删除时，SteamVR 可能留下失效的 manifest 路径。安装器
  应尽量在卸载时调用移除；绿色版提供「移除注册」按钮。
- **自启动窗口打扰**：通过"托盘完成后才上线"解决；SteamVR 启动时只进托盘，
  不弹窗口。托盘本身不在本 ADR 的 2–4 人日内。
- **关窗语义变化**：引入托盘后，"关闭窗口 = 停引擎"（ADR-0016）需要改成
  "关闭窗口 = 收进托盘；托盘退出 = 停引擎"，实现托盘时补 ADR 并同步文档。

## 遗留问题（需要确认）

1. 托盘菜单的最小集合：显示窗口 / 停止引擎 / 退出 / 随 SteamVR 启动开关，
   是否还要"重启引擎"与"输入源状态"？
2. 是否需要在 SteamVR 启动时自动把上次的输入源/模块选好？（当前模块选择已持久化，
   但"没装模块就退出码 2"的看门狗弹窗在自启动场景下会打扰用户，需要静默降级策略）
3. `app_key` 的最终命名（`com.facegazebridge.desktop`）是否与未来产品域名一致。
