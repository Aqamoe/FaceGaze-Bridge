# ADR-0001 · VRCFT 实际 OSC 输出的初步观察

- 状态：观察记录（非决策）
- 日期：2026-10-28
- 来源：本机运行 VRCFT 时，用测试脚本旁听 `127.0.0.1:9000` 抓到的真实流量

## 背景

冒烟测试时脚本误绑了 VRChat 的 OSC 端口 9000，意外截获了本机 VRCFT 正在发送的数据。
虽然是一次事故，但拿到了一手样本，记录下来供后续参数层实现参考。

## 观察到的现象

**1. 未做 MTU 分包**

单个 bundle 达到 4088 字节，包含 81 条消息。这意味着这些包必然被 IP 分片
（以太网 MTU 1500），任何一片丢失整个 bundle 就废了。

FGB 的选择是主动分包到 1200 字节以内，代价是包数变多、syscall 变多，收益是
无线串流等易丢包环境下更稳。这是一个有意的差异，需要在对比测试里确认没有副作用。

**2. 确实在使用 binary 参数编码**

抓到的地址里出现了：

```
/avatar/parameters/FT/v2/EyeSquintRight8
/avatar/parameters/FT/v2/EyeLidRight4
/avatar/parameters/FT/v2/EyeLidRight2
/avatar/parameters/FT/v2/BrowExpressionRight2
/avatar/parameters/FT/v2/SmileFrownLeft1
```

后缀数字是 2 的幂次位编号，即把 float 拆成多个 bool 传输（`Param1/2/4/8...`）。
这印证了架构文档 §9.2 里"需要复刻 BinaryBaseParameter"的判断——它不是理论，
是线上正在用的机制。

**3. 存在基础形状集之外的派生参数**

`SmileFrownLeft`、`BrowExpressionRight` 都不在 Unified Expressions v2 的基础形状里，
属于 VRCFT 的简化/组合参数层。

这确认了 FGB 的分层是对的：`Shape` 枚举只放基础形状（86 项），派生参数是单独一层，
由基础形状计算得出。两者混在一个枚举里会导致"某个参数到底该谁写"永远吵不清。

## 对后续工作的影响

| 事项 | 结论 |
|---|---|
| 分包策略 | 保持 FGB 的 1200 字节上限；需要在真实 VRChat 里对比两种策略的观感 |
| binary 编码 | M3 必须实现，且要能通过配置开关（部分头像的 bool 参数配额也紧张） |
| 参数分层 | 维持 `Shape`（86）+ `Derived`（约 30–40）两层结构 |
| 参数顺序 | 观察到的顺序与枚举顺序不一致，说明顺序不重要，**只有地址和值重要** |

## 待补

- [ ] 抓取完整的一轮输出，整理出 VRCFT 实际发送的完整参数清单（用于对齐验收）
- [ ] 确认 binary 参数的位数与量化方式（步长、是否四舍五入）
- [ ] 确认心跳频率与参数相关性裁剪的实际行为
