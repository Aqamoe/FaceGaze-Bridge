# ADR-0013 · 多输入源合并：按通道选提供者，而不是"谁最后到谁赢"

- 状态：已采纳
- 日期：2026-10-29
- 影响：新增 `engine/source_merger`；`FaceFrame` 增加 `channels`；`Engine::tick_once` 改为"先合并、再后处理、再发布"；CLI 支持重复 `--source` 与 `--eye-source / --expression-source / --head-source / --stale-ms`；`status` 增加 `sources`

## 问题

M1 的合并写法是：

```cpp
for (auto& source : sources_) {
    if (source->poll(frame)) { ...; last_frame_ = frame; }
}
```

即"按注册顺序 poll，后一个覆盖前一个"。单源时没问题；两个源同时喂就一定会丢东西：
VRCFT 模块给表情 + 眼追、VMC 给表情，注册顺序决定了最后剩下谁的整帧。用户看到的现象是
"表情正常但眼睛不动"（或反过来），而且没有任何日志能解释。

同时还有一个隐患：后处理（校准 / One Euro 滤波）写在那个循环里，等于**每个源各处理一遍**。
两个源频率不同时滤波会被叠加，参数变得又慢又钝，但看不出是这里的锅。

## 决定

### 1. 一帧拆成三个通道，各自选提供者

| 通道 | 内容 | 谁能提供 |
|---|---|---|
| Expression | 88 个形状 + `shapeValid` | 模块、VMC、回放 |
| Eyes | gaze / openness / pupil / vergence | 模块、回放（VMC 协议没有眼追） |
| Head | `hasHeadPose` + 旋转 + 位置 | VMC（模块协议里没有头姿） |

每一路独立选：显式指定（`--eye-source`）优先，否则按注册顺序取第一个"新鲜且提供该通道"的源。
三路可以来自三个不同的源，互不影响。

### 2. 通道必须由源显式声明，不能靠值猜

`FaceFrame::neutral()` 里眼睛是 **valid + 睁开正对前方**——这是有意的：降级时也要发合法值
（架构 §7.3）。但代价是"从 neutral 起手的源"看起来都像提供了眼追。第一版实现就是靠
`eyes.any_valid()` 判断能力，结果 VMC 被当成眼追来源，硬件眼追被中性眼遮掉。

所以给 `FaceFrame` 加了 `channels` 位掩码（`channel::kExpression/kEyes/kHead`），由 source 在
发布前声明这一帧实际带了哪几路；合并层只认声明。这比"看值猜"多一行代码，但把一类
非常难查的错（眼睛永远不回正）变成了不可能。

### 3. 过期就换人，全都没了就清空

每个源记录"最后一次拿到它的帧"的时刻；超过 `stale_ns`（默认 500 ms）不算新鲜。
500 ms 是权衡：VMC 常见 30–60 Hz、模块可能 30 Hz，阈值太小会把正常源误判成掉线。

某一路没有任何新鲜来源时，**清空该通道**（eyes invalid、shapes 归零、`channels` 位清掉），
而不是继续发上一次的值。冻住的值比中性值更糟：用户会以为设备还在工作。

### 4. 先合并，再后处理，再发布

```
poll 所有源 → 按通道合并 → 处理器链（校准/滤波）→ 发布快照 → 扇出 sink
```

后处理只在合并后的帧上跑一次。这也是为什么 `frames_in`（源帧数）和"发布的帧数"不再是同一个
数字——界面上的"累计帧"现在明确是**输入帧总数**。

### 5. CLI 与界面

```powershell
# 硬件眼追 + 摄像头表情各一个模块：两个 vrcft 实例，靠 --module-id 指定
fgb-cli run --source vrcft --module-id <A> --source vrcft --module-id <B> `
            --eye-source "vrcft[1]" --expression-source "vrcft[2]"

# 模块 + VMC：VMC 优先管表情，模块管眼追（也可以反过来显式指定）
fgb-cli run --source vmc --source vrcft --eye-source vrcft
```

- `--source` 可重复，顺序即优先级；多实例的 vrcft 显示名为 `vrcft[1]`、`vrcft[2]`
- `--stale-ms` 调整过期阈值
- 状态页新增「输入源」卡片：每个源的帧数、距上次更新的时间、以及**哪几路正在用它**
  （绿色 = 在用，灰色 = 该源有但没选它，最暗 = 该源没有这一路）

## 代价与遗留

- 合并是"整通道替换"，没有做加权融合。硬件眼追 + 摄像头眼追同时存在时不会按置信度混合，
  而是按优先级取一个。加权融合等有真实重叠硬件时再做，现在没有可验证的场景。
- 两个 vrcft 实例各自起一个模块进程，端口由沙箱随机分配；界面的「模块」页仍然只显示第一个
  实例的状态（`status.module`）。多实例的模块页留到后续。
- VMC 的头姿与形状共用 `pending_`，某轮只发头姿时 `channels` 只标 Head——这是对的，
  但意味着"头姿源"可能每隔几轮才出现一次；只要在 `stale_ns` 内就不影响。

## 验证

- `tests/test_merge.cpp`：8 个用例，覆盖通道拆分、优先级、显式指定、拼错名字回落、
  过期换人、缺失通道清空、三源三通道、无新帧不发布、以及**后处理只跑一次**（两源各 5 帧 →
  处理器调用 5 次而不是 10 次）
- `fgb-tests` 全部 **97 个用例**通过
- 实跑 `--source vmc --source replay`（回放 25 s 真实抓包，VMC 用脚本 30 Hz 灌 `jawOpen`）：
  `/api/status` 的 `sources` 显示 vmc 只被用于表情、replay 只被用于眼追，`JawOpen` 跟随 VMC；
  加上 `--expression-source replay` 后两路互换
- `tools/probe_ui.py` 增加断言：输入源卡片行数 = `status.sources` 长度、至少有一路被标记在用；
  截图核对了「输入源」卡片的三态样式
