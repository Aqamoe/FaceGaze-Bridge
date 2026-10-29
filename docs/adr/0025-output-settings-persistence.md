# ADR-0025 · 输出设置持久化与输出页编辑

- 状态：**已实施**（2026-10-30）
- 日期：2026-10-30
- 影响：`src/config/outputs.*`（新增）、`src/cli/main.cpp`、`src/web/ipc.*`、
  `ui/`（输出页表单 + 三语文案 + 样式）、`tools/smoke_vmc.py`、`tools/probe_ui.py`
- 关系：接在 [ADR-0024](0024-streaming-outputs.md) 之后；4.8 的"命名预设"仍未做

## 问题

WebSocket JSON / UDP 二进制 / VMC / 共享内存都只能靠启动参数开：

```powershell
"FaceGaze Bridge.exe" --vmc-out ... --ws-out 9101 --udp-out ...
```

普通用户不会改快捷方式；输出页能看到目标却不能改。结果就是"功能做了，但用户发现不了"。
另外，设置只存在这一次进程里，重启后全部回到默认。

## 决定

### 1. 单独一份 `outputs.ini`

路径：`%LOCALAPPDATA%\FaceGazeBridge\outputs.ini`。不塞进 `modules.ini`：
模块注册表会整份重写自己的状态，两拨代码共用一个文件迟早互相覆盖。

```ini
[output]
vrchat = 127.0.0.1:9000
eye_mode = parameters
params_mode = v2

[vmc]
enabled = 0
target = 127.0.0.1:39539

[shm]
enabled = 0
name = default

[ws]
enabled = 1
port = 9101
rate = 30
all_shapes = 0

[udp]
enabled = 1
target = 127.0.0.1:9102
rate = 60
```

保存走 `.tmp` → 旧文件留 `.bak` → `MoveFileExW(REPLACE_EXISTING)`。读到非法值
（端口 0、缺 host、未知模式）整份拒绝并回到默认，不应用半份配置。

### 2. 优先级：默认值 < outputs.ini < 命令行

`fgb-cli run` 启动时：

1. `--outputs-config <路径>` 指定文件（默认上面的路径）；
2. `--no-saved-outputs` 本次忽略保存值（命令行参数仍然生效）；
3. 加载成功后先套到各 sink 配置，再解析命令行——**命令行永远覆盖保存值**。

`--no-saved-outputs` 在正式解析前先扫一遍 argv，因为"要不要加载"必须在这之前决定。

### 3. 界面：输出页可编辑，保存 ≠ 立即生效

输出页新增「输出设置」表单：VRChat 目标、眼追/参数集、VMC、共享内存、WS、UDP。
两个动作：

- **保存**：写 `outputs.ini`，状态里 `outputs` 立刻更新；当前引擎不变。
- **保存并重启引擎**：保存成功后走已有的 `restart_engine` 命令，外壳原地拉起引擎，
  新设置生效。

`/api/status` 里同时存在两套字段，语义必须分清：

| 字段 | 含义 |
|---|---|
| `outputs` | **下次启动会按什么开**（保存值，表单用） |
| `vmc_out` / `shm_out` / `ws_out` / `udp_out` | **这次实际开了什么**（运行态） |

保存后如果没重启，两者会不一致——这是诚实的状态，不把"保存了"伪装成"生效了"。

WS 端口被别的程序占用时，`WsJsonSink::start()` **不会**让引擎启动失败：它只关闭这一路，
日志给出绑定错误，`ws_out.listening=false`，输出页显示「未监听（端口被占用？）」。
一个可选输出不该把整个追踪拖死。

### 4. IPC：`set_outputs`

前端发一条扁平 JSON（缺省字段保持原值），服务端：

1. `parse_target` 拆分 host:port；2. `validate_outputs` 校验模式/端口/帧率；
3. `save_outputs` 原子落盘；4. 更新内存中的保存值；5. ack 带 `restart_needed=true`，
   并广播一次 status。

错误原样回给界面（例如 `vrchat 需要 host:port` / `ws.port 必须是 1..65535`）。

### 5. 探针与冒烟测试不读用户配置

- UI 探针启动引擎时用 `--outputs-config %TEMP%\fgb-probe-outputs.ini`，
  和 `--calibration` 同一原则：探针会真的保存，不能写用户默认档案。
- `tools/smoke_vmc.py` 加 `--no-saved-outputs`，避免开发机上的保存值改变输出集合，
  让"管道通不通"的断言保持确定。

## 实测证据（2026-10-30）

```
outputs.ini 往返：4 个单测（parse/validate/roundtrip/missing-or-invalid）通过
CLI：--outputs-config 指向临时文件 → ws 9111 / udp 9112 自动开启，日志与 /api/status 一致
CLI：--no-saved-outputs → ws/udp 关闭，--vrchat 65501 覆盖保存值
UI 探针：改 WS 端口 9113 / 帧率 25 → 点保存 → dirty=false，status.outputs 回读 9113/25；
         临时文件内容与表单一致
单测总数 144，全过
```

## 不采用的做法

| 做法 | 不用的原因 |
|---|---|
| 写进 `modules.ini` | 模块注册表会整份重写，必然丢 `[output]` |
| 运行时热插拔 sink | 引擎的 sink 列表在 start 前固定；热插拔要处理线程/端口/资源回收，收益不值得 |
| 保存即重启 | 用户可能只想先看一眼；把重启做成显式按钮，行为可预期 |
| 只加 CLI 文档 | 用户不会读 --help；输出页才是发现入口 |
| 命名预设（多套 Profile） | 属于 4.8 的后半；先把"当前一套"做扎实 |

## 已知限制

- 输出设置只有一套；命名预设/快速切换仍未做。
- 表单显示的是保存值，不是命令行覆盖后的运行值；界面文案已注明"命令行参数优先"。
- 所有输出变更都要重启引擎才生效（校准/滤波仍是热更新）。
- 保存失败只回错误，不会自动回滚内存中的值（文件没写成，内存保持旧值）。
