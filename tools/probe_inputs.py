"""通用 OSC 输入与 ARKit LiveLink 输入的端到端探针。

自己拉起引擎、自己发包、自己读 /api/params 断言，不依赖 VRChat 或硬件：
  1. 通用 OSC：地址→形状/眼睛映射、EyeLid 语义、视线轴、通道声明
  2. ARKit LiveLink：61 曲线二进制帧、blink → 睁眼度、look* → 视线、头姿进入 /api/gaze

用法：
    python tools/probe_inputs.py
    python tools/probe_inputs.py --cli build/werror/bin/Debug/fgb-cli.exe

端口都临时选空闲的，不会碰用户配置或正在运行的引擎。
"""
import argparse
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.request

DEFAULT_CLI = os.path.join("build", "test", "bin", "Debug", "fgb-cli.exe")


def free_udp_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def free_tcp_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def osc_float(address, value):
    raw = address.encode("utf-8") + b"\x00"
    while len(raw) % 4:
        raw += b"\x00"
    return raw + b",f\x00\x00" + struct.pack(">f", value)


def livelink_frame(curves, subject="iPhone"):
    def put_string(text):
        encoded = text.encode("utf-8")
        return struct.pack(">i", len(encoded)) + encoded

    body = struct.pack(">B", 6)
    body += put_string("DEADC0DE-1337-1337-1337-CAFEBABE")
    body += put_string(subject)
    body += struct.pack(">IfII", 1, 0.0, 60, 1)
    body += struct.pack(">B", len(curves))
    body += b"".join(struct.pack(">f", v) for v in curves)
    return body


def http_json(port, path, timeout=5.0):
    with urllib.request.urlopen(f"http://127.0.0.1:{port}{path}", timeout=timeout) as response:
        return json.load(response)


def wait_ready(port, timeout=10.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            http_json(port, "/api/status", timeout=0.5)
            return True
        except Exception:
            time.sleep(0.1)
    return False


def wait_params(port, predicate, timeout=3.0):
    deadline = time.time() + timeout
    last = {}
    while time.time() < deadline:
        try:
            last = {p["name"]: p["value"] for p in http_json(port, "/api/params")["params"]}
            if predicate(last):
                return last
        except Exception:
            pass
        time.sleep(0.05)
    return last


def run_engine(cli, args, seconds):
    # 显式给一个不存在的校准档案：避免用户机器上的 calibration.ini 改变期望值
    no_calibration = os.path.join(tempfile.gettempdir(), "fgb-probe-no-calibration.ini")
    return subprocess.Popen([cli, "run", *args, "--calibration", no_calibration,
                             "--duration", str(seconds)],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            stdin=subprocess.DEVNULL)


def stop_engine(proc):
    if proc is None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()


def near(actual, expected, eps=0.01):
    return actual is not None and abs(actual - expected) <= eps


def probe_osc(cli, failures):
    osc_port = free_udp_port()
    panel = free_tcp_port()
    with tempfile.TemporaryDirectory(prefix="fgb-osc-") as tmp:
        map_path = os.path.join(tmp, "osc_map.ini")
        with open(map_path, "w", encoding="utf-8") as handle:
            handle.write("[input]\nport=%d\n\n[map]\n" % osc_port)
            handle.write("/jaw = JawOpen\n")
            handle.write("/lid = EyeLidLeft\n")
            handle.write("/ex = EyeLeftX\n")
            handle.write("/ey = EyeLeftY\n")

        proc = run_engine(cli, ["--source", "osc", "--osc-map", map_path,
                                "--osc-in-port", str(osc_port), "--panel", str(panel)], 20)
        try:
            if not wait_ready(panel):
                failures.append("OSC: 引擎未在 10 s 内就绪")
                return
            sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            for _ in range(10):
                sender.sendto(osc_float("/jaw", 0.75), ("127.0.0.1", osc_port))
                sender.sendto(osc_float("/lid", 0.25), ("127.0.0.1", osc_port))
                sender.sendto(osc_float("/ex", 0.5), ("127.0.0.1", osc_port))
                sender.sendto(osc_float("/ey", -0.5), ("127.0.0.1", osc_port))
                time.sleep(0.03)
            sender.close()

            values = wait_params(panel, lambda v: v.get("v2/JawOpen", 0.0) > 0.5)
            checks = [
                ("JawOpen", values.get("v2/JawOpen"), 0.75),
                ("EyeLidLeft", values.get("v2/EyeLidLeft"), 0.75 * 0.75),
                ("EyeLeftX", values.get("v2/EyeLeftX"), 0.5),
                ("EyeLeftY", values.get("v2/EyeLeftY"), -0.5),
            ]
            for name, actual, expected in checks:
                if not near(actual, expected):
                    failures.append(f"OSC: {name} 期望 {expected}，实际 {actual}")

            sources = http_json(panel, "/api/status").get("sources") or []
            osc = next((s for s in sources if s.get("name") == "osc"), None)
            if osc is None or not osc.get("has_expression") or not osc.get("has_eyes"):
                failures.append(f"OSC: 通道声明不对: {osc}")
            print(f"OSC  params={values.get('v2/JawOpen')}/{values.get('v2/EyeLidLeft')}"
                  f"/{values.get('v2/EyeLeftX')}/{values.get('v2/EyeLeftY')}")
        finally:
            stop_engine(proc)


def probe_livelink(cli, failures):
    ll_port = free_udp_port()
    panel = free_tcp_port()
    proc = run_engine(cli, ["--source", "livelink", "--livelink-port", str(ll_port),
                            "--panel", str(panel)], 20)
    try:
        if not wait_ready(panel):
            failures.append("LiveLink: 引擎未在 10 s 内就绪")
            return

        curves = [0.0] * 61
        curves[0] = 0.25   # eyeBlinkLeft
        curves[17] = 0.6   # jawOpen
        curves[2] = 0.5    # eyeLookInLeft → 视线向右
        curves[53] = 0.5   # headPitch → 45°
        packet = livelink_frame(curves)

        sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        for _ in range(20):
            sender.sendto(packet, ("127.0.0.1", ll_port))
            time.sleep(0.03)
        sender.close()

        values = wait_params(panel, lambda v: v.get("v2/JawOpen", 0.0) > 0.5)
        checks = [
            ("JawOpen", values.get("v2/JawOpen"), 0.6),
            ("EyeLidLeft", values.get("v2/EyeLidLeft"), 0.75 * 0.75),
            ("EyeLeftX", values.get("v2/EyeLeftX"), 0.5),
        ]
        for name, actual, expected in checks:
            if not near(actual, expected):
                failures.append(f"LiveLink: {name} 期望 {expected}，实际 {actual}")

        gaze = http_json(panel, "/api/gaze")
        head = gaze.get("head_pose") or {}
        quat = head.get("q") or []
        if len(quat) != 4 or not near(quat[0], 0.3827, 0.02):
            failures.append(f"LiveLink: 头姿没进 /api/gaze: {head}")
        if not gaze.get("valid"):
            failures.append(f"LiveLink: /api/gaze 无效: {gaze}")

        sources = http_json(panel, "/api/status").get("sources") or []
        ll = next((s for s in sources if s.get("name") == "livelink"), None)
        if ll is None or not (ll.get("has_expression") and ll.get("has_eyes") and ll.get("has_head")):
            failures.append(f"LiveLink: 通道声明不对: {ll}")
        print(f"LiveLink params={values.get('v2/JawOpen')}/{values.get('v2/EyeLidLeft')}"
              f"/{values.get('v2/EyeLeftX')} head_q={quat[:1]} gaze={gaze.get('yaw_deg')}")
    finally:
        stop_engine(proc)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", default=DEFAULT_CLI)
    args = parser.parse_args()

    if not os.path.isfile(args.cli):
        print(f"FAIL 找不到引擎：{args.cli}")
        return 2

    failures = []
    probe_osc(args.cli, failures)
    probe_livelink(args.cli, failures)

    if failures:
        for message in failures:
            print(f"FAIL {message}")
        return 1
    print("PASS OSC + LiveLink 输入")
    return 0


if __name__ == "__main__":
    sys.exit(main())
