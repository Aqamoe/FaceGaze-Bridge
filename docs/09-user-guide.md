# 09 · 用户手册（v1.0.x）

> 面向最终用户与排障人员。开发/集成请看 [README](../README.md)、
> [05-native-module-guide](05-native-module-guide.md)、[07-streaming-outputs](07-streaming-outputs.md)。

## 1. 安装与启动

**安装版**：运行 `FaceGazeBridge-Setup-<版本>.exe`。用户级安装，不需要管理员，
默认在 `%LOCALAPPDATA%\Programs\FaceGazeBridge`，会创建开始菜单快捷方式。
静默安装加 `/VERYSILENT /SUPPRESSMSGBOXES /NORESTART`。
卸载在 Windows「设置 → 应用」；交互式卸载会询问是否同时删除用户数据
（校准、预设、模块、日志），静默卸载不会删。

**绿色版**是目录：

```
FaceGaze Bridge.exe    唯一入口（窗口 + 拉起引擎）
fgb-cli.exe            引擎/命令行（平时不用手动开）
runtime\vrcft\         VRCFT 模块宿主运行时
ui\                    页面资源
sdk\                   集成文档与示例
```

1. 解压到**固定目录**（SteamVR 自启动会记录这个路径，移动后要重新注册）。
2. 双击 `FaceGaze Bridge.exe`。
3. 第一次启动如果没有任何模块，界面会提示去「模块」页导入或安装。

**关窗 = 收进托盘，追踪继续**；托盘右键「退出」才真正停止引擎。
`--tray` 启动只驻留托盘、不弹窗口（SteamVR 自启动用的就是它）。

### 第一次运行检查清单

- [ ] VRChat 已启动，游戏内 `Options → OSC` 已打开（默认发到 `127.0.0.1:9000`）
- [ ] VRCFT 本体已完全退出（模块沙箱端口独占，VRCFT 占着时 FGB 加载不了）
- [ ] 系统有 WebView2 Runtime（Win10/11 一般随 Edge 预装）
- [ ] 头显/串流软件已连上，PICO 的 `faceTrackingTransferProtocol` 已设为 1 或 2
- [ ] 「模块」页选中了模块，状态是「运行中」

## 2. 界面速览

| 页面 | 能做什么 |
|---|---|
| 状态 | 输入帧率、tick 迟到、OSC 计数、头像与命中参数、输入源、注视卡片、性能与资源 |
| 模块 | 导入本机 VRCFT 模块、zip 安装、启停/删除、设为启动、在线模块库、运行时来源 |
| 模块库 | 浏览 `registry.vrcft.io`，一键安装/更新（带缓存与进度） |
| 参数 | 365 个派生参数的热力图，搜索过滤 |
| 校准 | 88 形状 lo/hi、视线 gain/center、3D 预览、取当前、保存/重载 |
| 输出 | 参数集/眼追模式/输出目标、命名预设、重新读取头像参数表 |
| 日志 | 引擎与模块日志，级别过滤、自动滚动 |
| 设置 | 滤波、引擎启停、诊断包、SteamVR 启动项、界面语言 |

## 3. 输入源

默认是 VRCFT 模块（桌面入口）或 VMC（`fgb-cli` 默认）。另外两路直接用命令行开启：

| 输入 | 命令 | 端口 | 说明 |
|---|---|---|---|
| VMC | `--source vmc` | 39539 | VSeeFace / Warudo；必须跟 `Apply` 才生效 |
| ARKit LiveLink | `--source livelink` | 11111 | iPhone + Live Link Face（ARKit 模式） |
| 通用 OSC | `--source osc --osc-map <文件>` | 9103 | 任意 OSC 发送端，按映射表翻译 |

可重复 `--source` 做多源合并；`--expression-source` / `--eye-source` /
`--gaze-source` / `--head-source` 用源名（`vmc` / `livelink` / `osc` / `native:face`）
指定某一路由谁提供。V2 阶段 A 还支持高级拆分：

- `--eye-left-source` / `--eye-right-source`：左右眼分别选源；
- `--gaze-left-source` / `--gaze-right-source`：左右渲染注视分别选源；
- `--avatar-eye-from-gaze`：头像眼睛保留 `EYE` 源的 openness/pupil，方向改用
  `Gaze` 通道（默认关闭，头像仍只看 `Eyes`）。
- `--native-host`：原生模块走独立 `fgb-host-native.exe` 进程，模块崩溃不带走引擎；
  默认关闭。`--native-host-exe <路径>` 可覆盖宿主位置；`--no-host-restart` 关闭
  崩溃后的自动重启。界面「模块 → 启动实例」的「独立进程」开关按实例配置，
  写入 `modules.ini` 的 `[instance_host]`；宿主实例行还有「停止 / 启动 / 重启实例」，
  可在不重启引擎的情况下替换 DLL（停止 → 替换文件 → 启动）。
- `--attr-source <属性=源[,源]>`：逐属性有序源列表，属性包括
  `left/right-openness`、`left/right-pupil`、`left/right-gaze`、`vergence`；
  `--attr-fuse` 让多个新鲜源按置信度加权平均。

注视通道默认 100 ms 过期，可用 `--gaze-stale-ms` 调整；`--stale-ms` 会一次
设置所有通道。桌面入口会把不认识的参数转给引擎，所以也可以先托盘「退出」，
再运行 `"FaceGaze Bridge.exe" --source livelink`。

多模块组合在界面「模块」页配置：把模块添加为启动实例，再给表情 / 眼睛 / 注视 /
头姿四个角色选来源；配置写入 `%LOCALAPPDATA%\FaceGazeBridge\modules.ini`，
实例列表变化后需要重启引擎。界面里暂时不能切换输入源或编辑 OSC 映射表。

### 3.1 ARKit LiveLink

1. iPhone 装 Live Link Face，设置里的捕捉模式选 **ARKit**
   （MetaHuman Animator 模式走 14785，不是这个协议）。
2. 在 LiveLink 设置里添加电脑 IP，端口 11111；需要头姿时打开 Stream Head Rotation。
3. 电脑上运行 `fgb-cli run --source livelink`。第一次会弹 Windows 防火墙，
   只放行专用网络即可。

日志出现 `livelink: 收到数据: subject=... curves=61` 就说明数据到了。
没有数据时先确认手机和电脑在同一网段、端口没被 UE 占用。

**注意**：LiveLink 直连**尚未在 iPhone 真机验收**。头姿把 -1..1 解释为 ±90°，
实机若发现方向相反/幅度不对，需要调整 `kLiveLinkHeadMaxDeg` 与符号（ADR-0029）。

### 3.2 通用 OSC

映射表默认 `%LOCALAPPDATA%\FaceGazeBridge\osc_map.ini`；第一次用 `--source osc`
时会自动生成一份空表。格式：

```ini
[input]
port=9103

[map]
/avatar/parameters/JawOpen = JawOpen
/avatar/parameters/EyeLidLeft = EyeLidLeft, invert
/avatar/parameters/MouthSmileLeft = MouthSmileLeft, scale=0.8, offset=0.1
```

- 目标写统一形状名（`JawOpen`、`MouthSmileLeft`，大小写不敏感），或眼睛目标
  `EyeLidLeft/Right`、`EyeLeftX/Y`、`EyeRightX/Y`；
- `scale` / `offset`：`输出 = 原始值 × scale + offset`；
- `invert`：0..1 目标用 `1 - 值`，视线轴直接取反；
- 同一地址后写覆盖先写；不同地址映射到同一形状时相加；
- 改完表要重启引擎（`--source osc` 只在启动时读一次）。

默认只绑回环；发送端在另一台机器时加 `--osc-in-lan`。
日志 `osc: 监听 UDP ... N 条映射` 的 N 应该等于表里的条数；目标名写错会逐条警告并跳过。

```powershell
fgb-cli run --source osc --osc-map .\osc_map.ini --panel 8787
```

## 4. 输出

默认只开 VRChat OSC。其余输出要在「输出」页打开（或命令行显式给）：

| 输出 | 开关 | 协议 |
|---|---|---|
| VRChat OSC | 始终开启 | `/avatar/parameters/*`，地址来自头像参数表 |
| VMC | 输出页「启用 VMC」或 `--vmc-out host:port` | `/VMC/Ext/Blend/Val` + `Apply` |
| 共享内存 | 「启用共享内存」或 `--shm <name>` | `Local\FGB_Shm_<name>`，见 [06](06-shared-memory.md) |
| WebSocket JSON | 「启用 WebSocket JSON」或 `--ws-out <port>` | 见 [07](07-streaming-outputs.md) |
| UDP 二进制 | 「启用 UDP 二进制」或 `--udp-out host:port` | 656 字节固定帧，见 [07](07-streaming-outputs.md) |

保存后要点「保存并重启引擎」才生效；命令行参数永远覆盖保存值。
WebSocket 与控制面只绑 `127.0.0.1`。

### 命名预设

- 在输出页填好一套目标/开关 → 输入名称 → 「保存为预设」；
- 下拉选预设 → 「应用并重启」，一次完成切换；
- 预设文件在 `%LOCALAPPDATA%\FaceGazeBridge\outputs-profiles\*.ini`，可直接备份/复制；
- 预设只包含输出设置，不含校准/滤波/模块选择。

## 5. 校准

- 校准页每个形状有 lo/hi：把静止值设 lo、夸张值设 hi（或点「取当前」）；
- 视线有 gain/center：可把当前视线设为中心；
- 拖动立即作用于输出，点「保存」写入 `calibration.ini`（上一版留 `.bak`）；
- 3D 预览是**示波器**，不是你的头像：通用风格化人头，用于看哪个部位在动；
- 没有模型映射的形状标「仅数值」，仍然可以调；
- 5 点注视校准与漂移补偿仍未做（需要 VR 内校准界面）。

## 6. 模块兼容性（实测范围）

VRCFT 协议层已真机跑通；**模块覆盖仍是当前最大缺口**（roadmap 2.10 要求 ≥5 个真实模块）。

| 模块 | 硬件/来源 | 状态 | 备注 |
|---|---|---|---|
| `VRCFTPicoModule` v0.1.11 | PICO + PICO Connect | ✅ 真机验收（2026-10-29） | 先退出 VRCFT；真实面捕数据 |
| 只报 2D 点的原生模块（`samples/native-gaze` 的 POINTS_ONLY 模式） | 无 | ✅ 单测 + ABI 端到端（2026-10-31） | 只置 `FGB_ATTR_LEFT/RIGHT_POINT`：`valid=false`、点正常透传；未标定就只显示 `2D~`，标定后再加 `FGB_ATTR_POINT_VIEWPORT` 才是 `2D` |
| `native-sine` 示例 | 无 | ✅ 单测 + 端到端 | 无硬件验证链路 |
| 回放（`.fgbrec`） | 无 | ✅ 单测 + 探针 | 金样为真实 PICO 抓包 |
| VMC 输入 | VSeeFace 等 | ✅ 协议冒烟 | 必须跟 `Apply` 才生效 |
| SteamLink VRCFT Module | Quest（Steam Link） | ⚠ 仅安装/加载路径验证 | 缺 Quest 真机 |
| ETVR Tracking Module | iPhone | ⚠ 仅安装/加载路径验证 | 缺真机 |
| MediaPipe / LiveLink | 摄像头/iPhone | ❌ 未验证 | 当前机器未安装 |

加载失败时优先看「模块」页卡片里的原因，以及日志页。

## 7. 性能与诊断

状态页「性能与资源」：

- **tick 耗时**：引擎每帧合并/处理/输出的平均与 P95；不含休眠停顿
- **CPU / 工作集 / 句柄**：长跑看有没有泄漏
- **休眠恢复**：机器睡眠/长时间调试后自动复位的次数

设置页「版本更新 → 检查更新」只读取 GitHub Releases 的 latest 并提示；不会自动
下载或替换程序。发现新版本时「打开下载页」会用默认浏览器打开 release 页，
用户手动安装。命令行等价：`fgb-cli update-check`。

设置页「诊断 → 导出诊断包」会生成
`%LOCALAPPDATA%\FaceGazeBridge\diagnostics\FaceGazeBridge-diag-*.zip`，
包含日志、配置、最近 5 份崩溃转储和系统信息；不含模块 DLL/头像文件。
命令行等价：`fgb-cli diag bundle --out D:\diag.zip`。

崩溃时会在 `%LOCALAPPDATA%\FaceGazeBridge\crashes\` 留 `.dmp` + `.txt`
（版本、时间、异常码、模块路径），保留最近 20 份。

## 8. 故障排查

| 现象 | 先查什么 |
|---|---|
| 界面一直「等待引擎」 | `engine.log` 最后几行；8787–8796 是否被占；内置运行库是否缺失 |
| 模块页空白/没有输入源 | 点「重新扫描」→ 导入 → 设为启动 → 重启引擎；或从模块库安装 |
| 模块状态「故障」 | 卡片里的原因；最常见是端口被占（VRCFT 还在跑）或头显没连上 |
| 有帧但 VRChat 不动 | 状态页「命中参数」是否为 0；输出页「重新读取头像参数表」；确认游戏内 OSC 已开 |
| 表情动但方向/幅度不对 | 校准 lo/hi 与视线 gain；检查头像是否绑了对应参数 |
| 输出页保存了不生效 | 需要「保存并重启引擎」；`status.outputs` 是保存值，运行态看目标卡片 |
| 90/30 Hz 实际只有 60/24 | 旧版本取整问题已由 `SinkThrottle` 修复；确认用的是当前版本 |
| WS 连不上 | 只绑回环；端口是否被占，状态页会显示「未监听」 |
| LiveLink 一直没有数据 | 手机捕捉模式是否为 ARKit；IP/端口 11111；防火墙；日志有没有 `收到数据` |
| 通用 OSC 收到包但参数不动 | 是否 `--source osc`；映射目标名是否拼对；端口是否一致；跨机发送是否加了 `--osc-in-lan` |
| 共享内存读不到 | 名称是否一致（非法字符会替换成 `_`）；引擎是否真的开了该输出 |
| 3D 预览只剩 2D | WebGL2 不可用（远程桌面/老显卡）；校准功能仍可用 |
| SteamVR 开关灰 | 卡片写原因：未安装 / 未运行。FGB 不会自动拉起 SteamVR |
| 中文预设名崩溃 | 已修复（UTF-8 路径）；升级到当前版本 |
| 长跑内存上涨 | 导出诊断包，看工作集/句柄曲线；soak 脚本见下 |

无硬件时自测：

```powershell
# 回放真实抓包（如果有）
fgb-cli run --source replay --replay golden.fgbrec --replay-loop --panel 8787
# 纯 VMC 输入，脚本自己灌数据
python tools/soak.py --duration 600
# 两路新输入的端到端探针（自己拉起引擎、自己发包）
python tools/probe_inputs.py --cli build/test/bin/Debug/fgb-cli.exe
```

## 9. 隐私与安全

- 控制面与 WebSocket 只绑 `127.0.0.1`，不监听局域网；
- VMC/模块 UDP 输入默认只绑回环（mDNS 例外，必须收网卡组播）；
- 不改写 VRChat `config.json`；OSC 开关由用户在游戏内管理；
- 诊断包由用户主动导出，不含模块 DLL 与头像文件；
- 模块 DLL 属于可执行代码，只从自己的模块仓加载，安装前请确认来源；
- 详细审查记录见 [10-security-review](10-security-review.md)。

## 10. FAQ

**Q：能和 VRCFT 同时开吗？**
不能。模块沙箱端口独占，先完全退出 VRCFT。

**Q：必须装 VRCFT 吗？**
不需要。发布包带精简运行时；VRCFT 只在导入模块时作为来源。

**Q：支持哪些头显？**
取决于 VRCFT 模块。当前实测只有 PICO；Quest/iPhone 等需要对应模块与真机验证。

**Q：能驱动 Unity 角色吗？**
可以：开共享内存后用 [samples/unity-face-gaze](../samples/unity-face-gaze/README.md)
（Unity 2021.3+，Windows），或开 VMC 输出给认 VMC 的软件。

**Q：眼追能喂给游戏吗？**
当前提供 `/api/gaze` 与 WS 30 Hz 数据（HMD 局部坐标），OpenXR 注入未做。

**Q：为什么 CPU 显示 0%？**
引擎占用很低时，Windows 的进程 CPU 计数可能显示 0.0；以 tick P95 与工作集为准。
