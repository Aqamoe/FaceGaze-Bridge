# ADR-0004 · 参数地址必须从头像配置发现，不能写死前缀

- 状态：已采纳
- 日期：2026-10-29
- 影响：`src/params/`、VRChat sink 的地址生成方式

## 背景

M1 阶段我把 v2 参数地址写成了 `/avatar/parameters/FT/v2/<名>`，依据是 M0 抓到的一段
本机 VRCFT 流量（见 ADR-0001）。这个决定当时看着有实证支撑，实际上是错的——或者说，
只对了一部分。

读 VRCFT 源码时发现矛盾：`UnifiedExpressionsParameters` 里的参数名是 `"v2/Eye"`，
`BaseParam` 的地址是 `DefaultPrefix + name`，即 `/avatar/parameters/v2/Eye`。
整个 Core.dll 里搜不到 `FT/` 这三个字符（UTF-16 下也搜过）。

那抓到的包为什么带 `FT/v2/`？看 `BaseParam.ResetParam` 就明白了：

```csharp
var compatibleParam = newParams.FirstOrDefault(param =>
    _regex.IsMatch(param.Address) && param.Type == typeof(T));
if (compatibleParam != null)
{
    Relevant = true;
    OscMessage.Address = compatibleParam.Address;   // ← 用头像声明的地址
}
else
{
    Relevant = false;                                // ← 头像没声明，压根不发
}
```

`FT/` 不是 VRCFT 加的，是**头像作者给参数起的名字**。VRCFT 在拿到头像的参数表之后，
直接把 `OscMessage.Address` 换成了头像里声明的那个完整地址。

## 实测：前缀确实没有统一约定

扫本机 `%LocalLow%\VRChat\VRChat\OSC\*\Avatars\*.json`（223 份，都是 VRChat 自己写的）：

| 前缀 | 参数数量 | 例子 |
|---|---|---|
| `FT/v2/` | 6534 | `/avatar/parameters/FT/v2/NoseSneer2` |
| `v2/` | 1315 | `/avatar/parameters/v2/NoseSneer4` |
| `EchoFT/v2/` | 384 | `/avatar/parameters/EchoFT/v2/MouthRaiserUpper4` |

写死任何一个都只能覆盖一部分头像。而且第三方工具也印证了这点：VRCFTReceiver 的模板
用的就是 `FT/v2/EyeLeftX`，与官方文档里的 `v2/EyeX` 不是一个字符串。

## 决策

**地址一律取自头像声明的参数表；拿不到才回退到可配置的固定前缀。**

数据来源：`%LocalLow%\VRChat\VRChat\OSC\<用户ID>\Avatars\<头像ID>.json`，
结构为 `{ id, name, hash, parameters: [{ name, input:{address,type}, output:{...} }] }`。
只取 `input`（`output` 地址相同），只认 `Float` / `Bool`（`Int`/`String` 不参与）。

匹配规则照抄 VRCFT 的正则 `(?<!(v\d+))(/<name>)$`：

- `v2/EyeLidLeft` 匹配 `FT/v2/EyeLidLeft`、`v2/EyeLidLeft`、`EchoFT/v2/EyeLidLeft`
- legacy 的 `EyeLidLeft` **不匹配** `FT/v2/EyeLidLeft`（前一段是 `v2`，被 lookbehind 挡掉）
- 带数字后缀的二进制参数 `FT/v2/MouthPress1` 不匹配基名 `v2/MouthPress`

第三条规则解释了一个容易踩的坑：**v2 参数名里的 `v2/` 是名字的一部分，不是目录前缀**。
所以 catalog 的每个分组都带一个 `name_prefix`，参与匹配而不参与地址拼接。

## 选哪份配置

VRChat 每次换头像都会写一份配置，最新那份很可能是根本没有面捕参数的普通模型。
因此不能"取最新"，而是**在最近改动的 8 份里挑 `v2/` 参数最多的那份**：
面捕头像动辄上百个 v2 参数，普通头像通常是 0。命中 200 个以上就提前结束。

实测在本机挑中的是 `Eku-FT2-S`（444 个声明参数，其中 156 个 v2），匹配上 42 个。

## 后果

- **写死的 `FT/v2/` 降级为兜底**（`--param-prefix` 可改），并在日志里明确告警
- 顺带解决了参数裁剪：头像没声明的参数**不发**。v1 legacy 有 189 个参数，全发会
  把 VRChat 的 OSC 接收端压垮；有了过滤，`--params both` 实际只发头像认的那些
- `fgb-cli params` 成为排查"表情不动"的第一个工具：它把"头像声明了什么"和
  "我们准备发到哪里"并排列出来

## 已知缺口

- **二进制参数（`Name1`/`Name2`/`Name4`…）尚未实现**。本机这个头像 156 个 v2 参数里
  大多数是二进制形式，目前只绑定了 52 个非后缀名中的 42 个。补齐后匹配率会显著上升
- **头像切换无法实时感知**。正确做法是 OSCQuery（mDNS 发现 + `/avatar/change`）；
  本机 VRChat 只监听了 UDP 9000 与 mDNS 5353，没开 9001 的 HTTP 服务，所以现阶段
  用"启动时挑选 + `reload_avatar()` 手动重载"。下一阶段做 OSCQuery。
