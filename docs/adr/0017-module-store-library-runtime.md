# ADR-0017 · 独立模块仓、模块库与内置运行时

- 状态：已采纳（取代 [ADR-0011](0011-module-management.md) 中"扫描 VRCFT CustomLibs 直接加载"的存储方式）
- 日期：2026-10-29
- 影响：`vrcft/module_registry` 重写；新增 `vrcft/module_library`、`net/http_client`、
  `tools/build_vrcft_runtime.py`；`src/ui` 增加引擎重启；install 后需补 runtime；发布包体积 +106 MB

## 问题

ADR-0011 的模块管理建立在"直接扫描 `%AppData%\VRCFaceTracking\CustomLibs` 并加载"上。真实使用后暴露四个问题：

1. **依赖别人的目录。** VRCFT 卸载、升级或删模块，FaceGaze Bridge 的输入源跟着消失；
   而我们执行的是用户没有明确授权给本软件的 DLL。
2. **没有管理。** 只有"下次启动加载哪一个"，不能停用、删除、安装，也不能看大小/来源。
3. **没有模块库。** 用户只能先装 VRCFT、在 VRCFT 里装模块，再回来扫。
4. **A 层依赖 VRCFT 安装目录里的 `ModuleProcess.exe`。** 只把模块 DLL 复制过来，
   卸载 VRCFT 后宿主仍然没了——"独立模块仓"必须同时解决运行时。

## 决定

### 1. 模块只从自己的仓加载

```
%LOCALAPPDATA%\FaceGazeBridge\
  modules\<ModuleId>\module.json + <DllFileName>.dll + config.ini
  modules.ini            启用状态、启动模块、来源、安装时间
  library.json           模块库缓存
  downloads\             下载/解包临时目录
```

- `%AppData%\VRCFaceTracking\CustomLibs` 与 `<exe>\vrcft-libs` 只作为**导入候选**，
  界面显示"检测到本机模块"，用户点导入才复制进仓；引擎不会从这些目录 `LoadLibrary`。
- 状态用 INI（`[state] active` + `[module.<id>] enabled/source/installed`）：只有标量，
  现有 `config::ini` 足够，不需要为此引入通用 JSON DOM。
- 启用（enabled）与启动模块（active）分开：停用不动文件，只是不参与启动选择；
  引擎仍然一次只加载一个模块（与 ADR-0015 一致）。
- 命令行保留 `fgb-cli modules`：`import-all` / `install` / `use` / `enable` / `disable` / `remove`，
  排障时不需要先有界面。

### 2. 安装/更新是"先落临时目录再替换"

- 支持 `.zip`、裸 `.dll`、目录三种来源。zip 用系统 `tar.exe`（Win10 1803+ 自带 bsdtar）
  解压，解压后逐项校验：越界路径、符号链接、解压后 >512 MB 或 >4000 个文件一律拒收。
- 复制到 `store/.tmp-copy-*`，完整成功后再改名到 `modules/<id>`；更新时旧目录在
  rename 前才删除。正在运行的模块 DLL 被占用会失败并如实报错，而不是半新半旧。
- 下载/解压来的文件会清掉 `Zone.Identifier`，否则 .NET 加载会拦。
- 裸 DLL 会补一份最小 `module.json`，保证之后列表、版本比较、模块库更新有元数据。

### 3. 模块库直接消费 VRCFT 的公开接口

- `GET https://registry.vrcft.io/modules` 是 VRCFT 界面用的同一份数据（模块名、作者、版本、
  评分、下载量、下载地址、DLL 名、MD5）。代码里只解析"数组套一层标量对象"这一种形状，
  但字符串转义、`\uXXXX`、描述里的括号/引号都按 JSON 处理。
- 网络与下载在后台线程（WinHTTP，不引 libcurl），注册表与磁盘安装只在引擎线程做，
  避免并发改模块仓。进度按 200 ms 推给界面。
- 有 `FileHash` 时校验 MD5；下载地址只接受 https（本地回环 http 供测试）。
  缓存 `library.json`，断网时显示上一次列表并标 stale。
- `registry.vrcft.io` 实测会偶发超时（同一地址连续第三次请求卡 20 s），刷新内部重试一次；
  仍失败就保留缓存并显示错误，不把列表清空。
- UI 在安装前明确提示"模块是可执行代码，只装可信来源"。

### 4. 内置精简 VRCFT 运行时

- 发布包带 `runtime/vrcft/`：按 `ModuleProcess.deps.json` 解析依赖闭包，从已安装 VRCFT
  复制（约 106 MB）。包含 ModuleProcess + Core/SDK + .NET 运行时与
  `Microsoft.Windows.SDK.NET`，不含 WinUI 界面、ONNX/DirectML。
- 解析顺序：`<exe>/runtime/vrcft` → `%LOCALAPPDATA%\FaceGazeBridge\runtime\vrcft` → 已安装 VRCFT。
  UI 的「运行时」标签显示当前来源（内置/本机副本/VRCFT 安装）。
- `tools/build_vrcft_runtime.py` 生成该目录，保留原项目 `LICENSE`/`NOTICE`（Apache-2.0）。
  个别模块若提示缺少程序集，从 VRCFT 目录补对应 DLL 即可——这是"精简"的已知代价。

### 5. 引擎可以以"没有输入源"启动

模块仓为空时，`fgb-cli run --source vrcft --panel …` 不再直接退出，而是跳过 vrcft 输入、
把界面拉起来，让用户在「模块」页导入/安装。没有面板的纯 CLI 仍然报错退出（行为不变）。

改完模块选择需要重启引擎：新增 IPC `restart_engine`，引擎以退出码 **3** 结束；
桌面入口看到 3 就原地重新拉起 `fgb-cli` 并重新导航，窗口不关。普通 `quit` 仍以 0 退出，
窗口随之关闭（单应用语义，见 ADR-0016）。

## 代价

- 发布包从 ~0.6 MB 增到 ~107 MB，主要是 .NET 运行时。换来"卸载 VRCFT 也能用"。
- 模块库依赖第三方服务与网络；重试与缓存只能缓解，不能消除。
- 一键安装会执行第三方 DLL，这是功能本身的风险；只能在 UI 明示 + MD5 校验 + 用户确认上尽力。
- 精简运行时是"按依赖闭包裁剪"，没有逐个模块做全量回归；缺程序集时日志会指出，补文件即可。

## 验证

- 106 个单测（新增安装/导入/启停/删除/zip slip/zip bomb/模块库 JSON）
- 真实数据：导入本机 `VRCFTPicoModule` → 用内置运行时加载并执行其初始化代码
- 界面/CDP：刷新模块库（21 条）→ 一键安装 ETVR → 来源=registry → 删除 → 重启引擎后页面重连、
  模块仓保留
- 空模块仓 + `--source vrcft --panel`：引擎以 0 输入启动，界面可打开
