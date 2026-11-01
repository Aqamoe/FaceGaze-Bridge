# ADR-0040 · VRSS provider 作为可选独立组件

- 状态：**已实施**（2026-11-01）
- 影响：`third_party/vrss/`、`src/vrss/`、`src/sinks/vrss_gaze_sink.*`、
  `src/config/outputs.*`、`src/web/ipc.*`、`ui/`、`CMakeLists.txt`
- 上游：VRSS-Tuner（MIT），固定源码副本 + 4 个小补丁

## 问题

FGB 已经有准确的逐眼注视数据（PICO 原生 2D 点、方向型来源），但缺一个把数据
交给 NVIDIA VRSS2 的出口。VRSS 是**可变速率超采样**：驱动按注视位置在中央做
更高倍率超采样，属于**注视点抗锯齿**，不是 foveated rendering，也不承诺帧率收益。

上游 VRSS-Tuner 已验证这条驱动级路径可行，且已有 `UdpGazeV1` 契约。问题是它带
GUI、多厂商桥接、未文档化 NVAPI 私有调用与管理员权限要求。

## 决定

1. **只 vendor provider 源码**，不搬 GUI / 厂商桥接。`third_party/vrss/provider/`
   保持上游文件布局，`third_party/vrss/nvapi` 与 `openvr` 只保留编译所需。
2. **provider 是独立 exe，不链接进 `fgb-cli`**：需要管理员令牌、会调用未文档化的
   NVAPI vtable、崩溃必须隔离。
3. **数据面用现成契约**：FGB 新 `VrssGazeSink` 发 44 字节 `UdpGazeV1` 到
   `127.0.0.1:5556`，provider 100 Hz 轮询。2D 点走
   `x=2px-1, y=1-2py`；方向型来源走 OpenVR 投影（与上游 `ProjectHeadTangent` 同式）。
4. **试卷与微调在 FGB 侧**：上游 UDP 路径不套 `x_gain/y_gain/*_deg`，所以 gain/offset
   必须在 Sink 内应用；provider 的 `calibration.txt` 保持中性。
5. **驱动白名单 + 自动只读探测**：先跑 `--driver-check-only`；不在白名单时跑
   `--driver-audit`（只读 vtable 地址，不调用私有注册/提交方法），通过后写实验合同，
   再跑一次 `--driver-check-only` 验证。合同按镜像哈希缓存。
6. **生命周期**：provider 由引擎控制面持有，但**不随引擎重启**（NVIDIA 在 VRChat
   持有旧连接时可能拒绝重注册，`-175`）。引擎每 2 s 写 lease；provider 15 s 未更新
   自退并 unregister。托盘退出时写 `stop.request`。
7. **一键流程在引擎侧状态机**：六步（环境 / 驱动 / 探测 / 权限与 MSAA / provider /
   VRChat / 验证），需要用户操作的步骤停在 blocked，不自动猜。
8. **无 EDID 头显的启动身份注入**：PICO/串流等 `candidate-auto` 身份必须通过
   `TEST_VENDOR_ID` / `TEST_PRODUCT_ID` 传给 VRChat 的受保护启动器；普通 Steam
   启动不会注入，provider 注册成功但 `consumerCount` 会一直是 0。

## 上游补丁（`third_party/vrss/PATCHES.md`）

- `--lease <file>`
- `--csv 0`
- `--experimental-contract <path>`（参数优先于环境变量，提权进程继承不可靠）
- `--driver-audit 1 --contract-out <path>` + `driverVersion=` 输出
- 异常写 `run/error.txt`（带 `E_DRIVER` / `E_ADMIN` 等原因码）

## 不采用的做法

| 做法 | 原因 |
|---|---|
| 把 provider 链接进主进程 | 管理员令牌、私有 NVAPI、崩溃域污染 |
| 直接复用 Baballonia OSC 路径 | 它没有逐眼方向/置信度/时间戳，精度不如 FGB 的 2D 点 |
| 让 provider 套 gain | UDP 路由此路径不生效，会造成两处调参 |
| 自动改 NVIDIA profile | 只允许用户显式调整 VRSS area（1–100%，默认 12%）；不伪造 allow/mode，也不改其它 profile 值 |
| 9 点标定 | VRSS 要每眼视口位置，不是角度；标定解决的是世界固定屏幕 |

## 风险

- 驱动镜像 SHA-256 / vtable RVA 一旦变化，功能失效；只能重新审计或等上游。
- 探测通过 ≠ 渲染正确；必须真机看四方向，UI 文案固定说明。
- UAC、杀软误报、未文档化 API 均无法消除，只能隔离与默认关闭。
- PICO 串流可能没有 EDID，依赖 `candidate-auto` 身份；失败时明确报错。
