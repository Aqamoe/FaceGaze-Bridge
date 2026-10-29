"""从已安装的 VRCFT 提取一份可独立运行的模块宿主运行时。

为什么需要：A 层（VRCFT 模块兼容）执行的是 VRCFaceTracking.ModuleProcess.exe，
它旁边需要 Core/SDK 和 .NET 运行时。只把模块 DLL 复制到 FGB 的模块仓还不够——
VRCFT 被卸载后，宿主就没了。这个脚本把 ModuleProcess 的依赖闭包（按
VRCFaceTracking.ModuleProcess.deps.json 解析）复制到发布包的 runtime/vrcft。

用法（在 dist 生成之后跑）：
    python tools/build_vrcft_runtime.py --dest dist/runtime/vrcft

默认从 Steam 的两个常见安装位置找 VRCFT，也可以 --source 指定。
"""

import argparse
import json
import os
import shutil
import sys
from datetime import datetime
from pathlib import Path

DEFAULT_CANDIDATES = [
    Path(r"C:\Program Files (x86)\Steam\steamapps\common\VRCFaceTracking"),
    Path(r"C:\Program Files\Steam\steamapps\common\VRCFaceTracking"),
]

# 这些即使 deps.json 没列出来也必须带：宿主启动器与运行时引导
ALWAYS = [
    "VRCFaceTracking.ModuleProcess.exe",
    "VRCFaceTracking.ModuleProcess.deps.json",
    "VRCFaceTracking.ModuleProcess.runtimeconfig.json",
    "VRCFaceTracking.Core.dll",
    "VRCFaceTracking.Core.deps.json",
    "VRCFaceTracking.Core.dll.config",
    "VRCFaceTracking.SDK.dll",
    "VRCFaceTracking.SDK.deps.json",
    "hostfxr.dll",
    "hostpolicy.dll",
    "coreclr.dll",
    "clrjit.dll",
    "System.Private.CoreLib.dll",
]


def find_source(explicit: str | None) -> Path:
    if explicit:
        path = Path(explicit)
        if (path / "VRCFaceTracking.ModuleProcess.exe").exists():
            return path
        raise SystemExit(f"指定目录里没有 VRCFaceTracking.ModuleProcess.exe：{path}")
    for candidate in DEFAULT_CANDIDATES:
        if (candidate / "VRCFaceTracking.ModuleProcess.exe").exists():
            return candidate
    raise SystemExit("没找到 VRCFT 安装目录，用 --source 指定（或先装一份 VRCFT）")


def dependency_files(root: Path) -> set[str]:
    deps_path = root / "VRCFaceTracking.ModuleProcess.deps.json"
    deps = json.loads(deps_path.read_text(encoding="utf-8-sig"))
    targets = deps.get("targets", {})
    target = None
    for name in targets:
        if "win-x64" in name:
            target = targets[name]
            break
    if target is None and targets:
        target = next(iter(targets.values()))

    names: set[str] = set()
    for info in (target or {}).values():
        for items in info.values():
            if not isinstance(items, dict):
                continue
            for rel in items:
                # 自包含发布的文件都被拍平到应用根目录，deps 里的 lib/netX/Name.dll
                # 实际对应 root/Name.dll
                names.add(os.path.basename(rel.replace("/", os.sep)))
    return names


def copy_file(src: Path, dest: Path) -> int:
    if not src.is_file():
        return 0
    shutil.copy2(src, dest / src.name)
    return src.stat().st_size


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", help="VRCFT 安装目录")
    parser.add_argument("--dest", default="dist/runtime/vrcft", help="输出目录")
    args = parser.parse_args()

    root = find_source(args.source)
    dest = Path(args.dest).resolve()
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True, exist_ok=True)

    wanted = set(ALWAYS) | dependency_files(root)
    copied = 0
    total = 0
    for name in sorted(wanted):
        size = copy_file(root / name, dest)
        if size:
            copied += 1
            total += size

    if not (dest / "VRCFaceTracking.ModuleProcess.exe").exists():
        raise SystemExit("复制结果里缺少 ModuleProcess.exe，检查源目录")

    # 许可证与来源说明：VRCFT 是 Apache-2.0，再分发二进制要保留声明。
    # Steam 安装目录里通常没有 LICENSE 文件，此时用本仓库的 Apache-2.0 全文，
    # 并单独写一份 VRCFaceTracking 的来源说明。
    license_copied = False
    for name in ("LICENSE", "LICENSE.txt", "LICENSE.md"):
        if (root / name).exists():
            copy_file(root / name, dest)
            license_copied = True
            break
    if not license_copied:
        repo_license = Path(__file__).resolve().parent.parent / "LICENSE"
        if repo_license.exists():
            shutil.copy2(repo_license, dest / "LICENSE")

    now = datetime.now().strftime("%Y-%m-%d %H:%M")
    notice = "\n".join(
        [
            "VRCFaceTracking (Apache-2.0)",
            "https://github.com/benaclejames/VRCFaceTracking",
            "",
            "本目录中的 VRCFaceTracking.ModuleProcess.exe、VRCFaceTracking.Core.dll、",
            "VRCFaceTracking.SDK.dll 及其 .NET 依赖来自上述项目。",
            "FaceGaze Bridge 仅按 ModuleProcess.deps.json 的依赖闭包复制文件，",
            "未修改这些二进制的内容。",
            "",
        ]
    )
    (dest / "NOTICE-VRCFaceTracking.txt").write_text(notice, encoding="utf-8")

    readme = "\n".join(
        [
            "FaceGaze Bridge 内置的 VRCFT 模块宿主运行时",
            "================================================",
            "",
            f"提取自：{root}",
            f"提取时间：{now}",
            f"文件数：{copied}",
            "",
            "来源项目：https://github.com/benaclejames/VRCFaceTracking",
            "许可证：Apache-2.0（见 LICENSE 与 NOTICE-VRCFaceTracking.txt）",
            "",
            "这是“精简”文件集：按 ModuleProcess.deps.json 解析出的依赖闭包，",
            "不含 VRCFT 的界面（WinUI）与内置推理（ONNX/DirectML）。",
            "个别模块如果提示缺少程序集，把对应 DLL 从 VRCFT 安装目录补进来即可。",
            "",
        ]
    )
    (dest / "README-FGB.txt").write_text(readme, encoding="utf-8")

    print(f"运行时已生成：{dest}")
    print(f"  {copied} 个文件，{total / 1048576:.1f} MB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
