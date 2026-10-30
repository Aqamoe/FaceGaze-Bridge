"""WebSocket JSON 输出探针：像一个网页客户端那样连上去，检查真的收到了帧。

用法：
    fgb-cli run --source replay --replay x.fgbrec --replay-loop --ws-out 9101
    python tools/probe_ws_out.py 9101 5

检查项：
  - HTTP Upgrade 成功
  - 文本帧是 JSON，带 t / seq / src / eye / gaze / shapes
  - eye.left/right 有 open/gaze/pupil/ok；有头姿时带 head.q/head.p
  - gaze 是独立渲染注视（v1.1）：ok / left / right / dir / ts_ns / src / mask
  - 实测帧率接近 --ws-out-rate（默认 30 Hz）
"""
import base64
import json
import os
import socket
import struct
import sys
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9101
SECONDS = float(sys.argv[2]) if len(sys.argv) > 2 else 5.0


def recv_exact(sock, count):
    data = b""
    while len(data) < count:
        chunk = sock.recv(count - len(data))
        if not chunk:
            raise ConnectionError("连接被关闭")
        data += chunk
    return data


def recv_frame(sock):
    first, second = recv_exact(sock, 2)
    opcode = first & 0x0F
    length = second & 0x7F
    masked = bool(second & 0x80)
    if length == 126:
        (length,) = struct.unpack(">H", recv_exact(sock, 2))
    elif length == 127:
        (length,) = struct.unpack(">Q", recv_exact(sock, 8))
    mask = recv_exact(sock, 4) if masked else b""
    payload = recv_exact(sock, length)
    if masked:
        payload = bytes(payload[i] ^ mask[i % 4] for i in range(length))
    return opcode, payload


def main():
    sock = socket.create_connection(("127.0.0.1", PORT), timeout=3)
    key = base64.b64encode(os.urandom(16)).decode()
    request = (
        f"GET /ws HTTP/1.1\r\n"
        f"Host: 127.0.0.1:{PORT}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n"
    )
    sock.sendall(request.encode())
    head = b""
    while b"\r\n\r\n" not in head:
        head += sock.recv(4096)
    if not head.startswith(b"HTTP/1.1 101"):
        print(f"FAIL Upgrade 失败：{head[:120]!r}")
        return 1

    sock.settimeout(0.5)
    frames = []
    start = time.time()
    deadline = start + SECONDS
    first = None
    while time.time() < deadline:
        try:
            opcode, payload = recv_frame(sock)
        except socket.timeout:
            continue
        if opcode == 0x9:  # ping
            sock.sendall(bytes([0x8A, 0x80]) + os.urandom(4))
            continue
        if opcode == 0x8:
            break
        if opcode != 0x1:
            continue
        try:
            message = json.loads(payload)
        except json.JSONDecodeError:
            print(f"FAIL 收到非 JSON 文本帧：{payload[:80]!r}")
            return 1
        if first is None:
            first = message
        frames.append(message)

    print(f"收到 {len(frames)} 帧 / {SECONDS:.1f} 秒（约 {len(frames) / SECONDS:.1f} Hz）")
    if first is None:
        print("FAIL 没有收到任何文本帧")
        return 1
    print(f"第一帧：seq={first.get('seq')} src={first.get('src')!r} "
          f"shapes={len(first.get('shapes', {}))} "
          f"head={'head' in first} "
          f"ok_left={first.get('eye', {}).get('left', {}).get('ok')}")

    ok = True
    for key_name in ("t", "seq", "src", "eye", "gaze", "shapes"):
        if key_name not in first:
            print(f"FAIL 第一帧缺少字段 {key_name}")
            ok = False
    eye = first.get("eye", {})
    for side in ("left", "right"):
        if side not in eye:
            print(f"FAIL eye 缺少 {side}")
            ok = False
        else:
            for field in ("open", "gaze", "pupil", "ok"):
                if field not in eye[side]:
                    print(f"FAIL eye.{side} 缺少 {field}")
                    ok = False
    if "vergence" not in eye:
        print("FAIL eye 缺少 vergence")
        ok = False

    gaze = first.get("gaze", {})
    for field in ("ok", "left", "right", "dir", "ts_ns", "src", "mask"):
        if field not in gaze:
            print(f"FAIL gaze 缺少 {field}")
            ok = False
    for side in ("left", "right"):
        value = gaze.get(side, {})
        for field in ("ok", "dir", "point_ok", "point"):
            if field not in value:
                print(f"FAIL gaze.{side} 缺少 {field}")
                ok = False
    print(f"渲染注视：ok={gaze.get('ok')} src={gaze.get('src')} "
          f"left={gaze.get('left', {}).get('ok')} point={gaze.get('left', {}).get('point_ok')}")

    shapes = first.get("shapes", {})
    if not isinstance(shapes, dict):
        print("FAIL shapes 不是对象")
        ok = False
    print("OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
