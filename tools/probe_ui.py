"""fgb-ui（WebView2 外壳）的自动化验收。

通过 WebView2 的远程调试端口（CDP）在页面里跑 JS，检查"页面真的渲染了数据"：
catalog 到了、WebSocket 连上了、参数格子上有非零值、翻页正常。

比截图可靠：截图只能说明"有个窗口"，这里能说明"窗口里的数字是对的"。

用法：
    fgb-cli run --source replay --replay x.fgbrec --panel 8787
    fgb-ui --port 8787 --debug-port 9222
    python tools/probe_ui.py 9222 8787

只依赖标准库：CDP 本身就是 WebSocket + JSON，帧编解码与 probe_ipc.py 同一套写法。
"""
import base64
import json
import os
import socket
import struct
import sys
import time
import urllib.request

CDP_PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9222
ENGINE_PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8787


def http_json(path):
    with urllib.request.urlopen(f"http://127.0.0.1:{CDP_PORT}{path}", timeout=5) as response:
        return json.loads(response.read())


def handshake(sock, host, path):
    key = base64.b64encode(os.urandom(16)).decode()
    request = (
        f"GET {path} HTTP/1.1\r\n"
        f"Host: {host}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n"
    )
    sock.sendall(request.encode())
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = sock.recv(4096)
        if not chunk:
            raise RuntimeError("握手失败")
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


class Cdp:
    def __init__(self, url):
        _, rest = url.split("://", 1)
        host_port, path = rest.split("/", 1)
        host, port = host_port.split(":")
        self.sock = socket.create_connection((host, int(port)), timeout=5)
        self.buffer = handshake(self.sock, host_port, "/" + path)
        self.next_id = 0

    def messages(self, deadline):
        while True:
            self.sock.settimeout(0.3)
            try:
                chunk = self.sock.recv(65536)
                if not chunk:
                    raise RuntimeError("CDP 连接关闭")
                self.buffer += chunk
            except socket.timeout:
                pass
            while True:
                frame = decode(self.buffer)
                if frame is None:
                    break
                _, payload, consumed = frame
                self.buffer = self.buffer[consumed:]
                yield json.loads(payload)
            if time.time() > deadline:
                return

    def evaluate(self, expression, timeout=6.0):
        self.next_id += 1
        request_id = self.next_id
        send_text(self.sock, json.dumps({
            "id": request_id,
            "method": "Runtime.evaluate",
            "params": {"expression": expression, "returnByValue": True, "awaitPromise": True},
        }))
        deadline = time.time() + timeout
        for message in self.messages(deadline):
            if message.get("id") != request_id:
                continue
            if "error" in message:
                raise RuntimeError(f"CDP 错误：{message['error']}")
            result = message["result"]
            if result.get("exceptionDetails"):
                raise RuntimeError(f"页面异常：{result['exceptionDetails']}")
            return result.get("result", {}).get("value")
        raise RuntimeError("CDP 评估超时")


def find_page():
    deadline = time.time() + 20
    while time.time() < deadline:
        try:
            for item in http_json("/json/list"):
                if item.get("type") == "page" and f"127.0.0.1:{ENGINE_PORT}" in item.get("url", ""):
                    return item
        except OSError:
            pass
        time.sleep(0.5)
    return None


def main():
    target = find_page()
    if target is None:
        print("FAIL 没找到 WebView2 页面目标（--debug-port 是否传了？）")
        return 1
    print(f"页面 URL: {target['url']}")

    cdp = Cdp(target["webSocketDebuggerUrl"])

    deadline = time.time() + 15
    summary = {}
    while time.time() < deadline:
        summary = json.loads(cdp.evaluate(
            "JSON.stringify({"
            "title: document.title,"
            "connected: typeof state !== 'undefined' ? state.connected : null,"
            "link: document.getElementById('link-text').textContent,"
            "fps: document.getElementById('m-fps').textContent,"
            "avatar: document.getElementById('a-name').textContent,"
            "catalog: typeof state !== 'undefined' ? state.names.length : -1,"
            "values: typeof state !== 'undefined' ? state.values.length : -1,"
            "cells: document.querySelectorAll('.cell').length"
            "})"))
        fps = float(summary["fps"] or 0)
        if summary["connected"] and summary["cells"] > 0 and fps > 0:
            break
        time.sleep(0.7)
    print(f"状态页：{summary}")

    params = json.loads(cdp.evaluate(
        "(() => {"
        "  document.querySelector('.nav-item[data-page=\"params\"]').click();"
        "  const cells = [...document.querySelectorAll('.cell')];"
        "  return JSON.stringify({"
        "    active: document.querySelector('.page.active').dataset.page,"
        "    visible: cells.filter(cell => !cell.hidden).length,"
        "    nonzero: cells.filter(cell => Number(cell.querySelector('.cell-val').textContent) > 0).length,"
        "    title: document.getElementById('page-title').textContent"
        "  });"
        "})()"))
    print(f"参数页：{params}")

    modules = json.loads(cdp.evaluate(
        "(() => {"
        "  document.querySelector('.nav-item[data-page=\"modules\"]').click();"
        "  const cards = [...document.querySelectorAll('.module-card')];"
        "  return JSON.stringify({"
        "    active: document.querySelector('.page.active').dataset.page,"
        "    cards: cards.length,"
        "    expected: state.modules ? state.modules.items.length : -1,"
        "    names: cards.map(card => card.querySelector('strong').textContent),"
        "    chips: cards.map(card => card.querySelector('[data-role=chip]').textContent),"
        "    config: document.getElementById('mod-store').textContent"
        "  });"
        "})()"))
    print(f"模块页：{modules}")

    # 运行中的模块实例：多实例时不能只显示第一个
    running = json.loads(cdp.evaluate(
        "(() => {"
        "  document.querySelector('.nav-item[data-page=\"modules\"]').click();"
        "  const rows = [...document.querySelectorAll('.running-row')];"
        "  return JSON.stringify({"
        "    rows: rows.length,"
        "    expected: state.status && state.status.modules ? state.status.modules.length : 0,"
        "    names: rows.map(row => row.querySelector('.running-name').textContent),"
        "    chips: rows.map(row => row.querySelector('.chip').textContent)"
        "  });"
        "})()"))
    print(f"运行实例：{running}")

    # 输入源卡片：多源合并后，"谁在喂哪一路"必须能在界面上看见
    sources = json.loads(cdp.evaluate(
        "(() => {"
        "  document.querySelector('.nav-item[data-page=\"status\"]').click();"
        "  const rows = [...document.querySelectorAll('.source-row')];"
        "  return JSON.stringify({"
        "    rows: rows.length,"
        "    expected: (state.status && state.status.sources ? state.status.sources.length : 0),"
        "    names: rows.map(row => row.querySelector('.source-name').textContent),"
        "    live: rows.map(row => [...row.querySelectorAll('.source-chip.on')].length),"
        "    frames: state.status ? state.status.frames_in : 0"
        "  });"
        "})()"))
    print(f"输入源：{sources}")

    # 三语切换：文案表是三份，切换后必须整页重画（标题、pill、卡片都在 JS 里生成）
    langs = json.loads(cdp.evaluate(
        "(() => {"
        "  const snap = () => ({"
        "    nav: [...document.querySelectorAll('.nav-item')].map(n => n.textContent.trim()),"
        "    title: document.getElementById('page-title').textContent,"
        "    stored: localStorage.getItem('fgb.lang')"
        "  });"
        "  const before = snap();"
        "  set_lang('en');"
        "  const en = snap();"
        "  set_lang('ja');"
        "  const ja = snap();"
        "  set_lang('zh');"
        "  return JSON.stringify({before, en, ja});"
        "})()"))
    print(f"语言：{langs}")

    # 输出页：VMC 第二路输出要能看见"发到哪、发了多少"
    output = json.loads(cdp.evaluate(
        "(() => {"
        "  document.querySelector('.nav-item[data-page=\"output\"]').click();"
        "  const vmc = state.status ? state.status.vmc_out : null;"
        "  return JSON.stringify({"
        "    active: document.querySelector('.page.active').dataset.page,"
        "    text: document.getElementById('o-vmc').textContent,"
        "    enabled: !!(vmc && vmc.enabled),"
        "    target: vmc ? vmc.target : null"
        "  });"
        "})()"))
    print(f"输出页：{output}")

    # 校准页（ADR-0021 P1/P2）：档案列表、30 Hz 预览、改动实时生效、保存/重载
    cal_page = json.loads(cdp.evaluate(
        "(() => {"
        "  document.querySelector('.nav-item[data-page=\"calibration\"]').click();"
        "  const c = state.calibration || {};"
        "  return JSON.stringify({"
        "    active: document.querySelector('.page.active').dataset.page,"
        "    rows: document.querySelectorAll('.cal-row').length,"
        "    expected: c.shapes ? c.shapes.length : -1,"
        "    path: c.path || '',"
        "    dirty: !!c.dirty,"
        "    subscribed: !!state.cal_preview_on"
        "  });"
        "})()"))
    print(f"校准页：{cal_page}")

    time.sleep(1.2)
    cal_live = json.loads(cdp.evaluate(
        "(() => {"
        "  const row = document.querySelector('.cal-row');"
        "  const preview = state.preview || {};"
        "  return JSON.stringify({"
        "    seq: (preview.raw || []).length ? preview.seq : -1,"
        "    raw_len: (preview.raw || []).length,"
        "    out_len: (preview.out || []).length,"
        "    raw_text: row ? row.querySelector('.cal-raw').textContent : '',"
        "    out_text: row ? row.querySelector('.cal-out').textContent : '',"
        "    eye: preview.eye ? preview.eye.valid : null"
        "  });"
        "})()"))
    print(f"校准预览：{cal_live}")

    # 改一条映射 → 立即生效 → 保存 → 读回档案
    cal_edit = json.loads(cdp.evaluate(
        "(() => {"
        "  send({type:'set_calibration_shape', name:'JawOpen', lo:0.1, hi:0.9, active:true});"
        "  return JSON.stringify({sent: true});"
        "})()"))
    time.sleep(0.6)
    cal_applied = json.loads(cdp.evaluate(
        "(() => {"
        "  const c = state.calibration || {};"
        "  const jaw = (c.shapes || []).find((s) => s.name === 'JawOpen') || {};"
        "  return JSON.stringify({"
        "    active: !!jaw.active, lo: jaw.lo, hi: jaw.hi, dirty: !!c.dirty,"
        "    count: c.active"
        "  });"
        "})()"))
    print(f"校准改动：{cal_edit} → {cal_applied}")

    # 「取当前视线为中心」：校准命令与预览取样必须共用同一份原始值
    cdp.evaluate("send({type:'capture_calibration', shape:'', which:'center'}); true")
    time.sleep(0.6)
    cal_eye = json.loads(cdp.evaluate(
        "(() => { const c = state.calibration || {}; const eye = c.eye || {};"
        "  return JSON.stringify({active: !!eye.active, cx: eye.center_x, cy: eye.center_y,"
        "    dirty: !!c.dirty}); })()"))
    print(f"校准视线：{cal_eye}")

    cdp.evaluate("send({type:'save_calibration'}); true")
    time.sleep(0.8)
    cal_saved = json.loads(cdp.evaluate(
        "(() => {"
        "  const c = state.calibration || {};"
        "  return JSON.stringify({dirty: !!c.dirty, loaded: !!c.loaded, path: c.path || '',"
        "    count: c.active});"
        "})()"))
    saved_exists = bool(cal_saved.get("path")) and os.path.exists(cal_saved["path"])
    print(f"校准保存：{cal_saved} 文件存在={saved_exists}")

    cdp.evaluate("send({type:'reset_calibration'}); true")
    time.sleep(0.5)
    cal_reset = json.loads(cdp.evaluate(
        "(() => { const c = state.calibration || {};"
        "  return JSON.stringify({dirty: !!c.dirty, count: c.active}); })()"))
    cdp.evaluate("send({type:'reload_calibration'}); true")
    time.sleep(0.5)
    cal_reloaded = json.loads(cdp.evaluate(
        "(() => { const c = state.calibration || {};"
        "  return JSON.stringify({dirty: !!c.dirty, count: c.active}); })()"))
    print(f"校准重置/重载：{cal_reset} → {cal_reloaded}")

    # 离开校准页后必须停推预览（引擎热路径回到零额外开销）
    cdp.evaluate(
        "document.querySelector('.nav-item[data-page=\"status\"]').click(); true")
    time.sleep(0.5)
    seq_before = json.loads(cdp.evaluate(
        "JSON.stringify({seq: (state.preview || {}).seq || 0,"
        " subscribed: !!state.cal_preview_on})"))
    time.sleep(0.6)
    seq_after = json.loads(cdp.evaluate(
        "JSON.stringify({seq: (state.preview || {}).seq || 0,"
        " subscribed: !!state.cal_preview_on})"))
    cal_stopped = {
        "subscribed": seq_after.get("subscribed"),
        "seq_stopped": seq_before.get("seq") == seq_after.get("seq"),
        "seq": seq_after.get("seq"),
    }
    print(f"校准预览停止：{cal_stopped}")

    logs = json.loads(cdp.evaluate(
        "(() => {"
        "  document.querySelector('.nav-item[data-page=\"logs\"]').click();"
        "  return JSON.stringify({"
        "    active: document.querySelector('.page.active').dataset.page,"
        "    lines: document.querySelectorAll('.log-line').length"
        "  });"
        "})()"))
    print(f"日志页：{logs}")

    ok = True
    if not summary.get("connected"):
        print("FAIL 页面没有连上引擎 WebSocket")
        ok = False
    if summary.get("catalog") != summary.get("values") or summary.get("cells") != summary.get("catalog"):
        print("FAIL catalog / values / DOM 格子数量不一致")
        ok = False
    if params.get("active") != "params" or params.get("visible", 0) == 0:
        print("FAIL 参数页没有渲染出格子")
        ok = False
    if params.get("nonzero", 0) == 0:
        print("FAIL 参数值全为 0，说明数据没有进到页面")
        ok = False
    if modules.get("active") != "modules" or modules.get("cards") != modules.get("expected"):
        print("FAIL 模块页与 catalog 不一致（期望渲染的卡片数对不上）")
        ok = False
    if running.get("rows") != running.get("expected"):
        print("FAIL 运行实例列表与 status.modules 不一致")
        ok = False
    if output.get("active") != "output" or not output.get("text"):
        print("FAIL 输出页的 VMC 行没有渲染")
        ok = False
    if output.get("enabled") and output.get("target") not in (output.get("text") or ""):
        print("FAIL VMC 输出行没显示目标地址")
        ok = False
    if sources.get("rows") != sources.get("expected"):
        print("FAIL 输入源卡片行数与 status.sources 不一致")
        ok = False
    # 有帧进来说明至少应该有一路被选中；一帧都没有（例如模块起不来）时全灰是对的
    if sources.get("frames", 0) > 0 and sources.get("rows", 0) > 0 and all(
            count == 0 for count in sources.get("live", [])):
        print("FAIL 有输入帧但没有任何源被标记为在用")
        ok = False
    if langs["en"]["nav"][0] != "Status" or langs["ja"]["nav"][0] != "状態":
        print("FAIL 三语文案没生效（检查 ui/i18n.js 与 set_lang）")
        ok = False
    if langs["en"]["stored"] != "en" or langs["ja"]["stored"] != "ja":
        print("FAIL 语言选择没写进 localStorage")
        ok = False
    if logs.get("active") != "logs" or logs.get("lines", 0) == 0:
        print("FAIL 日志页没有内容（新连接应补发最近日志）")
        ok = False
    if cal_page.get("active") != "calibration" or cal_page.get("rows") != cal_page.get("expected"):
        print("FAIL 校准页形状行数与 calibration.shapes 不一致")
        ok = False
    if not cal_page.get("subscribed"):
        print("FAIL 进入校准页没有订阅预览")
        ok = False
    if cal_live.get("seq", -1) < 0 or cal_live.get("raw_len") != 88 or cal_live.get("out_len") != 88:
        print("FAIL 校准预览没有收到 88 个形状的 raw/out")
        ok = False
    if cal_live.get("raw_text") in ("", "—", "-"):
        print("FAIL 校准行没有显示原始值")
        ok = False
    if not cal_applied.get("active") or abs(float(cal_applied.get("lo", 0)) - 0.1) > 0.01 \
            or abs(float(cal_applied.get("hi", 0)) - 0.9) > 0.01:
        print("FAIL 校准改动没有实时生效（set_calibration_shape）")
        ok = False
    if not cal_applied.get("dirty"):
        print("FAIL 改动后没有标记未保存")
        ok = False
    if not cal_eye.get("active"):
        print("FAIL 「取当前视线为中心」没有启用视线校准")
        ok = False
    if cal_saved.get("dirty") or not saved_exists:
        print("FAIL 保存后 dirty 仍为真，或档案文件没落盘")
        ok = False
    if not cal_reset.get("dirty") or cal_reset.get("count") != 0:
        print("FAIL 恢复默认没有生效")
        ok = False
    if cal_reloaded.get("dirty") or cal_reloaded.get("count") != 1:
        print("FAIL 重新加载没有回到磁盘上的档案")
        ok = False
    if cal_stopped.get("subscribed") or not cal_stopped.get("seq_stopped"):
        print("FAIL 离开校准页后仍在推预览")
        ok = False

    print("PASS" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
