# ADR-0036 · 自动更新：启动自检、静默下载、用户确认后安装

- 状态：**已实施**（2026-10-31）
- 日期：2026-10-31
- 影响：`src/net/update_check.*`（assets 解析）、`src/net/update_manager.*`（新）、
  `src/web/ipc.*`、`src/cli/main.cpp`（退出码 4）、`src/ui/main.cpp`（Job 放行 breakaway）、
  `ui/index.html`、`ui/app.js`、`ui/i18n.js`、`packaging/facegazebridge.iss`、
  `tests/test_update.cpp`、`tools/probe_update.py`（新）
- 关系：取代 [M6.5](../03-roadmap.md) 里"只提示、不下载"的临时策略；
  受 [ADR-0022](0022-tray-shell-and-close-semantics.md)（Job 绑定生命周期）与
  [ADR-0016](0016-single-entry-desktop-app.md)（单一入口）约束

## 问题

M6.5 只做了"查一下 GitHub Releases 的 latest，然后让用户自己去下载页"。实际后果是：
用户装完就停在旧版本，而我们每次发版修的都是硬件兼容和崩溃问题——用户不升级，
等于修复没送达。但"自动下载并替换正在运行的程序"又要面对三件硬约束：

1. **进程拓扑**：引擎是外壳的子进程，外壳用 `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` 把引擎
   绑在自己的生命周期上（ADR-0022）。任何由外壳/引擎启动的子进程都同属这个 Job，
   外壳一退出，连同安装器一起被杀——**方案做错的话，安装器会"跑到一半消失"**。
2. **文件占用**：安装器要覆盖 `FaceGaze Bridge.exe`、`fgb-cli.exe`、`ui/`、`runtime/vrcft/`，
   而前两个正在运行。必须是"先退出、后覆盖"。
3. **没有代码签名**：`release.ps1` 明确写了"安装包未签名"。因此不能靠
   `WinVerifyTrust` 建立信任，也不能假设系统会给下载来的文件打 Mark-of-the-Web。

## 决定

### 1. 检查放在引擎，节流 24 小时

引擎本来就持有 HTTP 客户端（WinHTTP）、日志、以及向界面广播的通道。启动后延迟 10 秒
（避开模块握手与首帧），查一次 `releases/latest`，结果通过 `update` 消息推给界面，
晚连上的界面也能拿到当前快照。

`update.ini` 里存 `next_check_at`：成功 +24h（外加 0–4h 抖动，避免发版当天全体同时请求），
失败 +6h（退避）。手动点「检查更新」不受节流约束。`--no-update-check` 关掉自动检查，
探针与回放测试用它保证不真的访问 GitHub。

### 2. 只对"安装器装的副本"自动安装

读
`HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\{7B2F3C64-…}_is1` 的
`InstallLocation`，与当前 `exe_dir` 相同才允许自动下载与安装。否则（`dist/` 开发版、
解压即用的绿色版）只提示并给「打开下载页」。

**理由**：静默把绿色版装进 `%LOCALAPPDATA%\Programs`，等于替用户搬家，而且会和用户
原来那份并存成两个版本。

### 3. 静默下载 + 强制校验，校验不过就不执行

- 资产选择：`FaceGazeBridge-Setup-<version>.exe`（找不到就不自动下载）。
- 校验优先级：资产自带的 `digest`（`sha256:…`）→ 同 release 的 `SHA256SUMS.txt` →
  **两者都没有就只提示、不自动安装**。
- 下载走 `net::http_download`（先写 `.part` 再改名），落盘在
  `%LOCALAPPDATA%\FaceGazeBridge\updates\`，大小与 SHA-256 都对上才算 `ready`。

不使用 `WinVerifyTrust`：安装包未签名，这条路径现在只会永远失败。信任锚点是
"HTTPS + 本项目 GitHub Release + 发布脚本生成的校验和"，这条边界写进 `docs/10-security-review.md`。

### 4. 安装器由**引擎**启动，启动即脱 Job；外壳只认退出码

```
UI 点「立即安装」
  → install_update
  → 引擎校验 staged 文件（存在 + 大小 + sha256）
  → 广播 update{state:"installing"}，flush 一帧
  → CreateProcessW(Setup.exe "/SILENT /CLOSEAPPLICATIONS /NORESTART /RELAUNCH=1",
                   CREATE_BREAKAWAY_FROM_JOB)
  → 引擎以退出码 4 结束
  → 外壳看到 4：不重启引擎、不弹"启动失败"，直接退出
  → Inno 用 Restart Manager 关掉残留进程，覆盖安装，末页 [Run] 依次重启新版本
```

为什么由引擎而不是外壳启动：staged 路径、校验结果都在引擎手里；外壳保持"无业务逻辑"。
为什么必须 `CREATE_BREAKAWAY_FROM_JOB`：见问题 1。为了让这个标志生效，外壳建 Job 时
要加 `JOB_OBJECT_LIMIT_BREAKAWAY_OK`——这是**唯一**的豁免口，只给安装器用，
引擎本身照旧随外壳退出（ADR-0022 的语义不变）。

权限：安装器是 `PrivilegesRequired=lowest`（用户级，装到 `%LOCALAPPDATA%\Programs`），
不需要 UAC；因此这条链路对标准用户也可用。

### 5. 失败不假装有回滚

Inno 覆盖安装不是事务。安装器失败时保留已下载的 Setup 与 `update.ini`，界面给出
「手动运行这个安装包」的路径和日志位置；回滚只能靠用户重装旧版。

### 6. 用户始终能反悔

界面给三个动作：**立即安装**（staged 完成才出现）、**稍后**（本次会话不再提）、
**忽略此版本**（写 `skipped_version`，同版本不再提示，手动检查仍会显示）。
托盘形态（`--tray`，无窗口）只用气泡提示，不弹模态框。

### 7. `update.ini` 会跨版本沿用，必须自己作废

安装器只覆盖程序文件，**不会碰** `%LOCALAPPDATA%\FaceGazeBridge\update.ini`。1.0.5 实测暴露两个后果：

- **装完仍然提示"可以安装"**：staged 记录（版本 1.0.5）在新版本启动时被恢复成 `ready`，
  而当前版本就是 1.0.5。规则：`staged_version <= current_version` 一律丢弃，
  内存、`update.ini`、磁盘上的安装包一起清掉。
- **装完当天不再自动检查**：`next_check_at` 是上一个版本写下的 24 小时窗口。
  规则：文件里记 `checked_version`，与当前版本不一致就把 `next_check_at` 清零。
  失败退避也写 `checked_version`——否则每次开程序都会因"版本不一致"重置退避，
  变成启动就重试一次失败请求。

`skipped_version` 同理：只对"比当前版本新"的版本有意义，装上了就失效。

## 后果

- 用户从"发现新版→手动下载→手动安装"（约 5 步、几次跳浏览器）变成"点一次安装"。
- 代价 1：每次升级都要下发完整 Setup（含 `runtime/vrcft` 约 100 MB）。**不做差分**：
  自写 bsdiff 的维护成本远高于收益，未签名时补丁链反而放大风险。
- 代价 2：Job 多了一个 breakaway 豁免口。它只作用于显式带
  `CREATE_BREAKAWAY_FROM_JOB` 的进程，代码里只有 `launch_installer()` 一处这么建进程。
- 代价 3：静默安装失败时的现场只能靠日志（`%LOCALAPPDATA%\FaceGazeBridge\engine.log`）。

## 备选（未采用）

| 方案 | 否决原因 |
|---|---|
| 引擎直接跑安装器，不加 breakaway | 安装器随外壳退出被杀，表现为"点了安装没反应" |
| 独立 `updater.exe` | 多一个待分发二进制；而且它自己也在被覆盖的目录里，还是要靠 breakaway |
| 复制自身到临时目录再跑安装器 | 同上，且临时目录清理/防重复执行都要额外写一遍 |
| 只依赖 Inno 的 `CloseApplications`，让界面自己退出 | 没有人负责启动安装器；装了 `RestartApplications` 也只能重启，不能"先退出再装" |
| 差分更新 | 见代价 1；且需要额外的历史版本服务端 |

## 验证

- 单测：版本比较、assets 解析、安装包选择、`SHA256SUMS.txt` 解析、节流判定（`next_check_at`）。
- 探针：`tools/probe_update.py` 起本地假 release（含假资产与校验和），断言
  `checking → downloading → ready` 与进度、`skip_update_version` 后不再提示；
  `--dry-run-install` 只打印将要执行的命令行，不真的启动安装器。
- 人工（只能人工）：装 1.0.x → 发一个 draft release → 启动后自动提示 → 点安装 →
  托盘恢复、版本号变成新的。`docs/03-roadmap.md` 的 6.5 按此验收。
