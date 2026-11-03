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
8. **无 EDID 头显的启动身份注入（v1，已降级为兜底）**：PICO/串流等 `candidate-auto`
   身份通过 `TEST_VENDOR_ID` / `TEST_PRODUCT_ID` 传给 VRChat 的受保护启动器。
   只在"持久身份关闭"时使用。
9. **持久 HMD 身份（v2，默认开）**：无 EDID 头显改用固定会话键 `0xFEEE:0x0001`
   （导出器 `--common-session-key`），把同一对值写成 `HKCU\Environment` 的
   `TEST_VENDOR_ID` / `TEST_PRODUCT_ID`（不需要管理员）。之后 **Steam 正常启动的
   VRChat 也能对上 provider**，不必由本软件代起游戏。
   - 生效条件：Steam 在进程启动时固定环境块，写完必须完全退出并重启 Steam。
   - 有真 EDID 的头显自动清除这对变量（否则游戏上报假身份，反而失配）。
   - 只删自己写的值：`%LOCALAPPDATA%\FaceGazeBridge\vrss\persist-identity.txt`
     记录 vendor/product 与写入时间；值被外部改过就不动。
   - **机制未确认**：`TEST_VENDOR_ID` 在 SteamVR / VRChat / PICO Connect 驱动的
     明文里都搜不到（ASCII 与 UTF-16 都扫过），大概率在加壳的受保护启动器里读取。
     这是实测有效的经验结论（2026-11-03 真机 A/B 验证），不是文档化契约。
10. **跟随 SteamVR 自动注册（可选）**：`vrss_auto_start` 打开时，FGB 随 SteamVR
    启动后自己跑流程（自动跳过 MSAA 确认按钮，仍由验证步骤兜底），一轮 SteamVR
    会话只自动跑一次；用户取消 UAC 或点「停止」后本轮不再重试。
    provider 需要管理员权限，因此**每次会话仍有一次 UAC**，不做计划任务/服务常驻。
11. **默认微调参数按实测对齐**：X/Y 增益 0.5、抗锯齿范围 10%（上限 50%）。
    依据是真机把注视点与高质量区位置逐方向比对：NVIDIA normalized location 的
    1 = 一个完整视口宽，而 FGB 的 NDC 换算按半宽算，正好差 2 倍。结果是"做完注视点
    校准就能直接用"，用户只在幅度/中心偏了时才手动调。
    注意这是**驱动侧显示偏差**的补偿，与注视校准（把原始点映射到视口）不是一回事。
12. **Y 轴方向不暴露给用户**：驱动 normalized location 的 +Y 向下，FGB 内部一律
    按 +Y 向上算，在 `submit()` 最后一步统一翻转（`driver_y`），因此增益/偏移的语义
    始终是"正 y = 上"。保留 `y_invert` 只作兜底（换驱动/来源导致上下颠倒时取消内部
    翻转），默认关闭；旧键 `flip_y` 不再读取，避免旧值（=1，旧语义为"翻转"）在新语义
    下被误解成"不翻转"。

## 上游补丁（`third_party/vrss/PATCHES.md`）

- `--lease <file>`
- `--csv 0`
- `--experimental-contract <path>`（参数优先于环境变量，提权进程继承不可靠）
- `--driver-audit 1 --contract-out <path>` + `driverVersion=` 输出
- 异常写 `run/error.txt`（带 `E_DRIVER` / `E_ADMIN` 等原因码）
- 重连检查改为路由感知：厂商自带的 `vrss_gaze_provider.exe`（Pimax）只告警不阻断，
  只有通用桥 `common_vrss_provider.exe` 才拒绝；枚举失败不再当作"有冲突"

## 不采用的做法

| 做法 | 原因 |
|---|---|
| 把 provider 链接进主进程 | 管理员令牌、私有 NVAPI、崩溃域污染 |
| 直接复用 Baballonia OSC 路径 | 它没有逐眼方向/置信度/时间戳，精度不如 FGB 的 2D 点 |
| 让 provider 套 gain | UDP 路由此路径不生效，会造成两处调参 |
| 自动改 NVIDIA profile | 只允许用户显式调整 VRSS area（1–50%，默认 10%）；不伪造 allow/mode，也不改其它 profile 值 |
| 9 点标定 | VRSS 要每眼视口位置，不是角度；标定解决的是世界固定屏幕 |

## 风险

- 驱动镜像 SHA-256 / vtable RVA 一旦变化，功能失效；只能重新审计或等上游。
- 探测通过 ≠ 渲染正确；必须真机看四方向，UI 文案固定说明。
- UAC、杀软误报、未文档化 API 均无法消除，只能隔离与默认关闭。
- PICO 串流可能没有 EDID，依赖候选身份；失败时明确报错。
- 持久身份是**用户级全局变量**：会影响该用户下所有 OpenVR 应用（其它 VR 游戏会看到
  假 HMD 身份）。换真机头显、或不再使用 VRSS 时必须清除（首页提供「清除持久身份」）。
- 持久身份与 MSAA 是**两个独立前提**：真机验证里 AA=x1 时 consumer 恒为 0，改 x4 后
  11 秒内变 1。页面文案不能只提身份。
