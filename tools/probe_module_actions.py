"""模块管理动作的端到端探针（删除 / 更新正在运行的模块）。

要验证的行为：模块正在跑的时候，用户点「删除」或「更新」并确认之后，
后台自己停实例 → 删文件/换 DLL → 装完把实例拉回来，最后用一句人话
（WebSocket `notice`）告诉用户成没成。用户不需要先去实例行手动点「停止」；
失败时也要有明确回执，并且把实例恢复运行。

为什么不用真实仓库：脚本自建临时 LOCALAPPDATA + 本地 HTTP registry，
装的是 build 目录里的 fgb-sample-native.dll，不碰用户真实的模块仓，
也不联网。

用法（先构建 Debug）：
    python tools/probe_module_actions.py
"""
import base64
import hashlib
import http.server
import io
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
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# 允许指向别的构建目录：主构建的 fgb-cli.exe 被正在运行的引擎占着时（Windows 不允许
# 覆盖正在执行的 exe），可以另建一个目录验证，不必先让用户关掉应用。
BIN = Path(os.environ.get("FGB_PROBE_BIN", ROOT / "build" / "windows-vs2022" / "bin" / "Debug"))
CLI = BIN / "fgb-cli.exe"
SAMPLE_DLL = Path(os.environ.get("FGB_PROBE_SAMPLE", BIN / "fgb-sample-native.dll"))

MODULE_ID = "probe-native"
MODULE_NAME = "Probe Native"
DLL_NAME = "probe-native.dll"


def fail(message):
    print(f"[FAIL] {message}")
    sys.exit(1)


def step(message):
    print(f"[ .. ] {message}")


def manifest(version):
    return json.dumps({
        "ModuleId": MODULE_ID,
        "ModuleName": MODULE_NAME,
        "AuthorName": "probe",
        "Version": version,
        "DllFileName": DLL_NAME,
        "ModuleDescription": "probe package",
        "AbiVersion": 2,
        "Capabilities": ["expression"],
    }, ensure_ascii=False)


def make_package(version, dll_bytes):
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("fgb-native.json", manifest(version))
        archive.writestr(DLL_NAME, dll_bytes)
    return buffer.getvalue()


class RegistryHandler(http.server.BaseHTTPRequestHandler):
    payload = b"[]"
    package = b""

    def do_GET(self):
        if self.path.startswith("/modules"):
            body = self.payload
            self.send_response(200)
        elif self.path.startswith("/probe-v2.zip"):
            body = self.package
            self.send_response(200)
            self.send_header("Content-Type", "application/zip")
        else:
            self.send_response(404)
            self.end_headers()
            return
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):  # 别把探针的访问日志刷到终端
        pass


def registry_payload(version, download_url, file_hash=""):
    entry = {
        "ModuleId": MODULE_ID,
        "ModuleName": MODULE_NAME,
        "AuthorName": "probe",
        "Version": version,
        "ModuleDescription": "probe package",
        "DownloadUrl": download_url,
        "DllFileName": DLL_NAME,
    }
    if file_hash:
        entry["FileHash"] = file_hash
    return json.dumps([entry]).encode()


class Socket:
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.buffer = self._handshake(port)
        self.messages = []
        self.cursor = 0

    def _handshake(self, port):
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall((
            "GET /ws HTTP/1.1\r\n"
            f"Host: 127.0.0.1:{port}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n"
        ).encode())
        data = b""
        deadline = time.time() + 5
        while b"\r\n\r\n" not in data:
            if time.time() > deadline:
                raise RuntimeError("握手超时")
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError("握手时连接被关闭")
            data += chunk
        head, rest = data.split(b"\r\n\r\n", 1)
        if not head.startswith(b"HTTP/1.1 101"):
            raise RuntimeError(f"握手失败：{head[:120]!r}")
        return rest

    def send(self, payload):
        body = json.dumps(payload).encode()
        header = bytearray([0x81])
        length = len(body)
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
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(body))
        self.sock.sendall(bytes(header) + masked)

    def _decode(self):
        buffer = self.buffer
        if len(buffer) < 2:
            return None
        opcode = buffer[0] & 0x0F
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
        masked = buffer[1] & 0x80
        if masked:
            offset += 4
        if len(buffer) < offset + length:
            return None
        payload = buffer[offset:offset + length]
        self.buffer = buffer[offset + length:]
        return opcode, payload

    def pump(self, seconds):
        """收一段时间消息，追加到 self.messages。"""
        deadline = time.time() + seconds
        while time.time() < deadline:
            self.sock.settimeout(0.1)
            try:
                chunk = self.sock.recv(65536)
                if not chunk:
                    break
                self.buffer += chunk
            except socket.timeout:
                pass
            while True:
                frame = self._decode()
                if frame is None:
                    break
                opcode, payload = frame
                if opcode == 0x1:
                    self.messages.append(json.loads(payload))
        return self.messages

    def try_wait(self, predicate, seconds):
        """等一条消息，超时返回 None（调用方自己决定要不要重试）。"""
        # 只往前走游标，不丢消息：调用方经常连着等两条（ack 之后还有 notice），
        # 两条可能在同一批 recv 里到达，丢掉第二条就会白等到超时。
        deadline = time.time() + seconds
        while True:
            while self.cursor < len(self.messages):
                message = self.messages[self.cursor]
                self.cursor += 1
                if predicate(message):
                    return message
            if time.time() > deadline:
                return None
            self.pump(0.1)

    def wait(self, predicate, seconds, description):
        message = self.try_wait(predicate, seconds)
        if message is None:
            fail(f"等不到{description}")
        return message

    def ack(self, command, seconds=15):
        return self.wait(
            lambda m: m.get("type") == "ack" and m.get("command") == command, seconds,
            f"{command} 回执")

    def notice(self, seconds=40):
        return self.wait(lambda m: m.get("type") == "notice", seconds, "安装结果 notice")


def wait_port(port, seconds=25):
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.3):
                return
        except OSError:
            time.sleep(0.2)
    fail(f"引擎没有在 {port} 上监听")


def start_engine(env, port, log_path):
    log = open(log_path, "wb")
    process = subprocess.Popen(
        [str(CLI), "run", "--source", "vrcft", "--module-id", MODULE_ID,
         "--panel", str(port),
         "--calibration", str(log_path.parent / f"cal-{port}.ini"),
         "--outputs-config", str(log_path.parent / f"out-{port}.ini")],
        env=env, stdout=log, stderr=subprocess.STDOUT,
    )
    wait_port(port)
    return process, log


def stop_engine(process, log):
    process.terminate()
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)
    log.close()


def active_state(message):
    for entry in message.get("modules") or []:
        if entry.get("module_id") == MODULE_ID:
            return entry.get("state")
    return None


def wait_active(client, seconds=15):
    client.wait(lambda m: m.get("type") == "status" and active_state(m) == "active", seconds,
                "模块进入 active")


def module_dir(store):
    if not store.exists():
        return None
    for entry in store.iterdir():
        if entry.is_dir():
            return entry
    return None


def install_v1(env, package_dir):
    done = subprocess.run([str(CLI), "modules", "install", str(package_dir)],
                          env=env, capture_output=True, text=True)
    if done.returncode != 0:
        fail(f"v1 安装失败：{done.stdout}{done.stderr}")


def refresh_to(client, version, seconds=20):
    """让模块库重新拉一次本地 registry，等到列表里出现指定版本。"""
    predicate = lambda m: m.get("type") == "library" and any(
        item.get("id") == MODULE_ID and item.get("version") == version
        for item in (m.get("items") or []))
    for _ in range(2):
        client.send({"type": "refresh_library"})
        if client.try_wait(predicate, seconds) is not None:
            return
    fail(f"模块库里没有 v{version}")


def main():
    if not CLI.exists() or not SAMPLE_DLL.exists():
        fail(f"缺少构建产物：{CLI} / {SAMPLE_DLL}（先构建 Debug）")

    work = Path(tempfile.mkdtemp(prefix="fgb-probe-modules-"))
    store = work / "local" / "FaceGazeBridge" / "modules"
    env = os.environ.copy()
    env["LOCALAPPDATA"] = str(work / "local")

    package_v2 = make_package("2.0.0", SAMPLE_DLL.read_bytes())
    RegistryHandler.package = package_v2
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), RegistryHandler)
    registry_port = server.server_port
    RegistryHandler.payload = registry_payload(
        "2.0.0", f"http://127.0.0.1:{registry_port}/probe-v2.zip",
        hashlib.md5(package_v2).hexdigest())
    env["FGB_MODULE_REGISTRY"] = f"http://127.0.0.1:{registry_port}/modules"
    threading.Thread(target=server.serve_forever, daemon=True).start()

    package_dir = work / "pkg-v1"
    package_dir.mkdir(parents=True)
    (package_dir / "fgb-native.json").write_text(manifest("1.0.0"), encoding="utf-8")
    shutil.copyfile(SAMPLE_DLL, package_dir / DLL_NAME)
    step("安装 v1 到临时模块仓")
    install_v1(env, package_dir)

    # ---------- 第一段：删除正在运行的模块 ----------
    step("启动引擎（模块在进程内加载，DLL 被占用）")
    process, log = start_engine(env, 18871, work / "engine-delete.log")
    try:
        client = Socket(18871)
        wait_active(client)

        # A1：外部进程攥着 DLL 时删除要失败，而且失败要有明确回执（不能只写日志）
        held = module_dir(store) / DLL_NAME
        lock = open(held, "rb")
        try:
            step("外部占用 DLL，发送 delete_module（预期失败并提醒）")
            client.send({"type": "delete_module", "id": MODULE_ID})
            ack = client.ack("delete_module")
            print(f"       ack: {ack.get('message') or ack.get('error')}")
            if ack.get("ok"):
                fail("DLL 被外部占用时删除居然成功了")
            if "删除失败" not in (ack.get("message") or ""):
                fail("删除失败的回执不是人话（界面 toast 会显示它）")
        finally:
            lock.close()
        time.sleep(0.5)

        # A2：正常删除：自动停实例 → 删文件，引擎不重启
        step("释放占用后重新删除（预期自动停实例后成功）")
        client.send({"type": "delete_module", "id": MODULE_ID})
        ack = client.ack("delete_module")
        if not ack.get("ok"):
            fail(f"删除失败：{ack.get('error')}")
        message = ack.get("message") or ""
        print(f"       ack: {message}")
        if "已自动停止" not in message:
            fail("删除回执没有说明自动停过实例")

        deadline = time.time() + 5
        while time.time() < deadline and store.exists() and any(store.iterdir()):
            time.sleep(0.2)
        if store.exists() and any(store.iterdir()):
            fail("模块目录还在，文件没删掉")

        # 删掉之后 status 里不能再有它的实例和输入源：只 stop 不摘的话，
        # 界面「运行中的实例 / 输入源 / 输入帧率」会一直挂着一个不存在的模块。
        def cleaned(message):
            if message.get("type") != "status":
                return False
            if any(entry.get("module_id") == MODULE_ID
                   for entry in (message.get("modules") or [])):
                return False
            if any(entry.get("name") == "native" for entry in (message.get("sources") or [])):
                return False
            return "native" not in (message.get("source") or "")

        client.wait(cleaned, 10, "删除后实例与输入源从 status 里消失")

        # 没有重启引擎：连接仍然活着，status 继续推
        client.wait(lambda m: m.get("type") == "status", 5, "删除后的 status")
        print("[ OK ] 删除：失败有提醒，成功会自动停实例并摘掉输入源，引擎没有重启")
    finally:
        stop_engine(process, log)

    # ---------- 第二段：更新正在运行的模块 ----------
    step("重新安装 v1，启动引擎，从本地 registry 更新到 v2")
    install_v1(env, package_dir)
    process, log = start_engine(env, 18872, work / "engine-update.log")
    try:
        client = Socket(18872)
        wait_active(client)
        refresh_to(client, "2.0.0")

        step("发送 install_library（更新）")
        client.send({"type": "install_library", "id": MODULE_ID})
        ack = client.ack("install_library")
        if not ack.get("ok"):
            fail(f"更新没能开始：{ack.get('error')}")
        print(f"       ack: {ack.get('message')}")
        if "已停止" not in (ack.get("message") or ""):
            fail("更新回执没有说明自动停过实例")

        notice = client.notice()
        print(f"       notice[{notice.get('level')}]: {notice.get('message')}")
        if notice.get("level") != "info":
            fail("更新失败（notice 是 error）")
        if "已更新到" not in notice.get("message", ""):
            fail("notice 没说清更新到了哪个版本")
        if "已自动恢复" not in notice.get("message", ""):
            fail("notice 没有说明实例已经恢复运行")

        installed = json.loads((module_dir(store) / "fgb-native.json")
                               .read_text(encoding="utf-8"))
        if installed.get("Version") != "2.0.0":
            fail(f"磁盘上的版本不是 2.0.0：{installed.get('Version')}")

        wait_active(client)
        print("[ OK ] 更新：自动停实例 → 换版本 → 自动恢复，结果用 notice 推给界面")

        # ---------- 第三段：更新失败也要提醒，并恢复实例 ----------
        step("换成一个 404 的下载地址，模拟更新失败")
        RegistryHandler.payload = registry_payload(
            "3.0.0", f"http://127.0.0.1:{registry_port}/missing.zip")
        refresh_to(client, "3.0.0")

        client.send({"type": "install_library", "id": MODULE_ID})
        ack = client.ack("install_library")
        if not ack.get("ok"):
            fail(f"失败路径的安装不该在开始时就被拒：{ack.get('error')}")

        notice = client.notice()
        print(f"       notice[{notice.get('level')}]: {notice.get('message')}")
        if notice.get("level") != "error":
            fail("更新失败却给了 info notice")
        if "更新失败" not in notice.get("message", ""):
            fail("notice 没有说明这是更新失败")
        if "已自动恢复" not in notice.get("message", ""):
            fail("更新失败后没有把实例恢复运行")

        installed = json.loads((module_dir(store) / "fgb-native.json")
                               .read_text(encoding="utf-8"))
        if installed.get("Version") != "2.0.0":
            fail(f"更新失败却动了磁盘版本：{installed.get('Version')}")

        wait_active(client)
        print("[ OK ] 更新失败：实例恢复运行、磁盘版本没变、界面收到 error notice")
    finally:
        stop_engine(process, log)
        server.shutdown()

    print("[DONE] 模块管理动作探针通过")
    shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
