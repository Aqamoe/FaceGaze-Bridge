"""VMC 输出的验收探针：在本地某端口收包，检查 fgb 到底发出了什么。

为什么单独写一个：单元测试只验证"能编码成合法的 OSC"，联调时真正会出问题的是
"对端收不到 / 名字对不上 / 没有 Apply"。这里按接收端的视角看一遍。

用法：
    fgb-cli run --source replay --replay x.fgbrec --replay-loop --vmc-out 127.0.0.1:9100
    python tools/probe_vmc_out.py 9100 6

检查项：
  - 收到 /VMC/Ext/Blend/Val（名字、值都合法）
  - 每帧以 /VMC/Ext/Blend/Apply 结束
  - 有头姿时收到 /VMC/Ext/Root/Pos（7 个 float，名字是 HMD）
  - 汇报最常见的几个 blendshape 名字与值，便于肉眼对照
"""
import collections
import socket
import struct
import sys
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9100
SECONDS = float(sys.argv[2]) if len(sys.argv) > 2 else 5.0


def parse_messages(packet):
    """把 bundle 或裸消息拆成 [(address, [args])]，只支持 s / f / i。"""
    out = []
    if packet[:8] == b"#bundle\0":
        pos = 16
        while pos + 4 <= len(packet):
            (length,) = struct.unpack(">i", packet[pos:pos + 4])
            pos += 4
            if length <= 0 or pos + length > len(packet):
                break
            out.extend(parse_messages(packet[pos:pos + length]))
            pos += length + ((4 - length % 4) % 4)
        return out

    end = packet.find(b"\0")
    if end <= 0:
        return out
    address = packet[:end].decode("utf-8", "replace")
    pos = (end + 4) & ~3
    if pos >= len(packet):
        return out
    tags_end = packet.find(b"\0", pos)
    if tags_end < 0:
        return out
    tags = packet[pos:tags_end].decode("ascii", "replace")
    pos = (tags_end + 4) & ~3

    args = []
    for tag in tags[1:]:
        if tag == "f":
            if pos + 4 > len(packet):
                break
            args.append(struct.unpack(">f", packet[pos:pos + 4])[0])
            pos += 4
        elif tag == "i":
            if pos + 4 > len(packet):
                break
            args.append(struct.unpack(">i", packet[pos:pos + 4])[0])
            pos += 4
        elif tag == "s":
            end_s = packet.find(b"\0", pos)
            if end_s < 0:
                break
            args.append(packet[pos:end_s].decode("utf-8", "replace"))
            pos = (end_s + 4) & ~3
        else:
            break
    out.append((address, args))
    return out


def main():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("127.0.0.1", PORT))
    sock.settimeout(0.4)

    counts = collections.Counter()
    values = {}
    applies = 0
    head = None
    datagrams = 0
    deadline = time.time() + SECONDS

    while time.time() < deadline:
        try:
            packet, _ = sock.recvfrom(65535)
        except socket.timeout:
            continue
        datagrams += 1
        for address, args in parse_messages(packet):
            counts[address] += 1
            if address == "/VMC/Ext/Blend/Val" and len(args) >= 2:
                values[args[0]] = args[1]
            elif address == "/VMC/Ext/Blend/Apply":
                applies += 1
            elif address == "/VMC/Ext/Root/Pos":
                head = args

    print(f"收到 {datagrams} 个数据报 / {SECONDS:.0f} 秒")
    for address, count in counts.most_common():
        print(f"  {address:<24} {count}")
    print(f"Apply 次数（= 提交的帧数）: {applies}")
    if head is not None:
        print(f"头姿: name={head[0]!r} pos=({head[1]:.3f}, {head[2]:.3f}, {head[3]:.3f})")
    sample = list(values.items())[:8]
    print(f"非零 blendshape 抽样（共 {len(values)} 个，其中非零 "
          f"{sum(1 for v in values.values() if abs(v) > 1e-4)} 个）:")
    for name, value in sample:
        print(f"  {name:<20} {value:+.3f}")

    ok = True
    if counts["/VMC/Ext/Blend/Val"] == 0:
        print("FAIL 没有收到任何 Blend/Val")
        ok = False
    if applies == 0:
        print("FAIL 没有收到 Apply：接收端不会提交任何一帧")
        ok = False
    print("PASS" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
