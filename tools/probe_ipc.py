"""控制面 IPC 的端到端探针。

验证的是"UI 与引擎之间那条线"本身，不涉及浏览器：
  1. HTTP 诊断接口（curl 能拿到的同一份 JSON）
  2. WebSocket 握手（Sec-WebSocket-Accept）
  3. 推送：hello / catalog / status / params 的节奏与字段一致性
  4. 上行：set_filter 的回执，以及状态里滤波开关是否真的变了

用法：
    fgb-cli run --source replay --replay x.fgbrec --panel 8787
    python tools/probe_ipc.py 8787

不依赖第三方库：握手与帧编解码都是手写的，正好和 C++ 侧互为对照——
两边各写一遍，协议理解错的那一方会立刻暴露。
"""
import base64
import json
import os
import socket
import struct
import sys
import time
import urllib.request

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8787


def http_get(path):
    with urllib.request.urlopen(f"http://127.0.0.1:{PORT}{path}", timeout=5) as response:
        return response.status, response.headers.get("Content-Type"), response.read()


def handshake(sock):
    key = base64.b64encode(os.urandom(16)).decode()
    request = (
        "GET /ws HTTP/1.1\r\n"
        f"Host: 127.0.0.1:{PORT}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n"
    )
    sock.sendall(request.encode())
    data = b""
    deadline = time.time() + 5
    while b"\r\n\r\n" not in data:
        if time.time() > deadline:
            raise RuntimeError("握手超时")
        chunk = sock.recv(4096)
        if not chunk:
            raise RuntimeError("握手时对端关闭")
        data += chunk
    head, rest = data.split(b"\r\n\r\n", 1)
    if not head.startswith(b"HTTP/1.1 101"):
        raise RuntimeError(f"握手失败：{head[:120]!r}")
    return rest


def decode(buffer):
    if len(buffer) < 2:
        return None
    opcode = buffer[0] & 0x0F
    masked = buffer[1] & 0x80
    length = buffer[1] & 0x7F
    offset = 2
    if length == 126:
        if len(buffer) < 4:
            return None
        length = struct.unpack(">H", buffer[2:4])[0]
        offset = 4
    elif length == 127:
        if len(buffer) < 10:
            return None
        length = struct.unpack(">Q", buffer[2:10])[0]
        offset = 10
    mask = b""
    if masked:
        if len(buffer) < offset + 4:
            return None
        mask = buffer[offset:offset + 4]
        offset += 4
    if len(buffer) < offset + length:
        return None
    payload = bytearray(buffer[offset:offset + length])
    if masked:
        for i in range(len(payload)):
            payload[i] ^= mask[i % 4]
    return opcode, bytes(payload), offset + length


def send_text(sock, text):
    data = text.encode()
    header = bytes([0x81])
    size = len(data)
    if size < 126:
        header += bytes([0x80 | size])
    elif size <= 0xFFFF:
        header += bytes([0x80 | 126]) + struct.pack(">H", size)
    else:
        header += bytes([0x80 | 127]) + struct.pack(">Q", size)
    mask = os.urandom(4)
    header += mask
    sock.sendall(header + bytes(byte ^ mask[i % 4] for i, byte in enumerate(data)))


def main():
    status, content_type, body = http_get("/api/status")
    metrics = json.loads(body)
    print(f"HTTP /api/status -> {status} {content_type}")
    print(f"  source={metrics['source']} avatar={metrics['avatar_name']!r} "
          f"params={metrics['param_count']} targets={metrics['target_count']}")
    print(f"  filter={metrics['filter']}")

    status, _, page = http_get("/")
    print(f"HTTP / -> {status}，{len(page)} 字节，引用 app.js: {b'app.js' in page}")
    status, _, script = http_get("/app.js")
    print(f"HTTP /app.js -> {status}，{len(script)} 字节")

    sock = socket.create_connection(("127.0.0.1", PORT), timeout=5)
    buffer = handshake(sock)
    print("WS 握手 -> 101")

    seen = {}
    catalog_count = 0
    params_len = 0
    status_msgs = 0
    filter_enabled = None
    ack_ok = None

    deadline = time.time() + 3.0
    sent_command = False
    while time.time() < deadline:
        sock.settimeout(0.3)
        try:
            chunk = sock.recv(65536)
            if not chunk:
                break
            buffer += chunk
        except socket.timeout:
            pass

        while True:
            frame = decode(buffer)
            if frame is None:
                break
            opcode, payload, consumed = frame
            buffer = buffer[consumed:]
            if opcode != 0x1:
                continue
            message = json.loads(payload)
            kind = message.get("type")
            seen[kind] = seen.get(kind, 0) + 1
            if kind == "hello":
                print(f"  hello: protocol={message['protocol']} version={message['version']} "
                      f"port={message['port']}")
            elif kind == "catalog":
                catalog_count = message["count"]
                sent = sum(1 for flag in message["sent"] if flag)
                print(f"  catalog: {catalog_count} 个参数名，{sent} 个已绑定")
            elif kind == "status":
                status_msgs += 1
                filter_enabled = message["filter"]["enabled"]
            elif kind == "params":
                params_len = len(message["values"])
            elif kind == "ack":
                ack_ok = message.get("ok")
                print(f"  ack: {message.get('command')} ok={ack_ok}")

        if not sent_command and catalog_count and status_msgs >= 2:
            sent_command = True
            send_text(sock, json.dumps({
                "type": "set_filter", "enabled": True, "min_cutoff": 2.5, "beta": 0.8}))
            print("  已发送 set_filter")

    # 再等一小会儿，收 ack 与更新后的 status
    deadline = time.time() + 1.0
    while time.time() < deadline:
        sock.settimeout(0.3)
        try:
            chunk = sock.recv(65536)
            if not chunk:
                break
            buffer += chunk
        except socket.timeout:
            pass
        while True:
            frame = decode(buffer)
            if frame is None:
                break
            opcode, payload, consumed = frame
            buffer = buffer[consumed:]
            if opcode != 0x1:
                continue
            message = json.loads(payload)
            kind = message.get("type")
            seen[kind] = seen.get(kind, 0) + 1
            if kind == "ack":
                ack_ok = message.get("ok")
            elif kind == "status":
                filter_enabled = message["filter"]["enabled"]
    sock.close()

    print(f"收到消息类型: {seen}")
    ok = True
    if catalog_count == 0 or params_len != catalog_count:
        print(f"FAIL 参数快照长度不匹配（catalog={catalog_count}, values={params_len}）")
        ok = False
    if status_msgs < 2:
        print("FAIL status 推送不足（10 Hz 下 3 秒应有约 30 条）")
        ok = False
    if ack_ok is not True:
        print("FAIL 没收到 set_filter 的成功回执")
        ok = False
    if filter_enabled is not True:
        print("FAIL 滤波开关没有生效")
        ok = False
    print("PASS" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
