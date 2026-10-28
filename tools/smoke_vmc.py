"""VMC -> FGB -> VRChat OSC 冒烟测试。

不依赖任何第三方库：手搓 OSC 编码，起 fgb-cli 子进程，发 VMC 数据，
再从 9000 端口收回 OSC，验证地址与数值。

用法：
    python tools/smoke_vmc.py [--cli build/test/bin/Debug/fgb-cli.exe]
"""

from __future__ import annotations

import argparse
import socket
import struct
import subprocess
import sys
import threading
import time
from collections import Counter


def osc_string(text: str) -> bytes:
    raw = text.encode("utf-8") + b"\0"
    pad = (4 - len(raw) % 4) % 4
    return raw + b"\0" * pad


def osc_message(address: str, *args) -> bytes:
    tags = ","
    payload = b""
    for arg in args:
        if isinstance(arg, str):
            tags += "s"
            payload += osc_string(arg)
        elif isinstance(arg, int):
            tags += "i"
            payload += struct.pack(">i", arg)
        elif isinstance(arg, float):
            tags += "f"
            payload += struct.pack(">f", arg)
        else:
            raise TypeError(f"不支持的参数类型: {type(arg)}")
    return osc_string(address) + osc_string(tags) + payload


def osc_bundle(*messages: bytes, timetag: int = 0) -> bytes:
    """按 OSC 规范打包多条消息。timetag=0 表示立即。"""
    out = b"#bundle\0" + struct.pack(">Q", timetag)
    for message in messages:
        out += struct.pack(">i", len(message)) + message
        out += b"\0" * ((4 - len(message) % 4) % 4)
    return out


def parse_addresses(packet: bytes) -> list[str]:
    """仅提取地址，不做完整解码——冒烟测试不需要。"""
    addresses: list[str] = []

    if packet[:8] == b"#bundle\0":
        pos = 16
        while pos + 4 <= len(packet):
            (length,) = struct.unpack(">i", packet[pos : pos + 4])
            pos += 4
            if length <= 0 or pos + length > len(packet):
                break
            chunk = packet[pos : pos + length]
            end = chunk.find(b"\0")
            if end > 0:
                addresses.append(chunk[:end].decode("utf-8", "replace"))
            pos += length + ((4 - length % 4) % 4)
    else:
        end = packet.find(b"\0")
        if end > 0:
            addresses.append(packet[:end].decode("utf-8", "replace"))

    return addresses


def force_utf8_io() -> None:
    """CI runner 的控制台编码可能是 cp1252，输出中文会抛 UnicodeEncodeError。"""
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(encoding="utf-8", errors="replace")


def main() -> int:
    force_utf8_io()

    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", default="build/test/bin/Debug/fgb-cli.exe")
    parser.add_argument("--vmc-port", type=int, default=39539)
    # 刻意不用 9000：那是 VRChat 的端口。抢占它会截走真实 VRChat/VRCFT 的
    # OSC 流量，既干扰用户，也会把别人的包混进测试结果。
    parser.add_argument("--osc-port", type=int, default=9100)
    parser.add_argument("--seconds", type=float, default=6.0)
    args = parser.parse_args()

    receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        receiver.bind(("127.0.0.1", args.osc_port))
    except OSError as exc:
        print(f"[skip] 无法绑定 {args.osc_port}：{exc}（VRChat 可能正在运行）")
        return 0
    receiver.settimeout(0.2)

    sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    cli = subprocess.Popen(
        [args.cli, "run", "--duration", str(int(args.seconds) + 2),
         "--vmc-port", str(args.vmc_port), "--vrchat", f"127.0.0.1:{args.osc_port}"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )

    received: Counter[str] = Counter()
    packets = 0
    stop = threading.Event()

    def drain() -> None:
        nonlocal packets
        while not stop.is_set():
            try:
                data, _ = receiver.recvfrom(65535)
            except socket.timeout:
                continue
            except OSError:
                break
            packets += 1
            for addr in parse_addresses(data):
                received[addr] += 1

    reader = threading.Thread(target=drain, daemon=True)
    reader.start()

    time.sleep(1.5)

    # 模拟一段说话 + 眨眼：jawOpen 起伏，眼睛左右扫视
    frames = 40
    for i in range(frames):
        t = i / frames
        jaw = 0.15 + 0.5 * abs((t * 3) % 1.0)
        yaw = 12.0 * (1 if (i // 10) % 2 == 0 else -1)

        batch = osc_bundle(
            osc_message("/VMC/Ext/Blend/Val", "jawOpen", float(jaw)),
            osc_message("/VMC/Ext/Blend/Val", "mouthSmileLeft", 0.35),
            osc_message("/VMC/Ext/Blend/Val", "browInnerUp", 0.2),
            osc_message("/VMC/Ext/Blend/Val", "eyeBlinkLeft", 0.0 if i % 20 else 1.0),
            osc_message("/VMC/Ext/Blend/Apply"),
        )
        sender.sendto(batch, ("127.0.0.1", args.vmc_port))
        time.sleep(0.033)

    time.sleep(1.0)
    stop.set()
    reader.join(timeout=1.0)

    cli.terminate()
    try:
        _, err = cli.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        cli.kill()
        _, err = cli.communicate()

    receiver.close()
    sender.close()

    print(f"收到 UDP 包: {packets}")
    print(f"不同地址数: {len(received)}")

    expect = [
        "/avatar/parameters/FT/v2/JawOpen",
        "/avatar/parameters/FT/v2/MouthCornerPullLeft",
        # browInnerUp 是双侧统一的 ARKit 输入，协议层拆成左右两个肌肉形状
        "/avatar/parameters/FT/v2/BrowInnerUpLeft",
        "/avatar/parameters/FT/v2/BrowInnerUpRight",
        "/avatar/parameters/FT/v2/EyeLidLeft",
    ]

    ok = True
    for addr in expect:
        hit = received.get(addr, 0)
        mark = "OK " if hit else "MISS"
        if not hit:
            ok = False
        print(f"  [{mark}] {addr}  x{hit}")

    if received:
        print("\n出现最多的前 8 个地址:")
        for addr, count in received.most_common(8):
            print(f"  {count:5d}  {addr}")

    if err:
        tail = err.decode("utf-8", "replace").strip().splitlines()[-6:]
        print("\nfgb-cli 输出尾部:")
        for line in tail:
            print("  " + line)

    print("\n冒烟测试", "通过" if ok else "失败")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
