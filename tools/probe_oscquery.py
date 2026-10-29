"""VRChat OSCQuery 探针：不启动 FGB，确认 mDNS 端点、头像和参数表。

用法：
    python tools/probe_oscquery.py            # 默认 1 秒发现窗口
    python tools/probe_oscquery.py 2          # 等 2 秒

为什么需要它：VRChat 的 OSCQuery HTTP 端口是动态的（本机实测 65393），
固定探 9001 会失败。这个脚本做三件事：
  1. mDNS 查询 _oscjson._tcp.local，列出所有服务（同机可能有 OVRToolkit）
  2. 选名字以 VRChat 开头的实例，拉 /?HOST_INFO
  3. 拉整棵树，统计 /avatar/parameters 下可写的 float / bool / int
只依赖 Python 标准库，和 tools/probe_ui.py 一样，排障时先跑它。
"""
import json
import socket
import struct
import sys
import time
import urllib.request

GROUP = "224.0.0.251"
PORT = 5353
SERVICE = "_oscjson._tcp.local"


def enc_name(name: str) -> bytes:
    out = b""
    for part in name.rstrip(".").split("."):
        raw = part.encode()
        out += bytes([len(raw)]) + raw
    return out + b"\x00"


def read_name(data: bytes, off: int):
    labels = []
    while True:
        length = data[off]
        if length == 0:
            return ".".join(labels), off + 1
        if length & 0xC0 == 0xC0:
            pointer = struct.unpack_from(">H", data, off)[0] & 0x3FFF
            sub, _ = read_name(data, pointer)
            labels.append(sub)
            return ".".join(labels), off + 2
        off += 1
        labels.append(data[off : off + length].decode("utf-8", "replace"))
        off += length


def discover(timeout: float):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("", PORT))
    sock.setsockopt(
        socket.IPPROTO_IP,
        socket.IP_ADD_MEMBERSHIP,
        socket.inet_aton(GROUP) + socket.inet_aton("0.0.0.0"),
    )
    sock.settimeout(0.2)
    query = struct.pack(">HHHHHH", 0, 0, 1, 0, 0, 0) + enc_name(SERVICE) + struct.pack(">HH", 12, 1)
    sock.sendto(query, (GROUP, PORT))

    ptr, srv, addrs, txt = {}, {}, {}, {}
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            data, _ = sock.recvfrom(9000)
        except socket.timeout:
            continue
        _, _, qd, an, ns, ar = struct.unpack_from(">HHHHHH", data, 0)
        off = 12
        for _ in range(qd):
            _, off = read_name(data, off)
            off += 4
        for _ in range(an + ns + ar):
            name, off = read_name(data, off)
            typ, _, _, length = struct.unpack_from(">HHIH", data, off)
            off += 10
            rdata = off
            off += length
            if typ == 12:
                target, _ = read_name(data, rdata)
                ptr[name] = target
            elif typ == 33:
                port, = struct.unpack_from(">H", data, rdata + 4)
                target, _ = read_name(data, rdata + 6)
                srv[name] = (port, target)
            elif typ == 1:
                addrs[name] = ".".join(str(b) for b in data[rdata : rdata + 4])
            elif typ == 16:
                values, cursor = [], rdata
                while cursor < rdata + length:
                    part = data[cursor]
                    cursor += 1
                    if part:
                        values.append(data[cursor : cursor + part].decode("utf-8", "replace"))
                    cursor += part
                txt[name] = values

    services = []
    for _, instance in ptr.items():
        if instance not in srv:
            continue
        port, host = srv[instance]
        services.append(
            {
                "instance": instance,
                "host": host,
                "port": port,
                "address": addrs.get(host, "127.0.0.1"),
                "txt": txt.get(instance, []),
            }
        )
    return services


def get_json(url: str):
    with urllib.request.urlopen(url, timeout=3) as response:
        return json.loads(response.read())


def walk(node, out):
    for child in (node.get("CONTENTS") or {}).values():
        if not isinstance(child, dict):
            continue
        path = child.get("FULL_PATH", "")
        typ = child.get("TYPE")
        access = child.get("ACCESS", 0)
        if path.startswith("/avatar/parameters/") and typ:
            out.append((path, typ, access))
        walk(child, out)


def main() -> int:
    timeout = float(sys.argv[1]) if len(sys.argv) > 1 else 1.0
    services = discover(timeout)
    if not services:
        print("FAIL 没发现 _oscjson._tcp（VRChat 未运行或 OSC 未启用）")
        return 1

    print("发现的服务：")
    for service in services:
        print(
            f"  {service['instance']}  {service['address']}:{service['port']}  txt={service['txt']}"
        )

    vrchat = next((s for s in services if s["instance"].lower().startswith("vrchat")), None)
    if vrchat is None:
        print("FAIL 有 OSCQuery 服务，但不是 VRChat")
        return 1

    base = f"http://{vrchat['address']}:{vrchat['port']}/"
    info = get_json(base + "?HOST_INFO")
    print(f"\nHOST_INFO：{json.dumps(info, ensure_ascii=False)}")

    root = get_json(base)
    identity = {}
    stack = [root]
    while stack:
        node = stack.pop()
        if node.get("FULL_PATH") in ("/avatar/change", "/avatar/name"):
            value = node.get("VALUE") or [""]
            identity[node["FULL_PATH"]] = value[0]
        stack.extend(v for v in (node.get("CONTENTS") or {}).values() if isinstance(v, dict))

    params = []
    walk(root, params)
    writable = [(p, t) for p, t, a in params if a & 2]
    floats = [p for p, t in writable if t == "f"]
    bools = [p for p, t in writable if t in ("T", "F")]
    ints = [p for p, t in writable if t == "i"]

    print(f"头像：{identity.get('/avatar/name', '?')}  {identity.get('/avatar/change', '?')}")
    print(f"可写参数：float {len(floats)} / bool {len(bools)} / int {len(ints)}（int 不发送）")
    print(f"参数树叶子：{len(params)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
