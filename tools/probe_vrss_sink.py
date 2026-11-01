"""验证 FGB 的 VRSS 注视输出（UdpGazeV1，44 字节）。

用法：
    python tools/probe_vrss_sink.py --port 5557 --seconds 5

不需要 NVIDIA、SteamVR 或头显；只检查"该发什么、发得对不对"。
"""
import argparse
import json
import socket
import struct
import time

MAGIC = 0x3147564E
VERSION = 1
PACKET = struct.Struct("<IIQQffffI")  # 44 bytes


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=5557)
    parser.add_argument("--seconds", type=float, default=5.0)
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((args.host, args.port))
    sock.settimeout(0.5)

    count = 0
    errors = []
    last_qpc = 0
    first = None
    last = None
    deadline = time.time() + args.seconds
    while time.time() < deadline:
        try:
            data, _ = sock.recvfrom(2048)
        except socket.timeout:
            continue
        if len(data) != PACKET.size:
            errors.append(f"长度 {len(data)} != 44")
            continue
        magic, version, sequence, qpc, lx, ly, rx, ry, mask = PACKET.unpack(data)
        if magic != MAGIC:
            errors.append(f"magic=0x{magic:08x}")
        if version != VERSION:
            errors.append(f"version={version}")
        if mask != 3:
            errors.append(f"validMask={mask}")
        if qpc <= last_qpc:
            errors.append("qpc 未递增")
        last_qpc = qpc
        for name, value in (("leftX", lx), ("leftY", ly), ("rightX", rx), ("rightY", ry)):
            if not (-1.0 <= value <= 1.0):
                errors.append(f"{name}={value:.3f} 越界")
        if first is None:
            first = (sequence, lx, ly, rx, ry)
        last = (sequence, lx, ly, rx, ry)
        count += 1

    summary = {
        "packets": count,
        "rate_hz": round(count / args.seconds, 1) if args.seconds > 0 else 0,
        "first": first,
        "last": last,
        "errors": errors[:10],
        "ok": count > 0 and not errors,
    }
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 0 if summary["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
