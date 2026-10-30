FaceGaze Bridge —— 绿色版
============================================================

如果你拿到的是安装版
--------------------
FaceGazeBridge-Setup-<版本>.exe 是用户级安装包（不需要管理员）：
  - 默认装到 %LOCALAPPDATA%\Programs\FaceGazeBridge
  - 创建开始菜单快捷方式，可选桌面图标
  - 静默安装：FaceGazeBridge-Setup-<版本>.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART
卸载：Windows 设置 → 应用 → FaceGaze Bridge，或安装目录的 unins000.exe。
交互式卸载会询问是否同时删除校准/预设/模块/日志；静默卸载不会删用户数据。

绿色版直接解压即用，下面的说明对两种形态都适用。

怎么用
------
双击 FaceGaze Bridge.exe，不需要任何脚本。
目录里的 fgb-cli.exe 是引擎本体（窗口会自动调用它），平时不用手动启动。

  1. 窗口打开后会自动拉起引擎（没有黑窗口，日志在界面「日志」页）
  2. 关闭窗口 = 收进托盘，追踪继续；要停止追踪请用托盘菜单的「退出」
     （托盘图标在任务栏右下角，右键有：显示窗口 / 启动或停止引擎 / 重启引擎 / 退出）
  3. 默认使用 VRCFT 模块作为输入
  4. 校准在界面「校准」页：左侧是实时 3D 面部预览（拖动旋转、滚轮缩放、
     双击复位），右侧调每个形状的 lo/hi 与视线中心，拖动立即生效；
     点「保存」后写入 %LOCALAPPDATA%\FaceGazeBridge\calibration.ini，下次启动自动加载
     预览用的是通用风格化人头，不是你的 VRChat 头像；没有模型映射的形状会标"仅数值"
  5. 想跟随 SteamVR 启动：先启动 SteamVR，再到「设置 → SteamVR 启动项」打开开关。
     设置只在 SteamVR 运行时可用；卸载/移动目录前请先点「移除注册」

第一次运行前确认：
  - VRChat 已启动，并在设置里打开了 OSC（默认发到 127.0.0.1:9000）
  - VRCFT 本体已完全退出（模块的沙箱端口是独占的）
  - 系统里有 WebView2 Runtime。Win10/11 随 Edge 预装；没有的话装
    "Microsoft Edge WebView2 Runtime"（Evergreen）即可
  - VC++ 运行库已随包提供（msvcp140 / vcruntime140），不需要另装

模块从哪来
----------
FaceGaze Bridge 有自己的模块仓，**不再直接加载 VRCFT 目录里的 DLL**：

  %LOCALAPPDATA%\FaceGazeBridge\modules\<模块 ID>\
  %LOCALAPPDATA%\FaceGazeBridge\modules.ini      启用状态与启动模块

「模块」页可以做：
  - 检测到本机 VRCFT 模块 → 导入（复制进自己的模块仓，之后不依赖 VRCFT）
  - 从 .zip / .dll 手动安装
  - 启用 / 停用 / 删除 / 设为启动模块
  - 「模块库」页（registry.vrcft.io，与 VRCFT 同源）浏览、搜索、一键安装/更新

改完模块选择后点「重启引擎」生效。引擎一次只加载一个模块。

VRCFT 运行时
------------
发布包自带一份精简的 VRCFT 宿主运行时（runtime\vrcft，约 106 MB），
从已安装的 VRCFT 提取，包含 ModuleProcess + Core/SDK + .NET 运行时，
不含 VRCFT 的界面与内置推理。这样卸载 VRCFT 后模块仍可运行。

找不到内置运行时时，会依次尝试：
  %LOCALAPPDATA%\FaceGazeBridge\runtime\vrcft → 已安装的 VRCFT
界面右上角的「运行时」标签会显示当前用的是哪一份。

固定用 VMC 输入（VSeeFace / VRChat OSC Trackers 等）
---------------------------------------------------
给 FaceGaze Bridge.exe 建一个快捷方式，在「目标」末尾加上 --source vmc：

    "D:\...\FaceGaze Bridge.exe" --source vmc

进阶：同时输出给别的程序
------------------------
认不出的参数会原样转给引擎，例如让同一张脸既驱动 VRChat 又驱动 Unity 场景：

    "FaceGaze Bridge.exe" --source vmc --vmc-out 127.0.0.1:9100

给网页/脚本（WebSocket JSON）或串流软件（UDP 二进制）：

    "FaceGaze Bridge.exe" --source vmc --ws-out 9101 --udp-out 127.0.0.1:9102

输出的目标/开关也可以在界面「输出」页保存；命令行参数优先于保存值，
某次启动想忽略保存值可加 --no-saved-outputs。

其它可用的引擎参数：--vrchat host:port、--vmc-port、--eye-mode、--params、
--filter、--module-id ...（完整列表在命令行跑 fgb-cli.exe run --help）。

界面里能做什么
--------------
  状态：帧率 / 累计帧 / tick 抖动 / OSC 条数 / 当前头像 / 输入源
  模块：本机导入、zip 安装、启停、删除、设为启动、模块库一键安装、运行时状态
  参数：365 个参数的热力图（只显示已绑定的），搜索过滤
  输出：参数集 / 眼追模式 / 绑定条数 / VMC / 共享内存 / WebSocket / UDP，
        可编辑所有输出目标与开关（保存到 outputs.ini，保存并重启引擎后生效），
        命名预设可保存/应用/删除（应用即快速切换），重新读取头像参数表
  日志：引擎与模块日志，可过滤级别
  设置：One Euro 滤波开关与滑杆，SteamVR 启动项（状态/自启动/重新检测/移除注册），
        日志级别，界面语言，停止引擎

文件说明
--------
  FaceGaze Bridge.exe   唯一入口：界面窗口 + 启动引擎
  fgb-cli.exe           引擎与命令行工具（modules / run / params …）
  sdk/fgb_module_api.h  原生模块 ABI 头（给模块开发者，普通用户不用管）
  sdk/fgb_shared_frame.h 共享内存输出布局（给 Unity/UE 集成开发）
  sdk/fgb_udp_frame.h    UDP 二进制输出布局（656 字节固定帧）
  sdk/SHARED-MEMORY.md   共享内存读取指南
  sdk/STREAMING-OUTPUTS.md  WebSocket JSON / UDP 二进制协议与读取示例
  sdk/GAZE-SERVICE.md    /api/gaze 与 WS gaze 的字段和边界
  sdk/NATIVE-MODULE-GUIDE.md  原生模块开发指南（含示例工程）
  sdk/sample-native-sine/     可运行的示例模块（正弦波，无硬件）
  sdk/unity-face-gaze/        Unity 示例包（UPM package.json + 共享内存驱动组件）
  runtime\vrcft\        VRCFT 模块宿主运行时（Apache-2.0，含 LICENSE/NOTICE）
  ui\                   页面资源（含 uiace 模型与映射表）。换页面不用重编 exe
  app.vrmanifest        SteamVR 注册清单（只借用 dashboard overlay 的自启动机制）
  NOTICE                第三方许可证说明（含 OpenVR / VRCFT）
  licenses\             openvr.h 的 BSD-3-Clause 许可证原文
  engine.log            %LOCALAPPDATA%\FaceGazeBridge\engine.log
  配置                  %APPDATA%\FaceGazeBridge\fgb.ini

常见问题
--------
  界面一直"等待引擎…"
     先看 %LOCALAPPDATA%\FaceGazeBridge\engine.log 的最后几行。
     最常见的是没有模块、内置运行时缺失，或 8787–8796 都被占用。

  模块页是空的 / 没有输入源
     点「重新扫描」看有没有"检测到本机模块"，导入后设为启动并重启引擎；
     或者到下面的模块库安装一个。

  模块库刷新失败 / 刷新很慢
     registry.vrcft.io 偶尔会超时。启动时会先显示本地缓存（6 小时内不重复联网），
     刷新在后台进行；刷新期间仍然可以安装缓存里的模块。断网时会保留上一次的缓存。

  更新模块失败
     模块正在被引擎加载时 DLL 会被占用。先在设置页停止引擎，再更新。
     删除正在运行的模块不需要手动停：应用会停止引擎、删除并自动重启。

  模块页显示"故障"
     卡片里就是模块自己报的原因。最常见的是端口被占（还在跑 VRCFT），
     或者头显/串流软件没连上。

  校准页没有 3D 面部，只显示 2D 示意图
     WebGL2 不可用（远程桌面、老显卡、禁用硬件加速）。页面会自动降级为 2D SVG，
     滑杆、保存、取当前都照常可用；想恢复 3D 可更新显卡驱动或关掉远程桌面。

  SteamVR 启动项开关是灰的
     卡片状态会写明原因：SteamVR 未安装 → 检查 Steam 安装；SteamVR 未运行 →
     先启动 SteamVR 再点「重新检测」。FGB 不会自己拉起 SteamVR。

  SteamVR 启动项提示 app_key 冲突
     旧安装目录还注册着同一个 app_key。先点「移除注册」，再在新目录打开开关。

  VRChat 里表情不动
     先看界面状态页的"命中参数"是不是 0：
     是 0 → 头像参数表没读到，去"输出"页点"重新读取头像参数表"；
     不是 0 → 检查 VRChat OSC 端口、以及模型是否绑了 FT/v2 参数。

  想自己起引擎（比如做自动化测试）
    fgb-cli.exe run --source vmc --panel 8787
    然后再开 FaceGaze Bridge.exe 时，它会连接已在运行的引擎，不会重复启动，
    关闭窗口也不会把那个引擎关掉。

  想让它在后台常驻（例如配合 SteamVR 自启动）
    "FaceGaze Bridge.exe" --tray
    不弹窗口，只驻留托盘，引擎照常工作；单击托盘图标可以打开窗口。
    已经有实例在跑时，再启动一次只会把现有窗口带到前台，不会开第二个托盘图标。

日志与配置
----------
  控制面地址：默认 http://127.0.0.1:8787/，端口被其它程序占用时自动顺延到 8788/8789…
  （界面「设置」页会显示实际地址；浏览器也能打开，方便对照）
  引擎日志：%LOCALAPPDATA%\FaceGazeBridge\engine.log
  模块仓：%LOCALAPPDATA%\FaceGazeBridge\modules
  模块库缓存：%LOCALAPPDATA%\FaceGazeBridge\library.json
  校准档案：%LOCALAPPDATA%\FaceGazeBridge\calibration.ini（上一版自动留成 .bak）
  输出预设：%LOCALAPPDATA%\FaceGazeBridge\outputs-profiles\*.ini
  配置：%APPDATA%\FaceGazeBridge\fgb.ini

作者与许可证
------------
  FaceGaze Bridge 由 Aqamoe（光水）开发，以 Apache-2.0 许可发布。
  https://github.com/Aqamoe/FaceGaze-Bridge
  许可证全文与第三方组件声明见本目录的 LICENSE、NOTICE 与 licenses\。

