"""VRCFT 模块 IPC 协议探测器（M0 spike）。

自己做 server 端，启动官方的 VRCFaceTracking.ModuleProcess.exe，走完
握手 / 能力查询 / 卸载，验证协议格式是否与源码一致。

安全边界：**只发 EventGetSupported 和 EventTeardown，绝不发 EventInit**。
模块不 Initialize 就不会碰硬件、不会抢 PICO Connect 的端口，因此对正在
运行的 VRCFT 无影响。

用法：
    python tools/ipc_probe.py
    python tools/ipc_probe.py --module-path <dll> --timeout 8
"""

from __future__ import annotations

import argparse
import os
import socket
import struct
import subprocess
import sys
import time

MAGIC = b"\xaf\xec\x00\x8d"
CHALLENGE = bytes([0xDE, 0xC1, 0xCA, 0x12, 0xD0])

PACKET_TYPES = {
    0: "Unknown",
    1: "Handshake",
    2: "SplitPacketChunk",
    100: "MetadataUpdate",
    101: "LipUpdate",
    102: "EyeUpdate",
    200: "EventGetSupported",
    201: "EventInit",
    202: "EventTeardown",
    203: "EventUpdate",
    204: "EventUpdateStatus",
    205: "EventLog",
    300: "ReplyGetSupported",
    301: "ReplyInit",
    302: "ReplyUpdate",
    303: "ReplyTeardown",
    1000: "DebugStreamFrame",
}

DEFAULT_INSTALL = r"C:\Program Files (x86)\Steam\steamapps\common\VRCFaceTracking"
DEFAULT_MODULE_DIR = os.path.join(
    os.environ.get("APPDATA", ""), "VRCFaceTracking", "CustomLibs",
    "f3df57d5-1c5b-887d-abb0-be555e14bf09", "VRCFTPicoModule.dll",
)


def build(ptype: int, body: bytes = b"") -> bytes:
    return MAGIC + struct.pack("<I", ptype) + body


def parse(data: bytes):
    if len(data) < 8 or data[:4] != MAGIC:
        return None, None
    return struct.unpack("<I", data[4:8])[0], data[8:]


def force_utf8_io() -> None:
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(encoding="utf-8", errors="replace")


def main() -> int:
    force_utf8_io()

    ap = argparse.ArgumentParser()
    ap.add_argument("--install-dir", default=DEFAULT_INSTALL)
    ap.add_argument("--module-path", default=DEFAULT_MODULE_DIR)
    ap.add_argument("--timeout", type=float, default=6.0)
    ap.add_argument("--init", action="store_true",
                    help="发送 EventInit 并收集数据包。注意：模块会绑定硬件端口，"
                         "与正在运行的 VRCFT 冲突，需先关闭它。")
    ap.add_argument("--init-seconds", type=float, default=3.0, help="Init 后收集时长")
    args = ap.parse_args()

    exe = os.path.join(args.install_dir, "VRCFaceTracking.ModuleProcess.exe")
    if not os.path.isfile(exe):
        print(f"[FAIL] 找不到 {exe}")
        return 1
    if not os.path.isfile(args.module_path):
        print(f"[FAIL] 找不到模块 {args.module_path}")
        return 1

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.settimeout(0.2)  # 短超时，便于在收包间隙检查时序
    print(f"[INFO] 本端监听 127.0.0.1:{port}")

    log_path = os.path.join(os.path.dirname(__file__), "..", ".workbuddy", "tmp", "moduleprocess.log")
    os.makedirs(os.path.dirname(log_path), exist_ok=True)
    log = open(log_path, "wb")

    cmd = [exe, "--port", str(port), "--module-path", args.module_path, "--parent-pid", str(os.getpid())]
    print(f"[INFO] 启动: {os.path.basename(exe)} --port {port} --module-path <dll>")
    proc = subprocess.Popen(cmd, cwd=args.install_dir, stdout=log, stderr=log, stdin=subprocess.DEVNULL)

    peer = None
    handshake_ok = False
    supported = None
    got_reply_update = 0
    init_sent_at = None
    teardown_sent = False
    deadline = time.time() + args.timeout

    def send(data: bytes, label: str) -> None:
        if peer is None:
            return
        sock.sendto(data, peer)
        print(f"[SEND] {label}  ({len(data)} 字节)")

    try:
        while time.time() < deadline:
            try:
                data, addr = sock.recvfrom(65535)
            except socket.timeout:
                # 收包间隙处理时序：Init 后等够时间再 Teardown
                if args.init and init_sent_at and not teardown_sent:
                    if time.time() - init_sent_at >= args.init_seconds:
                        send(build(202), "EventTeardown")
                        teardown_sent = True
                        deadline = time.time() + 2.0
                continue

            ptype, body = parse(data)
            if ptype is None:
                print(f"[WARN] 非 VRCFT 包，{len(data)} 字节: {data[:16].hex(' ')}")
                continue

            name = PACKET_TYPES.get(ptype, f"Unknown({ptype})")
            peer = addr
            print(f"[RECV] {name:22s} {len(data):4d} 字节  {data[:40].hex(' ')}")

            if ptype == 1 and len(data) >= 21:
                challenge = data[8:13]
                pid = struct.unpack("<i", data[13:17])[0]
                path_len = struct.unpack("<i", data[17:21])[0]
                module_path = data[21:21 + path_len].decode("utf-8", "replace")
                print(f"        challenge={challenge.hex(' ')}  匹配={challenge == CHALLENGE}")
                print(f"        pid={pid}  module={os.path.basename(module_path)}")
                if challenge == CHALLENGE:
                    handshake_ok = True
                    send(data, "Handshake ACK")
                    send(build(200), "EventGetSupported")

            elif ptype == 300 and body:
                flags = body[0]
                supported = (bool(flags & 1), bool(flags & 2))
                print(f"        eye={supported[0]}  expression={supported[1]}")
                if args.init:
                    packed = (1 if supported[0] else 0) | (2 if supported[1] else 0)
                    send(build(201, bytes([packed])), "EventInit")
                    init_sent_at = time.time()
                    deadline = time.time() + args.init_seconds + 3.0
                else:
                    # 只握手不初始化：绝不发 Init，避免碰硬件
                    send(build(202), "EventTeardown")

            elif ptype == 301 and len(body) >= 5:
                flags = body[0]
                name_len = struct.unpack("<i", body[1:5])[0]
                name_text = body[5:5 + name_len].decode("utf-8", "replace")
                streams = struct.unpack("<i", body[5 + name_len:9 + name_len])[0] if len(body) >= 9 + name_len else 0
                print(f"        eyeSuccess={bool(flags & 1)} expressionSuccess={bool(flags & 2)}")
                print(f"        模块名={name_text!r}  图标流={streams}")
                # 数据是拉模式：每发一个 EventUpdate，模块才回一个 ReplyUpdatePacket
                if args.init and peer and not teardown_sent:
                    sock.sendto(build(203), peer)
                    print("[SEND] EventUpdate  （开始拉取数据）")

            elif ptype == 302 and len(body) >= 4:
                struct_size = struct.unpack("<i", body[0:4])[0]
                expected = (16 + 89) * 4
                if got_reply_update == 0:
                    print(f"        structSize={struct_size} (预期 {expected}) "
                          f"{'✓' if struct_size == expected else '✗ 不一致！'}")
                    if struct_size == expected:
                        vals = struct.unpack("<16f", body[4:4 + 64])
                        labels = ["MaxDilation", "MinDilation",
                                  "L_GazeX", "L_GazeY", "L_Pupil", "L_Openness",
                                  "R_GazeX", "R_GazeY", "R_Pupil", "R_Openness",
                                  "Head_Yaw", "Head_Pitch", "Head_Roll",
                                  "Head_PosX", "Head_PosY", "Head_PosZ"]
                        print("        " + "  ".join(f"{k}={v:.3f}" for k, v in zip(labels, vals)))
                        shapes = struct.unpack("<89f", body[68:68 + 356])
                        nonzero = [(i, v) for i, v in enumerate(shapes) if abs(v) > 1e-4]
                        print(f"        非零形状 {len(nonzero)} 个: {nonzero[:6]}")
                got_reply_update += 1
                # 继续拉下一帧（限时），达到 60 Hz 左右的往返节奏
                if args.init and peer and not teardown_sent:
                    if time.time() - init_sent_at < args.init_seconds:
                        sock.sendto(build(203), peer)

            elif ptype == 303:
                print("        teardown 已确认")

            elif ptype == 205 and len(body) >= 8:
                # EventLog: level(int32) + length(int32) + text
                level, text_len = struct.unpack("<ii", body[0:8])
                text = body[8:8 + text_len].decode("utf-8", "replace")
                if text and level >= 3:
                    print(f"        log[{level}]: {text.strip()[:110]}")

    finally:
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            print("[INFO] 超时，已终止 ModuleProcess")
        finally:
            log.close()
            sock.close()

    print()
    print("=" * 60)
    print(f"握手成功      : {handshake_ok}")
    print(f"模块能力      : {supported}")
    print(f"收到数据包数  : {got_reply_update}")
    print(f"进程退出码    : {proc.returncode}")
    print(f"进程日志      : {log_path}")
    print("=" * 60)

    return 0 if handshake_ok else 1


if __name__ == "__main__":
    sys.exit(main())
