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
import http.server
import json
import os
import socket
import struct
import sys
import tempfile
import threading
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


class ReleaseHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        payload = json.dumps({
            "tag_name": "v9.9.9",
            "name": "Probe Release",
            "html_url": "https://github.com/Aqamoe/FaceGaze-Bridge/releases/tag/v9.9.9",
        }).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, *args):
        pass


def main():
    release_server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), ReleaseHandler)
    release_url = f"http://127.0.0.1:{release_server.server_port}/release.json"
    threading.Thread(target=release_server.serve_forever, daemon=True).start()

    status, content_type, body = http_get("/api/status")
    metrics = json.loads(body)
    print(f"HTTP /api/status -> {status} {content_type}")
    print(f"  source={metrics['source']} avatar={metrics['avatar_name']!r} "
          f"params={metrics['param_count']} targets={metrics['target_count']}")
    print(f"  filter={metrics['filter']}")
    work = metrics.get("tick_work", {})
    process = metrics.get("process", {})
    print(f"  tick work: avg={work.get('avg_us')}µs p50={work.get('p50_us')}µs "
          f"p95={work.get('p95_us')}µs max={work.get('max_us')}µs "
          f"samples={work.get('samples')}")
    print(f"  process: cpu={process.get('cpu_percent')}% "
          f"mem={process.get('working_set_mb')}MB handles={process.get('handles')} "
          f"resumes={metrics.get('resumes')}")

    status, _, body = http_get("/api/gaze")
    gaze_http = json.loads(body)
    print(f"HTTP /api/gaze -> {status}：valid={gaze_http.get('valid')} "
          f"pitch={gaze_http.get('pitch_deg')} yaw={gaze_http.get('yaw_deg')} "
          f"vergence={gaze_http.get('vergence_distance')}")

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
    gaze_msgs = 0
    last_gaze = None
    filter_enabled = None
    ack_ok = None
    diag_result = None
    diag_path = os.path.join(tempfile.gettempdir(), f"fgb-probe-diag-{os.getpid()}.zip")
    update_result = None

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
            elif kind == "gaze":
                gaze_msgs += 1
                last_gaze = message
            elif kind == "ack":
                ack_ok = message.get("ok")
                print(f"  ack: {message.get('command')} ok={ack_ok}")
            elif kind == "diagnostics":
                diag_result = message
            elif kind == "update":
                update_result = message

        if not sent_command and catalog_count and status_msgs >= 2:
            sent_command = True
            send_text(sock, json.dumps({
                "type": "set_filter", "enabled": True, "min_cutoff": 2.5, "beta": 0.8}))
            send_text(sock, json.dumps({"type": "export_diagnostics", "out": diag_path}))
            send_text(sock, json.dumps({"type": "check_updates", "url": release_url}))
            print("  已发送 set_filter / export_diagnostics / check_updates")

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
            elif kind == "gaze":
                gaze_msgs += 1
                last_gaze = message
            elif kind == "diagnostics":
                diag_result = message
            elif kind == "update":
                update_result = message
    sock.close()
    release_server.shutdown()

    print(f"收到消息类型: {seen}")
    ok = True
    if not work.get("samples") or not work.get("p95_us"):
        print("FAIL 缺少 tick 耗时分布（tick_work）")
        ok = False
    if work.get("p95_us", 0) < work.get("p50_us", 0):
        print("FAIL tick P95 小于 P50")
        ok = False
    if not process.get("working_set_mb") or not process.get("handles"):
        print("FAIL 缺少进程资源指标（process）")
        ok = False
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
    if gaze_msgs < 10:
        print(f"FAIL gaze 推送不足（3 秒只有 {gaze_msgs} 条，期望 ~90 条 @30 Hz）")
        ok = False
    if not diag_result or not diag_result.get("ok"):
        print(f"FAIL 诊断包没有导出成功：{diag_result}")
        ok = False
    else:
        try:
            with open(diag_path, "rb") as handle:
                magic = handle.read(2)
            size = os.path.getsize(diag_path)
            print(f"  诊断包：{size} 字节 magic={magic!r}")
            if magic != b"PK" or size < 256:
                print("FAIL 诊断包不是有效 zip")
                ok = False
        except OSError as exc:
            print(f"FAIL 诊断包文件读取失败：{exc}")
            ok = False
        finally:
            try:
                os.remove(diag_path)
            except OSError:
                pass
    if not update_result or not update_result.get("ok") or not update_result.get("available") \
            or update_result.get("latest") != "v9.9.9":
        print(f"FAIL 更新检查结果异常：{update_result}")
        ok = False
    else:
        print(f"  更新检查：current={update_result.get('current')} "
              f"latest={update_result.get('latest')} available={update_result.get('available')}")
    if not last_gaze or not gaze_http.get("dir_local") or len(last_gaze.get("dir_local", [])) != 3:
        print("FAIL gaze 消息缺少 dir_local")
        ok = False
    elif "fixation" not in last_gaze or "saccade_vel_deg_s" not in last_gaze:
        print("FAIL gaze 消息缺少 fixation / saccade_vel_deg_s")
        ok = False
    else:
        print(f"  gaze WS：{gaze_msgs} 条，最后 valid={last_gaze.get('valid')} "
              f"fixation={last_gaze.get('fixation')} "
              f"saccade={last_gaze.get('saccade_vel_deg_s')}°/s")
    print("PASS" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
