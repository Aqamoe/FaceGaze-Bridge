"""命名校准存档的 IPC 探针。

自己拉起一个临时引擎（VMC 输入、临时 calibration.ini 与存档目录），通过 WebSocket 走一遍：
新建默认参数存档（形状全透传）→ 重名新建被拒 → 改一条形状后保存到该存档 → 恢复默认 →
应用存档（写回 calibration.ini）→ /api/calibration 回读 → 新建当前参数副本 → 非法名被拒 →
删除 → 列表与目录都清空。

不需要硬件，也不碰用户的默认 calibration.ini 与存档目录。

用法：
    python tools/probe_calibration_profiles.py
    python tools/probe_calibration_profiles.py --cli build/windows-vs2022/bin/Debug/fgb-cli.exe
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

NAME = "探针 存档"
COPY_NAME = "探针 副本"


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


def wait_ack(sock, command, seconds=3.0):
    return wait_for(
        sock,
        lambda m: m.get("type") == "ack" and m.get("command") == command,
        seconds, f"{command} ack")


def jaw_shape(profile):
    return next((s for s in profile.get("shapes", []) if s.get("name") == "JawOpen"), None)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli",
                        default=os.path.join("build", "windows-vs2022", "bin", "Debug",
                                             "fgb-cli.exe"))
    args = parser.parse_args()
    if not os.path.isfile(args.cli):
        print(f"FAIL 找不到引擎：{args.cli}")
        return 1

    panel_port = free_tcp_port()
    vmc_port = free_udp_port()
    vrchat_port = free_udp_port()
    tmpdir = tempfile.mkdtemp(prefix="fgb-calib-archive-probe-")
    calibration_path = os.path.join(tmpdir, "calibration.ini")
    profiles_dir = os.path.join(tmpdir, "profiles")

    proc = subprocess.Popen([
        args.cli, "run", "--source", "vmc", "--vmc-port", str(vmc_port),
        "--vrchat", f"127.0.0.1:{vrchat_port}",
        "--panel", str(panel_port),
        "--no-avatar-discovery", "--no-oscquery", "--no-saved-outputs",
        "--calibration", calibration_path,
        "--calibration-profiles", profiles_dir,
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
        initial, _ = wait_for(sock, lambda m: m.get("type") == "calibration_profiles", 3.0,
                              "初始存档列表")
        if initial is None:
            print("FAIL 连接后没有收到 calibration_profiles")
            return 1
        print(f"初始存档：{initial.get('profiles')} dir={initial.get('dir')!r}")

        # 「新建：默认参数」= 与恢复默认同一份透传档案，且创建后立刻生效
        send_json(sock, {"type": "create_calibration_profile", "name": NAME,
                         "source": "default"})
        ack, _ = wait_ack(sock, "create_calibration_profile")
        if not ack or not ack.get("ok"):
            print(f"FAIL 新建默认参数存档失败：{ack}")
            return 1
        created = http_json(panel_port, "/api/calibration")
        if any(shape.get("active") for shape in created.get("shapes", [])):
            print("FAIL 新建默认存档后仍有形状处于校准状态")
            return 1
        listed, _ = wait_for(
            sock,
            lambda m: m.get("type") == "calibration_profiles" and NAME in m.get("profiles", []),
            3.0, "新建后的列表")
        if listed is None:
            print("FAIL 新建后列表里没有新存档")
            return 1
        print(f"新建默认 → {listed.get('profiles')}，形状全部透传")

        # 重名新建必须拒绝：否则一次手滑就把旧存档抹了
        send_json(sock, {"type": "create_calibration_profile", "name": NAME,
                         "source": "default"})
        ack, _ = wait_ack(sock, "create_calibration_profile")
        if ack is None or ack.get("ok"):
            print(f"FAIL 重名新建没有被拒绝：{ack}")
            return 1
        print(f"重名新建被拒绝：{ack.get('error')}")

        # 造一套非默认校准，然后「保存」直接写回选中的存档（正常保存就是一个存档）
        send_json(sock, {"type": "set_calibration_shape", "name": "JawOpen",
                         "lo": 0.12, "hi": 0.78, "active": True})
        ack, _ = wait_ack(sock, "set_calibration_shape")
        if not ack or not ack.get("ok"):
            print(f"FAIL 设置形状失败：{ack}")
            return 1

        send_json(sock, {"type": "save_calibration", "profile": NAME})
        ack, _ = wait_ack(sock, "save_calibration")
        if not ack or not ack.get("ok"):
            print(f"FAIL 保存到存档失败：{ack}")
            return 1
        if not os.path.isfile(calibration_path):
            print("FAIL 保存到存档时没有同时写 calibration.ini")
            return 1
        print(f"保存到存档 → {ack.get('message')}")

        # 恢复默认后再应用：必须真的把存档内容写回 calibration.ini 并热加载
        send_json(sock, {"type": "reset_calibration"})
        ack, _ = wait_ack(sock, "reset_calibration")
        if not ack or not ack.get("ok"):
            print(f"FAIL 恢复默认失败：{ack}")
            return 1
        reset_profile = http_json(panel_port, "/api/calibration")
        if jaw_shape(reset_profile) is None or jaw_shape(reset_profile).get("active"):
            print(f"FAIL 恢复默认后 JawOpen 仍在校准：{jaw_shape(reset_profile)}")
            return 1

        send_json(sock, {"type": "apply_calibration_profile", "name": NAME})
        ack, _ = wait_ack(sock, "apply_calibration_profile")
        if not ack or not ack.get("ok"):
            print(f"FAIL 应用存档失败：{ack}")
            return 1
        shape = jaw_shape(http_json(panel_port, "/api/calibration"))
        print(f"应用 → JawOpen: {shape}")
        if shape is None or not shape.get("active"):
            print("FAIL 应用存档后 /api/calibration 里 JawOpen 没有启用")
            return 1
        if abs(shape["lo"] - 0.12) > 1e-3 or abs(shape["hi"] - 0.78) > 1e-3:
            print(f"FAIL 应用后 lo/hi 与存档不一致：{shape}")
            return 1
        print(f"回读 → {calibration_path}")

        # 「新建：当前参数」= 把现在这套复制一份，应用后数值应当一致
        send_json(sock, {"type": "create_calibration_profile", "name": COPY_NAME,
                         "source": "current"})
        ack, _ = wait_ack(sock, "create_calibration_profile")
        if not ack or not ack.get("ok"):
            print(f"FAIL 复制当前参数新建失败：{ack}")
            return 1
        both, _ = wait_for(
            sock,
            lambda m: m.get("type") == "calibration_profiles"
            and NAME in m.get("profiles", []) and COPY_NAME in m.get("profiles", []),
            3.0, "复制后的列表")
        if both is None:
            print("FAIL 复制后列表里没有两份存档")
            return 1
        print(f"新建副本 → {both.get('profiles')}")

        send_json(sock, {"type": "reset_calibration"})
        wait_ack(sock, "reset_calibration")
        send_json(sock, {"type": "apply_calibration_profile", "name": COPY_NAME})
        ack, _ = wait_ack(sock, "apply_calibration_profile")
        copy_shape = jaw_shape(http_json(panel_port, "/api/calibration"))
        if not ack or not ack.get("ok") or copy_shape is None or not copy_shape.get("active"):
            print(f"FAIL 副本应用后没有恢复校准：{ack} {copy_shape}")
            return 1
        if abs(copy_shape["lo"] - 0.12) > 1e-3 or abs(copy_shape["hi"] - 0.78) > 1e-3:
            print(f"FAIL 副本内容与复制时不一致：{copy_shape}")
            return 1

        # 非法名称必须拒绝（路径分隔符不能进文件名）
        send_json(sock, {"type": "create_calibration_profile", "name": "a/b",
                         "source": "default"})
        ack, _ = wait_ack(sock, "create_calibration_profile")
        if ack is None or ack.get("ok"):
            print(f"FAIL 非法存档名没有被拒绝：{ack}")
            return 1
        print(f"非法名称被拒绝：{ack.get('error')}")

        for name in (NAME, COPY_NAME):
            send_json(sock, {"type": "delete_calibration_profile", "name": name})
            ack, _ = wait_ack(sock, "delete_calibration_profile")
            if not ack or not ack.get("ok"):
                print(f"FAIL 删除存档失败：{ack}")
                return 1
            gone, _ = wait_for(
                sock,
                lambda m: m.get("type") == "calibration_profiles"
                and name not in m.get("profiles", []),
                3.0, "删除后的列表")
            if gone is None:
                print(f"FAIL 删除后列表里仍有该存档：{name}")
                return 1
            print(f"删除 → {gone.get('profiles')}")

        remaining = [f for f in os.listdir(profiles_dir) if f.endswith(".ini")] \
            if os.path.isdir(profiles_dir) else []
        if remaining:
            print(f"FAIL 存档目录仍有残留：{remaining}")
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
