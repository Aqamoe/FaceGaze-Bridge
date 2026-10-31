# 04 · 工程规范与风险管理

---

## 1. 技术栈

### 1.1 核心

| 项 | 选型 | 版本 | 理由 |
|---|---|---|---|
| 语言 | C++ | C++20 | `concepts` / `span` / `format` / `atomic::wait` 都用得上；C++23 的编译器支持仍不稳 |
| 编译器 | MSVC | 19.4x (VS2022 17.10+) | Windows 首选；CI 加 Clang 交叉验证 |
| 构建 | CMake | ≥ 3.28 | Presets + File API，IDE 无关 |
| 生成器 | Ninja | 最新 | 比 MSBuild 快 2–3 倍 |
| 依赖 | vcpkg | manifest 模式 | 可复现，锁版本 |
| 标准库扩展 | 不自研 | — | 优先 `std::`，缺的用少量 header-only |

### 1.2 第三方库

| 用途 | 库 | 许可 | 备注 |
|---|---|---|---|
| 日志 | spdlog | MIT | 异步 sink + 环形缓冲 |
| 格式化 | fmt | MIT | spdlog 已带 |
| 数学 | glm | MIT | 向量/四元数；开 SIMD |
| 配置 | toml++ | MIT | 头文件only |
| JSON | nlohmann/json | MIT | 配置/协议；热路径不用（用二进制） |
| HTTP | cpp-httplib | MIT | OSCQuery 客户端 + 本地 API |
| WebSocket | ixwebsocket | BSD-3 | 服务端 + 客户端 |
| 测试 | Catch2 | BSL-1.0 | v3，支持 BDD 风格 |
| 基准 | Google Benchmark | Apache-2.0 | OSC 编码、ring buffer |
| 崩溃 | Crashpad | Apache-2.0 | 本地转储，不上传 |
| UI 运行时 | WebView2 | 微软 | Win10+ 已预装（Evergreen）；SDK 编译期从 NuGet 取一次 |
| SteamVR 集成（规划） | OpenVR 头文件 | BSD-3 | 只 vendor `openvr.h`；`openvr_api.dll` 从用户 SteamVR 加载，不随包分发（[ADR-0020](adr/0020-steamvr-autostart.md)） |
| 3D 校准预览（规划） | 自有人头模型 | 自有 / CC0 | 不引 Three.js；模型 ≤ 3 MB，来源进 `THIRD_PARTY_NOTICES.md`（[ADR-0021](adr/0021-calibration-workbench-3d-preview.md)） |
| 前端 | 原生 JS（当前）→ TypeScript + Vite + Svelte（按需） | MIT | 先保持零构建：改一行 CSS 刷新就见效；页面过千行或需要类型约束时再上构建链 |
| UI ↔ 引擎 | WebSocket + JSON（自写 RFC 6455 子集） | — | 回环、可 curl 对照、可脚本验证；见 ADR-0010 |
| .NET 宿主 | .NET 8 SDK | MIT | 仅编译 `vrcft-host` |
| 安装包 | Inno Setup | 自由 | 比 WiX 简单，脚本化好 |

**明确不用**：Boost（体积）、Qt（许可与体积）、Electron（内存）、任何 GPL 库。

### 1.3 自研而非引库的部分

| 模块 | 为什么自研 |
|---|---|
| OSC 编解码 | 热路径，需要零分配 + bundle 复用 + 精确控制发送时机。现成库（oscpack 等）都带分配 |
| SPSC ring / seqlock | 20 行能写对的东西，引库反而增加不确定性 |
| 协议解码（VRCFT IPC） | 无现成实现 |
| 参数表 | 从 JSON 规范代码生成，不手写 |

---

## 2. 仓库结构

```
FaceGaze-Bridge/
├── CMakeLists.txt
├── CMakePresets.json
├── vcpkg.json                    # manifest：锁所有依赖版本
├── .clang-format / .clang-tidy
├── src/
│   ├── base/                     # 日志、时钟、配置、错误码、字符串工具
│   ├── core/                     # 纯计算：帧、合并、映射、滤波、校准、注视估计
│   │   ├── model/                # FaceFrame、Shape 枚举（生成）
│   │   ├── merge/                # 时间对齐、外推
│   │   ├── resolve/              # raw → unified 映射
│   │   ├── filter/               # One Euro、限速、死区
│   │   ├── calibrate/            # 拟合、漂移
│   │   └── gaze/                 # 聚散、扫视、注视
│   ├── osc/                      # 编解码 + 传输
│   ├── net/                      # UDP/TCP/WS 封装、mDNS
│   ├── sources/                  # vmc / osc / livelink / openxr / vrcft
│   ├── sinks/                    # vrchat / vmc / shm / ws / udp
│   ├── service/                  # 主进程、SourceMgr、SinkMgr、IPC
│   ├── cli/                      # fgb-cli
│   ├── vrcft-host/               # C# 项目：.NET 宿主宿主
│   └── openxr-layer/             # API Layer（M5）
├── ui/                           # TS + Vite + Svelte
├── modules/                      # 原生示例模块（C ABI 演示）
├── tests/
│   ├── unit/
│   ├── integration/
│   └── fixtures/                 # 抓包样本、.fgbcap、金样输出
├── bench/
├── tools/
│   ├── gen_shapes/               # 从 JSON 规范生成 Shape 枚举与参数表
│   └── pcap_decode/              # 协议分析辅助
└── docs/
```

**分层规则（CI 强制）**：
- `core/` 不得 include `net/`、`sinks/`、`sources/`、任何 OS 头（除 `base/` 的时钟抽象）
- `base/` 不得依赖任何上层
- 违反即编译失败（用 include guard 宏 + 目录约定的 clang-tidy 检查）

---

## 3. 构建

### 3.1 CMakePresets 要点

```json
{
  "version": 6,
  "configurePresets": [
    {
      "name": "windows-x64-debug",
      "generator": "Ninja",
      "binaryDir": "${sourceDir}/build/${presetName}",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Debug",
        "CMAKE_CXX_STANDARD": "20",
        "CMAKE_TOOLCHAIN_FILE": "$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake",
        "FGB_ENABLE_ASAN": "ON",
        "FGB_BUILD_TESTS": "ON"
      }
    },
    {
      "name": "windows-x64-release",
      "inherits": "windows-x64-debug",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "RelWithDebInfo",
        "FGB_ENABLE_ASAN": "OFF",
        "FGB_ENABLE_LTO": "ON"
      }
    }
  ]
}
```

### 3.2 依赖管理纪律

- `vcpkg.json` 里**锁定版本**（`version>=` + `builtin-baseline`），不允许 `latest`
- 新增依赖需要 ADR：说明为什么不能自研、体积影响、许可
- CI 缓存 vcpkg 二进制包，否则每次构建 20 分钟起步

### 3.3 编译时间预算

| 目标 | 全量 | 增量（改一个 .cpp） |
| --- | --- | --- |
| Debug | < 3 min | < 8 s |
| Release | < 6 min | < 15 s |

超标就上预编译头或拆 target。**编译慢会直接杀死迭代意愿。**

---

## 4. 代码规范

### 4.1 命名

| 类别 | 风格 | 例 |
|---|---|---|
| 类型 | PascalCase | `FaceFrame`, `SpscRing` |
| 函数/方法 | snake_case | `try_push`, `publish` |
| 成员变量 | snake_case + `_` 后缀 | `frame_`, `seq_` |
| 常量 | `k` + PascalCase | `kShapeCount` |
| 枚举值 | PascalCase | `Shape::JawOpen` |
| 宏 | `FGB_` + UPPER | `FGB_EXPORT` |
| 命名空间 | 全小写 | `fgb::core` |

### 4.2 硬性规则

1. **不裸 `new`/`delete`**。用 `std::make_unique`、容器、或自定义 arena。
2. **热路径不分配**。`submit()` 链路内禁止 `std::string`、`std::vector` 扩容、`std::function`。用 `std::span`、`fixed_vector`、预分配缓冲。
3. **异常只用于初始化**。运行时链路用 `expected<T, Error>` 或错误码，不用异常（避免栈展开带来的不确定性）。
4. **所有跨线程共享状态要么 atomic，要么有明确的所有权注释**。
5. **坐标系转换只能出现在 `core/coord.hpp`**。CI 用 grep 卡 `-v.z` / `+v.z` 字面量。
6. **公开头文件不用 `#include <windows.h>`**（用 PIMPL 或前置声明），否则宏污染会传染。
7. **每个 `catch` 必须记录日志或转换错误码**，禁止空 catch。
8. **`printf`/`fprintf` 的格式串里，不要紧跟在中文/全角标点后面写 `%s`**。
   MSVC 的格式检查（在 `/W4 /WX` 下会变成错误 C4474）会把前面那个多字节字符的尾字节
   当成 MBCS 前导字节，连 `%` 一起吃掉，于是"参数太多"。要么在 `%s` 前留一个 ASCII 字符，
   要么用 `std::string` 拼好再 `fputs`。`FGB_INFO/FGB_ERROR` 没有格式检查，不受影响——
   所以这个坑只在直接调 `printf` 的地方出现。

### 4.3 注释纪律

只注释"为什么"，不注释"是什么"：

```cpp
// 好：解释非显然的决策
// 这里不能用 mutex：sink 线程持锁 1ms 就会造成输出抖动，
// 而抖动对观感的影响比延迟更大（见 ADR-005）。
// 故用 seqlock：写者永不阻塞，读者最多重试 3 次。

// 差：复述代码
// 检查 seq 是否为偶数
if ((seq & 1) != 0) return false;
```

---

## 5. 测试策略

### 5.1 分层

| 层 | 范围 | 工具 | 覆盖率目标 |
|---|---|---|---|
| 单元 | `core/` 纯函数、`osc/` 编解码、ring/seqlock | Catch2 | 75% |
| 属性测试 | 滤波器的单调性、校准的逆变换一致性 | Catch2 + 随机输入 | — |
| 协议 | 用真实抓包 fixture 解码 | Catch2 | 100% 分支 |
| 集成 | 录制回放 → 金样对比 | 自研 harness | 关键路径全覆盖 |
| 基准 | OSC 编码、merge tick、ring 吞吐 | Google Benchmark | 回归 > 10% 报警 |
| 稳定性 | 8 小时长跑（nightly CI） | 自研 soak | 零崩溃 |

### 5.2 金样测试（最有价值的测试）

`.fgbcap` 的第二个用途：**把真实数据变成可重复的断言**。

```cpp
TEST_CASE("calibration: 5-point fit reduces error below 2deg") {
    auto cap = fgbcap::load("fixtures/questpro_5pt_calib.fgbcap");
    Calibrator cal;
    cal.fit(cap.frames());

    for (auto& f : cap.frames()) {
        auto corrected = cal.apply(f.eyes);
        auto truth = cap.meta().ground_truth_gaze_at(f.sequence);
        REQUIRE(angular_error(corrected, truth) < 2.0_deg);
    }
}
```

**没有这一层，校准和滤波的每次改动都是盲改。**

### 5.3 测试数据从哪来

| 数据 | 来源 |
|---|---|
| 抓包 fixture | M0 阶段录 10 组（不同模块、不同 VRCFT 版本） |
| `.fgbcap` | 自己戴设备录；招募 3–5 个用户录（覆盖不同眼型、眼镜佩戴者） |
| 金样输出 | 由人力确认一次的"正确输出"，之后作为基线 |

---

## 6. CI / CD

```yaml
# .github/workflows/ci.yml 骨架
jobs:
  build-win:
    runs-on: windows-2022
    steps:
      - uses: actions/checkout@v4
        with: { submodules: recursive }
      - uses: lukka/run-vcpkg@v11        # 带二进制缓存
      - run: cmake --preset windows-x64-release
      - run: cmake --build --preset windows-x64-release
      - run: ctest --preset windows-x64-release --output-on-failure
      - run: clang-format --dry-run --Werror src/**/*.cpp src/**/*.hpp
```

| 触发 | 做什么 |
|---|---|
| PR | Debug + Release 构建、单测、clang-format、clang-tidy（仅改动文件） |
| merge to main | 以上 + 基准对比（对比 baseline，退化 > 10% 报警） |
| nightly | 8 小时 soak、ASAN/UBSAN 全量、依赖漏洞扫描 |
| tag `v*` | 构建 → 签名 → 打包 → 生成 changelog → 发布 draft |

**签名**：必须买代码签名证书（OV 约 $200–400/年，EV 约 $400–700/年）。没有签名，每次下载都会被 SmartScreen 拦截，转化率掉一半以上。

### 6.1 实际发布流水线

- **本地一键**：`packaging/release.ps1` 做完整链路——构建、单测、`cmake --install` 到
  `dist/`、抽取/解压 VRCFT 运行时、部署 VC 运行库、编译 Setup、生成 SDK 压缩包、
  `SHA256SUMS.txt` 与 `vrcft-runtime.zip`。加 `-Publish`（需 `gh auth login`）创建或
  更新 **draft** Release，不自动公开。
- **CI**：`.github/workflows/release.yml` 由 tag `v*` 触发，先校验 tag 与
  `CMakeLists.txt` 版本一致，再执行同样的构建/测试/打包（含
  `packaging/pack_sdk.ps1` 产出的 `FaceGazeBridge-SDK-<version>.zip`），上传 workflow
  artifact 并创建 draft Release。
- **CI 的 VRCFT 运行时**：runner 上没有 VRCFT。工作流优先读仓库变量
  `VRCFT_RUNTIME_URL`，否则从最近一个**已发布** Release 下载同名资产。仓库是私有的，
  该变量要填 Release 资产的 **API 地址**
  （`https://api.github.com/repos/<owner>/<repo>/releases/assets/<id>`），工作流会带
  token 与 `Accept: application/octet-stream`；重新上传资产后需要更新变量。首次发布
  或更换 VRCFT 版本时，用本地 `packaging/release.ps1` 生成
  `dist-installer/vrcft-runtime.zip` 并上传到 Release。
- **签名**仍未接入：当前 Release 说明里会提示 SmartScreen 与 SHA256 校验方式。

```powershell
# 本地完整发布（含运行时，自动编译 Setup）
powershell -ExecutionPolicy Bypass -File packaging\release.ps1

# 同时创建 draft Release（工作区必须干净）
powershell -ExecutionPolicy Bypass -File packaging\release.ps1 -Publish
```

---

## 7. 可观测性

### 7.1 日志

- 分级：`trace/debug/info/warn/error`
- 默认 `info`，UI 可切 `debug`（会明显掉帧，需提示）
- 内存中保留最近 10 分钟环形缓冲，崩溃时整体落盘（崩溃前的上下文比崩溃点更重要）
- 文件滚动：10 MB × 7 个
- **日志中禁止出现**：文件绝对路径中的用户名（脱敏为 `%USER%`）、摄像头画面、IP 地址（保留网段）

### 7.2 指标

```cpp
struct Metrics {
    // 每条链路
    uint64_t frames_in, frames_dropped, frames_stale;
    LatencyHistogram source_to_merge, merge_to_sink;  // P50/P95/P99
    uint64_t tick_jitter_ns_p99;
    // 每个 sink
    uint64_t msgs_sent, bytes_sent, msgs_suppressed;
    // 资源
    double cpu_percent, rss_mb;
};
```

UI 的"诊断"页直接渲染这个结构，同时 `fgb-cli diag` 输出文本版供用户贴到 issue。

### 7.3 诊断包

一键导出 zip：
```
diagnostic-20261028-2310/
├── logs/（最近 7 天）
├── config.toml（脱敏）
├── modules.json（已装模块清单 + 版本）
├── system.txt（OS 版本、CPU/GPU、运行库、.NET 版本）
├── metrics.json（最近 5 分钟采样）
└── crash.dmp（若有）
```

**这是降低支持成本的最高杠杆功能**，比写十页 FAQ 都有效。

---

## 8. 许可证合规

### 8.1 本项目

采用 **Apache-2.0**（根目录 `LICENSE`；版权与第三方声明见 `NOTICE`，安装包随附 `licenses/`）：
- 与 VRCFT 生态一致，社区接受度高
- 含专利授权条款，对大公司集成友好
- 允许闭源商业集成

### 8.2 使用 VRCFT 资产时的义务

| 情形 | 义务 |
|---|---|
| 仅实现协议兼容（clean-room） | 无强制义务；建议在致谢中说明 |
| 参考其源码实现 | 衍生作品，必须保留 `LICENSE` + `NOTICE`，标注修改文件与日期 |
| 再分发其二进制（`ModuleProcess.exe` 等） | 必须随包附带 `LICENSE`、`NOTICE`，并在"关于"页列出 |
| 使用 "VRCFT" / "VRCFaceTracking" 名称 | **不得作为产品名/商标使用**；只能作事实性描述（"兼容 VRCFaceTracking 模块"） |

**本项目做法**：核心 clean-room，`runtime/vrcft` 组件以"再分发官方二进制"方式集成，随包附带
`runtime/vrcft/LICENSE`、`runtime/vrcft/NOTICE-VRCFaceTracking.txt` 与根目录 `NOTICE`；
设置页「关于」卡片给出摘要和入口。

### 8.3 厂商 SDK 的红线

| SDK | 可否打包进安装程序 | 做法 |
|---|---|---|
| HTC SRanipal | ❌ 通常禁止再分发 | 引导用户自行安装 SteamVR 版 |
| Meta XR SDK | ❌ 需接受条款 | 同 VRCFT：由模块自行处理，我们不打包 |
| PICO SDK | ❌ 同上 | 同上 |
| OpenXR Loader | ✅ Apache-2.0 | 可打包 |
| MediaPipe | ⚠️ 代码 Apache-2.0，**模型许可单独** | 需核对模型条款；不确定就不打包，首次使用时下载 |
| OpenVR / SteamVR | ✅ 头文件 BSD-3；运行时 DLL 不打包 | 只分发 `openvr.h` 与许可证；运行时加载用户 SteamVR 的 `openvr_api.dll`（ADR-0020） |

**执行纪律**：任何新增二进制依赖，必须在 PR 里附许可结论，否则不合入。

### 8.4 3D 资产

校准页的面部模型属于"产品资产"，不是代码依赖：

- 自制或委托制作：版权归项目，需在 `THIRD_PARTY_NOTICES.md` 记录作者与授权
- 使用 CC0 / CC-BY 扫描模型：保留来源、作者、许可证文本；CC-BY 必须在关于页署名
- **不使用**来源不明的"ARKit 表情模型"或从商业作品提取的网格
- 贴图与模型分开记录；导出脚本放 `tools/`，资产源文件不进仓库（或走 LFS）

---

## 9. 风险登记册

按"影响 × 概率"排序。

| # | 风险 | 概率 | 影响 | 触发信号 | 缓解措施 |
|---|---|---|---|---|---|
| R1 | **VRCFT 修改模块 IPC 协议** | 中 | 高 | M0 抓包结果与文档不符 | ①自带 pinned 版本运行时；②版本探测 + 多解码器；③与上游沟通，争取协议稳定承诺 |
| R2 | **`ModuleProcess` 无法脱离 VRCFT 安装运行**（硬编码路径/注册表依赖） | 中 | 中 | M0 假设 1 失败 | 降级为"要求安装 VRCFT"，检测其安装路径后复用 |
| R3 | **硬件不可得，模块兼容性无法验证** | 高 | 高 | M2 只有 1–2 个模块通过 | ①优先纯软件源（MediaPipe/LiveLink）；②社区众测；③提供"兼容性自检工具"让用户跑 |
| R4 | **反作弊冲突**（D3D 注入） | 高 | 中 | 用户报告 VRChat 踢出 | 从一开始就不做注入式功能，或严格限定"仅单机游戏"，UI 显式警告 |
| R5 | **VRChat 改变 OSC 行为** | 低 | 高 | 官方更新日志 | 抽象 OSC 层，参数表数据驱动；跟版本快速适配 |
| R6 | **单人项目范围失控** | 高 | 高 | 连续两个里程碑超期 > 30% | 严格执行 Go/No-Go 检查点；P2/P3 功能一律推迟 |
| R7 | **性能不达预期**（WebView2 内存、UI 卡顿影响主链路） | 中 | 中 | 诊断页显示 tick 抖动上升 | UI 与引擎分进程；引擎线程提优先级 |
| R8 | **VRCFT 上游也做了多输出/注视点** | 中 | 中 | 上游 release note | 我们的护城河是 C++ 性能 + 多目标架构，不是单一功能；持续跟进上游但保持架构差异 |
| R9 | **杀软误报 / SmartScreen 拦截** | 高 | 中 | 用户反馈"安装被拦" | 代码签名证书 + 提交微软误报申诉 + 提供绿色版 |
| R10 | **用户期望管理失败**（以为能"给 VRChat 开注视点渲染"） | 高 | 中 | 论坛/评论区出现"骗人" | 功能页明确写能力边界（见 features §5.5），FAQ 前置 |
| R11 | **隐私争议**（摄像头 + 眼动数据） | 中 | 高 | 媒体报道/用户质疑 | 全本地处理、开源可审计、默认关遥测、提供"数据流向"说明页 |
| R12 | **上游 Apache-2.0 合规疏漏** | 低 | 高 | 律师/社区指出 | 建立 `THIRD_PARTY_NOTICES.md`，CI 检查二进制依赖是否都有记录 |
| R13 | **写实面部资产做不出来/授权不清**（ADR-0021） | 中 | 中 | 预览像玩具，或资产来源无法证明 | 先做风格化写实自有模型；照片级单独立项；资产来源入 `THIRD_PARTY_NOTICES.md` |
| R14 | **SteamVR 注册无法在 CI 验证/用户环境差异**（ADR-0020） | 高 | 低 | 开关状态与实际不符、注册失败 | 只在 SteamVR 运行时允许设置；调用后读回校验；失败给具体原因；绝不自动注册 |
| R15 | **3D 预览拖慢界面或引擎** | 中 | 中 | 开校准页后 tick 抖动上升、UI 掉帧 | preview 只在校准页可见时 30 Hz；独立 RAF；CPU/GPU 预算；WebGL2 不可用时降级 |
| R16 | **托盘改变"关窗即停"的预期** | 中 | 中 | 用户关窗后以为追踪已停，实际还在输出 | 首次收进托盘给明确提示；托盘菜单显示运行状态；文档写清"托盘退出才停引擎" |
| R17 | **OSCQuery 端口发现依赖 mDNS/Bonjour**（ADR-0023） | 中 | 低 | 启动落回本地 JSON、换头像不自动 | 150 ms 收集窗口避开别的 `_oscjson` 服务；5353 被占时退回临时端口；失败逐级回退本地 JSON；`tools/probe_oscquery.py` 可直接排障 |
| R18 | **流式输出成为外部契约**（ADR-0024） | 中 | 中 | 第三方按旧偏移读取，字段/形状顺序变更后错位 | WS JSON 只看键名；UDP 固定小端布局 + magic/version，新增字段只追加；形状顺序变更必须升 version；`sdk/docs/streaming-outputs.md` 与 `sdk/fgb_udp_frame.h` 同步；两个探针用于手工验收（CI 只跑单测与 VMC 冒烟） |

### 重点风险详解

**R1 与 R2 是"项目能不能成立"级的风险，必须在 M0 用两周验证清楚。** 其他风险都是"做得累不累"，这两个是"做不做得成"。

**R6 是最常见的实际死因**。单人项目做到 M3 时最容易膨胀——看到什么功能都想加。纪律：**每个里程碑只做列表内的东西，想法记进 `IDEAS.md`，不实现。**

---

## 10. 开发环境准备（第一天）

```powershell
# 1. 必备
winget install Microsoft.VisualStudio.2022.Community --override "--add Microsoft.VisualStudio.Workload.NativeDesktop"
winget install Kitware.CMake
winget install Ninja-build.Ninja
winget install Git.Git

# 2. vcpkg
git clone https://github.com/microsoft/vcpkg C:\dev\vcpkg
C:\dev\vcpkg\bootstrap-vcpkg.bat
setx VCPKG_ROOT C:\dev\vcpkg

# 3. .NET 8 SDK（编译 vrcft-host 用）
winget install Microsoft.DotNet.SDK.8

# 4. Node（UI）
winget install OpenJS.NodeJS.LTS

# 5. 验证
cmake --version && ninja --version && dotnet --version && node -v
```

**额外建议装**：
- Wireshark（协议分析，M0 必需）
- Visual Studio 的 "OpenXR Tools" 与 SteamVR（调试眼追）
- Process Explorer（看句柄泄漏，模块宿主常见问题）

---

## 11. 文档与协作

| 文档 | 位置 | 更新时机 |
|---|---|---|
| 架构决策记录 | `docs/adr/NNNN-*.md` | 每个非平凡决策 |
| 协议字段表 | `docs/adr/0001-protocol-findings.md` | M0 产出，后续随版本追加 |
| 模块兼容性矩阵 | `docs/compat.md` | 每验证一个模块 |
| 用户手册 | `docs/user/` | M6 |
| `IDEAS.md` | 根目录 | 随时（替代"顺手实现"） |

**ADR 模板**：
```markdown
# ADR-0005: 快照发布用 seqlock 而非 mutex
## 状态：已采纳
## 背景：...
## 决策：...
## 后果：正面 ... / 负面 ...
## 替代方案：mutex（抖动）、RCU（复杂度）
```
