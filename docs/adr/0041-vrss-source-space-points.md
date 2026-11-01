# ADR-0041 · VRSS 遇到"来源空间"2D 点时的取舍

- 状态：**已实施**（2026-11-01）
- 日期：2026-11-01
- 影响：`src/sinks/vrss_gaze_sink.*`、`src/config/outputs.*`、`src/web/ipc.cpp`、
  `src/vrss/workflow.cpp`、`ui/`（VRSS 页）、`docs/09-user-guide.md`
- 关系：修订 [ADR-0033](0033-gaze-point-2d-space.md) 在 VRSS 这一条消费路径上的落地方式；
  不改变 ADR-0033 对"来源空间 ≠ 屏幕位置"的结论

## 问题

PICO Connect Gaze（`com.picogaze.pico-connect-gaze`）只给**逐眼原始 2D 点**，不声明
`FGB_ATTR_POINT_VIEWPORT`——按 ADR-0033 这是"来源空间"，不能当屏幕位置用。实测：

```
point_space = "source"
left_point_2d  ≈ 0.73   right_point_2d ≈ 0.43   → dx ≈ 0.29（随注视深度在 0.10..0.30 变化）
dy ≈ 0.000（两眼 y 恒相同）
```

VRSS sink 的第一条取数分支要求 `point_viewport`，第二条需要方向（该模块没有方向），
于是**一包都不发**。provider 侧表现为 `fresh=false`、`sent` 不再增长，驱动保留上一次
（或默认）位置——头显里就是"高质量区固定在一个角上，不跟随注视"。而页面的验证步骤
当时只看 `consumerCount > 0`，于是显示"正在提交注视"，把故障伪装成了成功。

## 决定

1. **默认行为不变**：`point_viewport == false` 且没有方向时，VRSS 继续不发包。
   这保住了 ADR-0033 的安全默认——不知道坐标空间就不要假装知道。
2. **新增显式实验开关** `vrss.allow_source_points`（默认 `false`，UI 文案
   「使用未标定的原始 2D 点（实验）」）。打开后：
   - 取 `(left + right) / 2`、`y = left.y` 作为**两眼共用**的位置；
   - 只发了单眼时用该眼自己的值（provider 要求 `validMask=3`，另一眼镜像）。
   - 依据是实测的两眼关系：`dy` 恒为 0、`dx` 是水平视差，真正注视的是中点；
     两眼各发各自的值会让高质量区被视差推开。
3. **失败必须说清原因**：sink 记录 `VrssNoData`（`waiting-gaze` / `stale-gaze` /
   `source-space` / `no-geometry` / `disabled`）与发包数，经 `/api/vrss` 的 `sink`
   字段给页面显示。
4. **验证步骤不再只看 consumer**：`consumerCount > 0 && fresh` 才算通过；
   consumer 有但 `sent == 0` 判为失败（"一帧注视都没收到"）。

## 不采用的做法

| 做法 | 不用的原因 |
|---|---|
| 直接把原始点当视口坐标发 | 两眼差 0.3 屏宽，高质量区会整体偏到一侧，比"不跟随"更难排查 |
| 在 FGB 里做逐眼仿射标定 | 模块侧已经试过并因不准而撤掉；FGB 没有真值，只能编系数 |
| 由 FGB 反投影成方向再走方向路径 | 需要内参 + 畸变模型，且方向路径本身仍要回到视口坐标 |
| 保持现状（静默不发） | 用户只能看到"固定在左上角"，没有任何可操作信息 |

## 后果

- 打开实验开关后，注视位置能跟随，但**精度未标定**：靠 X/Y 增益（0.1–2.0）与 X/Y 中心偏移
  （±1 归一化单位）做一次仿射校正。四方向验证仍然是唯一验收标准。
- 该类来源的 y 原点在**下方**（实测：抬头时数值变小），与 ADR-0033 的视口约定相反，
  因此原始点走 `source_point_to_ndc`（`y = 2y-1`）；`flip_y` 只作为方向仍反时的手动开关。
- 中点法的前提是"两眼对称"。若某模块一眼偏差明显，中点会被拉偏——此时应回到模块侧
  标定，而不是继续调 FGB 的增益。
- **驱动侧会自己加一层双眼视差**：实测 provider 提交的两眼值是逐位相同的
  （`status.json` 的 `locations` 完全一致），头显里却是右眼镜区偏右、左眼镜区偏左。
  也就是 `fGazeNormalizedLocation` 不是纯粹的"逐眼视口 UV"，驱动还会按它自己的模型
  重新投影一次。因此新增 `vrss.stereo_offset`（正值内收、一正一负横移）作为补偿旋钮，
  而不是继续在输入数据上猜。这个参数不改变 ADR-0033 的坐标约定。
- `sink.reason` 成为页面判断"为什么不动"的第一手信息；后续任何新的拒收原因都要补一条
  `VrssNoData` 与对应文案，而不是让它退化成"就是不动"。
