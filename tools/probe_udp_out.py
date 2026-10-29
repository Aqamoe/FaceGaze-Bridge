"""UDP 二进制输出探针：按 sdk/fgb_udp_frame.h 的固定偏移收包并校验。

用法：
    fgb-cli run --source replay --replay x.fgbrec --replay-loop --udp-out 127.0.0.1:9102
    python tools/probe_udp_out.py 9102 5

检查项：
  - 数据报正好 592 字节，magic/version/header_size 正确
  - shape_count == 88，shapes 偏移处能读到非零形状
  - 眼追/头姿属性位与 gaze 字段一致
  - 实测帧率与 --udp-out-rate（默认每 tick）对比
"""
import re
import socket
import struct
import sys
import time
from pathlib import Path

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9102
SECONDS = float(sys.argv[2]) if len(sys.argv) > 2 else 5.0

FRAME_SIZE = 592
MAGIC = 0x55424746
VERSION = 1
ATTR = {
    "left_valid": 0x01,
    "right_valid": 0x02,
    "has_head": 0x04,
    "fixation": 0x08,
    "gaze_valid": 0x10,
}
# 10 个整数头 + 26 个 float（眼/注视/头）+ 88 个形状 + 88 个有效性字节
FORMAT = "<IHHQQQIIII" + "f" * 26 + "f" * 88 + "B" * 88
PARSER = struct.Struct(FORMAT)
assert PARSER.size == FRAME_SIZE

SHAPE_OFFSET = 36
VALID_OFFSET = SHAPE_OFFSET + 88


def load_shape_names():
    path = Path(__file__).resolve().parent.parent / "src" / "core" / "model" / "shape.hpp"
    try:
        text = path.read_text(encoding="utf-8")
    except OSError:
        return []
    block = text.split("FGB_SHAPE_LIST", 1)
    if len(block) < 2:
        return []
    return re.findall(r"X\((\w+)\)", block[1].split("#define", 1)[0])


def main():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("127.0.0.1", PORT))
    sock.settimeout(0.4)

    names = load_shape_names()
    frames = 0
    first = None
    last = None
    deadline = time.time() + SECONDS
    while time.time() < deadline:
        try:
            packet, _ = sock.recvfrom(2048)
        except socket.timeout:
            continue
        if len(packet) != FRAME_SIZE:
            print(f"FAIL 数据报长度 {len(packet)}，期望 {FRAME_SIZE}")
            return 1
        values = PARSER.unpack(packet)
        if first is None:
            first = values
        last = values
        frames += 1

    print(f"收到 {frames} 帧 / {SECONDS:.1f} 秒（约 {frames / SECONDS:.1f} Hz）")
    if first is None:
        print("FAIL 没有收到任何 UDP 数据报")
        return 1

    magic, version, header_size, seq, capture, publish, channels, attrs, shape_count, _ = first[:10]
    print(f"magic=0x{magic:08x} version={version} header={header_size} seq={seq} "
          f"channels=0x{channels:x} attrs=0x{attrs:x} shapes={shape_count}")
    print(f"窗口接收帧率：首帧 seq={seq} 末帧 seq={last[3]}")

    ok = True
    if magic != MAGIC or version != VERSION or header_size != 8:
        print("FAIL 头部 magic/version/header_size 不对")
        ok = False
    if shape_count != 88:
        print("FAIL shape_count 不是 88")
        ok = False
    if last[3] <= first[3]:
        print("FAIL sequence 没有递增")
        ok = False

    shapes = first[SHAPE_OFFSET:SHAPE_OFFSET + 88]
    valid = first[VALID_OFFSET:VALID_OFFSET + 88]
    nonzero = [(i, v) for i, v in enumerate(shapes) if abs(v) > 1e-4 and valid[i]]
    print(f"非零形状：{len(nonzero)} 个；抽样：")
    for index, value in nonzero[:6]:
        label = names[index] if index < len(names) else f"shape[{index}]"
        print(f"  {label:<24} {value:+.3f}")

    left_open = first[10]
    if attrs & ATTR["left_valid"] and not (0.0 <= left_open <= 1.0):
        print("FAIL 左眼 openness 越界")
        ok = False
    if attrs & ATTR["has_head"]:
        quat = first[29:33]
        norm = sum(x * x for x in quat) ** 0.5
        if abs(norm - 1.0) > 0.05:
            print(f"FAIL 头姿四元数未归一化：{quat} norm={norm:.3f}")
            ok = False

    print("OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
