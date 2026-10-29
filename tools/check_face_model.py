#!/usr/bin/env python3
"""校验 .fgbface 与 face_map.json 的一致性。

渲染器在浏览器里跑，CI 不启 WebGL；这个脚本把"能离线测的部分"补上：
文件头与长度、索引越界、morph/区域引用、88 个形状的映射完整性。

用法：python tools/check_face_model.py
"""
import json
import math
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FACE_DIR = ROOT / "ui" / "face"


def fail(message):
    print(f"FAIL {message}")
    return False


def shape_names_from_header():
    text = (ROOT / "src/core/model/shape.hpp").read_text(encoding="utf-8")
    block = text.split("#define FGB_SHAPE_LIST(X)", 1)[1].split("// clang-format on", 1)[0]
    return re.findall(r"X\((\w+)\)", block)


def half_to_float(h):
    sign = -1.0 if h & 0x8000 else 1.0
    exp = (h >> 10) & 0x1F
    frac = h & 0x3FF
    if exp == 0:
        return sign * (2.0 ** -14) * (frac / 1024.0)
    if exp == 31:
        return float("nan") if frac else sign * float("inf")
    return sign * (2.0 ** (exp - 15)) * (1.0 + frac / 1024.0)


def check_binary(path, morphs):
    data = path.read_bytes()
    ok = True
    if data[:4] != b"FGBF":
        return fail(f"{path.name}: magic 不是 FGBF")
    version, flags, vertex_count, index_count = struct.unpack_from("<IIII", data, 4)
    morph_count, region_count, _ = struct.unpack_from("<HBB", data, 20)
    if version != 1:
        ok = fail(f"{path.name}: version={version}")
    if vertex_count < 100 or index_count < 100 or morph_count != len(morphs):
        ok = fail(f"{path.name}: header 计数异常 {vertex_count}/{index_count}/{morph_count}")
    if morph_count > 64 or region_count > 32:
        ok = fail(f"{path.name}: morph/区域超过契约上限")
    offset = 24
    positions = struct.unpack_from(f"<{vertex_count * 3}f", data, offset)
    offset += vertex_count * 12
    offset += vertex_count * 3  # normals i8
    indices = struct.unpack_from(f"<{index_count}I", data, offset)
    offset += index_count * 4
    regions = struct.unpack_from(f"<{vertex_count}I", data, offset)
    offset += vertex_count * 4
    names = []
    for _ in range(morph_count):
        length = data[offset]
        offset += 1
        names.append(data[offset:offset + length].decode("utf-8"))
        offset += length
    if names != morphs:
        ok = fail(f"{path.name}: morph 名与 face_map 不一致")
    expected = offset + vertex_count * 3 * morph_count * 2
    if len(data) != expected:
        ok = fail(f"{path.name}: 文件长度 {len(data)}，期望 {expected}")
    if any(not math.isfinite(v) for v in positions):
        ok = fail(f"{path.name}: positions 含 NaN/Inf")
    if any(i >= vertex_count for i in indices):
        ok = fail(f"{path.name}: 索引越界")
    if any(r >> region_count for r in regions):
        ok = fail(f"{path.name}: region_mask 用了未定义的位")
    max_delta = 0.0
    for _ in range(morph_count):
        deltas = struct.unpack_from(f"<{vertex_count * 3}e", data, offset)
        offset += vertex_count * 6
        if any(not math.isfinite(v) for v in deltas):
            ok = fail(f"{path.name}: morph delta 含 NaN/Inf")
        max_delta = max(max_delta, max(abs(v) for v in deltas))
    if max_delta <= 0.001 or max_delta > 0.5:
        ok = fail(f"{path.name}: morph delta 范围异常 {max_delta}")
    print(f"OK 二进制：{vertex_count} 顶点 / {index_count} 索引 / {morph_count} morph，"
          f"最大位移 {max_delta:.3f}")
    return ok


def check_map(path, all_shapes):
    data = json.loads(path.read_text(encoding="utf-8"))
    ok = True
    regions = data.get("regions", [])
    if len(regions) != len(set(regions)) or len(regions) > 32:
        ok = fail("face_map.regions 重复或超过 32 个")
    morphs = [item["name"] for item in data.get("morphs", [])]
    if len(morphs) != len(set(morphs)):
        ok = fail("face_map.morphs 名称重复")
    for item in data.get("morphs", []):
        for name, weight in item.get("shapes", {}).items():
            if name not in all_shapes:
                ok = fail(f"morph {item['name']} 引用未知形状 {name}")
            if not math.isfinite(float(weight)) or abs(float(weight)) > 2.0:
                ok = fail(f"morph {item['name']} 的 {name} 权重异常 {weight}")
    shape_regions = data.get("shape_regions", {})
    numeric_only = data.get("numeric_only", [])
    mapped = set(shape_regions)
    if mapped - set(all_shapes):
        ok = fail(f"shape_regions 含未知形状 {sorted(mapped - set(all_shapes))}")
    if mapped | set(numeric_only) != set(all_shapes):
        missing = sorted(set(all_shapes) - mapped - set(numeric_only))
        extra = sorted((mapped | set(numeric_only)) - set(all_shapes))
        ok = fail(f"形状覆盖不完整：missing={missing} extra={extra}")
    overlap = mapped & set(numeric_only)
    if overlap:
        ok = fail(f"形状同时被标为可可视化与仅数值：{sorted(overlap)}")
    for shape, names in shape_regions.items():
        for region in names:
            if region not in regions:
                ok = fail(f"{shape} 引用未知区域 {region}")
    # 有 morph 映射的形状必须也有区域，否则选中时无法高亮
    morph_shapes = set()
    for item in data.get("morphs", []):
        morph_shapes |= set(item.get("shapes", {}))
    if morph_shapes != mapped:
        ok = fail(f"morph 映射与 shape_regions 不一致："
                  f"缺区域={sorted(morph_shapes - mapped)} 缺 morph={sorted(mapped - morph_shapes)}")
    print(f"OK 映射：{len(mapped)}/{len(all_shapes)} 个形状可视化，"
          f"{len(numeric_only)} 个仅数值，{len(morphs)} 个 morph，{len(regions)} 个区域")
    return ok


def main():
    all_shapes = shape_names_from_header()
    if len(all_shapes) != 88:
        return 1 if fail(f"shape.hpp 解析出 {len(all_shapes)} 个形状") else 1
    map_path = FACE_DIR / "face_map.json"
    if not map_path.exists():
        return 1 if fail(f"缺少 {map_path}") else 1
    data = json.loads(map_path.read_text(encoding="utf-8"))
    model_path = FACE_DIR / data.get("model", "head_v1.fgbface")
    morphs = [item["name"] for item in data.get("morphs", [])]
    ok = check_binary(model_path, morphs)
    ok = check_map(map_path, all_shapes) and ok
    print("PASS" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
