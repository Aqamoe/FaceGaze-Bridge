"""验证二进制参数的实际取值是否正确。

发送 jawOpen=0.5，检查 FT/v2/JawOpen 的 5 个二进制位（1/2/4/8/16）。
5 位 = 32 级，0.5 * 32 = 16 = 0b10000，所以只有 16 位应为 True。
"""

import socket
import struct
import subprocess
import sys
import threading
import time


def pad(b: bytes) -> bytes:
    # OSC 字符串必须至少有一个 null，再补到 4 字节对齐
    return b + b"\0" * (4 - len(b) % 4)


def msg_sf(addr: str, name: str, value: float) -> bytes:
    return pad(addr.encode()) + pad(b",sf") + pad(name.encode()) + struct.pack(">f", value)


def msg_apply() -> bytes:
    # VMC 是"提交式"协议：Blend/Val 只写缓冲，收到 Apply 才真正生效
    return pad(b"/VMC/Ext/Blend/Apply") + pad(b",")


def main() -> int:
    cli = sys.argv[1]
    recv = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    recv.bind(("127.0.0.1", 9100))
    recv.settimeout(0.2)
    send = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    proc = subprocess.Popen(
        [cli, "run", "--duration", "9", "--vmc-port", "39538",
         "--vrchat", "127.0.0.1:9100"],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
    )

    values: dict[str, object] = {}

    def drain() -> None:
        while True:
            try:
                data, _ = recv.recvfrom(65535)
            except socket.timeout:
                continue
            except OSError:
                return
            i = 0
            while True:
                i = data.find(b"/avatar/parameters/", i)
                if i < 0:
                    break
                end = data.find(b"\0", i)
                if end < 0:
                    break
                addr = data[i:end].decode()
                tstart = (end + 4) & ~3
                if data[tstart:tstart + 1] == b",":
                    tend = data.find(b"\0", tstart)
                    tag = data[tstart + 1:tend].decode()
                    body = (tend + 4) & ~3
                    if tag == "T":
                        values[addr] = True
                    elif tag == "F":
                        values[addr] = False
                    elif tag == "f":
                        values[addr] = struct.unpack(">f", data[body:body + 4])[0]
                i = end + 1

    threading.Thread(target=drain, daemon=True).start()
    time.sleep(3)

    for _ in range(60):
        send.sendto(msg_sf("/VMC/Ext/Blend/Val", "jawOpen", 0.5), ("127.0.0.1", 39538))
        send.sendto(msg_apply(), ("127.0.0.1", 39538))
        time.sleep(0.05)
    time.sleep(0.5)
    proc.wait(timeout=15)
    err = proc.stderr.read().decode("utf-8", "replace")
    print("cli 日志尾部:", err.strip().splitlines()[-4:] if err else "(空)")

    bits = {k: v for k, v in values.items() if "JawOpen" in k}
    for k in sorted(bits):
        print(f"  {k:52s} {bits[k]}")

    base = "/avatar/parameters/FT/v2/JawOpen"
    expected_true = base + "16"
    ok = True
    for shift, suffix in enumerate(["1", "2", "4", "8", "16"]):
        want = (shift == 4)
        got = bool(bits.get(base + suffix))
        if got != want:
            ok = False
            print(f"  [FAIL] {base + suffix} 期望 {want} 实际 {got}")
    print("位值校验:", "通过" if ok else "失败", f"（float 版本 = {values.get(base)}）")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
