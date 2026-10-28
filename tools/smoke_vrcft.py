"""VRCFT 源 → 参数层 → VRChat OSC 的端到端验证。

重点不是"有没有包"，而是**合成参数有没有出现**：
JawX / BrowDown / EyeLidLeft 这些 avatar 真正绑的名字，在协议层是不存在的，
只有参数派生层工作正常才会出现在 OSC 里。

用法（需先关闭 VRCFT，模块端口是独占的）：
    python tools/smoke_vrcft.py
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


def force_utf8() -> None:
    for stream in (sys.stdout, sys.stderr):
        rc = getattr(stream, "reconfigure", None)
        if rc is not None:
            rc(encoding="utf-8", errors="replace")


def parse_addresses(packet: bytes) -> list[str]:
    out: list[str] = []
    if packet[:8] == b"#bundle\0":
        pos = 16
        while pos + 4 <= len(packet):
            (length,) = struct.unpack(">i", packet[pos:pos + 4])
            pos += 4
            if length <= 0 or pos + length > len(packet):
                break
            chunk = packet[pos:pos + length]
            end = chunk.find(b"\0")
            if end > 0:
                out.append(chunk[:end].decode("utf-8", "replace"))
            pos += length + ((4 - length % 4) % 4)
    else:
        end = packet.find(b"\0")
        if end > 0:
            out.append(packet[:end].decode("utf-8", "replace"))
    return out


def main() -> int:
    force_utf8()

    ap = argparse.ArgumentParser()
    ap.add_argument("--cli", default="build/ci/bin/Debug/fgb-cli.exe")
    ap.add_argument("--osc-port", type=int, default=9100)
    ap.add_argument("--seconds", type=float, default=10.0)
    args = ap.parse_args()

    receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        receiver.bind(("127.0.0.1", args.osc_port))
    except OSError as exc:
        print(f"[FAIL] 无法绑定 {args.osc_port}: {exc}")
        return 1
    receiver.settimeout(0.3)

    cli = subprocess.Popen(
        [args.cli, "run", "--source", "vrcft", "--vrchat", f"127.0.0.1:{args.osc_port}",
         "--duration", str(int(args.seconds) + 4), "--verbose"],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
    )

    seen: Counter[str] = Counter()
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
                seen[addr] += 1

    reader = threading.Thread(target=drain, daemon=True)
    reader.start()

    time.sleep(args.seconds)
    stop.set()
    reader.join(timeout=1.0)

    cli.terminate()
    try:
        _, err = cli.communicate(timeout=6)
    except subprocess.TimeoutExpired:
        cli.kill()
        _, err = cli.communicate()
    receiver.close()

    # 协议层形状（88 个）与合成参数（应来自参数层）
    composite = [
        "/avatar/parameters/FT/v2/JawX",            # = JawRight - JawLeft
        "/avatar/parameters/FT/v2/BrowDown",        # = (Lowerer*.75 + Pinch*.25)
        "/avatar/parameters/FT/v2/EyeLidLeft",      # = Openness*.75 + EyeWide*.25
        "/avatar/parameters/FT/v2/SmileFrownLeft",  # = smile - frown
        "/avatar/parameters/FT/v2/MouthOpen",
        "/avatar/parameters/FT/v2/TongueY",
    ]
    base = [
        "/avatar/parameters/FT/v2/JawOpen",         # 协议层基础形状
        "/avatar/parameters/FT/v2/MouthCornerPullLeft",
    ]

    print(f"收到 UDP 包: {packets}")
    print(f"不同参数地址: {len(seen)}")
    print()

    ok = True
    print("合成参数（证明参数层在工作）:")
    for addr in composite:
        hit = seen.get(addr, 0)
        if not hit:
            ok = False
        print(f"  [{'OK ' if hit else 'MISS'}] {addr}  x{hit}")

    print("基础形状（直接来自协议层）:")
    for addr in base:
        hit = seen.get(addr, 0)
        if not hit:
            ok = False
        print(f"  [{'OK ' if hit else 'MISS'}] {addr}  x{hit}")

    if err:
        lines = err.decode("utf-8", "replace").strip().splitlines()
        print("\nfgb-cli 输出尾部:")
        for line in lines[-8:]:
            print("  " + line)

    print("\n结果:", "通过" if ok else "失败")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
