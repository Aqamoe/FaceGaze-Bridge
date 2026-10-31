// FaceGaze Bridge 共享内存读取端（Unity / .NET）。
//
// 布局必须与 sdk/fgb_shared_frame.h 的 FgbSharedFrameV1 / FgbSharedBlockV1 一致：
//   块头 32 字节 + 两个 584 字节槽 = 1200 字节
//   sequence 是 seqlock：奇数表示正在写，读完再比一次，变了就重试
//
// 这里没有用 unsafe 指针：1200 字节的拷贝在 120 Hz 下可以忽略，
// 换来的是不需要在 Unity 里打开 Allow Unsafe Code。
using System;
using System.Diagnostics;
using System.IO;
using System.IO.MemoryMappedFiles;

namespace FaceGazeBridge
{
    /// <summary>一帧合并后的面部数据（坐标：右手系，+Y 上，-Z 前，单位米）。</summary>
    [Serializable]
    public struct FaceGazeFrame
    {
        public ulong Sequence;
        public ulong CaptureTimestampNs;
        public ulong PublishTimestampNs;
        public uint Channels;
        public uint Attributes;

        public float LeftOpenness;
        public float LeftOpennessConfidence;
        public UnityEngine.Vector3 LeftGazeDirection;
        public float LeftGazeConfidence;

        public float RightOpenness;
        public float RightOpennessConfidence;
        public UnityEngine.Vector3 RightGazeDirection;
        public float RightGazeConfidence;

        public float VergenceDistanceMeters;

        public UnityEngine.Vector3 GazeDirection;
        public float GazePitchDegrees;
        public float GazeYawDegrees;
        public float GazeConfidence;

        public UnityEngine.Quaternion HeadRotation;
        public UnityEngine.Vector3 HeadPosition;

        /// <summary>88 个肌肉形状，0..1；顺序见 FaceGazeShapeNames。</summary>
        public float[] Shapes;
        /// <summary>每个形状本帧是否有数据。</summary>
        public byte[] ShapeValid;

        public bool HasHeadPose => (Attributes & 0x4u) != 0;
        public bool IsFixation => (Attributes & 0x8u) != 0;
        public bool LeftEyeValid => (Attributes & 0x1u) != 0;
        public bool RightEyeValid => (Attributes & 0x2u) != 0;
    }

    /// <summary>打开 Local\FGB_Shm_&lt;name&gt; 并按 seqlock 读取。</summary>
    public sealed class FaceGazeSharedMemory : IDisposable
    {
        public const uint Magic = 0x53424746u; // "FBGS"
        public const uint Version = 1u;
        public const int FrameSize = 584;
        public const int BlockSize = 1200;

        const int SequenceOffset = 16;
        const int ActiveSlotOffset = 24;
        const int SlotsOffset = 32;

        MemoryMappedFile _file;
        MemoryMappedViewAccessor _view;
        readonly byte[] _block = new byte[BlockSize];
        readonly byte[] _frameBytes = new byte[FrameSize];
        FaceGazeFrame _frame;

        public bool IsOpen => _view != null;
        public string MappingName { get; private set; } = string.Empty;
        public FaceGazeFrame Latest => _frame;
        /// <summary>publish 到这次读取的毫秒数（两端都用 QPC，可能有固定偏移，适合看抖动）。</summary>
        public float LastFrameAgeMs { get; private set; }

        public bool Open(string name, out string error)
        {
            Close();
            error = string.Empty;
            MappingName = "Local\\FGB_Shm_" + Sanitize(name);

            try
            {
                _file = MemoryMappedFile.OpenExisting(MappingName, MemoryMappedFileRights.Read);
                _view = _file.CreateViewAccessor(0, BlockSize, MemoryMappedFileAccess.Read);
            }
            catch (FileNotFoundException)
            {
                error = $"找不到共享内存 {MappingName}；先在 FaceGaze Bridge 输出页启用共享内存（或 --shm {name}）";
                Close();
                return false;
            }
            catch (Exception exception)
            {
                error = $"打开共享内存失败：{exception.Message}";
                Close();
                return false;
            }

            _view.ReadArray(0, _block, 0, BlockSize);
            var magic = BitConverter.ToUInt32(_block, 0);
            var version = BitConverter.ToUInt32(_block, 4);
            var structSize = BitConverter.ToUInt32(_block, 8);
            if (magic != Magic || version != Version || structSize != BlockSize)
            {
                error = $"共享内存版本不匹配（magic=0x{magic:X8} version={version} size={structSize}）";
                Close();
                return false;
            }
            return true;
        }

        /// <summary>读一帧。返回 false 表示没打开、正在写、或本帧被覆盖。</summary>
        public bool TryRead(out FaceGazeFrame frame)
        {
            frame = _frame;
            if (_view == null)
            {
                return false;
            }

            for (var attempt = 0; attempt < 4; ++attempt)
            {
                _view.ReadArray(0, _block, 0, BlockSize);
                var seq1 = BitConverter.ToUInt64(_block, SequenceOffset);
                if ((seq1 & 1UL) != 0UL)
                {
                    continue;
                }

                var slot = BitConverter.ToUInt32(_block, ActiveSlotOffset);
                if (slot >= 2u)
                {
                    continue;
                }

                var offset = SlotsOffset + (int)slot * FrameSize;
                Buffer.BlockCopy(_block, offset, _frameBytes, 0, FrameSize);

                var seq2 = BitConverter.ToUInt64(_block, SequenceOffset);
                if (seq1 != seq2 || (seq2 & 1UL) != 0UL)
                {
                    continue;
                }

                Parse(_frameBytes);
                var now = NowNs();
                LastFrameAgeMs = now > _frame.PublishTimestampNs
                    ? (now - _frame.PublishTimestampNs) / 1_000_000f
                    : 0f;
                frame = _frame;
                return true;
            }
            return false;
        }

        void Parse(byte[] data)
        {
            _frame.Sequence = BitConverter.ToUInt64(data, 0);
            _frame.CaptureTimestampNs = BitConverter.ToUInt64(data, 8);
            _frame.PublishTimestampNs = BitConverter.ToUInt64(data, 16);
            _frame.Channels = BitConverter.ToUInt32(data, 24);
            _frame.Attributes = BitConverter.ToUInt32(data, 28);

            _frame.LeftOpenness = BitConverter.ToSingle(data, 40);
            _frame.LeftOpennessConfidence = BitConverter.ToSingle(data, 44);
            _frame.LeftGazeDirection = ReadVector3(data, 48);
            _frame.LeftGazeConfidence = BitConverter.ToSingle(data, 60);

            _frame.RightOpenness = BitConverter.ToSingle(data, 64);
            _frame.RightOpennessConfidence = BitConverter.ToSingle(data, 68);
            _frame.RightGazeDirection = ReadVector3(data, 72);
            _frame.RightGazeConfidence = BitConverter.ToSingle(data, 84);

            _frame.VergenceDistanceMeters = BitConverter.ToSingle(data, 88);

            _frame.GazeDirection = ReadVector3(data, 92);
            _frame.GazePitchDegrees = BitConverter.ToSingle(data, 104);
            _frame.GazeYawDegrees = BitConverter.ToSingle(data, 108);
            _frame.GazeConfidence = BitConverter.ToSingle(data, 112);

            _frame.HeadRotation = new UnityEngine.Quaternion(
                BitConverter.ToSingle(data, 116),
                BitConverter.ToSingle(data, 120),
                BitConverter.ToSingle(data, 124),
                BitConverter.ToSingle(data, 128));
            _frame.HeadPosition = ReadVector3(data, 132);

            // shapes 在 144，shape_valid 在 496（88 个 float + 88 个字节）
            if (_frame.Shapes == null)
            {
                _frame.Shapes = new float[FaceGazeShapeNames.Count];
                _frame.ShapeValid = new byte[FaceGazeShapeNames.Count];
            }
            Buffer.BlockCopy(data, 144, _frame.Shapes, 0, FaceGazeShapeNames.Count * sizeof(float));
            Buffer.BlockCopy(data, 496, _frame.ShapeValid, 0, FaceGazeShapeNames.Count);
        }

        static UnityEngine.Vector3 ReadVector3(byte[] data, int offset)
        {
            return new UnityEngine.Vector3(
                BitConverter.ToSingle(data, offset),
                BitConverter.ToSingle(data, offset + 4),
                BitConverter.ToSingle(data, offset + 8));
        }

        static ulong NowNs()
        {
            return (ulong)(Stopwatch.GetTimestamp() * (1_000_000_000.0 / Stopwatch.Frequency));
        }

        // 与 C++ ShmSink::sanitize 保持一致，否则用户填的名字和引擎映射名对不上
        static string Sanitize(string name)
        {
            if (string.IsNullOrEmpty(name))
            {
                return "default";
            }
            var chars = name.ToCharArray();
            for (var i = 0; i < chars.Length; ++i)
            {
                var c = chars[i];
                var ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                         c == '.' || c == '-' || c == '_';
                if (!ok)
                {
                    chars[i] = '_';
                }
            }
            return new string(chars);
        }

        public void Close()
        {
            _view?.Dispose();
            _view = null;
            _file?.Dispose();
            _file = null;
        }

        public void Dispose()
        {
            Close();
        }
    }
}
