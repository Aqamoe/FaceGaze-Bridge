#!/usr/bin/env python3
"""从 VRCFaceTracking 源码生成 v1 legacy 唇形合并表。

    python tools/gen_v1_tables.py --src /tmp/vrcft_full

为什么用生成而不是手抄：91 条规则每条都是 (正形状集合, 负形状集合, 混合方式) 的三元组，
手抄时的错位不会导致编译失败，只会让某个表情在某个方向上悄悄失灵——这类 bug 极难发现。
生成脚本让"表"成为上游源码的确定性投影，同时把差异限制在一处。

输出：src/params/v1_lip_tables.inc
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

SOURCE = (
    "VRCFaceTracking.Core/Params/Expressions/Legacy/Lip/LipShapeMerger.cs"
)

BLEND_BY_CLASS = {
    "PositiveNegativeShape": "Diff",
    "PositiveNegativeAveragedShape": "Avg",
}


def parse_entries(text: str) -> list[tuple[str, str, str]]:
    body = text[text.index("MergedShapes") : text.index("AllLipParameters")]
    # {"Name", new Class(args)},
    return re.findall(r'\{"(\w+)",\s*new (\w+)\((.*?)\)\}\s*,', body, re.S)


def split_args(args: str) -> tuple[list[str], list[str], str]:
    """把构造参数拆成 (正形状, 负形状, 混合方式)。"""
    array_lists = re.findall(r"new\s*SRanipal_LipShape_v2\[\]\s*\{([^}]*)\}", args)
    rest = re.sub(r"new\s*SRanipal_LipShape_v2\[\]\s*\{[^}]*\}", "", args)
    scalars = re.findall(r"SRanipal_LipShape_v2\.(\w+)", rest)

    def names(chunk: str) -> list[str]:
        # 数组字面量里的元素写作 `SRanipal_LipShape_v2.X`，标量已经剥掉了前缀，
        # 这里统一成裸枚举名，避免生成出 `SRanipalShape::SRanipal_LipShape_v2.X`。
        cleaned = chunk.replace("SRanipal_LipShape_v2.", "")
        return [x.strip() for x in cleaned.split(",") if x.strip()]

    if array_lists:
        positive = names(array_lists[0])
        negative = names(array_lists[1]) if len(array_lists) > 1 else []
        blend = "Max" if re.search(r",\s*true\s*$", args.strip()) else "Avg"
    else:
        positive = scalars[:1]
        negative = scalars[1:]
        blend = "DiffStep" if re.search(r",\s*true\s*$", args.strip()) else "Diff"
    return positive, negative, blend


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True, help="VRCFaceTracking 源码根目录")
    ap.add_argument(
        "--out",
        default="src/params/v1_lip_tables.inc",
        help="输出文件",
    )
    args = ap.parse_args()

    src_path = pathlib.Path(args.src) / SOURCE
    if not src_path.is_file():
        print(f"找不到源码：{src_path}", file=sys.stderr)
        return 1

    entries = parse_entries(src_path.read_text(encoding="utf-8-sig"))
    if not entries:
        print("解析不到任何规则，源码格式可能变了", file=sys.stderr)
        return 1

    pool: list[str] = []
    rules: list[str] = []
    for name, cls, raw_args in entries:
        positive, negative, blend = split_args(raw_args)
        if cls not in BLEND_BY_CLASS:
            print(f"未知形状类 {cls}（{name}）", file=sys.stderr)
            return 1
        if cls == "PositiveNegativeShape" and blend not in ("Diff", "DiffStep"):
            blend = "Diff"
        pos_begin = len(pool)
        pool.extend(positive)
        neg_begin = len(pool)
        pool.extend(negative)
        rules.append(
            f'    {{"{name}", Blend::{blend}, {pos_begin}, {len(positive)}, '
            f"{neg_begin}, {len(negative)}}},"
        )

    lines = [
        "// 由 tools/gen_v1_tables.py 从 VRCFaceTracking 源码生成，请勿手改。",
        "// 上游：VRCFaceTracking.Core/Params/Expressions/Legacy/Lip/LipShapeMerger.cs",
        f"// 规则数：{len(rules)}，形状引用数：{len(pool)}",
        "",
        "// clang-format off",
        "static constexpr SRanipalShape kMergePool[] = {",
    ]
    for i in range(0, len(pool), 6):
        chunk = ", ".join(f"SRanipalShape::{n}" for n in pool[i : i + 6])
        lines.append(f"    {chunk},")
    lines += [
        "};",
        "",
        "static constexpr MergeRuleDef kMergeRules[] = {",
        *rules,
        "};",
        "// clang-format on",
        "",
    ]

    out = pathlib.Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(lines), encoding="utf-8")
    print(f"写入 {out}：{len(rules)} 条规则，{len(pool)} 个形状引用")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
