FaceGaze Bridge —— 绿色版
============================================================

怎么用
------
双击 FaceGaze Bridge.exe 就行，不需要任何脚本。

  1. 窗口打开后会自动拉起引擎（没有黑窗口，日志在界面「日志」页）
  2. 关掉窗口 = 停止追踪，不会留下后台进程
  3. 默认使用 VRCFT 模块作为输入

第一次运行前确认：
  - VRChat 已启动，并在设置里打开了 OSC（默认发到 127.0.0.1:9000）
  - VRCFT 本体已完全退出（模块的沙箱端口是独占的）
  - 系统里有 WebView2 Runtime。Win10/11 随 Edge 预装；没有的话装
    "Microsoft Edge WebView2 Runtime"（Evergreen）即可

没有 VRCFT 模块会怎样
---------------------
窗口会提示"引擎启动失败"，并可以直接选择改用 VMC 输入重试。
详细原因写在 %LOCALAPPDATA%\FaceGazeBridge\engine.log。

固定用 VMC 输入（VSeeFace / VRChat OSC Trackers 等）
---------------------------------------------------
给 FaceGaze Bridge.exe 建一个快捷方式，在「目标」末尾加上 --source vmc：

    "D:\...\FaceGaze Bridge.exe" --source vmc

之后双击这个快捷方式，引擎会监听 UDP 39539 的 VMC 数据。

进阶：同时输出给别的程序
------------------------
认不出的参数会原样转给引擎，例如让同一张脸既驱动 VRChat 又驱动 Unity 场景：

    "FaceGaze Bridge.exe" --source vmc --vmc-out 127.0.0.1:9100

其它可用的引擎参数：--vrchat host:port、--vmc-port、--eye-mode、--params、
--filter、--module-id ...（完整列表在命令行跑 fgb-cli.exe run --help）。

界面里能做什么
--------------
  状态：帧率 / 累计帧 / tick 抖动 / OSC 条数 / 当前头像
  模块：扫描磁盘上的 VRCFT 模块，选启动时加载哪个，故障时显示模块自己报的原因
  参数：365 个参数的热力图（只显示已绑定的），搜索过滤
  输出：参数集 / 眼追模式 / 绑定条数，重新读取头像参数表
  日志：引擎与模块日志，可过滤级别
  设置：One Euro 滤波开关与滑杆，日志级别，界面语言，停止引擎

文件说明
--------
  FaceGaze Bridge.exe   唯一入口：界面窗口 + 启动引擎
  fgb-cli.exe           引擎本体（命令行用户也可以直接跑）
  ui\                   页面资源。换页面不用重编 exe
  engine.log            %LOCALAPPDATA%\FaceGazeBridge\engine.log
  配置                  %APPDATA%\FaceGazeBridge\fgb.ini

常见问题
--------
  界面一直"等待引擎…"
     先看 %LOCALAPPDATA%\FaceGazeBridge\engine.log 的最后几行。
     最常见的是没装模块、VRCFT 本体没关，或 8787 端口被别的程序占用。

  模块页显示"故障"
     卡片里就是模块自己报的原因。最常见的是端口被占（还在跑 VRCFT），
     或者头显/串流软件没连上。修好后点"重新扫描"，或重开应用。

  VRChat 里表情不动
     先看界面状态页的"命中参数"是不是 0：
     是 0 → 头像参数表没读到，去"输出"页点"重新读取头像参数表"；
     不是 0 → 检查 VRChat OSC 端口、以及模型是否绑了 FT/v2 参数。

  关掉界面后追踪还在继续吗
     不会。窗口和引擎是一体的，关闭窗口即停止输出。

  想自己起引擎（比如做自动化测试）
     fgb-cli.exe run --source vmc --panel 8787
     然后再开 FaceGaze Bridge.exe 时，它会连接已在运行的引擎，不会重复启动，
     关闭窗口也不会把那个引擎关掉。

日志与配置
----------
  控制面地址：http://127.0.0.1:8787/（浏览器也能打开，方便对照）
  引擎日志：%LOCALAPPDATA%\FaceGazeBridge\engine.log
  配置：%APPDATA%\FaceGazeBridge\fgb.ini
