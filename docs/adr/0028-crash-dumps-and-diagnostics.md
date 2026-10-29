# ADR-0028 · 崩溃转储与诊断包（无第三方）

- 状态：**已实施**（2026-10-30）
- 日期：2026-10-30
- 影响：`src/base/crash.*`（新增）、`src/base/diagnostics.*`（新增）、
  `src/cli/main.cpp`、`src/ui/main.cpp`、`src/web/ipc.*`、`ui/`（设置页诊断卡片）、
  `tests/test_crash.cpp`、`tests/crash_child.cpp`、`tools/probe_crash.py`、`tools/probe_ipc.py`
- 关系：roadmap 6.3；M6 硬化的一部分

## 问题

用户现场崩了以后，现有材料只有 `engine.log`：没有调用栈、没有线程状态，复现靠猜。
也不应该要求用户手动翻 `%LOCALAPPDATA%` 收集日志、配置和模块列表。

## 决定

### 1. 用 Windows 自带 MiniDump，不引 Crashpad

`SetUnhandledExceptionFilter` + `std::terminate` + `MiniDumpWriteDump`（DbgHelp）：

- 发布包不增加第三方 DLL/几 MB 体积；
- dmp 能被 WinDbg / Visual Studio 直接打开；
- 崩溃路径只做"尽力而为"：静态原子量防递归，写失败就留 `.txt`，不再抛异常。

类型用 `MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithHandleData`：足够看栈和
句柄，又不会像全内存转储那样动辄几百 MB。

转储与说明写到 `%LOCALAPPDATA%\FaceGazeBridge\crashes`，只保留最近 20 份。
`fgb-cli` 和 `FaceGaze Bridge.exe` 两个进程都安装，因为模块加载崩溃发生在引擎侧，
WebView2 外壳自己也可能崩。

### 2. 诊断包用系统 tar.exe

`fgb-cli diag bundle [--out zip]` 与界面「设置 → 诊断 → 导出诊断包」都打包：

- `system.txt`：版本、时间、Windows 版本（RtlGetVersion）、架构、CPU 数、PID
- `engine.log`
- `outputs.ini` / `calibration.ini` / `modules.ini` / `library.json` / `fgb.ini`
- `outputs-profiles/*.ini`
- `crashes/` 最近 5 份 dmp + txt
- `modules.txt`：只列模块目录名和体积
- `app.vrmanifest`

**不收集**模块 DLL、头像配置/模型、VRChat 账号信息。打包用系统 `tar.exe -a`（Windows 10
1803+），不引 zip 库。附件名做相对路径校验，不能借诊断包写到目录外。

### 3. 后台打包，不卡引擎

界面命令 `export_diagnostics` 立即 ack，实际打包在工作线程；结果由 `pump()` 在引擎线程
广播 `diagnostics` 消息（`WsServer` 不是线程安全的，不能让后台线程直接 send）。
可选的 `out` 字段让探针把包写到临时目录。

## 验证（2026-10-30）

```
单测：write_minidump 生成 MDMP、prune 保留最新 2 份；总数 157 全过
端到端：crash_child 空指针 → 0xC0000005，生成 55 KB dmp + .txt（异常码/地址）
CLI：fgb-cli diag bundle --out ... → 105 KB zip，含 system.txt/engine.log/配置
IPC：export_diagnostics(out=临时路径) → diagnostics.ok=true，zip magic PK
UI：设置页按钮 → 完成提示 → 文件存在；探针删除临时产物
```

## 不采用的做法

| 做法 | 原因 |
|---|---|
| Crashpad | 需要 third_party 依赖，体积/构建成本高于收益 |
| 全内存 dump | 文件太大，用户上传困难 |
| 自动上传诊断 | 隐私与网络依赖；只提供本地导出 |
| 后台线程直接 `server_.broadcast` | `WsServer` 无锁、只保证引擎线程调用 |

## 已知限制

- 堆损坏严重时 MiniDump 也可能失败；此时只能依赖 `.txt` 与日志。
- dmp 需要匹配版本的 PDB 才能看到高质量调用栈；发布包目前不带 PDB（后续可出符号包）。
- 诊断包含本机路径/用户名等文本，分享前请自行检查。
