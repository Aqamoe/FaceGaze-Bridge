# AGENT.md · 项目规则与作业须知

面向"接手这个仓库的人或 AI"。目标只有一个：**改动不能把已经跑通的东西弄坏**，
而且每次交付都要能被用户双击运行、亲手验证。

先读这三份，别从代码开始猜：

| 文档 | 作用 |
|---|---|
| `docs/01-architecture.md` | 分层、线程模型、数据流 |
| `docs/03-roadmap.md` | 计划与验收标准（"按计划进行"指的是这个） |
| `docs/adr/` | 所有"为什么这么选"的决定。新决定要补 ADR，别只写在提交信息里 |

---

## 1. 铁律

1. **不提交 `.workbuddy/`**。那里是本项目的内部工作数据（记忆、临时抓包、截图、探针脚本草稿），
   `.gitignore` 已排除；不要在仓库其它位置散落临时文件。
2. **提交前三件套**：本地构建成功 → 单测全绿 → 改动涉及 IPC/UI 时探针通过（见 §4）。
   **不要**"先推上去让 CI 告诉我哪错"。
3. **每次推送后必须盯 CI 到绿**：`gh run watch <id> --exit-status`。CI 带 `/WX`（警告即错误），
   本地没开 werror 时容易漏。
4. **每完成一个计划点，出一份可双击运行的发布包**（§3.3）。用户要实测，不要只给源码。
5. **一个计划点一次提交**，提交信息说清"为什么"，不要堆一大坨混合改动。
6. **不做没要求的重构**。大文件不要整份重写，用精确替换。
7. **注释只写"为什么"**，中文；不写"这行在干什么"。
8. **不引第三方库**（JSON、WS、测试框架、格式化都自己写了几十行）。真要引，先写 ADR 并说明体积/许可证代价。

## 2. 代码规矩

- C++20，MSVC `/W4 /permissive- /utf-8`；**热路径零分配、零异常**（见 `docs/04-engineering.md` §4）。
- 跨线程共享要么 `std::atomic`，要么有明确的所有权注释；无锁容器在 `core/cont/`。
- 公开头文件不 `#include <windows.h>`（宏污染），需要时 PIMPL / 前置声明。
- 坐标系转换只允许出现在 `core/coord.hpp`。
- 结构化数据一律**数据驱动**（参数目录、模块元数据都是查表，不硬编码分支）。
- UI 是原生 JS，无构建步骤；**不要**为了一个功能引入 npm 工具链。
- 界面文案一律走 `ui/i18n.js` 的 `t()`：新增文案**三语齐全**（zh/en/ja），
  静态文案用 `data-i18n` 属性。引擎日志保持中文，不要翻译。
- **原生模块必须带 `fgb-native.json`**（字段同 module.json），模块仓不加载 DLL 就列出元数据；
  运行态统一走 `ISource::module_status()`，不要按具体 Source 类型分支。
- **模块只从 `%LOCALAPPDATA%\FaceGazeBridge\modules` 加载**；VRCFT 的 `CustomLibs` 只是导入候选，
  任何"直接扫别人的目录然后 LoadLibrary"的写法都不要加（[ADR-0017](docs/adr/0017-module-store-library-runtime.md)）。
- **新增输入源必须声明 `frame.channels`**（`channel::kExpression / kEyes / kGaze / kHead`）。
  合并层只认声明，不猜值——`FaceFrame::neutral()` 里眼睛是 valid 的，靠值猜会让源被误判成眼追来源。
- **VMC 的 blendshape 名字表只有一份**（`src/vmc/vmc_map`）。输入和输出都 include 它，
  不要各写一份——漂移的症状是"能收不能发"，且看起来像对端的问题。

### 2.1 界面规则（每一条都是踩过坑后加的）

| 规则 | 具体要求 |
|---|---|
| 技术栈 | 原生 JS/HTML/CSS，无构建步骤，不引前端框架/图标库；图标用内联 SVG，颜色用 `:root` 变量 |
| 文案 | 所有可见文字走 `ui/i18n.js`：静态用 `data-i18n`，动态用 `t()`；**新增文案必须 zh/en/ja 三语齐全**，缺键会回落中文 |
| 确认框 | **禁止 `window.confirm/alert/prompt`**。用应用内 `ask_confirm({title,message,danger})`，支持 Esc/遮罩取消、Enter 确认 |
| 操作反馈 | 任何会失败或耗时的操作都必须有反馈：进行中状态 + 成功 toast + 失败 toast（`show_toast(text,"error")`）。**不允许点完没反应** |
| 长任务 | 下载/安装显示进度条与百分比；刷新期间列表仍可浏览和安装缓存里的模块，不因刷新禁用整个页面 |
| 隐藏元素 | 全局 `[hidden]{display:none!important}` 必须保留——组件自己的 `display:flex/grid` 会压过 UA 的 `[hidden]`，曾导致空弹窗盖住页面 |
| 滚动 | 滚动容器（`.main`/`.content`）必须显式 `min-height:0`，否则 flex/grid 子项撑破视口、底部内容看不到 |
| 页面结构 | 模块管理（已安装/导入）与在线模块库是两个独立页面；模块库有侧栏入口，模块页有快捷按钮 |
| 失败文案 | 显示可操作的具体原因（端口占用、缺 DLL、校验失败），不要只给红点或"失败" |
| 状态一致性 | 服务端消息是唯一事实来源；UI 只做展示与命令发送，不在页面里猜状态。库/模块同步后必须重算"已安装/可更新" |
| 版本比较 | 已安装/有新版本用**语义版本比较**，不要字符串相等；包内版本可能比库条目新 |
| 边界 | 最小窗口 1100×700 DIP 下布局可用；窄窗口侧栏只留图标；窗口尺寸按 DIP 缩放 |
| 验证 | 界面改动必须用 `tools/probe_ui.py`（CDP）验证真实数据与交互，不能只靠截图或"看起来对" |
| 关窗语义 | 关闭窗口 = 收进托盘，追踪继续；托盘「退出」才停引擎（[ADR-0022](docs/adr/0022-tray-shell-and-close-semantics.md)）。页面/文档不要再写"关窗即停" |


## 3. 构建 / 测试 / 发布

### 3.1 本机工具链

- `build/test`：VS2019 生成器的 Debug 目录（日常编译与单测）。
- `build/release`：VS2019 生成器的 Release 目录（出发布包用）。
- CI 用 VS2022（`-G "Visual Studio 17 2022"`），Debug + Release 各跑一遍，两边都要过。

### 3.2 日常命令（PowerShell）

```powershell
# 配置 + 增量构建
cmake -S . -B build/test -G "Visual Studio 16 2019" -A x64
cmake --build build/test --config Debug --parallel

# 单测（当前 170 个用例）
./build/test/bin/Debug/fgb-tests.exe
# 或
ctest --test-dir build/test -C Debug --output-on-failure
```

原生模块示例（`samples/native-sine`）由 `-DFGB_BUILD_SAMPLE_MODULE=ON` 构建（默认开），
产物 `fgb-sample-native.dll` 在 bin 目录；它不依赖硬件，单测和 `--source vrcft`
（模块仓里选它）都能直接跑。

WebView2 SDK 不在 vcpkg：configure 时从 NuGet 抓一次到 `build/_deps`。
离线/内网用 `-DFGB_WEBVIEW2_DIR=<已解压目录>`；完全不要界面用 `-DFGB_BUILD_UI=OFF`。
`ui/` 里的资源由 CMake 在 `fgb-cli` 链接后复制到 exe 旁——**只改页面也要重新构建 fgb-cli**
（或手动 `cmake -E copy_directory ui build/test/bin/Debug/ui`），否则跑的还是旧页面。

模块仓与运行时（[ADR-0017](docs/adr/0017-module-store-library-runtime.md)）：

```
%LOCALAPPDATA%\FaceGazeBridge\modules\<id>\   module.json + dll + config.ini
%LOCALAPPDATA%\FaceGazeBridge\modules.ini       启用/启动选择/来源
%LOCALAPPDATA%\FaceGazeBridge\library.json     模块库缓存
<exe>/runtime/vrcft/                          内置精简 VRCFT 运行时（发布包带）
```

```powershell
fgb-cli modules                     # 仓里有什么、本机有什么可导入、运行时来源
fgb-cli modules import-all          # 导入本机 VRCFT 模块（复制进仓）
fgb-cli modules install <x.zip|dll> # 手动安装
fgb-cli modules use <id>|none       # 启动模块（重启引擎后生效）
fgb-cli modules enable|disable <id>
fgb-cli modules remove <id>
```

### 3.3 发布包（用户双击就能跑）

```powershell
cmake -S . -B build/release -G "Visual Studio 16 2019" -A x64
cmake --build build/release --config Release --parallel
cmake --install build/release --config Release --prefix dist
```

产物在 `dist/`：

```
dist/FaceGaze Bridge.exe   唯一入口：界面窗口 + 拉起引擎（图标/版本信息在资源里）
dist/fgb-cli.exe           引擎（命令行用；外壳启动时会调用它）。不带产品图标，
                           免得在资源管理器里被当成第二个启动入口
dist/fgb-host-native.exe   原生模块跨进程宿主（--native-host 时使用）
dist/runtime/vrcft/        精简 VRCFT 宿主运行时（约 106 MB，卸载 VRCFT 也能跑模块）
dist/sdk/fgb_module_api.h  原生模块 ABI 头（给模块开发者）
dist/sdk/fgb_native_host_shm.h 跨进程宿主共享内存布局
dist/ui/                   页面资源（index.html / app.js / face.js / style.css / i18n.js / logo.png）
dist/ui/face/              3D 模型与形状映射（head_v1.fgbface / face_map.json）
dist/app.vrmanifest        SteamVR 启动项注册清单（与入口 exe 同目录）
dist/NOTICE                第三方许可证说明（含 OpenVR / VRCFT）
dist/licenses/             openvr.h 的 BSD-3-Clause 许可证原文
dist/README.txt            怎么用、日志在哪
```

`cmake --install` **不会**生成 `runtime/vrcft`（不能把 100 MB 二进制塞进仓库），
装完补一步（需要本机装有 VRCFT）：

```powershell
python tools/build_vrcft_runtime.py --dest dist/runtime/vrcft
```

入口只有一个（[ADR-0016](docs/adr/0016-single-entry-desktop-app.md)）：
外壳探到 8787 没有引擎就隐藏启动 `fgb-cli run --source vrcft --panel 8787`，
用 Job 对象把引擎绑在**外壳进程**的生命周期上（[ADR-0022](docs/adr/0022-tray-shell-and-close-semantics.md)）：
关闭窗口 = 收进托盘、追踪继续；托盘「退出」才停引擎。不再有 `.cmd`。

`FaceGaze Bridge.exe --tray` 是自启动形态：不显示窗口、不预建 WebView2，只驻留托盘
（SteamVR 自启动会用它）。已有实例时再次启动只把现有窗口带到前台，不会开第二个托盘图标。

发布前**必须**真的启动一次 `dist/FaceGaze Bridge.exe`，确认窗口起来、界面连上
（`tools/probe_ui.py` 可以顶替肉眼）。发布包不随仓库提交（`/dist/` 已忽略）。

### 3.4 探针（比截图可靠）

```powershell
# 控制面 IPC：HTTP 诊断 + WebSocket 推送/命令（含 /api/gaze 与 30 Hz gaze）
python tools/probe_ipc.py 8787

# 界面：通过 WebView2 远程调试端口跑 JS，验证页面真的渲染了数据
# 包含 3D 模型加载、morph、区域高亮、SteamVR 卡片（只读检测，不切换真实自启动）
# 校准页/输出设置页会真的保存：引擎启动时加 --calibration 与 --outputs-config 指到临时文件，
# 别让探针写用户默认的 calibration.ini / outputs.ini
# 已经有别的实例在跑时，必须给测试实例单独的 user-data，否则 CDP 端口不会监听；
# 带 --debug-port 的实例不参与单实例检查，可以故意多开（ADR-0022 §4）
"FaceGaze Bridge.exe" --user-data %TEMP%
gb-wv --debug-port 9222
python tools/probe_ui.py 9222 8787

# OSCQuery：不开 FGB，直接看 mDNS 端点、头像名/ID、可写参数计数
python tools/probe_oscquery.py

# 流式输出：WS JSON 握手/字段/实测帧率；UDP 二进制长度/magic/属性位/形状
fgb-cli run --source replay --replay <抓包> --replay-loop --ws-out 9101 --udp-out 127.0.0.1:9102
python tools/probe_ws_out.py 9101 5
python tools/probe_udp_out.py 9102 5

# VMC 第二路输出：按接收端视角收包，检查 Blend/Val + Apply + 头姿
fgb-cli.exe run --source replay --replay <抓包> --replay-loop --vmc-out 127.0.0.1:9100
python tools/probe_vmc_out.py 9100 5

# 五路 Sink 并发压测：VRChat/VMC/SHM/WS/UDP 同时开，报帧率与延迟增量
python tools/probe_sinks_stress.py --seconds 5

# 输出预设：自己拉起临时引擎，走保存/应用/删除（会写临时 outputs.ini）
python tools/probe_output_profiles.py

# 崩溃捕获：故意空指针子进程，检查 dmp/说明文件
python tools/probe_crash.py

# 长跑：默认 10 分钟，--duration 28800 为 8 小时
python tools/soak.py --duration 600

# 更新检查（探针内置本地 HTTP 服务，不走 GitHub）
python tools/probe_ipc.py 8787
# 安装包（需要 Inno Setup 6；先 cmake --install 到 dist）
powershell -ExecutionPolicy Bypass -File packaginguild_installer.ps1
```

没有真机数据时用回放跑通链路：

```powershell
./build/test/bin/Debug/fgb-cli.exe run --source replay --replay .workbuddy/tmp/golden.fgbrec --replay-loop --panel 8787
```

`.workbuddy/tmp/golden.fgbrec` 是 25 秒真实 PICO 抓包（不进仓库），金样测试与回放都靠它。

多源合并的本地验证（**不需要硬件**）：

```powershell
./build/test/bin/Debug/fgb-cli.exe run --source vmc --source replay `
    --replay .workbuddy/tmp/golden.fgbrec --replay-loop --panel 8787
python tools/send_vmc.py            # 30 Hz 灌 jawOpen，造第二路输入
# then: GET /api/status 的 sources 里，vmc 应只喂表情、replay 只喂眼追；
# 加 --expression-source replay 后两路互换。界面的「输入源」卡片同理。
```

## 4. 推送流程

```powershell
git add -A && git commit -F <消息文件>
git push origin main
gh run list --limit 1          # 拿 run id
gh run watch <id> --exit-status
```

- 提交信息：英文祈使句标题（≤72 字符）+ 正文解释动机与取舍，列出验证方式。
- **不 amend 已推送的提交，不 force push main。**
- `CRLF will be replaced by LF` 的警告是正常的（仓库按 LF 存，工作区是 CRLF）。
- 误提交大文件/产物/缓存时，补一个 `chore:` 提交清理，并补 `.gitignore`，不要改写历史。

## 5. 已知坑（都是踩过的）

| 坑 | 现象 | 处理 |
|---|---|---|
| MSVC printf + 中文/全角标点紧跟 `%s` | C4474「参数太多」，`/WX` 下编译失败 | `%s` 前留 ASCII 字符，或 `std::string` 拼好再 `fputs`（`FGB_*` 宏没有格式检查，不受影响） |
| JS 局部 `const state` 遮蔽全局 `state` | `Cannot access 'state' before initialization`，表现为界面某块永远不刷新 | 局部变量换名（如 `chip_state`） |
| WebView2 导航完成事件 | `ICoreWebView2NavigationCompletedHandler` 不存在 | 正确名字是 `...EventHandler` |
| `CoreWebView2EnvironmentOptions` | `Make<T>(args)` 编译失败 | 只有默认构造：`Make<T>()` 后 `put_AdditionalBrowserArguments(...)` |
| 页面连不上 `ws://` | https 页面被 mixed-content/PNA 拦 | 页面必须由引擎的 `http://127.0.0.1:<port>/` 提供（同源同端口） |
| 模块 sandbox socket | 模块报 `SocketException (10048)` | 上一个模块进程/VRCFT 本体占着端口；先关掉再试 |
| 后台进程 | 命令返回后进程被杀 | 启动→验证→收尾写在**同一条命令**里 |
| 端口探测 | 首次启动要多等 2–3 s | 有的机器（防火墙/安全软件）对未监听端口的 SYN 静默丢弃，逐个 `/healthz` 会等满超时。先用 bind 判断有没有人监听，只对有人听的端口做健康检查 |
| 截图偏移/尺寸不对 | 截出来的窗口错位 | PowerShell 先 `SetProcessDpiAwarenessContext(-4)` 再量窗口 |
| `fgb.ini` 写盘 | 整份重写 | 加新键时必须**先读再写**，否则盖掉用户手改的内容 |
| zip 安装 | 解压后目录结构不对/越界 | 用系统 `tar.exe` 解压，再校验路径/符号链接/体积；不要自己拼 zip 解析 |
| 模块更新失败 | 提示"正在被占用" | 引擎加载模块时 DLL 被锁；先停引擎再更新（界面会拒绝删除运行中的模块） |
| 模块装完仍显示"安装" | 包内没有 module.json，ID 退化成 DLL 名 | 模块库安装必须传 `ModuleMetadataOverride` 兜底；版本比较用语义版本，不要用字符串相等 |
| 模块库刷新超时 | WinHTTP 12002 | registry.vrcft.io 偶发卡 20 s；库内部重试一次，失败时保留缓存 |
| WebView2 调试端口 | 9222 不监听 | 已有实例共用 user-data 时新实例不会开 CDP；测试实例加 `--user-data %TEMP%\...` |
| `hidden` 不生效 | 弹窗/提示条启动就在，按钮点不动 | 组件 CSS 的 `display:flex/grid` 会压过 UA 的 `[hidden]`；全局加 `[hidden]{display:none!important}` |
| 页面不能滚动/底部被裁 | 内容超出窗口但没有滚动条 | grid/flex 子项默认 `min-height:auto`；`.main`/`.content` 必须显式 `min-height:0`，否则 `.content` 的 `overflow-y:auto` 不生效 |
| 托盘图标消失 | 资源管理器重启后图标不再显示 | `Shell_NotifyIcon` 的图标属于 explorer；收到系统广播 `TaskbarCreated` 要重新 `NIM_ADD`（`src/ui/main.cpp` 已接） |
| `FindWindow` 找不到自家窗口 | 另一个进程里按类名找隐藏窗口返回 0，`EnumWindows` 却能看到 | 不要用 `FindWindow` 做单实例唤醒；用 `PostMessageW(HWND_BROADCAST, 注册消息)`（ADR-0022 §4） |
| 校准档案写坏 | 保存时断电/被杀，档案变成半截，下次启动校准全丢 | 用 `save_profile_atomic()`：`.tmp` → 旧档案留 `.bak` → `MoveFileExW(REPLACE_EXISTING)` |
| 校准页滑杆"动了没反应" | 勾了启用但 lo/hi 还是 0..0 | `set_calibration_shape` 在"启用且跨度近 0"时给 0..1 默认；`capture_calibration` 跨度不足会明确回 error |
| 引擎在 24 秒后 CPU 100% | 回放 `--replay-loop` 到 EOF 后整份文件在同一 tick 里无限重放 | `ReplaySource::poll()` 的 `elapsed` 必须在循环内重算（loop 会重置 `t0_`） |
| 页面一卡/暂停，引擎就不动了 | 阻塞 socket 的 `send` 等对端读；`accept` 在无连接时还会再阻塞一次 | 监听/客户端 socket 都设非阻塞；accept 一次只收一个；出站缓冲 + 写集合，慢客户端丢弃（`net/ws_server.cpp`） |
| 3D 模型与映射表对不上 | morph/区域改名后页面请求不到，或选中高亮为空 | 跑 `python tools/check_face_model.py`；资产由 `tools/gen_face_model.py` 生成，morph/区域名是契约 |
| SteamVR 开关一直是灰的 | 状态是 `not_running`/`not_installed`，或缺失 manifest | 卡片会写原因；`/api/steamvr` 可 curl；真实注册必须 SteamVR 正在运行 |
| OSCQuery 找不到或误判 | 启动回退本地 JSON；同机 OVRToolkit 也广播 `_oscjson._tcp` | mDNS 收满 150 ms 窗口再选，只认实例名以 VRChat 开头；HTTP 端口动态，别探 9001；`python tools/probe_oscquery.py` 先确认 |
| 输出帧率比设定低 | 90 Hz 实际 60、30 Hz 实际 24 | `now-last >= interval` 在 120 Hz tick 下会被量化；用 `sinks/throttle.hpp` 的 `SinkThrottle` 按截止时间调度 |
| 输出页保存了但没生效 | 运行态仍是旧目标/开关 | 保存只写 `outputs.ini`，要点「保存并重启引擎」；`status.outputs` 是保存值，`*_out` 是运行态（ADR-0025） |
| 中文预设名让测试 abort | 堆栈在 `std::filesystem::_Convert_narrow_to_wide` | Windows 的 `path(std::string)` 按 ANSI 解释；用户文本路径走 `config::path_from_utf8()`，INI 层用宽字符 API（ADR-0027） |
| VMC/模块 UDP 收不到局域网包 | 这是刻意加固：输入口默认只绑 `127.0.0.1`；mDNS 显式 `any_interface=true` | 不要把输入口改回 `INADDR_ANY`（docs/10） |
| 崩溃后找不到转储 | 看 `%LOCALAPPDATA%\FaceGazeBridge\crashes\` 的 `.dmp`+`.txt`；设置页可导出诊断包 | MiniDump 不是全内存 dump；堆损坏严重时可能写失败（ADR-0028） |
| PowerShell/Inno 脚本中文乱码或解析失败 | `.ps1`/`.iss` 必须保存为 **UTF-8 with BOM**（Windows PowerShell 5.1 按 ANSI 读无 BOM 文件） | 编辑后确认前 3 字节是 EF BB BF |
| 安装器找不到 ISCC.exe | Inno Setup 可能装在 `%LOCALAPPDATA%\Programs\Inno Setup 6` | `build_installer.ps1` 已覆盖该路径；也可 `winget install JRSoftware.InnoSetup` |

## 6. 代码地图

```
src/base/      日志（含订阅回调）、时钟、极简 JSON、进程资源、崩溃转储、诊断包
src/core/      数据模型（含 GazeState）、无锁容器、滤波、校准与 GazeEstimator
src/osc/       OSC 编解码（零分配写入器）
src/net/       UDP / TCP / WebSocket 服务端（自写 RFC 6455 子集）、WinHTTP 下载、更新检查、mDNS 发现
src/config/    INI 与输出设置（outputs.ini + outputs-profiles/ 命名预设；命令行可覆盖）
src/vrcft/     VRCFT 协议、沙箱、模块进程托管、模块仓（module_registry）、模块库（module_library）
src/vmc/       VMC blendshape 名字表（输入/输出共用，只有这一份）
src/params/    参数目录、v2/v1 派生、地址路由、OSCQuery 头像发现、校准（含预览取样 preview_tap）
src/sources/   输入（VMC / VRCFT 模块 / 原生 C ABI 模块 / 抓包回放）
src/sinks/     输出（VRChat OSC / VMC / 共享内存 / WebSocket JSON / UDP 二进制；SinkThrottle）
src/engine/    合并、快照发布、扇出
src/openvr/    SteamVR 启动项：运行库定位、openvr_api.dll 动态加载、IVRApplications 注册
src/web/       控制面 IPC（WS 消息 + 静态页面）
src/ui/        FaceGaze Bridge.exe：WebView2 窗口 + 托盘 + 拉起/回收引擎（无业务逻辑）
sdk/           公开 ABI 头（模块 fgb_module_api.h、共享内存 fgb_shared_frame.h、UDP 帧 fgb_udp_frame.h）
samples/       native-sine：无硬件的原生模块示例；unity-face-gaze：Unity 共享内存读取/驱动示例
ui/            页面资源（无构建步骤；face.js 是 WebGL2 渲染器，face/ 是模型与映射表）
tools/         抓包、冒烟、探针，以及 build_vrcft_runtime.py（出精简运行时）、
               gen_face_model.py / check_face_model.py（3D 资产生成与校验）
docs/          方案、ADR、路线图、原生模块指南（05）、共享内存（06）、流式输出（07）、注视服务（08）
```

## 7. IPC 协议速查（`ws://127.0.0.1:<port>/ws`）

| 方向 | 消息 | 频率 / 时机 |
|---|---|---|
| →UI | `hello`（协议版本、版本号、端口） | 连接时 |
| →UI | `catalog`（365 个参数名 + 是否已绑定） | 连接时 / 换头像后 |
| →UI | `modules`（已安装 + 可导入 + 运行时 + 启动选择） | 连接时 / 扫描或任何模块变更后 |
| →UI | `library`（模块库列表 + 缓存状态 + 下载进度） | 连接时 / 刷新或安装过程中 5 Hz |
| →UI | `status`（帧率、tick、OSC、头像、滤波、`modules[]` 每实例状态、`sources[]` 每源贡献） | 10 Hz |
| →UI | `params`（数值数组，顺序同 `catalog`） | 10 Hz |
| →UI | `gaze`（方向/俯仰偏航/聚散/置信度/fixation/saccade/head_pose） | 30 Hz |
| →UI | `calibration`（88 个形状的 lo/hi/active + 视线 + 路径 + 未保存标记） | 连接时 / 任何校准改动后 |
| →UI | `preview`（校准前 `raw[88]` + 校准后 `out[88]` + 视线 + seq） | 30 Hz，**只在校准页订阅时** |
| →UI | `steamvr`（状态/原因/路径/registered/autolaunch/busy） | 连接时快照；每次检测/开关/移除完成后 |
| →UI | `outputs_profiles`（预设名列表/active/dir） | 连接时 / 任何预设或保存值变更后 |
| →UI | `diagnostics`（导出结果/路径/错误） | 后台打包完成后 |
| →UI | `update`（current/latest/tag/name/url/available/error） | 更新检查完成后 |
| →UI | `log`（级别/tag/正文；连接时补发最近 120 行） | 有日志时 |
| UI→ | `set_filter` / `set_outputs` / `set_module` / `set_module_enabled` / `scan_modules` / `reload_avatar` / `set_log_level` / `quit` | 按操作 |
| UI→ | `import_module` / `import_modules`（逗号分隔 id）/ `import_all` / `install_module_archive` / `delete_module` / `open_modules_dir` | 模块管理 |
| UI→ | `refresh_library` / `install_library` / `restart_engine` | 模块库与重启（退出码 3） |
| UI→ | `set_preview` / `set_calibration_shape` / `set_calibration_eye` / `capture_calibration` / `save_calibration` / `reload_calibration` / `reset_calibration` | 校准（实时生效，显式保存） |
| UI→ | `refresh_steamvr` / `set_steamvr_autolaunch` / `remove_steamvr_manifest` | SteamVR（后台线程执行，完成后推 `steamvr`） |
| UI→ | `save_output_profile` / `apply_output_profile` / `delete_output_profile` | 命名输出预设（应用会写 outputs.ini，界面随后重启引擎） |
| UI→ | `export_diagnostics`（可选 `out` 路径） | 后台导出诊断包，完成后推 `diagnostics` |
| UI→ | `check_updates`（可选 `url`，探针用）/ `open_update_page` | 只读检查 GitHub Releases；只允许打开本项目 GitHub 页 |
| →UI | `ack`（命令、ok、error） | 每条命令 |

HTTP（同一端口，只绑回环）：`/healthz`、`/api/status`、`/api/params`、
`/api/calibration`、`/api/preview`、`/api/gaze`、`/api/steamvr` 只读；`/api/quit`、`/api/restart`
是控制动作，只给托盘外壳与排障用（外壳不实现 WebSocket 客户端，见 ADR-0022 §5）。

## 8. 完成的定义（DoD）

一个计划点算"做完"，要同时满足：

- [ ] 代码 + 注释（说清为什么）
- [ ] 单测覆盖新增逻辑（能测的部分）
- [ ] 涉及界面/IPC 时，探针通过（不是"我看了一眼觉得对"）
- [ ] `docs/03-roadmap.md` 的进度说明更新；架构级取舍补 ADR
- [ ] Release 发布包出到 `dist/` 且真的启动过一次
- [ ] 提交 → 推送 → CI 绿
