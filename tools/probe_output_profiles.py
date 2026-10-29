"""命名输出预设的 IPC 探针（roadmap 4.8）。

自己拉起一个临时引擎（VMC 输入、临时 outputs/预设目录），通过 WebSocket 走一遍：
保存预设 → 列表里出现 → 应用 → outputs.ini 与 status 回读 → 删除 → 列表消失。

用法：
    python tools/probe_output_profiles.py
    python tools/probe_output_profiles.py --cli build/test/bin/Debug/fgb-cli.exe
"""
import argparse
import base64
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.request

NAME = "探针 预设"


def free_tcp_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def free_udp_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def http_json(port, path):
    with urllib.request.urlopen(f"http://127.0.0.1:{port}{path}", timeout=2) as resp:
        return json.loads(resp.read().decode("utf-8"))


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


def connect_ws(port):
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
        raise RuntimeError(f"WS Upgrade 失败：{head[:80]!r}")
    sock.settimeout(0.2)
    return sock


def send_json(sock, payload):
    data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    mask = os.urandom(4)
    header = bytearray([0x81])
    if len(data) < 126:
        header.append(0x80 | len(data))
    else:
        header.append(0x80 | 126)
        header += struct.pack(">H", len(data))
    header += mask
    sock.sendall(bytes(header) + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))


def collect(sock, seconds):
    messages = []
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            opcode, payload = recv_frame(sock)
        except socket.timeout:
            continue
        if opcode == 0x9:
            continue
        if opcode != 0x1:
            continue
        messages.append(json.loads(payload))
    return messages


def wait_for(sock, predicate, seconds, label):
    deadline = time.time() + seconds
    seen = []
    while time.time() < deadline:
        try:
            opcode, payload = recv_frame(sock)
        except socket.timeout:
            continue
        if opcode != 0x1:
            continue
        message = json.loads(payload)
        seen.append(message.get("type"))
        if predicate(message):
            return message, seen
    return None, seen


def profile_payload(name):
    return {
        "type": "save_output_profile",
        "name": name,
        "vrchat": "127.0.0.1:9000",
        "eye_mode": "both",
        "params_mode": "v2",
        "vmc_enabled": True,
        "vmc_target": "127.0.0.1:39539",
        "shm_enabled": True,
        "shm_name": "probe-profile",
        "ws_enabled": True,
        "ws_port": 9543,
        "ws_rate": 45,
        "ws_all": True,
        "udp_enabled": True,
        "udp_target": "127.0.0.1:9542",
        "udp_rate": 90,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", default=os.path.join("build", "test", "bin", "Debug", "fgb-cli.exe"))
    args = parser.parse_args()
    if not os.path.isfile(args.cli):
        print(f"FAIL 找不到引擎：{args.cli}")
        return 1

    panel_port = free_tcp_port()
    vmc_port = free_udp_port()
    vrchat_port = free_udp_port()
    tmpdir = tempfile.mkdtemp(prefix="fgb-profile-probe-")
    outputs_path = os.path.join(tmpdir, "outputs.ini")
    profiles_dir = os.path.join(tmpdir, "profiles")

    proc = subprocess.Popen([
        args.cli, "run", "--source", "vmc", "--vmc-port", str(vmc_port),
        "--vrchat", f"127.0.0.1:{vrchat_port}",
        "--panel", str(panel_port),
        "--no-avatar-discovery", "--no-oscquery", "--no-saved-outputs",
        "--outputs-config", outputs_path,
        "--outputs-profiles", profiles_dir,
        "--calibration", os.path.join(tmpdir, "calibration.ini"),
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    sock = None
    ok = True
    try:
        deadline = time.time() + 15
        while time.time() < deadline:
            try:
                http_json(panel_port, "/healthz")
                break
            except Exception:  # noqa: BLE001
                if proc.poll() is not None:
                    raise RuntimeError("引擎启动失败")
                time.sleep(0.1)
        else:
            raise RuntimeError("等待控制面超时")

        sock = connect_ws(panel_port)
        initial, _ = wait_for(sock, lambda m: m.get("type") == "outputs_profiles", 3.0,
                              "初始预设列表")
        if initial is None:
            print("FAIL 连接后没有收到 outputs_profiles")
            return 1
        print(f"初始预设：{initial.get('profiles')} active={initial.get('active')!r}")

        send_json(sock, profile_payload(NAME))
        ack, _ = wait_for(sock, lambda m: m.get("type") == "ack", 3.0, "保存 ack")
        if not ack or not ack.get("ok"):
            print(f"FAIL 保存预设被拒绝：{ack}")
            return 1
        listed, _ = wait_for(
            sock, lambda m: m.get("type") == "outputs_profiles" and NAME in m.get("profiles", []),
            3.0, "保存后的列表")
        if listed is None:
            print("FAIL 保存后列表里没有新预设")
            return 1
        print(f"保存 → {listed.get('profiles')}")

        # 应用：写 outputs.ini，status 回读；本探针不触发 restart_engine
        send_json(sock, {"type": "apply_output_profile", "name": NAME})
        ack, _ = wait_for(sock, lambda m: m.get("type") == "ack", 3.0, "应用 ack")
        if not ack or not ack.get("ok") or not ack.get("restart_needed"):
            print(f"FAIL 应用预设失败或未标记 restart_needed：{ack}")
            return 1
        applied, _ = wait_for(
            sock, lambda m: m.get("type") == "outputs_profiles" and m.get("active") == NAME,
            3.0, "应用后的列表")
        if applied is None:
            print("FAIL 应用后 active 没有指向该预设")
            return 1
        status = http_json(panel_port, "/api/status")["outputs"]
        print(f"应用 → outputs: ws={status['ws_port']}@{status['ws_rate']}Hz "
              f"udp={status['udp_rate']}Hz shm={status['shm_name']!r} eye={status['eye_mode']}")
        if (status["ws_port"] != 9543 or status["ws_rate"] != 45 or status["udp_rate"] != 90
                or status["shm_name"] != "probe-profile" or status["eye_mode"] != "both"):
            print("FAIL outputs.ini 回读的值与预设不一致")
            return 1

        # 非法名称必须拒绝
        send_json(sock, profile_payload("a/b"))
        ack, _ = wait_for(sock, lambda m: m.get("type") == "ack", 3.0, "非法名称 ack")
        if ack is None or ack.get("ok"):
            print(f"FAIL 非法预设名没有被拒绝：{ack}")
            return 1
        print(f"非法名称被拒绝：{ack.get('error')}")

        send_json(sock, {"type": "delete_output_profile", "name": NAME})
        ack, _ = wait_for(sock, lambda m: m.get("type") == "ack", 3.0, "删除 ack")
        if not ack or not ack.get("ok"):
            print(f"FAIL 删除预设失败：{ack}")
            return 1
        gone, _ = wait_for(
            sock, lambda m: m.get("type") == "outputs_profiles" and NAME not in m.get("profiles", []),
            3.0, "删除后的列表")
        if gone is None:
            print("FAIL 删除后列表里仍有该预设")
            return 1
        print(f"删除 → {gone.get('profiles')}")

        remaining = [f for f in os.listdir(profiles_dir) if f.endswith(".ini")] \
            if os.path.isdir(profiles_dir) else []
        if remaining:
            print(f"FAIL 预设目录仍有残留：{remaining}")
            return 1
    except Exception as exc:  # noqa: BLE001
        print(f"FAIL {exc}")
        ok = False
    finally:
        if sock is not None:
            sock.close()
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()

    print("OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
