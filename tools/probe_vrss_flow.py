"""驱动 VRSS 一键流程并断言步骤门控（不需要 NVIDIA / SteamVR）。

用法：
    python tools/probe_vrss_flow.py 8799 --command vrss_run --seconds 20

脚本只发命令与读推送；真正的硬件动作由引擎侧状态机执行。
无 SteamVR 时预期第 1 步失败、后续步骤保持 pending。
"""
import argparse
import json
import os
import socket
import struct
import sys
import time


def handshake(sock: socket.socket) -> bytes:
    key = os.urandom(16).hex()
    request = (
        "GET /ws HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n"
    )
    sock.sendall(request.encode())
    response = b""
    while b"\r\n\r\n" not in response:
        chunk = sock.recv(4096)
        if not chunk:
            raise RuntimeError("WebSocket 握手失败")
        response += chunk
    if b"101" not in response.split(b"\r\n")[0]:
        raise RuntimeError(response.decode(errors="replace"))
    return response


def send_text(sock: socket.socket, text: str) -> None:
    payload = text.encode()
    header = b"\x81"
    length = len(payload)
    if length < 126:
        header += struct.pack("!B", 0x80 | length)
    elif length < 65536:
        header += struct.pack("!BH", 0x80 | 126, length)
    else:
        header += struct.pack("!BQ", 0x80 | 127, length)
    mask = os.urandom(4)
    body = bytes(byte ^ mask[i % 4] for i, byte in enumerate(payload))
    sock.sendall(header + mask + body)


def decode(buffer: bytes):
    if len(buffer) < 2:
        return None
    opcode = buffer[0] & 0x0F
    length = buffer[1] & 0x7F
    offset = 2
    if length == 126:
        length = struct.unpack("!H", buffer[2:4])[0]
        offset = 4
    elif length == 127:
        length = struct.unpack("!Q", buffer[2:10])[0]
        offset = 10
    if len(buffer) < offset + length:
        return None
    return opcode, buffer[offset:offset + length], offset + length


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("port", type=int, nargs="?", default=8787)
    parser.add_argument("--command", default="vrss_run")
    parser.add_argument("--payload", default="")
    parser.add_argument("--seconds", type=float, default=20.0)
    args = parser.parse_args()

    sock = socket.create_connection(("127.0.0.1", args.port), timeout=5)
    handshake(sock)
    if args.payload:
        send_text(sock, args.payload)
        print(f"发送 payload：{args.payload}")
    else:
        send_text(sock, json.dumps({"type": args.command}))
        print(f"发送命令：{args.command}")

    deadline = time.time() + args.seconds
    last_steps = None
    ack_seen = False
    sock.settimeout(0.5)
    buffer = b""
    while time.time() < deadline:
        try:
            chunk = sock.recv(8192)
        except socket.timeout:
            continue
        if not chunk:
            break
        buffer += chunk
        while buffer:
            frame = decode(buffer)
            if frame is None:
                break
            opcode, payload, used = frame
            buffer = buffer[used:]
            if opcode != 0x1:
                continue
            try:
                message = json.loads(payload)
            except (json.JSONDecodeError, UnicodeDecodeError):
                continue
            if message.get("type") == "ack":
                print(f"ACK {message.get('command')} ok={message.get('ok')} "
                      f"error={message.get('error', '')} message={message.get('message', '')}")
                if message.get("command") == args.command:
                    ack_seen = True
            if message.get("type") == "vrss":
                steps = message.get("steps") or []
                signature = tuple((step.get("id"), step.get("state"), step.get("detail")) for step in steps)
                if signature != last_steps:
                    print(f"state={message.get('state')} enabled={message.get('enabled')}")
                    for step in steps:
                        print(f"  {step.get('id'):<12} {step.get('state'):<8} {step.get('detail', '')}")
                    last_steps = signature
    sock.close()

    if last_steps is None:
        print("FAILED 没有收到 vrss 推送")
        return 1
    if not ack_seen:
        print("FAILED 没有收到命令回执")
        return 1
    if last_steps[0][1] != "fail":
        print("注意：第 1 步不是 fail（有 SteamVR 时可能继续往下走）")
    if any(step[1] == "pending" for step in last_steps[1:]):
        print("PASS 步骤门控有效：第 1 步失败后后续保持 pending")
        return 0
    print("FAILED 后续步骤没有被门控")
    return 1


if __name__ == "__main__":
    sys.exit(main())
