# ADR-0005 · 二进制参数编码

- 状态：已采纳
- 日期：2026-10-29
- 影响：`params/avatar_config`、VRChat sink 的发送路径

## 背景

面捕头像很少把参数声明成 Float。实测本机 `Avatar-A` 的 156 个 v2 参数里，
绝大多数是 `FT/v2/JawOpen1`、`FT/v2/JawOpen2`、`FT/v2/JawOpen4`… 这种 bool 组，
外加一个 `FT/v2/JawOpenNegative`。这是 VRCFT 的"二进制参数"：一个 0..1 的浮点值
被拆成若干 bool，头像侧再按位拼回一个整数。

只发 Float 的话，这类头像的笑容、嘴型全都收不到——匹配数会停在 52/365 而不是现在的 52。

## 规则（照抄 VRCFT 的 BinaryBaseParameter）

对一个逻辑参数 `N`：

1. 头像声明的 bool 地址里，找 `<N><数字>` 形式的，数字必须是 **2 的幂**（1/2/4/8…），
   移位量 = log2(数字)。非 2 的幂直接丢弃（VRCFT 的 `GetBinarySteps` 返回 null）。
2. **量化级数 = 2^(声明的位数)**，不是 2^最大移位。声明 1/2/4 三位 → 8 级。
3. 取值：`(int)(|v| * 级数)`，然后取第 `移位量` 位。
4. `|v| > 0.99999` 直接全置位——否则 `(int)(1.0 * 8) = 8` 会落到不存在的第 4 位。
5. 负值：只有头像声明了 `<N>Negative` 时才有意义，此时取绝对值编码，符号走
   `Negative` 一个 bool。**没声明 Negative 时负值整位归零**，不是取绝对值——
   这一条最容易写错，写错的表现是"撇嘴时反而在笑"。

## 实现

- `params::binary_bit(value, shift, bit_count, has_negative)` 是纯函数，单测覆盖量化、
  满量程、负值三种情况
- 路由阶段把每个逻辑参数解析成若干 `OutputTarget`：`Float` / `Bool` / `BinaryBit` / `Negative`
- sink 按目标列表发送，同一个参数的多条目标共用一次求值

## 验证

真实头像 `Avatar-A`，VMC 输入 `jawOpen = 0.5`，该头像声明了 5 位（1/2/4/8/16）：

```
FT/v2/JawOpen     0.5
FT/v2/JawOpen1    False
FT/v2/JawOpen2    False
FT/v2/JawOpen4    False
FT/v2/JawOpen8    False
FT/v2/JawOpen16   True     ← 0.5 × 32 = 16 = 0b10000
```

负值路径 `mouthLeft = 0.5`（3 位，`MouthX = -0.5`）：

```
FT/v2/MouthX           -0.5
FT/v2/MouthX4          True     ← |−0.5| × 8 = 4
FT/v2/MouthXNegative   True
```

匹配数从 42 提升到 52（该头像所有非后缀参数），OSC 目标数 156。

## 尚未实现

- **`Int` 类型的头像参数**。VRCFT 对 int 有单独处理，实际面捕头像几乎不用，跳过
- **`<N>Negative` 与 float 同时存在时的优先级**。VRCFT 是两者都发，我们也是，
  没观察到冲突，但如果遇到"符号翻转"式 bug，先查这里
