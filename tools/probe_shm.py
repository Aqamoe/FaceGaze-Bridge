"""按消费者视角读取 FaceGaze Bridge 的共享内存输出。

用法:
    fgb-cli run --source replay --replay x.fgbrec --replay-loop --shm fgbtest
    python tools/probe_shm.py fgbtest 3

只依赖标准库：ctypes 打开命名映射，按 sdk/fgb_shared_frame.h 的布局解析。
"""

import ctypes
import sys
import time

FGB_SHM_MAGIC = 0x53424746
CHANNELS = {0x1: "eye", 0x2: "expression", 0x4: "head"}
JAW_OPEN_INDEX = 22  # Unified Expressions 顺序里的 JawOpen


class Frame(ctypes.Structure):
    _fields_ = [
        ("sequence", ctypes.c_uint64),
        ("capture_ts_ns", ctypes.c_uint64),
        ("publish_ts_ns", ctypes.c_uint64),
        ("channels", ctypes.c_uint32),
        ("attributes", ctypes.c_uint32),
        ("shape_count", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32),
        ("left_openness", ctypes.c_float),
        ("left_openness_confidence", ctypes.c_float),
        ("left_gaze_dir", ctypes.c_float * 3),
        ("left_gaze_confidence", ctypes.c_float),
        ("right_openness", ctypes.c_float),
        ("right_openness_confidence", ctypes.c_float),
        ("right_gaze_dir", ctypes.c_float * 3),
        ("right_gaze_confidence", ctypes.c_float),
        ("vergence_distance_m", ctypes.c_float),
        ("gaze_dir", ctypes.c_float * 3),
        ("gaze_pitch_deg", ctypes.c_float),
        ("gaze_yaw_deg", ctypes.c_float),
        ("gaze_confidence", ctypes.c_float),
        ("head_rotation", ctypes.c_float * 4),
        ("head_position", ctypes.c_float * 3),
        ("shapes", ctypes.c_float * 88),
        ("shape_valid", ctypes.c_uint8 * 88),
    ]


class Block(ctypes.Structure):
    _fields_ = [
        ("magic", ctypes.c_uint32),
        ("version", ctypes.c_uint32),
        ("struct_size", ctypes.c_uint32),
        ("slot_count", ctypes.c_uint32),
        ("sequence", ctypes.c_uint64),
        ("active_slot", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32),
        ("slots", Frame * 2),
    ]


def main() -> int:
    name = sys.argv[1] if len(sys.argv) > 1 else "default"
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 3.0
    mapping_name = f"Local\\FGB_Shm_{name}"

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenFileMappingW.restype = ctypes.c_void_p
    kernel32.OpenFileMappingW.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_wchar_p]
    kernel32.MapViewOfFile.restype = ctypes.c_void_p
    kernel32.MapViewOfFile.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32,
                                       ctypes.c_uint32, ctypes.c_size_t]
    kernel32.UnmapViewOfFile.argtypes = [ctypes.c_void_p]
    kernel32.CloseHandle.argtypes = [ctypes.c_void_p]

    handle = kernel32.OpenFileMappingW(0x0004, False, mapping_name)  # FILE_MAP_READ
    if not handle:
        print(f"FAIL 打不开共享内存 {mapping_name}（引擎是否带了 --shm {name}？）")
        return 1

    view = kernel32.MapViewOfFile(handle, 0x0004, 0, 0, ctypes.sizeof(Block))
    if not view:
        print("FAIL MapViewOfFile 失败")
        kernel32.CloseHandle(handle)
        return 1

    block = Block.from_address(view)
    print(f"mapping={mapping_name} magic=0x{block.magic:08X} version={block.version} "
          f"struct_size={block.struct_size} slots={block.slot_count}")
    if block.magic != FGB_SHM_MAGIC:
        print("FAIL magic 不匹配")
        kernel32.UnmapViewOfFile(view)
        kernel32.CloseHandle(handle)
        return 1

    deadline = time.time() + seconds
    last_sequence = None
    ok = True
    while time.time() < deadline:
        # 读者协议：sequence 为偶数且读前后一致
        for _ in range(100):
            seq = block.sequence
            if seq & 1:
                continue
            slot = block.active_slot
            if slot >= block.slot_count:
                continue
            frame = block.slots[slot]
            if frame.sequence == 0:
                continue
            seq2 = block.sequence
            if seq == seq2 and not (seq2 & 1):
                break
        else:
            print("FAIL 一直没有读到稳定帧")
            ok = False
            break

        if frame.sequence != last_sequence:
            last_sequence = frame.sequence
            channels = "|".join(name for bit, name in CHANNELS.items() if frame.channels & bit)
            jaw = frame.shapes[JAW_OPEN_INDEX]
            print(f"seq={frame.sequence} channels={channels} "
                  f"left_open={frame.left_openness:.3f} jawOpen={jaw:.3f} "
                  f"attr=0x{frame.attributes:X}")
        time.sleep(0.1)

    kernel32.UnmapViewOfFile(view)
    kernel32.CloseHandle(handle)
    print("PASS" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
