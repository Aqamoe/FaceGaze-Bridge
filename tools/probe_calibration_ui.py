"""校准页的离线渲染验收（不需要引擎、不需要硬件）。

起一个同端口的 HTTP + WebSocket mock：静态文件来自 ui/，/ws 上推一份合成的
calibration 消息，然后用 Edge headless 打开页面、切到校准页、截图并断言关键结构。

比"看截图"可靠：88 行、176 个拇指、中文名命中、曲线浮层展开都是可断言的。
它不替代 fgb-ui + WebView2 的真机验收，只保证"改了 DOM 之后页面还能渲染"。

用法：
    python tools/probe_calibration_ui.py [输出png]

依赖：Windows 自带的 Edge（headless），不装任何第三方包。
"""
import base64
import hashlib
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "ui"))
PORT = 8791
CDP_PORT = 9333
GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

PRESETS = ["linear", "ease_in", "ease_out", "smooth", "step"]


def build_calibration():
    """构造一份"每十一条里有一条用自定义曲线"的档案，覆盖所有分支。"""
    with open(os.path.join(ROOT, "shapes.json"), encoding="utf-8") as handle:
        table = json.load(handle)
    shapes = []
    for index, name in enumerate(table["shapes"]):
        shapes.append({
            "name": name,
            "lo": round(0.05 + (index % 5) * 0.03, 4),
            "hi": round(0.72 + (index % 4) * 0.06, 4),
            "active": index % 3 != 0,
            "curve": "custom:0" if index % 11 == 0 else PRESETS[index % 5],
            "curve_param": 0.5,
        })
    return {
        "type": "calibration",
        "available": True,
        "path": "C:/probe/calibration.ini",
        "default_path": "C:/probe/calibration.ini",
        "dirty": True,
        "loaded": True,
        "active": sum(1 for shape in shapes if shape["active"]),
        "shapes": shapes,
        "curves": ["0.000,0.000 0.280,0.120 0.700,0.880 1.000,1.000"] + [""] * 7,
        "eye": {"active": True, "gain_x": 1.2, "gain_y": 1.0, "center_x": 0.4, "center_y": -1.2},
    }


CALIBRATION = build_calibration()
PREVIEW = {"type": "preview", "seq": 1, "raw": [0.42] * 88, "out": [0.61] * 88,
           "eye": {"valid": True, "x": 2.4, "y": -1.1}}


def ws_frame(payload, opcode=0x1):
    data = payload.encode("utf-8")
    header = bytes([0x80 | opcode])
    length = len(data)
    if length < 126:
        header += bytes([length])
    elif length < 65536:
        header += bytes([126]) + struct.pack(">H", length)
    else:
        header += bytes([127]) + struct.pack(">Q", length)
    return header + data


def read_frame(sock):
    head = sock.recv(2)
    if len(head) < 2:
        raise ConnectionError("closed")
    length = head[1] & 0x7F
    if length == 126:
        length = struct.unpack(">H", sock.recv(2))[0]
    elif length == 127:
        length = struct.unpack(">Q", sock.recv(8))[0]
    mask = sock.recv(4) if head[1] & 0x80 else b""
    payload = b""
    while len(payload) < length:
        chunk = sock.recv(length - len(payload))
        if not chunk:
            raise ConnectionError("closed")
        payload += chunk
    if mask:
        payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    return payload.decode("utf-8", "replace")


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def do_GET(self):
        if self.headers.get("Upgrade", "").lower() == "websocket":
            key = self.headers.get("Sec-WebSocket-Key", "")
            accept = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
            self.wfile.write(
                b"HTTP/1.1 101 Switching Protocols\r\n"
                b"Upgrade: websocket\r\nConnection: Upgrade\r\n"
                b"Sec-WebSocket-Accept: " + accept.encode() + b"\r\n\r\n")
            self.wfile.flush()
            self.ws_session()
            return

        path = self.path.split("?")[0]
        if path in ("/", ""):
            path = "/index.html"
        target = os.path.join(ROOT, path.lstrip("/").replace("/", os.sep))
        if not os.path.isfile(target):
            self.send_error(404)
            return
        with open(target, "rb") as handle:
            body = handle.read()
        types = {
            ".html": "text/html; charset=utf-8",
            ".js": "application/javascript; charset=utf-8",
            ".css": "text/css; charset=utf-8",
            ".json": "application/json; charset=utf-8",
            ".png": "image/png",
            ".fgbface": "application/octet-stream",
        }
        self.send_response(200)
        self.send_header("Content-Type", types.get(os.path.splitext(target)[1].lower(),
                                                   "application/octet-stream"))
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def ws_session(self):
        sock = self.connection
        for message in (
            {"type": "hello", "protocol": 1, "app": "FaceGazeBridge", "version": "probe",
             "port": PORT},
            CALIBRATION,
            {"type": "catalog", "names": [], "sent": [], "count": 0},
        ):
            sock.sendall(ws_frame(json.dumps(message)))

        def reader():
            try:
                while True:
                    text = read_frame(sock)
                    if '"set_preview"' in text and '"enabled":true' in text:
                        sock.sendall(ws_frame(json.dumps(PREVIEW)))
            except Exception:
                return

        threading.Thread(target=reader, daemon=True).start()
        while True:
            time.sleep(1)


class CDP:
    def __init__(self, url):
        self.sock = socket.create_connection(("127.0.0.1", CDP_PORT), timeout=10)
        key = base64.b64encode(os.urandom(16)).decode()
        path = url.split(str(CDP_PORT), 1)[1]
        self.sock.sendall((
            f"GET {path} HTTP/1.1\r\nHost: 127.0.0.1:{CDP_PORT}\r\n"
            f"Upgrade: websocket\r\nConnection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
        buffer = b""
        while b"\r\n\r\n" not in buffer:
            buffer += self.sock.recv(4096)
        self.sock.settimeout(20)
        self.next_id = 1

    def call(self, method, params=None):
        self.next_id += 1
        message_id = self.next_id
        payload = json.dumps({"id": message_id, "method": method, "params": params or {}})
        data = payload.encode()
        header = bytearray([0x81])
        length = len(data)
        if length < 126:
            header.append(0x80 | length)
        elif length < 65536:
            header.append(0x80 | 126)
            header += struct.pack(">H", length)
        else:
            header.append(0x80 | 127)
            header += struct.pack(">Q", length)
        mask = os.urandom(4)
        header += mask
        self.sock.sendall(bytes(header) + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))
        while True:
            reply = json.loads(read_frame(self.sock))
            if reply.get("id") == message_id:
                return reply

    def evaluate(self, expression):
        reply = self.call("Runtime.evaluate", {"expression": expression, "returnByValue": True})
        return reply.get("result", {}).get("result", {}).get("value")


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(tempfile.gettempdir(), "fgb-calib.png")
    server = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()

    edge = r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"
    if not os.path.exists(edge):
        edge = r"C:\Program Files\Microsoft\Edge\Application\msedge.exe"
    profile = os.path.join(tempfile.gettempdir(), "fgb-ui-probe-profile")
    process = subprocess.Popen([
        edge, "--headless=new", "--disable-gpu", "--no-first-run",
        "--disable-extensions", "--hide-scrollbars",
        f"--user-data-dir={profile}",
        f"--remote-debugging-port={CDP_PORT}",
        "--window-size=1500,1060",
        f"http://127.0.0.1:{PORT}/index.html",
    ])

    failures = []
    try:
        page = None
        for _ in range(60):
            try:
                with urllib.request.urlopen(f"http://127.0.0.1:{CDP_PORT}/json",
                                            timeout=2) as response:
                    pages = json.loads(response.read())
                page = next((p for p in pages if p.get("type") == "page"), None)
                if page:
                    break
            except Exception:
                pass
            time.sleep(0.3)
        if not page:
            raise SystemExit("CDP 页面没起来")

        client = CDP(page["webSocketDebuggerUrl"])
        time.sleep(1.5)
        client.evaluate("document.querySelector('.nav-item[data-page=\"calibration\"]').click()")
        time.sleep(2.5)

        stats = json.loads(client.evaluate("""JSON.stringify({
            rows: document.querySelectorAll('.cal-row').length,
            groups: document.querySelectorAll('.cal-group').length,
            thumbs: document.querySelectorAll('.cal-thumb').length,
            icons: document.querySelectorAll('.cal-curve-btn svg').length,
            firstZh: (document.querySelector('.cal-zh') || {}).textContent,
            overflow: [...document.querySelectorAll('.cal-row')].slice(0, 6).map(r => {
                const s = r.querySelector('.cal-slider');
                return Math.round(s.getBoundingClientRect().bottom - r.getBoundingClientRect().bottom);
            })
        })"""))
        print("ROWS:", stats)
        if stats["rows"] != 88:
            failures.append(f"行数 {stats['rows']} != 88")
        if stats["groups"] != 9:
            failures.append(f"分组数 {stats['groups']} != 9")
        if stats["thumbs"] != 176 or stats["icons"] != 88:
            failures.append("滑块或曲线图标数量不对")
        if any(value > 0 for value in stats["overflow"]):
            failures.append("滑块溢出卡片")

        # 曲线浮层：预设 → 自定义，两个状态都要能画出来
        client.evaluate("document.querySelectorAll('.cal-curve-btn')[2].click()")
        time.sleep(0.6)
        pop = json.loads(client.evaluate("""JSON.stringify({
            open: !document.querySelector('.cal-curve-pop').hidden,
            presets: document.querySelectorAll('.cc-card').length,
            live: document.querySelector('.cc-live').textContent
        })"""))
        print("POPOVER:", pop)
        if not pop["open"] or pop["presets"] != 6:
            failures.append("曲线浮层没打开或预设数量不对")

        client.evaluate("document.querySelector('.cc-card[data-curve=custom]').click()")
        time.sleep(0.8)
        custom = json.loads(client.evaluate("""JSON.stringify({
            open: !document.querySelector('.cc-custom').hidden,
            dots: document.querySelectorAll('.cc-dot').length,
            btnH: Math.round(document.querySelector('.cc-tools .btn').getBoundingClientRect().height)
        })"""))
        print("CUSTOM:", custom)
        if not custom["open"] or custom["dots"] < 2:
            failures.append("自定义曲线面板没展开")
        if custom["btnH"] < 20:
            failures.append("自定义曲线按钮被压扁")

        client.call("Page.enable")
        shot = client.call("Page.captureScreenshot", {"format": "png"})
        data = shot.get("result", {}).get("data", "")
        if data:
            with open(out, "wb") as handle:
                handle.write(base64.b64decode(data))
            print("SCREENSHOT:", out)

        # 中文搜索：搜"眯眼"必须命中左右两条，否则名称表等于白加
        client.evaluate("document.querySelector('.cal-curve-pop').hidden = true")
        client.evaluate("""(() => {
            const input = document.querySelector('#cal-search');
            input.value = '眯眼';
            input.dispatchEvent(new Event('input'));
        })()""")
        time.sleep(0.4)
        search = json.loads(client.evaluate("""JSON.stringify({
            visible: [...document.querySelectorAll('.cal-row')].filter(r => !r.hidden).length,
            names: [...document.querySelectorAll('.cal-row')].filter(r => !r.hidden)
                .map(r => r.querySelector('.cal-en-name').textContent)
        })"""))
        print("SEARCH:", search)
        if search["visible"] != 2:
            failures.append(f"中文搜索命中 {search['visible']} 条，期望 2 条")
    finally:
        process.terminate()
        server.shutdown()

    if failures:
        print("FAIL:", "; ".join(failures))
        raise SystemExit(1)
    print("校准页渲染检查通过")


if __name__ == "__main__":
    main()
