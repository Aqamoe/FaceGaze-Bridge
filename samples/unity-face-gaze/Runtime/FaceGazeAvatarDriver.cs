// 把 FaceGaze Bridge 的 88 个肌肉形状驱动到 Unity 头像的 blendshape。
//
// 默认按"形状名 → lowerCamelCase blendshape 名"自动绑定（VRM/ARKit 风格），
// 常见 ARKit 名称差异走 Aliases 表；对不上的用 Inspector 的 overrides 逐项指定。
// 权重单位是 Unity 的 0..100，不是引擎的 0..1。
using System;
using System.Collections.Generic;
using UnityEngine;

namespace FaceGazeBridge
{
    [AddComponentMenu("FaceGaze Bridge/Avatar Driver")]
    [DefaultExecutionOrder(1000)]
    public sealed class FaceGazeAvatarDriver : MonoBehaviour
    {
        [Header("数据源")]
        [Tooltip("与 FaceGaze Bridge 输出页的共享内存名一致（默认 default）")]
        public string sharedMemoryName = "default";
        [Tooltip("找不到映射时每隔多少秒重试")]
        public float reconnectIntervalSeconds = 2f;

        [Header("头像")]
        public SkinnedMeshRenderer target;
        [Tooltip("按形状名自动匹配 blendshape；关闭后只使用 overrides")]
        public bool autoBindByName = true;
        [Tooltip("Unity blendshape 权重是 0..100")]
        public float weightScale = 100f;
        [Range(0f, 0.1f)]
        [Tooltip("低于这个值不写权重，避免每帧无意义的 SetBlendShapeWeight")]
        public float minVisibleValue = 0.0005f;

        [Header("眼睑（可选）")]
        public bool driveBlink = true;
        public string blinkBlendShapeLeft = "eyeBlinkLeft";
        public string blinkBlendShapeRight = "eyeBlinkRight";

        [Header("头姿（可选）")]
        [Tooltip("留空则不动头；模型自身有头部骨骼时填该 Transform")]
        public Transform headTarget;
        public bool driveHeadPosition = false;

        [Serializable]
        public class ShapeOverride
        {
            [Tooltip("引擎形状名，例如 MouthCornerPullLeft")]
            public string shape;
            [Tooltip("目标 blendshape 名；留空表示不做映射")]
            public string blendShape;
            public float scale = 1f;
            public float offset;
        }

        [Tooltip("覆盖自动匹配；同名形状以这里为准")]
        public List<ShapeOverride> overrides = new List<ShapeOverride>();

        public FaceGazeFrame Latest => _frame;
        public bool Connected => _source != null && _source.IsOpen;
        public float LastFrameAgeMs { get; private set; }
        public int BoundShapeCount { get; private set; }
        public int MissingShapeCount { get; private set; }

        FaceGazeSharedMemory _source;
        FaceGazeFrame _frame;
        float _nextConnectTime;
        Mesh _boundMesh;
        readonly Binding[] _bindings = new Binding[FaceGazeShapeNames.Count];
        float[] _pendingWeights = Array.Empty<float>();
        int _blinkLeftIndex = -1;
        int _blinkRightIndex = -1;

        struct Binding
        {
            public int BlendShapeIndex;
            public float Scale;
            public float Offset;
            public bool Mapped => BlendShapeIndex >= 0;
        }

        // 引擎按肌肉层左右拆分，很多 ARKit/VRM 模型只有一个对称项。
        // 这类项多个形状会写到同一个 blendshape，取值时用"本帧最大值"。
        static readonly Dictionary<string, string> Aliases =
            new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
            {
                { "BrowInnerUpLeft", "browInnerUp" },
                { "BrowInnerUpRight", "browInnerUp" },
                { "CheekPuffLeft", "cheekPuff" },
                { "CheekPuffRight", "cheekPuff" },
                { "JawClench", "jawClench" },
                { "MouthClosed", "mouthClose" },
                { "MouthCornerPullLeft", "mouthSmileLeft" },
                { "MouthCornerPullRight", "mouthSmileRight" },
                { "MouthCornerSlantLeft", "mouthSmileLeft" },
                { "MouthCornerSlantRight", "mouthSmileRight" },
                { "MouthRaiserUpper", "mouthShrugUpper" },
                { "MouthRaiserLower", "mouthShrugLower" },
                { "LipFunnelUpperLeft", "mouthFunnel" },
                { "LipFunnelUpperRight", "mouthFunnel" },
                { "LipFunnelLowerLeft", "mouthFunnel" },
                { "LipFunnelLowerRight", "mouthFunnel" },
                { "LipPuckerUpperLeft", "mouthPucker" },
                { "LipPuckerUpperRight", "mouthPucker" },
                { "LipPuckerLowerLeft", "mouthPucker" },
                { "LipPuckerLowerRight", "mouthPucker" },
                { "LipSuckUpperLeft", "mouthRollUpper" },
                { "LipSuckUpperRight", "mouthRollUpper" },
                { "LipSuckLowerLeft", "mouthRollLower" },
                { "LipSuckLowerRight", "mouthRollLower" },
                { "TongueOut", "tongueOut" },
            };

        void Awake()
        {
            _source = new FaceGazeSharedMemory();
            for (var i = 0; i < _bindings.Length; ++i)
            {
                _bindings[i].BlendShapeIndex = -1;
            }
        }

        void Update()
        {
            if (!Connected)
            {
                if (Time.unscaledTime < _nextConnectTime)
                {
                    return;
                }
                _nextConnectTime = Time.unscaledTime + Mathf.Max(0.25f, reconnectIntervalSeconds);
                if (_source.Open(sharedMemoryName, out var error))
                {
                    Debug.Log($"[FaceGaze] 已连接 {_source.MappingName}");
                }
                else
                {
                    Debug.LogWarning($"[FaceGaze] {error}");
                }
                return;
            }

            if (!_source.TryRead(out _frame))
            {
                return;
            }
            LastFrameAgeMs = _source.LastFrameAgeMs;
            ApplyFrame();
        }

        void ApplyFrame()
        {
            if (target == null || target.sharedMesh == null)
            {
                return;
            }
            if (target.sharedMesh != _boundMesh)
            {
                Bind();
            }
            if (_boundMesh == null || _frame.Shapes == null)
            {
                return;
            }

            if (_pendingWeights.Length != target.sharedMesh.blendShapeCount)
            {
                _pendingWeights = new float[target.sharedMesh.blendShapeCount];
            }
            for (var i = 0; i < _pendingWeights.Length; ++i)
            {
                _pendingWeights[i] = -1f;
            }

            var shapes = _frame.Shapes;
            var valid = _frame.ShapeValid;
            for (var i = 0; i < _bindings.Length; ++i)
            {
                var binding = _bindings[i];
                if (!binding.Mapped || i >= valid.Length || valid[i] == 0)
                {
                    continue;
                }
                var value = shapes[i];
                if (value < minVisibleValue)
                {
                    value = 0f;
                }
                var weight = Mathf.Clamp01(value * binding.Scale + binding.Offset) * weightScale;
                // 多个形状共用一个 blendshape 时取最大值
                if (weight > _pendingWeights[binding.BlendShapeIndex])
                {
                    _pendingWeights[binding.BlendShapeIndex] = weight;
                }
            }

            if (driveBlink)
            {
                AddBlink(_blinkLeftIndex, _frame.LeftOpenness);
                AddBlink(_blinkRightIndex, _frame.RightOpenness);
            }

            for (var i = 0; i < _pendingWeights.Length; ++i)
            {
                if (_pendingWeights[i] >= 0f)
                {
                    target.SetBlendShapeWeight(i, _pendingWeights[i]);
                }
            }

            if (headTarget != null && _frame.HasHeadPose)
            {
                headTarget.localRotation = _frame.HeadRotation;
                if (driveHeadPosition)
                {
                    headTarget.localPosition = _frame.HeadPosition;
                }
            }
        }

        void AddBlink(int index, float openness)
        {
            if (index < 0 || index >= _pendingWeights.Length)
            {
                return;
            }
            var weight = Mathf.Clamp01(1f - openness) * weightScale;
            if (weight > _pendingWeights[index])
            {
                _pendingWeights[index] = weight;
            }
        }

        /// <summary>手动重新绑定（运行时换 mesh 后调用）。</summary>
        public void Bind()
        {
            if (target == null || target.sharedMesh == null)
            {
                BoundShapeCount = 0;
                MissingShapeCount = FaceGazeShapeNames.Count;
                return;
            }

            _boundMesh = target.sharedMesh;
            var mesh = _boundMesh;
            var lookup = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
            for (var i = 0; i < mesh.blendShapeCount; ++i)
            {
                var name = mesh.GetBlendShapeName(i);
                if (!string.IsNullOrEmpty(name) && !lookup.ContainsKey(name))
                {
                    lookup[name] = i;
                }
            }

            var overrideMap = new Dictionary<string, ShapeOverride>(StringComparer.OrdinalIgnoreCase);
            foreach (var entry in overrides)
            {
                if (entry != null && !string.IsNullOrEmpty(entry.shape))
                {
                    overrideMap[entry.shape] = entry;
                }
            }

            var bound = 0;
            var missing = 0;
            var missingNames = new List<string>();
            for (var i = 0; i < FaceGazeShapeNames.Count; ++i)
            {
                var shapeName = FaceGazeShapeNames.Names[i];
                var binding = new Binding { BlendShapeIndex = -1, Scale = 1f, Offset = 0f };

                if (overrideMap.TryGetValue(shapeName, out var custom))
                {
                    binding.Scale = custom.scale;
                    binding.Offset = custom.offset;
                    if (!string.IsNullOrEmpty(custom.blendShape) &&
                        lookup.TryGetValue(custom.blendShape, out var customIndex))
                    {
                        binding.BlendShapeIndex = customIndex;
                    }
                }
                else if (autoBindByName)
                {
                    var wanted = Aliases.TryGetValue(shapeName, out var alias)
                        ? alias
                        : char.ToLowerInvariant(shapeName[0]) + shapeName.Substring(1);
                    if (lookup.TryGetValue(wanted, out var index))
                    {
                        binding.BlendShapeIndex = index;
                    }
                }

                _bindings[i] = binding;
                if (binding.Mapped)
                {
                    ++bound;
                }
                else
                {
                    ++missing;
                    if (missingNames.Count < 12)
                    {
                        missingNames.Add(shapeName);
                    }
                }
            }

            BoundShapeCount = bound;
            MissingShapeCount = missing;
            _blinkLeftIndex = lookup.TryGetValue(blinkBlendShapeLeft, out var left) ? left : -1;
            _blinkRightIndex = lookup.TryGetValue(blinkBlendShapeRight, out var right) ? right : -1;

            Debug.Log($"[FaceGaze] 绑定 {bound}/{FaceGazeShapeNames.Count} 个形状" +
                      (missing > 0 ? $"，未匹配：{string.Join(", ", missingNames)}" : string.Empty));
        }

        void OnDestroy()
        {
            _source?.Dispose();
            _source = null;
        }
    }
}
