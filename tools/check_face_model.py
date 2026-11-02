#!/usr/bin/env python3
"""校验 .fgbface（v3）与 face_map.json 的一致性。

渲染器在浏览器里跑，CI 不启 WebGL；这个脚本把"能离线测的部分"补上：
文件头与长度、索引越界、材质/区域合法性、稀疏位移的区间、morph/区域引用、
88 个形状的映射完整性。

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
MAX_DELTA = 0.5


def fail(message):
    print(f"FAIL {message}")
    return False


def shape_names_from_header():
    text = (ROOT / "src/core/model/shape.hpp").read_text(encoding="utf-8")
    block = text.split("#define FGB_SHAPE_LIST(X)", 1)[1].split("// clang-format on", 1)[0]
    return re.findall(r"X\((\w+)\)", block)


def check_binary(path, morphs, mapped_materials):
    data = path.read_bytes()
    ok = True
    if data[:4] != b"FGBF":
        return fail(f"{path.name}: magic 不是 FGBF")
    version, _flags, vertex_count, index_count = struct.unpack_from("<IIII", data, 4)
    morph_count, region_count, material_count = struct.unpack_from("<HBB", data, 20)
    if version != 3:
        return fail(f"{path.name}: version={version}（渲染器需要 v3）")
    if vertex_count < 100 or vertex_count > 65535 or index_count < 100:
        return fail(f"{path.name}: header 计数异常 {vertex_count}/{index_count}")
    if morph_count != len(morphs):
        return fail(f"{path.name}: morph 数 {morph_count} 与 face_map {len(morphs)} 不一致")
    if morph_count > 64 or region_count > 32:
        return fail(f"{path.name}: morph/区域超过契约上限")
    if material_count == 0 or material_count > 16:
        return fail(f"{path.name}: 材质数异常 {material_count}")
    if mapped_materials is not None and len(mapped_materials) != material_count:
        return fail(f"{path.name}: face_map.materials {len(mapped_materials)} 个，"
                    f"资产写的是 {material_count} 个")

    offset = 24
    positions = struct.unpack_from(f"<{vertex_count * 3}f", data, offset)
    offset += vertex_count * 12
    offset += vertex_count * 3  # normals i8
    indices = struct.unpack_from(f"<{index_count}H", data, offset)
    offset += index_count * 2
    regions = struct.unpack_from(f"<{vertex_count}I", data, offset)
    offset += vertex_count * 4
    materials = data[offset:offset + vertex_count]
    offset += vertex_count
    colors = data[offset:offset + vertex_count * 3]
    offset += vertex_count * 3
    if len(colors) != vertex_count * 3:
        return fail(f"{path.name}: 颜色数据长度不足")

    names = []
    for _ in range(morph_count):
        length = data[offset]
        offset += 1
        names.append(data[offset:offset + length].decode("utf-8"))
        offset += length
    if names != morphs:
        ok = fail(f"{path.name}: morph 名与 face_map 不一致")

    if any(not math.isfinite(v) for v in positions):
        ok = fail(f"{path.name}: positions 含 NaN/Inf")
    if any(i >= vertex_count for i in indices):
        ok = fail(f"{path.name}: 索引越界")
    if any(r >> region_count for r in regions):
        ok = fail(f"{path.name}: region_mask 用了未定义的位")
    if any(m >= material_count for m in materials):
        ok = fail(f"{path.name}: 材质 id 越界")
    if len(set(colors)) < 8:
        ok = fail(f"{path.name}: 颜色几乎是单一值（{len(set(colors))} 种分量），疑似没写进颜色")

    peaks = []
    touched = []
    for _ in range(morph_count):
        scale, count = struct.unpack_from("<fI", data, offset)
        offset += 8
        if not math.isfinite(scale) or scale < 0:
            ok = fail(f"{path.name}: morph scale 非法 {scale}")
        if count > vertex_count:
            ok = fail(f"{path.name}: 稀疏位移顶点数 {count} 超过顶点总数")
        index = struct.unpack_from(f"<{count}H", data, offset)
        offset += count * 2
        values = struct.unpack_from(f"<{count * 3}h", data, offset)
        offset += count * 6
        if any(i >= vertex_count for i in index):
            ok = fail(f"{path.name}: 稀疏位移索引越界")
        peak = (max(abs(v) for v in values) * scale) if values else 0.0
        peaks.append(peak)
        touched.append(count)
    if offset != len(data):
        ok = fail(f"{path.name}: 文件长度 {len(data)}，解析停在 {offset}")
    if not peaks or min(peaks) <= 0.001 or max(peaks) > MAX_DELTA:
        ok = fail(f"{path.name}: morph 峰值位移区间异常 [{min(peaks):.4f}, {max(peaks):.4f}]")
    print(f"OK 二进制：{vertex_count} 顶点 / {index_count // 3} 面 / {morph_count} morph，"
          f"峰值位移 {min(peaks):.3f}–{max(peaks):.3f}，"
          f"稀疏顶点 {min(touched)}–{max(touched)}，"
          f"{len(data) / 1048576:.2f} MiB")
    return ok


def check_map(path, all_shapes):
    data = json.loads(path.read_text(encoding="utf-8"))
    ok = True
    if data.get("version") != 3:
        ok = fail(f"face_map.version={data.get('version')}（应为 3）")
    regions = data.get("regions", [])
    if len(regions) != len(set(regions)) or len(regions) > 32:
        ok = fail("face_map.regions 重复或超过 32 个")
    materials = data.get("materials", [])
    if not materials or len(materials) != len(set(materials)):
        ok = fail("face_map.materials 缺失或重复")
    morphs = [item["name"] for item in data.get("morphs", [])]
    if len(morphs) != len(set(morphs)):
        ok = fail("face_map.morphs 名称重复")
    for item in data.get("morphs", []):
        if not item.get("shapes"):
            ok = fail(f"morph {item['name']} 没有映射任何形状")
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
        if not names:
            ok = fail(f"{shape} 的区域列表为空")
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
          f"{len(numeric_only)} 个仅数值，{len(morphs)} 个 morph，"
          f"{len(regions)} 个区域，{len(materials)} 个材质")
    return ok, materials


def main():
    all_shapes = shape_names_from_header()
    if len(all_shapes) != 88:
        fail(f"shape.hpp 解析出 {len(all_shapes)} 个形状")
        return 1
    map_path = FACE_DIR / "face_map.json"
    if not map_path.exists():
        fail(f"缺少 {map_path}")
        return 1
    data = json.loads(map_path.read_text(encoding="utf-8"))
    model_path = FACE_DIR / data.get("model", "head_v2.fgbface")
    if not model_path.exists():
        fail(f"缺少模型 {model_path}")
        return 1
    morphs = [item["name"] for item in data.get("morphs", [])]
    ok, materials = check_map(map_path, all_shapes)
    ok = check_binary(model_path, morphs, materials) and ok
    print("PASS" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
