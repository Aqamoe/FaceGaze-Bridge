"""自动更新的端到端探针（ADR-0036）。

验证的是"检查 → 静默下载 → 校验 → 待安装 → 忽略版本 → 安装失败要报错"这条链路，
全程只连本机 HTTP 服务，不碰 GitHub，也不会真的装任何东西：

  * 本地假 release：Setup 资产 + SHA256SUMS.txt（内容对得上）
  * 引擎跑在临时 LOCALAPPDATA 下，`--update-assume-installed` 假装是安装版
  * "安装"这一步故意用一段乱码当 exe：CreateProcess 必然失败，
    断言引擎给出可读错误而不是"点了没反应"

用法（先构建 Debug）：
    python tools/probe_update.py
    python tools/probe_update.py --keep      # 保留临时目录，便于看日志

不依赖第三方库：WebSocket 帧编解码手写，和 C++ 侧互为对照。
"""

import base64
import hashlib
import http.server
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CLI = os.path.join(ROOT, "build", "windows-vs2022", "bin", "Debug", "fgb-cli.exe")
PANEL_PORT = 8791
FAKE_VERSION = "9.9.9"
SETUP_NAME = f"FaceGazeBridge-Setup-{FAKE_VERSION}.exe"


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class ReleaseHandler(http.server.BaseHTTPRequestHandler):
    """把 release JSON / 安装包 / 校验和都从同一台本机服务发出去。"""

    setup_bytes = b""
    setup_hash = ""
    requests = []

    def log_message(self, *_args):
        pass

    def do_GET(self):
        ReleaseHandler.requests.append(self.path)
        if self.path.startswith("/release"):
            payload = json.dumps({
                "tag_name": f"v{FAKE_VERSION}",
                "name": "Probe Release",
                "html_url": "https://github.com/Aqamoe/FaceGaze-Bridge/releases/tag/"
                            f"v{FAKE_VERSION}",
                "body": "probe",
                "assets": [
                    {
                        "name": SETUP_NAME,
                        "size": len(ReleaseHandler.setup_bytes),
                        # 不给 digest：强制走 SHA256SUMS.txt 这条回退路径
                        "browser_download_url": f"http://127.0.0.1:{self.server.server_port}"
                                                f"/{SETUP_NAME}",
                    },
                    {
                        "name": "SHA256SUMS.txt",
                        "size": 0,
                        "browser_download_url": f"http://127.0.0.1:{self.server.server_port}"
                                                "/SHA256SUMS.txt",
                    },
                ],
            }).encode()
            self._send(payload, "application/json")
        elif self.path.startswith(f"/{SETUP_NAME}"):
            self._send(ReleaseHandler.setup_bytes, "application/octet-stream")
        elif self.path.startswith("/SHA256SUMS.txt"):
            line = f"{ReleaseHandler.setup_hash}  {SETUP_NAME}\n"
            self._send(line.encode(), "text/plain")
        else:
            self.send_error(404)

    def _send(self, payload, content_type):
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)


# 本机回环不要走系统代理：开发机上常设 http_proxy，urllib 会把 127.0.0.1 也发过去，
# 现象是 502 Bad Gateway，看着像引擎挂了（这个探针踩过一次）。
LOCAL_OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def http_json(path):
    with LOCAL_OPENER.open(f"http://127.0.0.1:{PANEL_PORT}{path}", timeout=5) as response:
        return json.loads(response.read())


class WsClient:
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        key = base64.b64encode(os.urandom(16)).decode()
        request = (
            "GET /ws HTTP/1.1\r\n"
            f"Host: 127.0.0.1:{port}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n"
        )
        self.sock.sendall(request.encode())
        data = b""
        while b"\r\n\r\n" not in data:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError("握手时对端关闭")
            data += chunk
        head, self.buffer = data.split(b"\r\n\r\n", 1)
        if not head.startswith(b"HTTP/1.1 101"):
            raise RuntimeError(f"握手失败：{head[:120]!r}")

    def send(self, payload):
        data = json.dumps(payload).encode()
        header = bytes([0x81])
        size = len(data)
        if size < 126:
            header += bytes([0x80 | size])
        else:
            header += bytes([0x80 | 126]) + struct.pack(">H", size)
        mask = os.urandom(4)
        self.sock.sendall(header + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

    def _decode(self):
        buffer = self.buffer
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
        if masked:
            offset += 4
        if len(buffer) < offset + length:
            return None
        payload = buffer[offset:offset + length]
        self.buffer = buffer[offset + length:]
        return opcode, payload

    def recv_until(self, predicate, timeout=8.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            message = self._decode()
            if message is None:
                self.sock.settimeout(max(0.05, deadline - time.time()))
                try:
                    chunk = self.sock.recv(65536)
                except socket.timeout:
                    continue
                if not chunk:
                    raise RuntimeError("引擎关闭了连接")
                self.buffer += chunk
                continue
            opcode, payload = message
            if opcode != 0x1:
                continue
            text = payload.decode("utf-8", "replace")
            try:
                parsed = json.loads(text)
            except json.JSONDecodeError:
                continue
            if predicate(parsed):
                return parsed
        raise RuntimeError("等待消息超时")

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def wait_for(predicate, timeout=20.0, interval=0.15):
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        last = http_json("/api/update")
        if predicate(last):
            return last
        time.sleep(interval)
    return last


class CdpClient:
    """最小的 CDP 客户端：只为在页面里跑一句 JS。"""

    def __init__(self, port):
        deadline = time.time() + 20
        target = None
        while time.time() < deadline:
            try:
                with LOCAL_OPENER.open(f"http://127.0.0.1:{port}/json", timeout=3) as response:
                    targets = json.loads(response.read())
                for item in targets:
                    if item.get("type") == "page" and item.get("webSocketDebuggerUrl"):
                        target = item
                        break
                if target:
                    break
            except OSError:
                pass
            time.sleep(0.3)
        if target is None:
            raise RuntimeError("拿不到 WebView2 的调试目标")

        url = target["webSocketDebuggerUrl"]
        host_port = url.split("//", 1)[1].split("/", 1)[0]
        host, _, port_text = host_port.partition(":")
        path = "/" + url.split("//", 1)[1].split("/", 1)[1]
        self.sock = socket.create_connection((host, int(port_text or 80)), timeout=10)
        key = base64.b64encode(os.urandom(16)).decode()
        request = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {host_port}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n"
        )
        self.sock.sendall(request.encode())
        data = b""
        while b"\r\n\r\n" not in data:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError("CDP 握手时对端关闭")
            data += chunk
        head, self.buffer = data.split(b"\r\n\r\n", 1)
        if not head.startswith(b"HTTP/1.1 101"):
            raise RuntimeError(f"CDP 握手失败：{head[:120]!r}")
        self.next_id = 0

    def _frame(self, payload):
        data = payload.encode()
        header = bytes([0x81])
        size = len(data)
        if size < 126:
            header += bytes([0x80 | size])
        else:
            header += bytes([0x80 | 126]) + struct.pack(">H", size)
        mask = os.urandom(4)
        self.sock.sendall(header + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

    def call(self, method, params=None, timeout=10.0):
        self.next_id += 1
        message_id = self.next_id
        self._frame(json.dumps({"id": message_id, "method": method, "params": params or {}}))
        deadline = time.time() + timeout
        while time.time() < deadline:
            if len(self.buffer) < 2:
                self.sock.settimeout(max(0.05, deadline - time.time()))
                try:
                    chunk = self.sock.recv(65536)
                except socket.timeout:
                    continue
                if not chunk:
                    raise RuntimeError("CDP 连接断了")
                self.buffer += chunk
                continue
            length = self.buffer[1] & 0x7F
            offset = 2
            if length == 126:
                if len(self.buffer) < 4:
                    continue
                length = struct.unpack(">H", self.buffer[2:4])[0]
                offset = 4
            elif length == 127:
                if len(self.buffer) < 10:
                    continue
                length = struct.unpack(">Q", self.buffer[2:10])[0]
                offset = 10
            if len(self.buffer) < offset + length:
                if len(self.buffer) < 2:
                    continue
                self.sock.settimeout(max(0.05, deadline - time.time()))
                try:
                    chunk = self.sock.recv(65536)
                except socket.timeout:
                    continue
                if not chunk:
                    raise RuntimeError("CDP 连接断了")
                self.buffer += chunk
                continue
            payload = self.buffer[offset:offset + length]
            self.buffer = self.buffer[offset + length:]
            try:
                parsed = json.loads(payload)
            except json.JSONDecodeError:
                continue
            if parsed.get("id") == message_id:
                return parsed
        raise RuntimeError(f"CDP {method} 超时")

    def eval(self, expression, timeout=10.0):
        result = self.call("Runtime.evaluate", {
            "expression": expression,
            "returnByValue": True,
            "awaitPromise": True,
        }, timeout=timeout)
        return result.get("result", {}).get("result", {}).get("value")

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def ui_check(release_url, check):
    """界面部分：提示条真的会出现在页面上，而不是只有 IPC 数据对。

    自己拉一套"临时引擎 + 临时外壳"：更新状态是全局的，复用前面那台引擎的话
    "初始应该是 idle"这条断言就不成立了。
    """
    shell = os.path.join(ROOT, "build", "windows-vs2022", "bin", "Debug",
                         "FaceGaze Bridge.exe")
    if not os.path.exists(shell):
        print("  SKIP 找不到 FaceGaze Bridge.exe（FGB_BUILD_UI 关着？）")
        return

    port = free_port()
    cdp_port = free_port()
    workdir = tempfile.mkdtemp(prefix="fgb-probe-update-ui-")
    env = dict(os.environ)
    env["LOCALAPPDATA"] = workdir

    # VMC 输入端口要单独给：前面那台探针引擎还占着默认的 39539，
    # 第二台绑不上就会直接退出，外壳随后会拉起一台"没有 assume 开关"的引擎，
    # 症状是页面能连上但 eligible=false（这条踩过一次）。
    engine = subprocess.Popen(
        [CLI, "run", "--source", "vmc", "--vmc-port", str(free_port()),
         "--panel", str(port), "--no-update-check", "--update-assume-installed",
         "--no-saved-outputs"],
        env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.time() + 20
    snapshot = {}
    while time.time() < deadline:
        try:
            with LOCAL_OPENER.open(f"http://127.0.0.1:{port}/api/update", timeout=2) as response:
                snapshot = json.loads(response.read())
            break
        except OSError:
            time.sleep(0.2)
    check("界面场景的引擎带 assume-installed",
          snapshot.get("eligible") is True, f"port={port} {snapshot}")

    app = subprocess.Popen(
        [shell, "--port", str(port), "--debug-port", str(cdp_port),
         "--user-data", os.path.join(workdir, "webview2")],
        env=env)
    try:
        client = CdpClient(cdp_port)
        try:
            deadline = time.time() + 25
            while time.time() < deadline:
                if client.eval("typeof state !== 'undefined' && !!state.update") is True:
                    break
                time.sleep(0.3)
            check("页面收到 update 快照",
                  client.eval("typeof state !== 'undefined' && !!state.update") is True)
            check("没有新版本时提示条隐藏",
                  client.eval("document.getElementById('update-bar').hidden") is True)

            client.eval(f"send({{type:'check_updates', url:'{release_url}'}})")
            deadline = time.time() + 25
            stage = ""
            while time.time() < deadline:
                stage = client.eval("state.update && state.update.state") or ""
                if stage in ("ready", "failed"):
                    break
                time.sleep(0.3)
            check("页面看到 ready", stage == "ready",
                  f"{stage} / {client.eval('state.update && state.update.error')}")
            check("提示条出现",
                  client.eval("document.getElementById('update-bar').hidden") is False)
            text = client.eval("document.getElementById('ub-text').textContent") or ""
            check("提示条写了版本号", FAKE_VERSION in text, text)
            check("设置页显示可安装",
                  client.eval("document.getElementById('u-install').hidden") is False)

            client.eval("document.getElementById('ub-later').click()")
            time.sleep(0.2)
            check("点「稍后」后提示条收起",
                  client.eval("document.getElementById('update-bar').hidden") is True)
        finally:
            client.close()
    finally:
        app.terminate()
        try:
            app.wait(timeout=8)
        except subprocess.TimeoutExpired:
            app.kill()
        engine.terminate()
        try:
            engine.wait(timeout=5)
        except subprocess.TimeoutExpired:
            engine.kill()
        shutil.rmtree(workdir, ignore_errors=True)


def main():
    keep = "--keep" in sys.argv
    with_ui = "--ui" in sys.argv
    if not os.path.exists(CLI):
        print(f"找不到 {CLI}，先构建 Debug")
        return 1

    # 假安装包：一段随机字节。故意不是有效 exe —— 安装那一步必须失败，
    # 这样探针就永远不会真的启动安装器。
    setup_bytes = os.urandom(200 * 1024)
    ReleaseHandler.setup_bytes = setup_bytes
    ReleaseHandler.setup_hash = hashlib.sha256(setup_bytes).hexdigest()

    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), ReleaseHandler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    release_url = f"http://127.0.0.1:{httpd.server_port}/release"

    workdir = tempfile.mkdtemp(prefix="fgb-probe-update-")
    env = dict(os.environ)
    env["LOCALAPPDATA"] = workdir  # 更新状态、下载的包都落在临时目录

    engine = subprocess.Popen(
        [CLI, "run", "--source", "vmc", "--panel", str(PANEL_PORT),
         "--no-update-check", "--update-assume-installed", "--no-saved-outputs"],
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )

    failures = []

    def check(name, condition, detail=""):
        if condition:
            print(f"  OK   {name}")
        else:
            failures.append(name)
            print(f"  FAIL {name} {detail}")

    try:
        deadline = time.time() + 20
        while time.time() < deadline:
            try:
                http_json("/api/update")
                break
            except OSError:
                time.sleep(0.2)
        else:
            print("引擎没起来（/api/update 不通）")
            return 1

        client = WsClient(PANEL_PORT)
        try:
            initial = http_json("/api/update")
            check("启动时 state=idle", initial.get("state") == "idle", initial)
            # 探针带了 --update-assume-installed：开发目录本身不是安装器装的，
            # 不放开它就走不到"下载/校验"这段（普通运行时这里应为 False）
            check("探针开关生效（eligible）", initial.get("eligible") is True, initial)

            print("· 手动检查（本地假 release）")
            client.send({"type": "check_updates", "url": release_url})
            client.recv_until(lambda m: m.get("type") == "ack" and m.get("command") == "check_updates")
            ready = wait_for(lambda s: s.get("state") in ("ready", "failed"), timeout=25)
            check("下载并校验完成", ready.get("state") == "ready", ready)
            check("版本号来自 release", ready.get("latest") == f"v{FAKE_VERSION}", ready)
            check("staged 有版本与大小", bool(ready.get("staged", {}).get("version")), ready)
            staged_size = ready.get("staged", {}).get("size")
            check("staged 大小与资产一致", staged_size == len(setup_bytes), staged_size)

            downloaded = os.path.join(workdir, "FaceGazeBridge", "updates", SETUP_NAME)
            check("安装包落在 updates/", os.path.exists(downloaded), downloaded)
            if os.path.exists(downloaded):
                with open(downloaded, "rb") as handle:
                    check("落盘内容与发布一致", handle.read() == setup_bytes)

            print("· 忽略这个版本")
            client.send({"type": "skip_update_version"})
            client.recv_until(lambda m: m.get("type") == "ack" and m.get("command") == "skip_update_version")
            skipped = wait_for(lambda s: s.get("skipped") is True, timeout=5)
            check("skipped 置位", skipped.get("skipped") is True, skipped)

            prefs = os.path.join(workdir, "FaceGazeBridge", "update.ini")
            check("update.ini 已落盘", os.path.exists(prefs), prefs)

            print("· 安装（假安装包必须失败，且要报可读原因）")
            client.send({"type": "install_update"})
            ack = client.recv_until(lambda m: m.get("type") == "ack" and m.get("command") == "install_update")
            check("安装失败有回执", ack.get("ok") is False, ack)
            check("失败原因可读", "安装器" in (ack.get("error") or ""), ack)

            # 引擎没退出 = 没有真的启动安装器（这正是探针要的）
            time.sleep(0.5)
            check("引擎仍在运行（没有真的安装）", engine.poll() is None)

            if with_ui:
                print("· 界面（WebView2 / CDP）")
                ui_check(release_url, check)
        finally:
            client.close()
    finally:
        engine.terminate()
        try:
            engine.wait(timeout=5)
        except subprocess.TimeoutExpired:
            engine.kill()
        httpd.shutdown()

    if keep:
        print(f"临时目录：{workdir}")
    else:
        shutil.rmtree(workdir, ignore_errors=True)

    if failures:
        print(f"\n{len(failures)} 项失败：{', '.join(failures)}")
        return 1
    print("\n更新链路探针通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
