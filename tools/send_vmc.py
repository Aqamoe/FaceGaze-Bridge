"""VMC 测试发送端：没有面捕硬件时，用它给引擎造一路真实输入。

发的是 VMC 标准的三件事：
  /VMC/Ext/Blend/Val (名字, 值)   —— 名字必须与 VMC 约定一致（camelCase）
  /VMC/Ext/Blend/Apply            —— 提交一帧
默认 30 Hz，`jawOpen` 在 0.9 / 0.2 之间每 30 帧切一次，界面上能看出在动。

用法：
    python tools/send_vmc.py                    # 39539 端口，15 秒
    python tools/send_vmc.py 39539 30 0.9       # 端口 / 秒数 / 峰值

配合多源验证：
    fgb-cli run --source vmc --source replay --replay x.fgbrec --replay-loop --panel 8787
    python tools/send_vmc.py
    # 状态页「输入源」卡片里，vmc 只有"表情"亮、replay 只有"眼追"亮
"""
import socket
import struct
import sys
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 39539
SECONDS = float(sys.argv[2]) if len(sys.argv) > 2 else 15.0
PEAK = float(sys.argv[3]) if len(sys.argv) > 3 else 0.9


def pad_string(text: str) -> bytes:
    """OSC 字符串：UTF-8 + 至少一个 \\0，补齐到 4 字节"""
    raw = text.encode("utf-8") + b"\x00"
    while len(raw) % 4:
        raw += b"\x00"
    return raw


def blend(address: str, name: str, value: float) -> bytes:
    # 地址 + 类型标签 ",sf"（恰好 4 字节）+ 名字串 + float
    return pad_string(address) + b",sf\x00" + pad_string(name) + struct.pack(">f", value)


def simple(address: str) -> bytes:
    return pad_string(address) + b",\x00\x00\x00"


def main() -> int:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    target = ("127.0.0.1", PORT)

    deadline = time.time() + SECONDS
    sent = 0
    while time.time() < deadline:
        value = PEAK if (sent // 30) % 2 == 0 else 0.2
        sock.sendto(blend("/VMC/Ext/Blend/Val", "jawOpen", value), target)
        sock.sendto(blend("/VMC/Ext/Blend/Val", "mouthSmileLeft", 0.6), target)
        sock.sendto(blend("/VMC/Ext/Blend/Val", "browInnerUp", 0.2), target)
        sock.sendto(simple("/VMC/Ext/Blend/Apply"), target)
        sent += 1
        time.sleep(1.0 / 30.0)

    print(f"sent {sent} frames to 127.0.0.1:{PORT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
