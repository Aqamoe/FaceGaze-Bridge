"""五路 Sink 并发压测探针（roadmap 4.9）。

同时开 VRChat OSC / VMC / 共享内存 / WebSocket JSON / UDP 二进制，用 VMC 输入
灌满 120 Hz 引擎，测量每一路的实际帧率与 UDP 端到端延迟；再跑一遍只开
VRChat + UDP 的基线，比较"多开 3 路"带来的延迟增量。

用法：
    python tools/probe_sinks_stress.py
    python tools/probe_sinks_stress.py --seconds 8 --cli build/test/bin/Debug/fgb-cli.exe

不需要硬件，也不需要 VRChat；面板/输出都绑临时端口，不会碰用户配置。
"""
import argparse
import base64
import json
import math
import mmap
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request

FRAME_SIZE = 592  # sdk/fgb_udp_frame.h
SHM_BLOCK_SIZE = 1200  # FgbSharedBlockV1：32 字节头 + 2 × 584 字节槽

VMC_ARG = {
    "/VMC/Ext/Blend/Val": b",sf\x00",
    "/VMC/Ext/Blend/Apply": b",\x00\x00\x00",
}


def pad_string(text):
    raw = text.encode("utf-8") + b"\x00"
    while len(raw) % 4:
        raw += b"\x00"
    return raw


def vmc_blend(name, value):
    return (pad_string("/VMC/Ext/Blend/Val") + VMC_ARG["/VMC/Ext/Blend/Val"] +
            pad_string(name) + struct.pack(">f", value))


def vmc_apply():
    return pad_string("/VMC/Ext/Blend/Apply") + VMC_ARG["/VMC/Ext/Blend/Apply"]


def free_tcp_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def free_udp_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def bind_udp():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", 0))
    sock.settimeout(0)
    return sock, sock.getsockname()[1]


def recv_exact(sock, count):
    data = b""
    while len(data) < count:
        chunk = sock.recv(count - len(data))
        if not chunk:
            raise ConnectionError("WS 连接被关闭")
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


class WsClient:
    def __init__(self, port):
        self.sock = None
        self.frames = 0
        self.feed = 0
        self.error = None
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self._run, args=(port,), daemon=True)
        self.thread.start()

    def _run(self, port):
        try:
            sock = socket.create_connection(("127.0.0.1", port), timeout=5)
            key = base64.b64encode(os.urandom(16)).decode()
            sock.sendall(
                (f"GET /ws HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\n"
                 "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                 f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
            head = b""
            while b"\r\n\r\n" not in head:
                head += sock.recv(4096)
            if not head.startswith(b"HTTP/1.1 101"):
                self.error = f"WS Upgrade 失败：{head[:80]!r}"
                return
            sock.settimeout(0.2)
            self.sock = sock
            while not self.stop.is_set():
                try:
                    opcode, payload = recv_frame(sock)
                except socket.timeout:
                    continue
                if opcode == 0x9:
                    sock.sendall(bytes([0x8A, 0x80]) + os.urandom(4))
                    continue
                if opcode == 0x8:
                    break
                if opcode != 0x1:
                    continue
                self.frames += 1
                if self.feed == 0:
                    try:
                        self.feed = 1 if json.loads(payload).get("shapes") else 0
                    except json.JSONDecodeError:
                        self.error = "WS 收到非 JSON 帧"
                        return
        except Exception as exc:  # noqa: BLE001 - 探针要把原因带回来
            self.error = str(exc)

    def close(self):
        self.stop.set()
        if self.sock is not None:
            try:
                self.sock.close()
            except OSError:
                pass
        self.thread.join(timeout=1)


class VmcFeeder(threading.Thread):
    def __init__(self, port, stop):
        super().__init__(daemon=True)
        self.target = ("127.0.0.1", port)
        self.stop = stop
        self.sent = 0

    def run(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        period = 1.0 / 120.0
        phase = 0.0
        next_at = time.perf_counter()
        while not self.stop.is_set():
            jaw = 0.5 + 0.45 * math.sin(phase)
            smile = 0.5 + 0.45 * math.cos(phase)
            brow = 0.5 + 0.4 * math.sin(phase * 0.5)
            sock.sendto(vmc_blend("jawOpen", jaw), self.target)
            sock.sendto(vmc_blend("mouthSmileLeft", smile), self.target)
            sock.sendto(vmc_blend("browInnerUp", brow), self.target)
            sock.sendto(vmc_apply(), self.target)
            self.sent += 1
            phase += 0.05
            next_at += period
            delay = next_at - time.perf_counter()
            if delay > 0:
                time.sleep(delay)
            else:
                next_at = time.perf_counter()
        sock.close()


def http_json(port, path):
    with urllib.request.urlopen(f"http://127.0.0.1:{port}{path}", timeout=2) as resp:
        return json.loads(resp.read().decode("utf-8"))


def read_shm_seq(name):
    try:
        mapping = mmap.mmap(-1, SHM_BLOCK_SIZE, tagname=f"Local\\FGB_Shm_{name}")
    except OSError:
        return None
    try:
        return struct.unpack_from("<Q", mapping, 16)[0]
    finally:
        mapping.close()


def run_case(cli, seconds, five_sinks):
    vrchat_sock, vrchat_port = bind_udp()
    vmc_sock, vmc_port = bind_udp()
    udp_sock, udp_port = bind_udp()
    panel_port = free_tcp_port()
    ws_port = free_tcp_port()
    vmc_in_port = free_udp_port()
    shm_name = f"probe-stress-{os.getpid()}-{int(time.time())}"

    tmpdir = tempfile.mkdtemp(prefix="fgb-stress-")
    outputs_path = os.path.join(tmpdir, "outputs.ini")
    calibration_path = os.path.join(tmpdir, "calibration.ini")

    args = [cli, "run", "--source", "vmc", "--vmc-port", str(vmc_in_port),
            "--vrchat", f"127.0.0.1:{vrchat_port}",
            "--udp-out", f"127.0.0.1:{udp_port}", "--udp-out-rate", "90",
            "--panel", str(panel_port),
            "--no-avatar-discovery", "--no-oscquery", "--no-saved-outputs",
            "--outputs-config", outputs_path, "--calibration", calibration_path]
    if five_sinks:
        args += ["--vmc-out", f"127.0.0.1:{vmc_port}",
                 "--shm", shm_name,
                 "--ws-out", str(ws_port), "--ws-out-rate", "90"]

    proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    ws = None
    feeder = None
    stop = threading.Event()
    try:
        deadline = time.time() + 15
        while time.time() < deadline:
            try:
                http_json(panel_port, "/healthz")
                break
            except Exception:  # noqa: BLE001 - 引擎还没起来
                if proc.poll() is not None:
                    raise RuntimeError("引擎启动失败（进程已退出）")
                time.sleep(0.1)
        else:
            raise RuntimeError("等待控制面超时")

        if five_sinks:
            ws = WsClient(ws_port)
        feeder = VmcFeeder(vmc_in_port, stop)
        feeder.start()

        time.sleep(0.5)  # 预热
        # 预热期间的包还堆在 socket 缓冲里，不清掉会把测得的帧率抬高
        for sock in (vrchat_sock, vmc_sock, udp_sock):
            while True:
                try:
                    sock.recvfrom(4096)
                except (BlockingIOError, OSError):
                    break

        shm_start = read_shm_seq(shm_name) if five_sinks else None
        ws_start = ws.frames if ws is not None else 0
        status_before = http_json(panel_port, "/api/status")

        counts = {"vrchat": 0, "vmc": 0, "udp": 0}
        latencies = []
        start = time.time()
        deadline = start + seconds
        while time.time() < deadline:
            for key, sock in (("vrchat", vrchat_sock), ("vmc", vmc_sock), ("udp", udp_sock)):
                while True:
                    try:
                        packet, _ = sock.recvfrom(4096)
                    except BlockingIOError:
                        break
                    except OSError:
                        break
                    counts[key] += 1
                    if key == "udp" and len(packet) == FRAME_SIZE:
                        # 偏移 16 是 capture_ts（来源时间，可能完全不同域）；
                        # publish_ts 在偏移 24
                        publish = struct.unpack_from("<Q", packet, 24)[0]
                        latencies.append(time.monotonic_ns() - publish)
            time.sleep(0.001)
        elapsed = time.time() - start

        status_after = http_json(panel_port, "/api/status")
        shm_end = read_shm_seq(shm_name) if five_sinks else None
        ws_frames = (ws.frames - ws_start) if ws is not None else 0
        ws_error = ws.error if ws is not None else None

        if latencies:
            # 两端都走 QPC，理论上零偏移；取最小样本校准，剩下的就是排程/排队延迟
            base = min(latencies)
            excess = sorted(v - base for v in latencies)
            p50 = excess[len(excess) // 2] / 1e6
            p95 = excess[min(len(excess) - 1, int(len(excess) * 0.95))] / 1e6
            max_ms = excess[-1] / 1e6
        else:
            p50 = p95 = max_ms = -1.0

        return {
            "five": five_sinks,
            "elapsed": elapsed,
            "rates": {k: v / elapsed for k, v in counts.items()},
            "ws": ws_frames / elapsed,
            "ws_ok": bool(ws and ws.feed),
            "ws_error": ws_error,
            # 共享内存的 sequence 是 seqlock 计数：每帧 +2（奇/偶各一次）
            "shm": ((shm_end - shm_start) / 2 / elapsed
                    if five_sinks and shm_end is not None else None),
            "lat_p50": p50,
            "lat_p95": p95,
            "lat_max": max_ms,
            "samples": len(latencies),
            "ticks": status_after["ticks"] - status_before["ticks"],
            "late": status_after["late_ticks"] - status_before["late_ticks"],
            "tick_avg": status_after.get("tick_work", {}).get("avg_us", 0.0),
            "tick_p95": status_after.get("tick_work", {}).get("p95_us", 0.0),
        }
    finally:
        stop.set()
        if feeder is not None:
            feeder.join(timeout=1)
        if ws is not None:
            ws.close()
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        for sock in (vrchat_sock, vmc_sock, udp_sock):
            sock.close()


def report(case):
    label = "五路" if case["five"] else "基线(VRChat+UDP)"
    rate = case["rates"]
    print(f"\n[{label}] {case['elapsed']:.1f}s")
    print(f"  VRChat OSC 数据报 {rate['vrchat']:.1f}/s")
    if case["five"]:
        print(f"  VMC {rate['vmc']:.1f}/s  SHM {case['shm']:.1f}/s  "
              f"WS {case['ws']:.1f}/s")
    print(f"  UDP 二进制 {rate['udp']:.1f}/s（样本 {case['samples']}）")
    print(f"  UDP 端到端延迟（含 Python 调度，仅参考）"
          f" p50={case['lat_p50']:.3f} ms  p95={case['lat_p95']:.3f} ms  "
          f"max={case['lat_max']:.3f} ms")
    print(f"  引擎 tick 工作 avg={case['tick_avg']:.1f} µs p95={case['tick_p95']:.1f} µs"
          f"  ticks={case['ticks']} late={case['late']}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--seconds", type=float, default=5.0)
    parser.add_argument("--cli", default=os.path.join("build", "test", "bin", "Debug", "fgb-cli.exe"))
    args = parser.parse_args()
    if not os.path.isfile(args.cli):
        print(f"FAIL 找不到引擎：{args.cli}")
        return 1

    baseline = run_case(args.cli, args.seconds, five_sinks=False)
    stress = run_case(args.cli, args.seconds, five_sinks=True)
    report(baseline)
    report(stress)

    ok = True
    if not (85 <= stress["rates"]["udp"] <= 95):
        print(f"FAIL UDP 帧率偏离 90 Hz（{stress['rates']['udp']:.1f}/s）")
        ok = False
    # VMC 输出在生产配置里是 60 Hz（VMC 对端普遍按 60 Hz 消费）；
    # 90 Hz 的同频压力在 tests/test_sinks_stress.cpp 里覆盖
    if not (55 <= stress["rates"]["vmc"] <= 65):
        print(f"FAIL VMC 帧率偏离 60 Hz（{stress['rates']['vmc']:.1f}/s）")
        ok = False
    if not (85 <= stress["ws"] <= 95) or not stress["ws_ok"]:
        print(f"FAIL WS 帧率/内容异常（{stress['ws']:.1f}/s, ok={stress['ws_ok']}, "
              f"err={stress['ws_error']}）")
        ok = False
    if stress["shm"] is None or not (110 <= stress["shm"] <= 135):
        print(f"FAIL 共享内存帧率异常（{stress['shm']:.1f}/s）")
        ok = False
    if stress["late"] > max(2, stress["ticks"] // 100):
        print(f"FAIL 引擎 late_ticks 过多（{stress['late']}/{stress['ticks']}）")
        ok = False
    # "主链路延迟"以引擎自己的 tick 工作时间为准：UDP 端到端还要叠加
    # Python 收发线程的调度抖动（实测 ±1 ms），不适合做 0.5 ms 的判定。
    delta_us = stress["tick_p95"] - baseline["tick_p95"]
    print(f"\n五路相对基线的引擎 tick P95 增量：{delta_us:+.1f} µs（目标 < 500 µs）")
    if delta_us >= 500.0:
        print("FAIL 多路输出使主链路 tick 耗时明显增加")
        ok = False
    udp_delta = stress["lat_p95"] - baseline["lat_p95"]
    print(f"（参考）UDP 端到端 p95 增量：{udp_delta:+.3f} ms")

    print("\nOK" if ok else "\nFAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
