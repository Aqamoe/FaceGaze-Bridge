"""崩溃捕获端到端探针。

启动 tests/crash_child（故意写空指针），确认：
  - 进程以异常码退出（不是 0）
  - 崩溃目录里出现 .dmp，开头是 MDMP
  - 旁边有 .txt 说明（版本/时间/异常码）

用法：
    python tools/probe_crash.py
    python tools/probe_crash.py --child build/test/bin/Debug/fgb-crash-child.exe
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--child", default=os.path.join("build", "test", "bin", "Debug",
                                                        "fgb-crash-child.exe"))
    args = parser.parse_args()
    if not os.path.isfile(args.child):
        print(f"FAIL 找不到崩溃子进程：{args.child}")
        return 1

    crash_dir = tempfile.mkdtemp(prefix="fgb-crash-probe-")
    try:
        result = subprocess.run([args.child, crash_dir], capture_output=True)
        print(f"子进程退出码 0x{result.returncode & 0xFFFFFFFF:08X}")
        if result.returncode == 0:
            print("FAIL 故意崩溃的子进程返回了 0")
            return 1

        dumps = [name for name in os.listdir(crash_dir) if name.endswith(".dmp")]
        texts = [name for name in os.listdir(crash_dir) if name.endswith(".txt")]
        if not dumps:
            print("FAIL 没有生成 .dmp")
            return 1
        path = os.path.join(crash_dir, dumps[0])
        size = os.path.getsize(path)
        with open(path, "rb") as handle:
            magic = handle.read(4)
        print(f"转储：{dumps[0]} {size} 字节 magic={magic!r}")
        if magic != b"MDMP" or size < 1024:
            print("FAIL 转储不是有效的 minidump")
            return 1
        if not texts:
            print("FAIL 缺少 .txt 说明")
            return 1
        with open(os.path.join(crash_dir, texts[0]), encoding="utf-8", errors="replace") as handle:
            text = handle.read()
        print("说明：", " | ".join(line for line in text.splitlines()[:3]))
        if "unhandled exception" not in text or "exception_code" not in text:
            print("FAIL 说明文件缺少异常信息")
            return 1
    finally:
        shutil.rmtree(crash_dir, ignore_errors=True)

    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
