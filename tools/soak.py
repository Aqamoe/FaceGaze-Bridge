"""长跑稳定性探针：盯 tick 迟到、内存、句柄、休眠恢复。

用法：
    python tools/soak.py --duration 600          # 10 分钟
    python tools/soak.py --duration 28800        # 8 小时
    python tools/soak.py --duration 3600 --replay path.fgbrec

不需要硬件：默认用 VMC 输入，脚本自己以 120 Hz 灌数据；引擎输出到一个临时 UDP
端口，不会打扰真正的 VRChat。失败条件：
  - 引擎进程提前退出
  - tick 迟到比例 > 0.5%
  - 工作集从基线增长 > 32 MB 或句柄增长 > 64

休眠/唤醒会记一次 resumes（警告，不算失败）；这是引擎的恢复逻辑在起作用。
"""
import argparse
import json
import math
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request


def free_port(kind=socket.SOCK_STREAM):
    with socket.socket(socket.AF_INET, kind) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def pad(text):
    raw = text.encode("utf-8") + b"\x00"
    while len(raw) % 4:
        raw += b"\x00"
    return raw


def blend(name, value):
    return (pad("/VMC/Ext/Blend/Val") + b",sf\x00" + pad(name) + struct.pack(">f", value))


def apply_message():
    return pad("/VMC/Ext/Blend/Apply") + b",\x00\x00\x00"


def http_json(port, path="/api/status"):
    with urllib.request.urlopen(f"http://127.0.0.1:{port}{path}", timeout=3) as resp:
        return json.loads(resp.read().decode("utf-8"))


class Feeder(threading.Thread):
    def __init__(self, port):
        super().__init__(daemon=True)
        self.target = ("127.0.0.1", port)
        self.stop = threading.Event()
        self.sent = 0

    def run(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        period = 1.0 / 120.0
        next_at = time.perf_counter()
        phase = 0.0
        while not self.stop.is_set():
            sock.sendto(blend("jawOpen", 0.5 + 0.45 * math.sin(phase)), self.target)
            sock.sendto(blend("mouthSmileLeft", 0.5 + 0.45 * math.cos(phase)), self.target)
            sock.sendto(blend("browInnerUp", 0.4), self.target)
            sock.sendto(apply_message(), self.target)
            self.sent += 1
            phase += 0.05
            next_at += period
            delay = next_at - time.perf_counter()
            if delay > 0:
                time.sleep(delay)
            else:
                next_at = time.perf_counter()
        sock.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--duration", type=float, default=600.0)
    parser.add_argument("--interval", type=float, default=5.0)
    parser.add_argument("--replay")
    parser.add_argument("--cli", default=os.path.join("build", "test", "bin", "Debug", "fgb-cli.exe"))
    args = parser.parse_args()
    if not os.path.isfile(args.cli):
        print(f"FAIL 找不到引擎：{args.cli}")
        return 1

    panel = free_port()
    vrchat_port = free_port(socket.SOCK_DGRAM)
    vmc_port = free_port(socket.SOCK_DGRAM)
    tmpdir = tempfile.mkdtemp(prefix="fgb-soak-")
    command = [args.cli, "run", "--panel", str(panel),
               "--vrchat", f"127.0.0.1:{vrchat_port}",
               "--no-avatar-discovery", "--no-oscquery", "--no-saved-outputs",
               "--outputs-config", os.path.join(tmpdir, "outputs.ini"),
               "--calibration", os.path.join(tmpdir, "calibration.ini")]
    if args.replay:
        command += ["--source", "replay", "--replay", args.replay, "--replay-loop"]
    else:
        command += ["--source", "vmc", "--vmc-port", str(vmc_port)]

    proc = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    feeder = None if args.replay else Feeder(vmc_port)
    if feeder is not None:
        feeder.start()

    samples = []
    ok = True
    try:
        deadline = time.time() + 20
        while time.time() < deadline:
            try:
                http_json(panel, "/healthz")
                break
            except Exception:  # noqa: BLE001
                if proc.poll() is not None:
                    raise RuntimeError("引擎启动失败（进程已退出）")
                time.sleep(0.2)
        else:
            raise RuntimeError("等待控制面超时")

        started = time.time()
        next_at = started
        while time.time() - started < args.duration:
            if proc.poll() is not None:
                print(f"FAIL 引擎提前退出，退出码 {proc.returncode}")
                ok = False
                break
            try:
                status = http_json(panel)
            except Exception as exc:  # noqa: BLE001
                print(f"WARN 状态读取失败：{exc}")
                time.sleep(args.interval)
                continue
            work = status.get("tick_work", {})
            process = status.get("process", {})
            elapsed = time.time() - started
            samples.append({
                "t": elapsed,
                "ticks": status.get("ticks", 0),
                "late": status.get("late_ticks", 0),
                "resumes": status.get("resumes", 0),
                "fps": status.get("fps", 0),
                "p50": work.get("p50_us", 0),
                "p95": work.get("p95_us", 0),
                "max": work.get("max_us", 0),
                "cpu": process.get("cpu_percent", 0),
                "mem": process.get("working_set_mb", 0),
                "handles": process.get("handles", 0),
            })
            if len(samples) <= 3 or len(samples) % 12 == 0:
                print(f"[{elapsed:7.0f}s] ticks={samples[-1]['ticks']:>9} "
                      f"late={samples[-1]['late']:>4} resumes={samples[-1]['resumes']} "
                      f"fps={samples[-1]['fps']:.0f} p95={samples[-1]['p95']:.0f}µs "
                      f"cpu={samples[-1]['cpu']:.1f}% mem={samples[-1]['mem']:.1f}MB "
                      f"handles={samples[-1]['handles']}")
            next_at += args.interval
            delay = next_at - time.time()
            if delay > 0:
                time.sleep(delay)
            else:
                next_at = time.time()
    except Exception as exc:  # noqa: BLE001
        print(f"FAIL {exc}")
        ok = False
    finally:
        if feeder is not None:
            feeder.stop.set()
            feeder.join(timeout=1)
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()

    if not samples:
        print("FAIL 没有采样到任何状态")
        return 1

    # 前 10 秒作为基线（启动/头像绑定/JIT 的波动不算泄漏）
    baseline = next((s for s in samples if s["t"] >= 10.0), samples[0])
    last = samples[-1]
    tick_delta = last["ticks"] - baseline["ticks"]
    late_delta = last["late"] - baseline["late"]
    late_ratio = late_delta / tick_delta if tick_delta > 0 else 0.0
    mem_growth = max(s["mem"] for s in samples) - baseline["mem"]
    handle_growth = max(s["handles"] for s in samples) - baseline["handles"]

    print(f"\n采样 {len(samples)} 次 / {last['t']:.0f}s")
    print(f"  tick 增量 {tick_delta}，迟到 {late_delta}（{late_ratio * 100:.3f}%）")
    print(f"  工作集 {baseline['mem']:.1f} → {last['mem']:.1f} MB（峰值增量 {mem_growth:.1f}）")
    print(f"  句柄 {baseline['handles']} → {last['handles']}（峰值增量 {handle_growth}）")
    print(f"  CPU 平均 {sum(s['cpu'] for s in samples) / len(samples):.1f}%，"
          f"tick P95 最大 {max(s['p95'] for s in samples):.0f} µs")

    if late_ratio > 0.005:
        print("FAIL tick 迟到比例超过 0.5%")
        ok = False
    if mem_growth > 32.0:
        print("FAIL 工作集增长超过 32 MB")
        ok = False
    if handle_growth > 64:
        print("FAIL 句柄增长超过 64")
        ok = False
    resumes = last["resumes"]
    if resumes:
        print(f"WARN 期间发生 {resumes} 次休眠/长停顿恢复（引擎已自动复位）")

    print("\nSOAK OK" if ok else "\nSOAK FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
