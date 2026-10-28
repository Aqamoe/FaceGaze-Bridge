"""抓包分析：看每个 bundle 的边界、长度、首尾地址。"""

import socket
import struct
import subprocess
import sys
import time

sys.path.insert(0, "tools")
from smoke_vmc import osc_message  # noqa: E402

CLI = "build/test/bin/Debug/fgb-cli.exe"
OSC_PORT = 9000
VMC_PORT = 39539


def split_bundle(packet):
    out = []
    if packet[:8] != b"#bundle\0":
        return [("<single>", packet)]
    pos = 16
    while pos + 4 <= len(packet):
        (length,) = struct.unpack(">i", packet[pos:pos + 4])
        if length <= 0 or pos + 4 + length > len(packet):
            out.append((f"<BAD len={length}>", b""))
            break
        body = packet[pos + 4: pos + 4 + length]
        end = body.find(b"\0")
        addr = body[:end].decode("utf-8", "replace") if end > 0 else "<nonul>"
        out.append((addr, body))
        pos += 4 + length
    return out


rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
rx.bind(("127.0.0.1", OSC_PORT))
rx.settimeout(2.5)

cli = subprocess.Popen([CLI, "run", "--duration", "5", "--vmc-port", str(VMC_PORT),
                        "--vrchat", f"127.0.0.1:{OSC_PORT}"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(1.0)

tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
batch = b"".join([
    osc_message("/VMC/Ext/Blend/Val", "jawOpen", 0.8),
    osc_message("/VMC/Ext/Blend/Apply"),
])
tx.sendto(batch, ("127.0.0.1", VMC_PORT))

for index in range(8):
    try:
        data, _ = rx.recvfrom(65535)
    except socket.timeout:
        print(f"第 {index} 包超时")
        break
    items = split_bundle(data)
    print(f"\n包 {index}: {len(data)} 字节, {len(items)} 条消息")
    for j, (addr, _) in enumerate(items[:3]):
        print(f"   [{j}] {addr}")
    if len(items) > 3:
        print(f"   ... 末条: {items[-1][0]}")

cli.terminate()
cli.wait(timeout=5)
rx.close()
tx.close()
