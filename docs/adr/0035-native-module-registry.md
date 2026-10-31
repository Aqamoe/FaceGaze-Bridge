# ADR-0035 · 原生模块的在线库（fgb-registry / registry schema v2）

- 状态：**已实施**（2026-10-31）
- 日期：2026-10-31
- 影响：`src/vrcft/native_library.*`（新）、`src/vrcft/module_registry.*`（暴露解压）、
  `src/web/ipc.*`、`src/base/json_value.*`（新）、`src/base/hash.*`（新）、`ui/app.js`、
  `ui/i18n.js`、`ui/index.html`、`tests/test_native_library.cpp`（新）、
  `tools/probe_ipc.py`、`tools/probe_ui.py`、`sdk/docs/native-module-guide.md`
- 关系：延伸 [ADR-0017](0017-module-store-library-runtime.md)（模块仓 + 在线库）与
  [ADR-0030](0030-native-module-v2-composition.md)（原生模块 compose）；
  与 VRCFT 老注册表客户端（`vrcft/module_library.*`）**并存但不合并**

## 问题

原生模块（ABI v2）此前只能手动装：用户在本地打 zip，再从界面选文件。这带来三个实际后果：

1. **没有发现路径**。作者发了新版也要靠作者自己在别处贴链接，用户装完就停在旧版本。
2. **没有完整性校验**。手动 zip 是"用户自己挑的文件"，宿主没有可核对的哈希；而模块是
   会被 `LoadLibrary` 进进程的 DLL，这等于把投毒面留给用户判断。
3. **与 VRCFT 老注册表混用会丢掉信息**。`registry.vrcft.io/modules` 是"扁平数组 + 全标量"：
   能力列表要拼成 `"expression,eyes"`，没有版本历史、通道、ABI/系统要求、更新说明，
   校验值只有 MD5。把这些塞进 `LibraryEntry` 只能靠新增一堆"仅原生用"的字段，
   两套语义会在同一个解析器里互相污染。

社区已经有了 `Aqamoe/fgb-registry`：GitHub Release 聚合器，产物是
`registry.json`（`schemaVersion: 2`），带 `sha256` + `size`、`versions[]`、`channel`、
`requires{abi,minHostVersion,os,arch}`、`i18n`。它明确不兼容老格式，也不依赖自建服务器。

## 决定

### 1. 新增独立的 `NativeLibrary`，不复用 `ModuleLibrary`

| | VRCFT 库（`ModuleLibrary`） | 原生库（`NativeLibrary`） |
|---|---|---|
| 地址 | `https://registry.vrcft.io/modules` | `https://aqamoe.github.io/fgb-registry/registry.json` |
| 环境变量覆盖 | `FGB_MODULE_REGISTRY` | `FGB_NATIVE_REGISTRY` |
| 缓存 | `%LOCALAPPDATA%\FaceGazeBridge\library.json` | `…\native-library.json` |
| 校验 | MD5（可能为空） | **SHA-256 必填** + 字节数 |
| 版本信息 | 单个 `Version` | `latest` + `versions[]`（可回滚） |
| 失败语义 | 解析失败保留缓存 | 结构版本不认识就整体拒绝 |

两个类共享同一个线程模型（网络/下载在后台线程，落盘只在引擎线程 `poll()`），
但**不共享解析器**：老格式的容错规则（键顺序不定、值为 null、嵌套随便跳）套到
schema v2 上只会把"结构版本不认识"降级成"静默少几个模块"。

### 2. 解析要求：schemaVersion 必须是 2，条目坏了只丢条目

- `$schema` 不参与判断，只认 `schemaVersion`；不等于 2 时报
  「模块库结构版本是 N，本程序只认识 2，请升级 FaceGaze Bridge」，并保留旧缓存。
- 条目缺 `id` / `name` / `latest.version`，或 `downloadUrl` 不是 `https`（仅允许
  `http://127.0.0.1` / `localhost` 供探针联调）时**丢弃该条目并告警**，不让整份清单失败：
  一个作者的清单写错不该让所有人的模块库变空。
- schema v2 是深结构，`base/json.hpp` 的"按 key 取一层标量"读不了，新增
  `base/json_value.*`（递归树，保序对象、深度上限 32）。它只服务模块库，不假装通用。

### 3. 安装链路：先验哈希，再验包内清单，最后才落盘

```
GET registry.json ──► 选择版本（latest 或用户指定）
        │
        ▼
下载到 %LOCALAPPDATA%\FaceGazeBridge\downloads\<id>-<version>.zip
        │  ① 字节数 == latest.size（size>0 时）
        │  ② sha256 必须存在且与文件一致（不匹配即删除）
        ▼
解压到临时目录（复用 ModuleRegistry 的越界/符号链接/解压炸弹检查）
        │  ③ 包内 fgb-native.json：ModuleId == 条目 id
        │     AbiVersion 必须存在且 == requires.abi
        │     清单声明的 DLL 必须真的在包里
        ▼
ModuleRegistry::install_path(dir, source="registry", replace=true)
        │  ④ 同 ModuleId 覆盖升级，保留模块自己的 config.ini
        ▼
写入模块仓并广播 modules / library
```

- 单文件 DLL 资产（schema 允许）没有包内清单，只能按库条目元数据安装；
  这种情况**记一条 WARN**，不假装校验过。
- 校验失败时删除下载文件，错误文案写"期望/实际"两侧的值，便于作者自查。
- 安装动作仍然只在引擎线程发生（`poll()`），避免与界面/扫描线程同时改模块仓。

### 4. 兼容性由宿主判定，界面只做本地化

`requires.abi` 必须等于宿主的 ABI（当前 2）；`minHostVersion` 用语义版本比较
（`update::compare_versions`，不信字符串）；`os` / `arch` 按当前平台匹配。
不兼容的条目**仍然列出来**（否则用户以为库里没有这个模块），但安装按钮禁用，
并在卡片上显示原因。原因同时给机读的 `incompatible_code`（`abi` / `host_version` /
`os` / `arch`）和中文 `incompatible_reason`（日志/排障用），界面按 code 出三语文案。

### 5. IPC 与界面：同一条 `library` 消息的 `native` 段

- `library` 消息新增 `native: {registry{title,description,homepage,generated_at}, status{…}, items[…]}`。
  两个标签页（VRCFT / 原生）共用一次广播和同一个"安装完成后重算已装/可更新"的时机。
- 新命令 `refresh_native_library`、`install_native_library`（`id` + 可选 `version`）。
  失败时 ack 带同一句 error，**并且仍然广播一次 library**：界面靠 `status.phase/error`
  显示"为什么没开始"，不能只弹一句 toast 就没了。
- 安装进度沿用下载后台线程的 5 Hz 推送（`status.received/total`）。
- `open_module_page` 的白名单扩到原生条目的 `homepage` / `icon` / `latest.release_url`：
  链接必须来自已解析的元数据，不能让页面脚本把一条 WS 命令变成"打开任意 URL"。
- 界面：原生标签页不再是"只有已安装模块"，而是在线条目 + 本地装但库里没有的模块；
  提供搜索、测试版开关（`channel: beta` 默认隐藏）、能力/审核/归档徽标、
  版本下拉（回滚）、更新说明与模块主页。文案三语齐全，图标优先用 registry 的
  `icon`（https 远程），已安装的优先用本地 `/module-icon/<id>`，失败回退首字母头像。

## 代价与边界

- **没有 ETag/304**。`net::http_client` 目前不暴露响应头，刷新靠"缓存 6 小时 + 手动刷新"。
  国内访问 `*.github.io` 不稳，失败时保留缓存并显示上次时间，不会清空列表。
- **包里没有签名**。`verified` 只是注册表维护者审核过的标记；`sha256` 能防传输损坏
  与"清单被改写但包没换"，防不住作者账号被盗后重新发版。这一点在用户手册里写明。
- **不支持自动更新**。库里的"有新版本"只是提示，安装仍需用户点一次确认（模块是可执行代码）。
- **i18n 只覆盖三个字段**（`summary`/`description`/`usage`），其它字段（如使用说明里的
  列表）仍以顶层字段为准；BCP-47 键按 `lang` → `lang-REGION` → 同语言前缀匹配。

## 被否决的替代方案

| 方案 | 否决理由 |
|---|---|
| 把原生条目塞进 `ModuleLibrary`，加 `kind` 字段 | 两套校验/版本/兼容语义写在一个解析器里，任何一处改动都会影响另一侧 |
| 继续用 `registry.vrcft.io` | 老格式装不下版本历史/通道/ABI 要求，校验只有 MD5 |
| 在宿主里内置一份静态模块清单 | 每发一个模块就要发一次宿主版本；与"社区提 PR"的流程冲突 |
| 只信任 `sha256`，不核对包内清单 | 只证明"下到的是清单说的那个文件"，防不住清单本身就是偷换的条目 |
| 刷新时直接替换内存里的列表 | 网络抖动会让界面在"有列表/空列表"之间跳；保留缓存 + `stale` 标记更可解释 |

## 验证

- 单测（`tests/test_native_library.cpp`）：真实形状的 schema v2 清单解析（嵌套、
  `versions[]`、`i18n`、多字节文本）、`schemaVersion != 2` 整体拒绝、
  https 之外下载地址丢弃、ABI/系统/架构/宿主版本四类不兼容判定、
  显式版本选择、包内清单核对（ID 不一致 / ABI 不一致 / 缺 AbiVersion / 缺 DLL 全部拒绝）、
  SHA-256 与 MD5 对照已知向量。
- IPC 探针（`tools/probe_ipc.py`）：`library.native` 段存在、条目带 `sha256`/`size`/
  `compatible`、`refresh_native_library` 有成功回执。
- 界面探针（`tools/probe_ui.py`）：原生标签页卡片数 == `library.native.items`（含本地独有条目）、
  卡片展开有介绍与使用说明、测试版开关与说明文案可见、`registry.homepage` 正确。
- 本机实测：真实 `registry.json` 刷新成功（1 个条目），无硬件无模块时也能打开页面。
