# FaceGaze Bridge · Unity 示例包

把 FaceGaze Bridge 当作 Unity 的面捕输入：引擎通过命名共享内存发布 88 个肌肉形状，
这个包里有一个 `MonoBehaviour` 读取它并驱动 `SkinnedMeshRenderer` 的 blendshape。

> 这是 **Windows 专用** 的读取端（共享内存走 `Local\FGB_Shm_*`）。Unity 版本要求 2021.3+。

## 1. 先让引擎发布共享内存

两种方式任选：

- **界面**：输出页 → 勾选「启用共享内存」→ 记下映射名（默认 `default`）→ 保存并重启引擎。
- **命令行**：

```powershell
fgb-cli run --source vrcft --shm default --vrchat 127.0.0.1:9000
```

映射名会变成 `Local\FGB_Shm_default`；名字里的非法字符会被替换成 `_`（与引擎规则一致）。

## 2. 导入 Unity

**方式 A：UPM（推荐）**

Unity → Window → Package Manager → 左上角 `+` → **Add package from disk…** →
选择本目录的 `package.json`。

**方式 B：直接拷贝**

把整个 `Runtime` 目录复制到 Unity 项目的 `Assets/FaceGazeBridge/` 下。

## 3. 挂到头像上

1. 选中头像根节点 → **Add Component** → `FaceGaze Bridge / Avatar Driver`。
2. **Shared Memory Name** 填引擎里的名字（默认 `default`）。
3. **Target** 拖入头像的 `SkinnedMeshRenderer`（只支持一个网格；多个网格请挂多个组件）。
4. 运行场景。Console 应出现：

```
[FaceGaze] 已连接 Local\FGB_Shm_default
[FaceGaze] 绑定 42/88 个形状，未匹配：NasalDilationLeft, ...
```

`BoundShapeCount` / `MissingShapeCount` 会实时显示在 Inspector 上。

## 4. 名称映射

引擎传的是**肌肉层**名字（`JawOpen`、`MouthCornerPullLeft`…），Unity 模型常见的是
ARKit/VRM 的 lowerCamelCase（`jawOpen`、`mouthSmileLeft`…）。默认规则：

1. 先查内置别名表（见 `FaceGazeAvatarDriver.Aliases`），处理左右拆分 → 对称项、
   `MouthClosed` → `mouthClose`、`LipFunnel*` → `mouthFunnel` 等常见差异；
2. 再按 `lowerCamelCase(形状名)` 精确匹配（忽略大小写）；
3. 对不上就跳过，并在 Console 列出前 12 个未匹配项。

**自定义**：在 Inspector 的 `Overrides` 里加一条：

| Shape | BlendShape | Scale | Offset |
|---|---|---|---|
| `MouthCornerPullLeft` | `mouthSmileLeft` | 1 | 0 |
| `BrowInnerUpRight` | `browInnerUp` | 0.8 | 0 |

- `BlendShape` 留空 = 明确不映射（即使自动匹配到了也不写）；
- 权重公式：`clamp01(value * scale + offset) * weightScale`，`weightScale` 默认 100。

眼睑默认用 `LeftOpenness` / `RightOpenness` 驱动 `eyeBlinkLeft` / `eyeBlinkRight`
（`1 - openness`），可在 Inspector 改名或关闭。

## 5. 可选的注视/头姿

组件暴露：

- `Latest`：最近一帧 `FaceGazeFrame`（眼方向、聚散、注视/扫视、头姿、88 形状）；
- `LastFrameAgeMs`：publish 到读取的毫秒数（看管线延迟/抖动）；
- `headTarget`：填了就把帧里的头姿写到该 Transform（`driveHeadPosition` 控制是否写位置）。

注视/扫视数据也可以直接用于 UI 或眼神朝向，组件本身不改材质。

## 6. 数据布局（排查用）

与 [`sdk/fgb_shared_frame.h`](../../sdk/fgb_shared_frame.h) 一致：

| 偏移 | 内容 |
|---|---|
| 0 / 4 / 8 / 12 | magic `0x53424746` / version 1 / struct_size 1200 / slot_count 2 |
| 16 | `sequence`（seqlock，奇数=写入中） |
| 24 | `active_slot` |
| 32 + slot×584 | 一帧：3×u64 时间戳、4×u32 头、眼/注视/头姿 float、88 形状、88 有效字节 |

读取端每次读 1200 字节，确认 sequence 前后一致且为偶数才采用；不一致就重试。
因此允许读到旧帧，但不会读到半帧。

## 7. 常见问题

| 现象 | 原因 / 处理 |
|---|---|
| `找不到共享内存 Local\FGB_Shm_xxx` | 引擎没开共享内存，或名字不一致（注意引擎会把非法字符换成 `_`） |
| 绑定数量是 0 | Target 没设、mesh 没有 blendshape，或 `autoBindByName` 关了且 overrides 为空 |
| 表情动但方向相反 | 在 Override 里用负 `scale`，或模型 blendshape 命名与 ARKit 相反 |
| 只有部分表情 | 模型没有对应 blendshape；看 Console 的未匹配列表，用 Overrides 指到自建 blendshape |
| 编辑器里连不上、打包后可以 | 编辑器默认也是 64 位 Windows，正常应能连；检查是否在非 Windows 平台 |
| 帧不更新 | 引擎没在跑、`--shm` 名字被改，或目标网格被换掉后没调用 `Bind()` |

## 8. 边界

- 包内只有**共享内存**读取路径；VMC/USB/网络路径请用各自的客户端。
- 只驱动一个 `SkinnedMeshRenderer`；多网格模型请挂多个组件（每个组件独立读共享内存）。
- 形状名表由 `tools/gen_unity_shape_names.py` 从 `src/core/model/shape.hpp` 生成，
  改形状枚举后要重新生成并同步更新 Unity 包。
- **本包尚未在 Unity 编辑器里实机验收**（当前开发机没有 Unity）。已验证的是：
  C# 通过 .NET 编译器做语法/类型检查、共享内存布局与 `test_shm` 一致。
  第一次在 Unity 里使用时请按第 3 步看 Console 的绑定日志。
