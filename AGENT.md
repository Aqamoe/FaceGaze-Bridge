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
- **新增输入源必须声明 `frame.channels`**（`channel::kExpression / kEyes / kHead`）。
  合并层只认声明，不猜值——`FaceFrame::neutral()` 里眼睛是 valid 的，靠值猜会让源被误判成眼追来源。

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

# 单测（当前 97 个用例）
./build/test/bin/Debug/fgb-tests.exe
# 或
ctest --test-dir build/test -C Debug --output-on-failure
```

WebView2 SDK 不在 vcpkg：configure 时从 NuGet 抓一次到 `build/_deps`。
离线/内网用 `-DFGB_WEBVIEW2_DIR=<已解压目录>`；完全不要界面用 `-DFGB_BUILD_UI=OFF`。
`ui/` 里的资源由 CMake 在 `fgb-cli` 链接后复制到 exe 旁——**只改页面也要重新构建 fgb-cli**
（或手动 `cmake -E copy_directory ui build/test/bin/Debug/ui`），否则跑的还是旧页面。

### 3.3 发布包（用户双击就能跑）

```powershell
cmake -S . -B build/release -G "Visual Studio 16 2019" -A x64
cmake --build build/release --config Release --parallel
cmake --install build/release --config Release --prefix dist
```

产物在 `dist/`：

```
dist/fgb-cli.exe        引擎（控制台）
dist/fgb-ui.exe         WebView2 界面外壳
dist/ui/                页面资源（index.html / app.js / style.css / i18n.js）
dist/启动 - VRCFT 模块.cmd   双击：接管 VRCFT 模块 + 开界面
dist/启动 - VMC.cmd          双击：VMC 输入 + 开界面
dist/README.txt         怎么用、日志在哪
```

发布前**必须**真的启动一次 `dist` 里的 .cmd，确认窗口起来、界面连上（`tools/probe_ui.py`
可以顶替肉眼）。发布包不随仓库提交（`/dist/` 已忽略）。

### 3.4 探针（比截图可靠）

```powershell
# 控制面 IPC：HTTP 诊断 + WebSocket 推送/命令
python tools/probe_ipc.py 8787

# 界面：通过 WebView2 远程调试端口跑 JS，验证页面真的渲染了数据
fgb-ui.exe --port 8787 --debug-port 9222
python tools/probe_ui.py 9222 8787
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
| 截图偏移/尺寸不对 | 截出来的窗口错位 | PowerShell 先 `SetProcessDpiAwarenessContext(-4)` 再量窗口 |
| `fgb.ini` 写盘 | 整份重写 | 加新键时必须**先读再写**，否则盖掉用户手改的内容 |

## 6. 代码地图

```
src/base/      日志（含订阅回调）、时钟、极简 JSON
src/core/      数据模型、无锁容器（SPSC ring / seqlock）、滤波与校准
src/osc/       OSC 编解码（零分配写入器）
src/net/       UDP / TCP / WebSocket 服务端（自写 RFC 6455 子集）
src/vrcft/     VRCFT 协议、沙箱、模块进程托管、模块发现（module_registry）
src/params/    参数目录、v2/v1 派生、地址路由、校准
src/sources/   输入（VMC / VRCFT 模块 / 抓包回放）
src/sinks/     输出（VRChat OSC）
src/engine/    合并、快照发布、扇出
src/web/       控制面 IPC（WS 消息 + 静态页面）
src/ui/        fgb-ui：WebView2 外壳（Win32 窗口，无业务逻辑）
ui/            页面资源（无构建步骤）
tools/         抓包、冒烟与探针脚本
docs/          方案、ADR、路线图
```

## 7. IPC 协议速查（`ws://127.0.0.1:<port>/ws`）

| 方向 | 消息 | 频率 / 时机 |
|---|---|---|
| →UI | `hello`（协议版本、版本号、端口） | 连接时 |
| →UI | `catalog`（365 个参数名 + 是否已绑定） | 连接时 / 换头像后 |
| →UI | `modules`（模块列表 + 当前选择 + 配置文件） | 连接时 / 扫描或改选择后 |
| →UI | `status`（帧率、tick、OSC、头像、滤波、`module{state,error}`、`sources[]` 每源贡献） | 10 Hz |
| →UI | `params`（数值数组，顺序同 `catalog`） | 10 Hz |
| →UI | `log`（级别/tag/正文；连接时补发最近 120 行） | 有日志时 |
| UI→ | `set_filter` / `set_module` / `scan_modules` / `reload_avatar` / `set_log_level` / `quit` | 按操作 |
| →UI | `ack`（命令、ok、error） | 每条命令 |

只读 HTTP 接口保留：`/api/status`、`/api/params`、`/healthz`（给 curl 与探针）。

## 8. 完成的定义（DoD）

一个计划点算"做完"，要同时满足：

- [ ] 代码 + 注释（说清为什么）
- [ ] 单测覆盖新增逻辑（能测的部分）
- [ ] 涉及界面/IPC 时，探针通过（不是"我看了一眼觉得对"）
- [ ] `docs/03-roadmap.md` 的进度说明更新；架构级取舍补 ADR
- [ ] Release 发布包出到 `dist/` 且真的启动过一次
- [ ] 提交 → 推送 → CI 绿
