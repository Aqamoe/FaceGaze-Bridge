"""fgb-ui（WebView2 外壳）的自动化验收。

通过 WebView2 的远程调试端口（CDP）在页面里跑 JS，检查"页面真的渲染了数据"：
catalog 到了、WebSocket 连上了、参数格子上有非零值、翻页正常。

比截图可靠：截图只能说明"有个窗口"，这里能说明"窗口里的数字是对的"。

用法：
    fgb-cli run --source replay --replay x.fgbrec --panel 8787 \
        --calibration %TEMP%\fgb-probe-calibration.ini \
        --outputs-config %TEMP%\fgb-probe-outputs.ini \
        --outputs-profiles %TEMP%\fgb-probe-profiles
    fgb-ui --port 8787 --debug-port 9222
    python tools/probe_ui.py 9222 8787

注意：校准页和输出设置页都会真的保存。一定要用 --calibration / --outputs-config
指到临时文件，否则探针会覆盖用户默认的 calibration.ini / outputs.ini。

只依赖标准库：CDP 本身就是 WebSocket + JSON，帧编解码与 probe_ipc.py 同一套写法。
"""
import base64
import json
import os
import socket
import struct
import sys
import tempfile
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
            "source: document.getElementById('a-source').textContent,"
            "catalog: typeof state !== 'undefined' ? state.names.length : -1,"
            "values: typeof state !== 'undefined' ? state.values.length : -1,"
            "cells: document.querySelectorAll('.cell').length,"
            "tick_text: document.getElementById('p-tick').textContent,"
            "p95_text: document.getElementById('p-p95').textContent,"
            "mem_text: document.getElementById('p-mem').textContent,"
            "handles_text: document.getElementById('p-handles').textContent,"
            "update_current: document.getElementById('u-current').textContent"
            "})"))
        fps = float(summary["fps"] or 0)
        if summary["connected"] and summary["cells"] > 0 and fps > 0:
            break
        time.sleep(0.7)
    print(f"状态页：{summary}")

    gaze = json.loads(cdp.evaluate(
        "(() => {"
        "  const g = state.gaze || null;"
        "  return JSON.stringify({"
        "    valid: g ? !!g.valid : null,"
        "    left_valid: g ? !!g.left_valid : null,"
        "    right_valid: g ? !!g.right_valid : null,"
        "    fixation: g ? !!g.fixation : null,"
        "    point2d_valid: g ? !!g.point2d_valid : null,"
        "    point_space: g ? (g.point_space || null) : null,"
        "    left_point_valid: g ? !!g.left_point_valid : null,"
        "    right_point_valid: g ? !!g.right_point_valid : null,"
        "    saccade: g ? g.saccade_vel_deg_s : null,"
        "    dir: document.getElementById('g-dir').textContent,"
        "    left_text: document.getElementById('g-eye-left').textContent,"
        "    right_text: document.getElementById('g-eye-right').textContent,"
        "    eye_source_text: document.getElementById('g-eye-source').textContent,"
        "    fixation_text: document.getElementById('g-fixation').textContent,"
        "    dot_left: !document.getElementById('g-dot-left').hidden,"
        "    dot_right: !document.getElementById('g-dot-right').hidden"
        "  });"
        "})()"))
    print(f"注视卡片：{gaze}")

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

    # 角色来源：四个通道各一个下拉，值来自 modules.ini 的 module_id
    roles = json.loads(cdp.evaluate(
        "(() => {"
        "  const selects = [...document.querySelectorAll('.role-select')];"
        "  const instances = (state.modules && state.modules.instances) || [];"
        "  return JSON.stringify({"
        "    selects: selects.length,"
        "    roles: selects.map(node => node.dataset.role),"
        "    values: selects.map(node => node.value),"
        "    options: selects.map(node => node.options.length),"
        "    hosts: document.querySelectorAll('.instance-host input').length,"
        "    host_values: [...document.querySelectorAll('.instance-host input')].map(n => n.checked),"
        "    rows: document.querySelectorAll('.instance-row').length,"
        "    native_instances: instances.filter(i => i.kind === 'native').length,"
        "    restart_buttons: document.querySelectorAll('.instance-row [data-role=restart]').length"
        "  });"
        "})()"))
    print(f"角色来源：{roles}")

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
        "  const status = state.status || {};"
        "  const vmc = status.vmc_out || null;"
        "  const ws = status.ws_out || null;"
        "  const udp = status.udp_out || null;"
        "  return JSON.stringify({"
        "    active: document.querySelector('.page.active').dataset.page,"
        "    text: document.getElementById('o-vmc').textContent,"
        "    enabled: !!(vmc && vmc.enabled),"
        "    target: vmc ? vmc.target : null,"
        "    ws_text: document.getElementById('o-ws').textContent,"
        "    ws_enabled: !!(ws && ws.enabled),"
        "    ws_target: ws ? ws.target : null,"
        "    udp_text: document.getElementById('o-udp').textContent,"
        "    udp_enabled: !!(udp && udp.enabled),"
        "    udp_target: udp ? udp.target : null"
        "  });"
        "})()"))
    print(f"输出页：{output}")

    # 输出设置表单：改 WS 端口/帧率 → 保存 → 状态回读（不触发重启）
    # 引擎必须用 --outputs-config 指向临时文件，否则会写用户的 outputs.ini。
    outputs_form = json.loads(cdp.evaluate(
        "(() => {"
        "  if (!state.status || !state.status.outputs) return JSON.stringify({ready: false});"
        "  const set = (id, value) => { const el = document.getElementById(id);"
        "    el.value = value; el.dispatchEvent(new Event('input', { bubbles: true })); };"
        "  const check = (id, value) => { const el = document.getElementById(id);"
        "    el.checked = value; el.dispatchEvent(new Event('change', { bubbles: true })); };"
        "  set('o-ws-port', '9113');"
        "  set('o-ws-rate', '25');"
        "  check('o-ws-on', true);"
        "  const dirty = state.outputs_dirty;"
        "  document.getElementById('o-save').click();"
        "  return JSON.stringify({ ready: true, dirty_after_edit: dirty });"
        "})()"))
    time.sleep(1.0)
    outputs_saved = json.loads(cdp.evaluate(
        "JSON.stringify({"
        "  dirty: state.outputs_dirty,"
        "  ws: !!(state.status && state.status.outputs && state.status.outputs.ws_enabled),"
        "  port: state.status && state.status.outputs ? state.status.outputs.ws_port : -1,"
        "  rate: state.status && state.status.outputs ? state.status.outputs.ws_rate : -1"
        "})"))
    print(f"输出表单：{outputs_form} → {outputs_saved}")

    # 输出预设：保存 → 列表出现 → 删除。应用会重启引擎，交给 probe_output_profiles.py。
    # 只在预设目录位于临时目录时才写，避免探针在用户默认目录里留垃圾。
    profile_state = json.loads(cdp.evaluate(
        "JSON.stringify({"
        "  dir: (state.output_profiles && state.output_profiles.dir) || '',"
        "  profiles: (state.output_profiles && state.output_profiles.profiles) || [],"
        "  has_select: !!document.getElementById('o-profile-select')"
        "})"))
    profile_ui = {"ran": False, "dir": profile_state.get("dir")}
    profile_name = "UI 探针预设"
    temp_root = tempfile.gettempdir().lower()
    dir_text = (profile_state.get("dir") or "").lower()
    if not profile_state.get("has_select"):
        print("FAIL 输出页缺少预设控件")
        ok = False
    elif temp_root not in dir_text and "fgb-probe" not in dir_text:
        print(f"跳过预设 UI 写操作：预设目录不在临时目录（{profile_state.get('dir')}）")
    else:
        cdp.evaluate(
            "(() => {"
            "  document.getElementById('o-profile-name').value = " + json.dumps(profile_name) + ";"
            "  document.getElementById('o-profile-save').click();"
            "  return true;"
            "})()")
        saved_profile = False
        for _ in range(25):
            time.sleep(0.2)
            current = json.loads(cdp.evaluate(
                "JSON.stringify({"
                "  profiles: (state.output_profiles && state.output_profiles.profiles) || [],"
                "  pending: !!state.profile_pending"
                "})"))
            if profile_name in current["profiles"] and not current["pending"]:
                saved_profile = True
                break
        select_profile = cdp.evaluate(
            "(() => {"
            "  const select = document.getElementById('o-profile-select');"
            "  select.value = " + json.dumps(profile_name) + ";"
            "  select.dispatchEvent(new Event('change'));"
            "  return JSON.stringify({"
            "    apply_disabled: document.getElementById('o-profile-apply').disabled,"
            "    delete_disabled: document.getElementById('o-profile-delete').disabled"
            "  });"
            "})()")
        buttons = json.loads(select_profile)
        cdp.evaluate("document.getElementById('o-profile-apply').click(); true")
        time.sleep(0.2)
        apply_modal = json.loads(cdp.evaluate(
            "JSON.stringify({"
            "  open: !document.getElementById('modal').hidden,"
            "  message: document.getElementById('modal-message').textContent"
            "})"))
        cdp.evaluate("document.getElementById('modal-cancel').click(); true")
        time.sleep(0.2)
        cancelled = json.loads(cdp.evaluate(
            "JSON.stringify({"
            "  modal_hidden: document.getElementById('modal').hidden,"
            "  restarting: !!state.restarting,"
            "  pending: !!state.pending_outputs_restart"
            "})"))

        cdp.evaluate(
            "(() => {"
            "  const select = document.getElementById('o-profile-select');"
            "  select.value = " + json.dumps(profile_name) + ";"
            "  select.dispatchEvent(new Event('change'));"
            "  document.getElementById('o-profile-delete').click();"
            "  return true;"
            "})()")
        time.sleep(0.2)
        modal_open = json.loads(cdp.evaluate("JSON.stringify({open: !document.getElementById('modal').hidden})"))
        cdp.evaluate("document.getElementById('modal-ok').click(); true")
        deleted_profile = False
        for _ in range(25):
            time.sleep(0.2)
            current = json.loads(cdp.evaluate(
                "JSON.stringify({"
                "  profiles: (state.output_profiles && state.output_profiles.profiles) || [],"
                "  pending: !!state.profile_pending"
                "})"))
            if profile_name not in current["profiles"] and not current["pending"]:
                deleted_profile = True
                break
        profile_ui = {
            "ran": True,
            "dir": profile_state.get("dir"),
            "saved": saved_profile,
            "apply_disabled": buttons.get("apply_disabled"),
            "delete_disabled": buttons.get("delete_disabled"),
            "apply_modal": apply_modal.get("open"),
            "apply_modal_names": profile_name in (apply_modal.get("message") or ""),
            "apply_cancelled": cancelled.get("modal_hidden") and not cancelled.get("restarting")
                               and not cancelled.get("pending"),
            "modal_open": modal_open.get("open"),
            "deleted": deleted_profile,
        }
        print(f"输出预设：{profile_ui}")

    # 诊断包：设置页按钮 → 后台打包 → diagnostics 消息 → 文件真的存在
    cdp.evaluate(
        "(() => {"
        "  document.querySelector('.nav-item[data-page=\"settings\"]').click();"
        "  document.getElementById('diag-export').click();"
        "  return true;"
        "})()")
    diag_ui = {"path": "", "done": False}
    for _ in range(40):
        time.sleep(0.5)
        current = json.loads(cdp.evaluate(
            "JSON.stringify({"
            "  path: document.getElementById('diag-path').textContent,"
            "  busy: !document.getElementById('diag-busy').hidden,"
            "  disabled: document.getElementById('diag-export').disabled"
            "})"))
        if not current["busy"] and not current["disabled"] \
                and current["path"] not in ("", "—"):
            diag_ui = {"path": current["path"], "done": True}
            break
    if diag_ui["done"]:
        try:
            with open(diag_ui["path"], "rb") as handle:
                magic = handle.read(2)
            diag_ui["size"] = os.path.getsize(diag_ui["path"])
            diag_ui["magic"] = magic.decode("ascii", "replace")
            if magic != b"PK":
                diag_ui["done"] = False
        except OSError as exc:
            diag_ui["error"] = str(exc)
            diag_ui["done"] = False
        finally:
            try:
                os.remove(diag_ui["path"])
            except OSError:
                pass
    print(f"诊断包：{diag_ui}")
    if not diag_ui.get("done"):
        print("FAIL 界面导出诊断包失败")
        ok = False

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

    # 3D 面部预览（ADR-0021 P3/P4）：模型、morph、区域高亮、2D 降级入口
    face = {}
    for _ in range(24):
        face = json.loads(cdp.evaluate(
            "(() => {"
            "  const m = state.face_map || {};"
            "  const r = state.face_renderer;"
            "  return JSON.stringify({"
            "    mode: state.face_mode,"
            "    error: state.face_error || '',"
            "    vertices: r ? r.model.vertex_count : 0,"
            "    morphs: r ? r.model.morph_names.length : (m.morphs || []).length,"
            "    regions: (m.regions || []).length,"
            "    numeric_only: (m.numeric_only || []).length,"
            "    none_rows: document.querySelectorAll('.cal-model-none').length,"
            "    mapped_rows: document.querySelectorAll('.cal-model-region').length,"
            "    fallback_regions: document.querySelectorAll('#cal-fallback [data-region]').length,"
            "    target_active: r ? [...r.target].filter(v => v > 0.01).length : -1,"
            "    canvas: [document.getElementById('cal-canvas').width,"
            "             document.getElementById('cal-canvas').height]"
            "  });"
            "})()"))
        if face.get("mode") in ("3d", "2d", "error"):
            break
        time.sleep(0.5)
    print(f"3D 预览：{face}")

    face_select = json.loads(cdp.evaluate(
        "(() => {"
        "  const row = document.querySelector('.cal-row[data-name=\"jawopen\"]');"
        "  row.click();"
        "  const r = state.face_renderer;"
        "  return JSON.stringify({"
        "    selected: state.face_selected,"
        "    detail: document.getElementById('cal-face-detail').textContent,"
        "    highlight: r ? r.highlight_target : -1,"
        "    selected_rows: document.querySelectorAll('.cal-row.selected').length"
        "  });"
        "})()"))
    print(f"3D 高亮：{face_select}")

    face_view = json.loads(cdp.evaluate(
        "(() => {"
        "  document.querySelector('[data-view=\"side\"]').click();"
        "  const r = state.face_renderer;"
        "  const yaw = r ? r.yaw : null;"
        "  if (r) { r.reset(); }"
        "  select_face_shape(null);"
        "  return JSON.stringify({yaw, selected: state.face_selected});"
        "})()"))
    print(f"3D 视角：{face_view}")

    # SteamVR 卡片（ADR-0020）：只读检测，探针绝不切换真实自启动
    cdp.evaluate(
        "document.querySelector('.nav-item[data-page=\"settings\"]').click(); true")
    steamvr = {}
    for _ in range(24):
        steamvr = json.loads(cdp.evaluate(
            "(() => {"
            "  const s = state.steamvr || {};"
            "  return JSON.stringify({"
            "    available: !!s.available,"
            "    state: s.state || 'unknown',"
            "    reason: s.reason || '',"
            "    busy: !!s.busy,"
            "    busy_local: !!state.steamvr_busy,"
            "    registered: !!s.registered,"
            "    autolaunch: !!s.autolaunch,"
            "    runtime: s.runtime_dir || '',"
            "    manifest: s.manifest_path || '',"
            "    reason_text: document.getElementById('sv-detail').textContent,"
            "    pill: document.getElementById('sv-state').textContent,"
            "    toggle_disabled: document.getElementById('sv-autolaunch').disabled,"
            "    remove_disabled: document.getElementById('sv-remove').disabled"
            "  });"
            "})()"))
        if (steamvr.get("state") not in ("unknown", "")
                and steamvr.get("reason") not in ("", "not_checked")
                and not steamvr.get("busy") and not steamvr.get("busy_local")):
            break
        time.sleep(0.5)
    print(f"SteamVR：{steamvr}")

    # 改一条映射 → 立即生效 → 保存 → 读回档案。
    # 先恢复默认：探针可能在同一份临时档案上跑第二遍，直接写相同值不会变 dirty。
    cdp.evaluate("send({type:'reset_calibration'}); true")
    time.sleep(0.4)
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
    if summary.get("tick_text") in ("", "—") or summary.get("p95_text") in ("", "—"):
        print("FAIL 状态页没有渲染 tick 耗时分布")
        ok = False
    if "MB" not in (summary.get("mem_text") or "") or not (summary.get("handles_text") or "").strip():
        print("FAIL 状态页没有渲染进程资源（工作集/句柄）")
        ok = False
    if (summary.get("update_current") or "") in ("", "—"):
        print("FAIL 设置页没有显示当前版本")
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
    if roles.get("selects") != 4 or roles.get("roles") != ["expression", "eyes", "gaze", "head"]:
        print(f"FAIL 角色来源没有渲染完整的四个下拉：{roles}")
        ok = False
    if any(count < 1 for count in roles.get("options", [])):
        print(f"FAIL 角色下拉没有选项（至少要有“自动”）：{roles}")
        ok = False
    # 「独立进程」只对原生模块有意义；VRCFT 模块永远跑在 ModuleProcess.exe 里
    if roles.get("hosts") != roles.get("native_instances"):
        print(f"FAIL 独立进程开关应只出现在原生实例上：{roles}")
        ok = False
    # 停止/启动/重启对所有实例都应该可用（进程内原生模块与 VRCFT 也支持重载）
    if roles.get("restart_buttons") != roles.get("rows"):
        print(f"FAIL 每个实例行都应有重启按钮：{roles}")
        ok = False
    if output.get("active") != "output" or not output.get("text"):
        print("FAIL 输出页的 VMC 行没有渲染")
        ok = False
    if output.get("enabled") and output.get("target") not in (output.get("text") or ""):
        print("FAIL VMC 输出行没显示目标地址")
        ok = False
    if output.get("ws_enabled") and output.get("ws_target") not in (output.get("ws_text") or ""):
        print("FAIL WebSocket 输出行没显示目标地址")
        ok = False
    if output.get("udp_enabled") and output.get("udp_target") not in (output.get("udp_text") or ""):
        print("FAIL UDP 输出行没显示目标地址")
        ok = False
    if not outputs_form.get("ready"):
        print("FAIL 输出设置表单没有拿到 status.outputs")
        ok = False
    elif not outputs_form.get("dirty_after_edit"):
        print("FAIL 改输出表单后没有标记未保存")
        ok = False
    elif outputs_saved.get("dirty"):
        print("FAIL 保存输出设置后 dirty 仍为真")
        ok = False
    elif not outputs_saved.get("ws") or outputs_saved.get("port") != 9113 \
            or outputs_saved.get("rate") != 25:
        print(f"FAIL 保存的输出设置没有回读成功：{outputs_saved}")
        ok = False
    if profile_ui.get("ran") and not profile_ui.get("saved"):
        print("FAIL 输出预设没有保存进列表")
        ok = False
    if profile_ui.get("ran") and (profile_ui.get("apply_disabled")
                                  or profile_ui.get("delete_disabled")):
        print("FAIL 选中预设后应用/删除按钮仍被禁用")
        ok = False
    if profile_ui.get("ran") and not (profile_ui.get("apply_modal")
                                      and profile_ui.get("apply_modal_names")):
        print("FAIL 应用预设没有弹出带名称的确认框")
        ok = False
    if profile_ui.get("ran") and not profile_ui.get("apply_cancelled"):
        print("FAIL 取消应用预设后仍进入了重启流程")
        ok = False
    if profile_ui.get("ran") and not profile_ui.get("modal_open"):
        print("FAIL 删除预设没有弹出确认框")
        ok = False
    if profile_ui.get("ran") and not profile_ui.get("deleted"):
        print("FAIL 输出预设删除后仍留在列表里")
        ok = False
    # 有方向或有 2D 点都算有数据：只给点的模块（PICO Connect 私有通道）valid 为假
    if not (gaze.get("valid") or gaze.get("point2d_valid")):
        print("FAIL 注视卡片没有收到眼追数据（回放应带方向或 2D 点）")
        ok = False
    elif gaze.get("valid") and (gaze.get("dir") or "").strip() in ("", "—"):
        print("FAIL 注视方向没有渲染")
        ok = False
    # 左右眼必须分开渲染：有数据的那只眼要有自己的文本和圆点
    elif not (gaze.get("left_valid") or gaze.get("right_valid")
              or gaze.get("left_point_valid") or gaze.get("right_point_valid")):
        print("FAIL 注视数据里左右眼都无效，逐眼显示无法验证")
        ok = False
    else:
        for side in ("left", "right"):
            has_eye = gaze.get(f"{side}_valid") or gaze.get(f"{side}_point_valid")
            if not has_eye:
                continue
            text = (gaze.get(f"{side}_text") or "").strip()
            if text in ("", "—"):
                print(f"FAIL {side} 眼没有单独渲染")
                ok = False
            # 有 2D 点就必须显示点，而不是模块填的占位方向；
            # 视口坐标写 "2D ("，来源原始坐标写 "2D~ ("（ADR-0033）
            elif gaze.get(f"{side}_point_valid"):
                expect = "2D (" if gaze.get("point_space") == "viewport" else "2D~ ("
                if not text.startswith(expect):
                    print(f"FAIL {side} 眼 2D 点显示与坐标空间不符"
                          f"（point_space={gaze.get('point_space')}）：{text}")
                    ok = False
            if not gaze.get(f"dot_{side}"):
                print(f"FAIL {side} 眼圆点没有显示")
                ok = False
        # 没数据的眼必须显示 —，不能留着上一帧的残值
        for side in ("left", "right"):
            has_eye = gaze.get(f"{side}_valid") or gaze.get(f"{side}_point_valid")
            if not has_eye and (gaze.get(f"{side}_text") or "").strip() != "—":
                print(f"FAIL {side} 眼无效时仍显示了数据：{gaze.get(f'{side}_text')}")
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
    if face.get("mode") not in ("3d", "2d"):
        print(f"FAIL 3D 预览没有就绪：mode={face.get('mode')} error={face.get('error')}")
        ok = False
    if face.get("morphs", 0) < 24 or face.get("regions", 0) < 20:
        print("FAIL 模型 morph/区域数量不足（检查 .fgbface 与 face_map）")
        ok = False
    if face.get("numeric_only", 0) <= 0 or face.get("none_rows") != face.get("numeric_only"):
        print("FAIL 「仅数值」标记数与 face_map.numeric_only 不一致")
        ok = False
    if face.get("mapped_rows", 0) <= 0:
        print("FAIL 没有任何形状显示模型区域")
        ok = False
    if face.get("fallback_regions", 0) <= 10:
        print("FAIL 2D 降级示意图缺少区域节点")
        ok = False
    if face.get("mode") == "3d" and (face.get("vertices", 0) < 1000
                                     or face.get("target_active", 0) <= 0
                                     or min(face.get("canvas", [0, 0])) <= 1):
        print("FAIL 3D 渲染器没有顶点/morph 权重/画布尺寸")
        ok = False
    if face_select.get("selected") != "JawOpen" or not face_select.get("highlight"):
        print("FAIL 选中 JawOpen 没有高亮区域")
        ok = False
    if face_select.get("selected_rows") != 1:
        print("FAIL 选中行数不是 1（行高亮状态重复或丢失）")
        ok = False
    if "jaw" not in (face_select.get("detail") or "").lower():
        print("FAIL 选中 JawOpen 后没有显示区域说明")
        ok = False
    if face_view.get("yaw") is None or abs(float(face_view.get("yaw")) - 1.5707963) > 0.01:
        print("FAIL 侧面预设没有把相机转到 90°")
        ok = False
    if face_view.get("selected") is not None:
        print("FAIL 清除选中没有生效")
        ok = False
    if not steamvr.get("available"):
        print("FAIL 设置页没有提供 SteamVR 控制")
        ok = False
    if steamvr.get("state") in ("unknown", "") or steamvr.get("busy") or steamvr.get("busy_local"):
        print("FAIL SteamVR 检测没有完成（服务线程未回状态）")
        ok = False
    if steamvr.get("state") in ("not_installed", "not_running", "error"):
        if not steamvr.get("toggle_disabled"):
            print("FAIL SteamVR 不可用时自启动开关没有禁用")
            ok = False
    if steamvr.get("state") in ("off", "on") and steamvr.get("toggle_disabled"):
        print("FAIL SteamVR 可用时自启动开关被禁用")
        ok = False
    if steamvr.get("state") != "not_installed" and not steamvr.get("runtime"):
        print("FAIL SteamVR 运行库路径为空")
        ok = False
    if not steamvr.get("manifest") or not steamvr.get("reason_text"):
        print("FAIL SteamVR 卡片缺少 manifest 或原因文案")
        ok = False

    print("PASS" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
