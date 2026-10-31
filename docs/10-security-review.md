# 10 · 安全审查（M6.7）

- 日期：2026-10-30
- 范围：本地控制面、静态文件、模块安装、配置写入、网络输入、诊断包、更新
- 结论：**未发现 P0/P1；修复 2 处加固点，其余为已接受的低风险**

## 1. 控制面与数据口

| 面 | 绑定 | 结论 |
|---|---|---|
| HTTP + WebSocket（`/ws`、`/api/*`） | `127.0.0.1`（`ws_server.cpp` 显式 `INADDR_LOOPBACK`） | ✅ 不暴露给局域网 |
| VMC 输入 | 本次修复前 `INADDR_ANY`；现在默认 `127.0.0.1` | ✅ 已加固 |
| VRCFT 模块沙箱 UDP | 同上，默认回环 | ✅ 已加固 |
| mDNS 发现 | `INADDR_ANY`（必须收网卡组播） | 允许，只读查询 |
| VRChat OSC / VMC / UDP 输出 | 用户指定的目标地址 | 由用户控制 |

**残余风险**：控制面无鉴权。任何本机进程（包括浏览器里的恶意页面通过 DNS rebinding 之外的
本地请求）都可以调用 `quit` / `restart_engine` / 安装模块。设计上它就是本机控制面；
不引入 token 是为了让 `curl`/探针/托盘都能直接用。若将来暴露到局域网，必须同时加鉴权。

## 2. 静态文件服务

`IpcServer::serve_static` 的防护：

- 拒绝长度 > 512、含 `..`、反斜杠、NUL 的路径；
- 只发白名单 MIME（html/js/css/json/svg/png/woff2/ico/fgbface）；
- `weakly_canonical` 后要求目标在 `ui` 目录内。

**本次修复**：前缀比较原先只做字符串 `compare`，`root=C:\app\ui` 会错误放行兄弟目录
`C:\app\uievil`（仅当 ui 目录里存在指向它的软链接时可达）。现在要求目标路径在 root 之后
紧跟 `\` 或 `/`。

## 3. 模块安装（zip / DLL）

- 模块只从 `%LOCALAPPDATA%\FaceGazeBridge\modules` 加载；VRCFT 目录只作为导入来源
  （[ADR-0017](adr/0017-module-store-library-runtime.md)）；
- zip 解压用系统 `tar.exe`；解压后校验：
  - 每个文件 canonical 后必须仍在目标目录内（zip slip）；
  - 拒绝符号链接；
  - 解压后总量 ≤ 512 MB、文件数 ≤ 4000（zip bomb）；
- 实测本机 `bsdtar 3.8.9` 对含 `../` 的 zip 直接报错并拒绝该条目（
  实验记录：`../escaped.txt: Path contains '..'`），不会写到目标目录外。

**残余风险**：校验在解压之后，依赖 bsdtar 自身的路径保护；若未来换用其它解压器，需要
在解压前先列条目校验。模块 DLL 本身是可执行代码，安装即信任其来源；库下载有 MD5 校验
（registry 提供），但没有代码签名。

## 4. 配置与用户数据

- 只写自己的目录：`%LOCALAPPDATA%\FaceGazeBridge\{outputs.ini, calibration.ini,
  modules.ini, outputs-profiles, crashes, diagnostics}` 与 `%APPDATA%\FaceGazeBridge\fgb.ini`；
- outputs/calibration 原子写（`.tmp` → 旧文件 `.bak` → `MoveFileEx`）；
- **不改写 VRChat `config.json`**；OSC 开关由用户在游戏内管理（OSC 自动启用仍暂缓）；
- 中文预设名修复了 Windows ANSI 路径问题（UTF-8 → 宽字符；INI 层 `_wfopen_s`）。

## 5. 诊断包与崩溃转储

- 用户主动导出；包含日志、配置、最近 5 份 dmp、系统信息；
- **不包含**模块 DLL、头像配置/模型、VRChat 账号数据；
- 附件名做相对路径与 `..` 校验，不能写到暂存目录外；
- 崩溃处理只写转储，不执行外部命令；
- 分享诊断包前仍建议检查 `engine.log` 中是否含本机路径/用户名。

## 6. 网络下载与自动更新

- VRCFT 模块库走 WinHTTP，下载后按 registry 给的 MD5 校验；
- **原生模块库（fgb-registry，schema v2）**：`schemaVersion != 2` 整体拒绝；
  只接受 `https` 下载地址（`127.0.0.1`/`localhost` 供探针联调）；安装前必须
  ① 字节数与索引 `size` 一致、② `sha256`（BCrypt）一致、③ 解压后核对包内
  `fgb-native.json` 的 `ModuleId` / `AbiVersion` / `DLL` 与索引条目一致，
  任何一步失败即删除下载文件并中止。裸 DLL 资产没有包内清单，只按索引元数据安装并记告警。
- 包内不含签名：`sha256` 防传输损坏与"索引和包不匹配"，防不住作者账号被盗后重新发版；
  `verified` 只是注册表审核标记，界面原样展示，不当作安全承诺。
- 更新检查（M6.5）只 GET GitHub Releases 的 latest，返回 tag/name/html_url；
  **不下载、不落盘、不执行**。界面「打开下载页」只允许
  `https://github.com/Aqamoe/FaceGaze-Bridge/` 前缀，经 ShellExecute 交给默认浏览器。
- 自动更新已实现（[ADR-0036](adr/0036-auto-update.md)），信任锚点是
  **HTTPS + 本项目 GitHub Release + 发布脚本生成的 SHA-256**：
  - 只从 `api.github.com/repos/Aqamoe/FaceGaze-Bridge/releases/latest` 取资产地址，
    不读用户可改的配置指向别处（界面只能传检查地址，用于本机探针）；
  - 下载必须过校验（资产 `digest` → `SHA256SUMS.txt`），拿不到哈希就不下载、不执行；
  - 安装包未签名，因此**不做** `WinVerifyTrust`，也不声称有发布者签名校验；
  - 只对安装器装的副本自动安装（HKCU 卸载项的 `InstallLocation` 必须等于 exe 目录），
    避免把绿色版搬进 `%LOCALAPPDATA%\Programs`；
  - 安装器以 `CREATE_BREAKAWAY_FROM_JOB` 启动。这是唯一豁免 Job 生命周期的地方，
    编码在 `update::launch_installer()` 一处，外壳与引擎本身照旧随外壳退出。

## 7. 建议（非阻塞）

1. 控制面 token（若未来支持远程访问）。
2. zip 解压前预列条目（防未来 tar 行为变化）。
3. 发布符号包（PDB）以提升崩溃转储可用性，同时注意 PDB 不泄漏源码路径。
4. 模块库增加发布者/签名展示（原生库已有 `verified` 与 `sha256`，仍缺 minisign /
   Authenticode 级别的作者签名）。

## 8. 发布门禁对照

| 门禁 | 状态 |
|---|---|
| 无隐私泄漏（诊断包不含头像/账号） | ✅ |
| 无越权写配置（不改 VRChat） | ✅ |
| 控制面不暴露局域网 | ✅ |
| 输入口默认回环 | ✅（本次修复） |
| 静态文件不能越出 ui 目录 | ✅（本次修复前缀比较） |
| 更新链路可校验 | ✅（哈希校验 + 只认官方 release 资产，ADR-0036） |
