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
import shutil
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

with open(os.path.join(ROOT, "face", "face_map.json"), encoding="utf-8") as handle:
    SHAPE_REGIONS = {name.lower(): regions
                     for name, regions in json.load(handle)["shape_regions"].items()}


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
        "eye": {"active": True, "gain_x": 1.2, "gain_y": 1.0, "center_x": 0.4, "center_y": -1.2,
                "eyelid_gain": 1.37, "eyelid_lo": 0.10, "eyelid_sep": 3.7},
    }


CALIBRATION = build_calibration()
# 存档清单：两条中文名，验证下拉没把名字弄丢、路径跟着选择走
CALIBRATION_PROFILES = {
    "type": "calibration_profiles", "available": True,
    "dir": "C:/probe/calibration-profiles",
    "profiles": ["夜间 直播", "默认"],
    "paths": ["C:/probe/calibration-profiles/night.ini",
              "C:/probe/calibration-profiles/default.ini"],
}
# 页面发出的命令：存档下拉必须真的按选择发 apply/reload，而不是只改 UI
RECEIVED = []
PREVIEW = {"type": "preview", "seq": 1, "raw": [0.42] * 88, "out": [0.61] * 88,
           "eye": {"valid": True, "open_l": 0.72, "open_r": 0.06, "x": 2.4, "y": -1.1}}
# eye_trim=stale 用来验证"标定后改了角度粗调"的提示会亮；其它字段走已完成态
GAZE_CALIB = {"type": "gaze_calib", "state": "done", "target": 0, "target_count": 13,
              "done": 13, "samples": 0, "needed": 0, "progress": 1.0, "message": "标定完成",
              "profile_version": 3, "residual": 0.021, "source_kind": "angles",
              "created_utc": "2026-11-02T03:00:00Z", "hmd_identity": "probe",
              "ready": True, "geometry": True, "point_space": "calibrated",
              "eye_trim": "stale", "probe_active": False, "probe_message": "",
              # 档案里记的粗调：页面要把它和滑杆并排显示，否则"过期"只能靠猜
              "eye_trim_saved": {"known": True, "active": True, "gain_x": 1.2, "gain_y": 1.0,
                                 "center_x": 0.0, "center_y": -8.0}}


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
            CALIBRATION_PROFILES,
            GAZE_CALIB,
            {"type": "catalog", "names": [], "sent": [], "count": 0},
        ):
            sock.sendall(ws_frame(json.dumps(message)))

        def reader():
            try:
                while True:
                    text = read_frame(sock)
                    try:
                        RECEIVED.append(json.loads(text))
                    except ValueError:
                        pass
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
    # 页面里的负号是 U+2212，Windows 控制台默认 GBK 会直接抛 UnicodeEncodeError
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except AttributeError:
        pass
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(tempfile.gettempdir(), "fgb-calib.png")
    server = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()

    edge = r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"
    if not os.path.exists(edge):
        edge = r"C:\Program Files\Microsoft\Edge\Application\msedge.exe"
    # 每次用干净的用户数据：镜像开关、左右合并这类界面偏好存在 localStorage，
    # 复用同一个目录会让"默认开启"这类断言取决于上一次跑完留下的状态
    profile = tempfile.mkdtemp(prefix="fgb-ui-probe-")
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
            splits: document.querySelectorAll('.cal-split-btn').length,
            firstZh: (document.querySelector('.cal-zh') || {}).textContent,
            // 卡片顺序：角度粗调 → 圆盘标定 → 眼睑（眼睑不夹在两步中间）
            cardOrder: [...document.querySelectorAll('.cal-side > article')].map(a =>
                a.classList.contains('cal-eye-card') ? 'eye'
                    : a.classList.contains('cal-dot') ? 'dot'
                        : a.classList.contains('cal-eyelid-card') ? 'eyelid' : 'other'),
            archive: {
                // 存档不再是独立卡片：入口就在顶部工具条里
                cards: document.querySelectorAll('.cal-archives').length,
                inToolbar: !!document.querySelector('.toolbar #cal-archive'),
                options: [...document.querySelectorAll('#cal-archive option')].map(o => o.textContent),
                deleteDisabled: [...document.querySelectorAll('#cal-archive option')]
                    .find(o => o.value === '__delete').disabled,
                disabled: (document.querySelector('#cal-archive') || {}).disabled,
                path: (document.querySelector('#cal-path') || {}).textContent,
                saveText: (document.querySelector('#cal-save') || {}).textContent
            },
            dotProfile: (document.querySelector('#cal-dot-profile') || {}).textContent,
            // 档案一行最长（版本 + 分支 + 粗调），不许被裁到卡片外
            dotProfileFits: (() => {
                const b = document.querySelector('#cal-dot-profile');
                if (!b) { return false; }
                const card = b.closest('.card');
                return !card || b.getBoundingClientRect().right <= card.getBoundingClientRect().right + 1;
            })(),
            stepBadges: document.querySelectorAll('.cal-step-no').length,
            eye: {
                groups: document.querySelectorAll('.cal-eye-group').length,
                reset: !!document.querySelector('#cal-eye-reset'),
                gainMin: (document.querySelector('#cal-eye-gain-x') || {}).min,
                gainMax: (document.querySelector('#cal-eye-gain-x') || {}).max,
                centerMin: (document.querySelector('#cal-eye-center-x') || {}).min,
                centerMax: (document.querySelector('#cal-eye-center-x') || {}).max,
                staleVisible: (document.querySelector('#cal-dot-trim') || {}).hidden === false
            },
            eyelid: {
                gain: (document.querySelector('#cal-eyelid-gain') || {}).value,
                lo: (document.querySelector('#cal-eyelid-lo') || {}).value,
                step1: (document.querySelector('#cal-eyelid-step1') || {}).textContent,
                step2: (document.querySelector('#cal-eyelid-step2') || {}).textContent,
                step3: (document.querySelector('#cal-eyelid-step3') || {}).textContent
            },
            overflow: [...document.querySelectorAll('.cal-row')].slice(0, 6).map(r => {
                const s = r.querySelector('.cal-slider');
                return Math.round(s.getBoundingClientRect().bottom - r.getBoundingClientRect().bottom);
            })
        })"""))
        print("ROWS:", stats)
        # 88 个形状里 30 对左右镜像合并成一张卡，剩下 10 个方向性 + 18 个单侧各自一张
        if stats["rows"] != 58:
            failures.append(f"行数 {stats['rows']} != 58")
        if stats["groups"] != 9:
            failures.append(f"分组数 {stats['groups']} != 9")
        if stats["thumbs"] != 116 or stats["icons"] != 58:
            failures.append("滑块或曲线图标数量不对")
        if stats["splits"] != 30:
            failures.append(f"分离按钮 {stats['splits']} != 30")
        if any(value > 0 for value in stats["overflow"]):
            failures.append("滑块溢出卡片")

        # 角度粗调：显式水平/垂直两块、可单独重置、量程与后端 clamp 对齐、
        # 以及"标定后改过粗调"的提示（GAZE_CALIB 里 eye_trim=stale）
        eye = stats["eye"]
        if eye["groups"] != 2:
            failures.append(f"角度分组 {eye['groups']} != 2")
        if not eye["reset"]:
            failures.append("缺少「重置这四项」按钮")
        if (eye["gainMin"], eye["gainMax"], eye["centerMin"], eye["centerMax"]) != \
                ("0.1", "5", "-60", "60"):
            failures.append(f"角度滑杆量程与后端 clamp 不一致：{eye}")
        if not eye["staleVisible"]:
            failures.append("eye_trim=stale 时没有显示过期提示")

        # 顺序：角度粗调 → 圆盘标定 → 眼睑，中间不许夹别的
        if stats["cardOrder"][:3] != ["eye", "dot", "eyelid"]:
            failures.append(f"卡片顺序 {stats['cardOrder']}，应为 eye → dot → eyelid")
        if stats["stepBadges"] != 2:
            failures.append(f"步骤序号 {stats['stepBadges']} != 2")

        # 存档：入口在顶部工具条，一个下拉包含「当前档案 + 各存档 + 新建/删除」动作
        archive = stats["archive"]
        print("ARCHIVE:", archive)
        if archive["cards"] != 0 or not archive["inToolbar"]:
            failures.append("存档仍不是工具条里的下拉（或还留着独立卡片）")
        for text in ("当前档案", "夜间 直播", "默认"):
            if text not in archive["options"]:
                failures.append(f"存档下拉缺项：{archive['options']}")
        if sum("新建" in text for text in archive["options"]) != 2:
            failures.append(f"下拉里应有两个新建动作：{archive['options']}")
        if "删除" not in " ".join(archive["options"]):
            failures.append("下拉里缺少删除动作")
        if archive["disabled"]:
            failures.append("存档可用时下拉仍是灰的")
        if not archive["deleteDisabled"]:
            failures.append("未选中命名存档时删除动作应不可用")
        if not (archive["path"] or "").endswith("calibration.ini"):
            failures.append(f"默认应显示当前档案路径：{archive['path']!r}")
        if archive["saveText"] != "保存":
            failures.append(f"保存按钮文案不对：{archive['saveText']!r}")

        # 选择存档：先取消（必须退回原选择），再确认（必须真的发 apply）
        RECEIVED.clear()
        client.evaluate("""(() => {
            const s = document.querySelector('#cal-archive');
            s.value = '夜间 直播';
            s.dispatchEvent(new Event('change'));
        })()""")
        time.sleep(0.4)
        modal = json.loads(client.evaluate("""JSON.stringify({
            open: !document.querySelector('#modal').hidden,
            text: document.querySelector('#modal-message').textContent,
            value: document.querySelector('#cal-archive').value
        })"""))
        print("SWITCH-MODAL:", modal)
        if not modal["open"] or "夜间 直播" not in modal["text"]:
            failures.append(f"有未保存改动时切换存档没有确认框：{modal}")
        client.evaluate("document.querySelector('#modal-cancel').click()")
        time.sleep(0.3)
        back = client.evaluate("document.querySelector('#cal-archive').value")
        if back != "":
            failures.append(f"取消切换后下拉没有退回「当前档案」：{back!r}")
        if any(m.get("type") == "apply_calibration_profile" for m in RECEIVED):
            failures.append("取消切换却仍然发出了 apply_calibration_profile")

        client.evaluate("""(() => {
            const s = document.querySelector('#cal-archive');
            s.value = '夜间 直播';
            s.dispatchEvent(new Event('change'));
        })()""")
        time.sleep(0.4)
        client.evaluate("document.querySelector('#modal-ok').click()")
        time.sleep(0.5)
        sent = [m for m in RECEIVED if m.get("type") == "apply_calibration_profile"]
        if not sent or sent[-1].get("name") != "夜间 直播":
            failures.append(f"确认切换后没有发出带名字的 apply_calibration_profile：{RECEIVED}")
        shown_path = client.evaluate("document.querySelector('#cal-path').textContent")
        if not (shown_path or "").endswith("night.ini"):
            failures.append(f"选中存档后路径没有跟着切换：{shown_path!r}")

        # 下拉里的「新建：当前参数」：先弹出带输入框的确认框，确认后发出 create
        RECEIVED.clear()
        client.evaluate("""(() => {
            const s = document.querySelector('#cal-archive');
            s.value = '__new_current';
            s.dispatchEvent(new Event('change'));
        })()""")
        time.sleep(0.4)
        prompt = json.loads(client.evaluate("""JSON.stringify({
            open: !document.querySelector('#modal').hidden,
            input: !document.querySelector('#modal-input').hidden,
            placeholder: document.querySelector('#modal-input').placeholder,
            value: document.querySelector('#cal-archive').value
        })"""))
        print("NEW-MODAL:", prompt)
        if not prompt["open"] or not prompt["input"] or not prompt["placeholder"]:
            failures.append(f"新建存档没有输入名称的弹窗：{prompt}")
        if prompt["value"] != "夜间 直播":
            failures.append(f"打开新建弹窗时下拉应停在原选择：{prompt['value']!r}")
        client.evaluate("document.querySelector('#modal-input').value = '新建 副本'")
        client.evaluate("document.querySelector('#modal-ok').click()")
        time.sleep(0.4)
        created = [m for m in RECEIVED if m.get("type") == "create_calibration_profile"]
        if (not created or created[-1].get("name") != "新建 副本"
                or created[-1].get("source") != "current"):
            failures.append(f"新建当前参数没有发出正确的 create 命令：{RECEIVED}")
        if not client.evaluate("document.querySelector('#modal-input').hidden"):
            failures.append("新建弹窗关闭后输入框没有藏起来")
        line = stats["dotProfile"] or ""
        if "1.20" not in line or "-8.0" not in line:
            failures.append(f"档案一行没有显示标定时的粗调：{line}")
        if not stats["dotProfileFits"]:
            failures.append("档案一行溢出卡片")

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

        # 第二张：关掉浮层，专门看列表的对齐
        client.evaluate("document.querySelector('.cal-curve-pop').hidden = true")
        time.sleep(0.3)
        shot2 = client.call("Page.captureScreenshot", {"format": "png"})
        data2 = shot2.get("result", {}).get("data", "")
        if data2:
            list_out = out.replace(".png", "-list.png")
            with open(list_out, "wb") as handle:
                handle.write(base64.b64decode(data2))
            print("SCREENSHOT-LIST:", list_out)

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
        if search["visible"] != 1:
            failures.append(f"中文搜索命中 {search['visible']} 条，期望 1 条（左右已合并）")

        # 选中合并卡：模型上必须亮两侧。修复前只按 names[0] 取区域，另一半永远不亮
        pick = json.loads(client.evaluate("""(() => {
            const input = document.querySelector('#cal-search');
            input.value = '';
            input.dispatchEvent(new Event('input'));
            const row = [...document.querySelectorAll('.cal-row')]
                .find(r => r.dataset.name.split(' ').length === 2);
            const names = row.dataset.name.split(' ');
            row.click();
            return JSON.stringify({
                names,
                selected: document.querySelectorAll('.cal-row.selected').length,
                detail: (document.querySelector('#cal-face-detail') || {}).textContent || '',
                canvas3d: !document.querySelector('#cal-canvas').hidden,
                hl: [...document.querySelectorAll('.fb-region.hl')].map(n => n.dataset.region)
            });
        })()"""))
        print("SELECT:", pick)
        expected_regions = []
        for name in pick["names"]:
            for region in SHAPE_REGIONS.get(name, []):
                if region not in expected_regions:
                    expected_regions.append(region)
        if pick["selected"] != 1:
            failures.append(f"点合并卡后高亮行 {pick['selected']} != 1")
        if len(expected_regions) < 2:
            failures.append(f"合并卡 {pick['names']} 没有两侧区域可验")
        missing = [region for region in expected_regions if region not in pick["detail"]]
        if missing:
            failures.append(f"合并卡只高亮了一侧，缺 {missing}（详情：{pick['detail']}）")
        if not pick["canvas3d"] and sorted(pick["hl"]) != sorted(expected_regions):
            failures.append(f"2D 示意图高亮 {sorted(pick['hl'])} != {sorted(expected_regions)}")

        # 镜像：默认开启，点一下关、再点回开；2D 示意图要跟着翻
        mirror = json.loads(client.evaluate("""(() => {
            const btn = document.querySelector('#cal-mirror');
            const svg = document.querySelector('#cal-fallback svg');
            const read = () => ({
                pressed: btn.getAttribute('aria-pressed'),
                on: btn.classList.contains('mirror-on'),
                flipped: !!svg && svg.classList.contains('mirrored'),
                fallback: !document.querySelector('#cal-fallback').hidden
            });
            const initial = read();
            btn.click();
            const off = read();
            btn.click();
            return JSON.stringify({ initial, off, back: read() });
        })()"""))
        print("MIRROR:", mirror)
        if not mirror["initial"]["on"] or mirror["initial"]["pressed"] != "true":
            failures.append(f"镜像按钮默认没开启：{mirror['initial']}")
        if mirror["off"]["on"] or mirror["off"]["pressed"] != "false":
            failures.append(f"镜像按钮点一下没关：{mirror['off']}")
        if mirror["back"] != mirror["initial"]:
            failures.append(f"镜像按钮再点回来不一致：{mirror['back']}")
        if mirror["initial"]["fallback"] and not mirror["initial"]["flipped"]:
            failures.append("2D 示意图没有跟随镜像开关")

        # 镜像必须真的改变画面：只看按钮状态的话，uniform 没上传也能过
        client.call("Page.enable")
        time.sleep(1.5)
        shot_on = client.call("Page.captureScreenshot", {"format": "png"})
        shot_on = shot_on.get("result", {}).get("data", "")
        time.sleep(0.4)
        shot_settle = client.call("Page.captureScreenshot", {"format": "png"})
        shot_settle = shot_settle.get("result", {}).get("data", "")
        client.evaluate("document.querySelector('#cal-mirror').click()")
        time.sleep(0.4)
        shot_off = client.call("Page.captureScreenshot", {"format": "png"})
        shot_off = shot_off.get("result", {}).get("data", "")
        client.evaluate("document.querySelector('#cal-mirror').click()")
        if shot_on != shot_settle:
            print("SKIP-MIRROR-PIXELS: 画面还在缓动，跳过像素比较")
        elif shot_on == shot_off:
            failures.append("切换镜像后画面没变（u_mirror 没生效）")

        # 复位：回默认视角的同时取消选中，卡片与模型高亮一起清掉
        reset = json.loads(client.evaluate("""(() => {
            document.querySelector('#cal-view-reset').click();
            return JSON.stringify({
                selected: document.querySelectorAll('.cal-row.selected').length,
                hl: document.querySelectorAll('.fb-region.hl').length
            });
        })()"""))
        print("RESET:", reset)
        if reset["selected"] != 0 or reset["hl"] != 0:
            failures.append(f"复位没有取消选中：{reset}")

        # 分离一张合并卡：行数 +1，并且出现两张独立的卡
        client.evaluate("""(() => {
            const input = document.querySelector('#cal-search');
            input.value = '';
            input.dispatchEvent(new Event('input'));
        })()""")
        time.sleep(0.3)
        client.evaluate("document.querySelector('.cal-split-btn').click()")
        time.sleep(0.8)
        split = json.loads(client.evaluate("""JSON.stringify({
            rows: document.querySelectorAll('.cal-row').length,
            mergeBtns: document.querySelectorAll('.cal-split-btn.active').length
        })"""))
        print("SPLIT:", split)
        if split["rows"] != 59:
            failures.append(f"分离后行数 {split['rows']} != 59")
        if split["mergeBtns"] != 2:
            failures.append(f"分离后应有 2 个合并按钮，实际 {split['mergeBtns']}")

        # 复原，别把状态留给下一次运行
        client.evaluate("document.querySelector('.cal-split-btn.active').click()")
        time.sleep(0.6)
        back = json.loads(client.evaluate(
            "JSON.stringify({rows: document.querySelectorAll('.cal-row').length})"))
        if back["rows"] != 58:
            failures.append(f"合并回来后行数 {back['rows']} != 58")
    finally:
        process.terminate()
        server.shutdown()
        shutil.rmtree(profile, ignore_errors=True)

    if failures:
        print("FAIL:", "; ".join(failures))
        raise SystemExit(1)
    print("校准页渲染检查通过")


if __name__ == "__main__":
    main()
