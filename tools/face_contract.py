#!/usr/bin/env python3
"""`.fgbface` 与 `face_map.json` 的格式契约：区域名、morph 名、形状映射表、写出器。

资产生成器与校验脚本共用这里的定义，避免"资产换了、映射表没换"：

- `tools/bake_gnm_head.py`：从 Google GNM Head（Apache-2.0）烘焙 `ui/face/`。
- `tools/check_face_model.py`：按这里的名字与结构校验产物。

区域名与 morph 名是**稳定契约**：渲染器（`ui/face.js`）、2D 降级示意图
（`ui/index.html`）、校验脚本（`tools/check_face_model.py`）都按名字引用，
换资产时只替换几何与位移，不改名字。

格式 v3（little-endian，见 docs/adr/0021）：

    magic "FGBF" | version u32 = 3 | flags u32
    vertex_count u32 | index_count u32 | morph_count u16
    region_count u8 | material_count u8
    positions    f32[3N]
    normals      i8[3N]
    indices      u16[M]           // v2：顶点数上限 65535，索引省一半
    region_mask  u32[N]
    materials    u8[N]            // 材质 id，见 MATERIALS（决定高光/粗糙度）
    colors       u8[3N]           // v3：逐顶点基色（sRGB 近似），见 bake 里的平滑权重
    morph_names  [u8 len + utf8] * K
    // 稀疏位移：一个 morph 只影响脸上的一小块，稠密存 32×N 是浪费
    for each morph: f32 scale | u32 count | u16 index[count] | i16 value[3*count]

v2 相对 v1 的改动：u16 索引、材质、i16 量化位移、稀疏位移。扫描头模有 17.8k
顶点，稠密存 32 个 morph 要 3.4 MB；稀疏后约 1.1 MB，且渲染器每帧只遍历真正
受影响的顶点（下颌 morph ≈ 4k 个，而不是 17.8k 个）。

v3 相对 v2 的改动：颜色从"每个顶点一个材质 id"变成"每个顶点一个颜色"。材质 id
只能表达硬边界，眉毛、唇线这种没有几何分界的区域会沿三角形锯齿化；改成逐顶点
颜色后由光栅化插值出平滑过渡。
"""
import json
import re
import struct
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
FACE_DIR = ROOT / "ui" / "face"

# ---------------------------------------------------------------- 区域

REGIONS = [
    "jaw", "chin", "lowerLip", "upperLip", "mouthCornerL", "mouthCornerR",
    "mouthInterior", "cheekL", "cheekR", "browL", "browR", "nose",
    "eyelidL", "eyelidR", "eyeWhiteL", "eyeWhiteR", "irisL", "irisR",
    "pupilL", "pupilR", "forehead", "templeL", "templeR", "neck",
    "earL", "earR", "eyebrowL", "eyebrowR", "nostrilL", "nostrilR",
    "lipColorUpper", "lipColorLower",
]
REGION_BIT = {name: 1 << i for i, name in enumerate(REGIONS)}
assert len(REGIONS) <= 32

# 材质（着色用，和区域高亮是两件事：一个顶点只能属于一个材质，但可以属于多个区域）
MATERIALS = [
    "skin", "lip", "brow", "sclera", "iris", "pupil",
    "mouth", "teeth", "gums", "tongue", "nostril",
]
MATERIAL_ID = {name: i for i, name in enumerate(MATERIALS)}

# ---------------------------------------------------------------- morph

MORPH_NAMES = [
    "jawOpen", "jawLeft", "jawRight", "jawForward",
    "smileL", "smileR", "frownL", "frownR",
    "pucker", "lipPress",
    "upperLipUpL", "upperLipUpR", "lowerLipDownL", "lowerLipDownR",
    "browInnerUpL", "browInnerUpR", "browOuterUpL", "browOuterUpR",
    "browDownL", "browDownR",
    "eyeBlinkL", "eyeBlinkR", "eyeWideL", "eyeWideR",
    "cheekSquintL", "cheekSquintR", "cheekPuffL", "cheekPuffR",
    "cheekSuckL", "cheekSuckR", "noseSneerL", "noseSneerR",
]

# 形状 → 模型 morph 权重。允许负权重：JawMandibleRaise 用来抵消 JawOpen。
SHAPE_MAP = {
    "EyeSquintRight": {"cheekSquintR": 0.80, "eyeBlinkR": 0.20},
    "EyeSquintLeft": {"cheekSquintL": 0.80, "eyeBlinkL": 0.20},
    "EyeWideRight": {"eyeWideR": 1.0},
    "EyeWideLeft": {"eyeWideL": 1.0},

    "BrowPinchRight": {"browDownR": 0.40, "browInnerUpR": 0.15},
    "BrowPinchLeft": {"browDownL": 0.40, "browInnerUpL": 0.15},
    "BrowLowererRight": {"browDownR": 1.0},
    "BrowLowererLeft": {"browDownL": 1.0},
    "BrowInnerUpRight": {"browInnerUpR": 1.0},
    "BrowInnerUpLeft": {"browInnerUpL": 1.0},
    "BrowOuterUpRight": {"browOuterUpR": 1.0},
    "BrowOuterUpLeft": {"browOuterUpL": 1.0},

    "NasalDilationRight": {"noseSneerR": 0.60},
    "NasalDilationLeft": {"noseSneerL": 0.60},
    "NasalConstrictRight": {"noseSneerR": 0.20},
    "NasalConstrictLeft": {"noseSneerL": 0.20},

    "CheekSquintRight": {"cheekSquintR": 1.0, "eyeBlinkR": 0.15},
    "CheekSquintLeft": {"cheekSquintL": 1.0, "eyeBlinkL": 0.15},
    "CheekPuffRight": {"cheekPuffR": 1.0},
    "CheekPuffLeft": {"cheekPuffL": 1.0},
    "CheekSuckRight": {"cheekSuckR": 1.0},
    "CheekSuckLeft": {"cheekSuckL": 1.0},

    "JawOpen": {"jawOpen": 1.0},
    "JawRight": {"jawRight": 1.0},
    "JawLeft": {"jawLeft": 1.0},
    "JawForward": {"jawForward": 1.0},
    "JawClench": {"jawOpen": 0.12, "lipPress": 0.25},
    "JawMandibleRaise": {"jawOpen": -0.50},
    "MouthClosed": {"lipPress": 0.60},

    "LipSuckUpperRight": {"lipPress": 0.30, "upperLipUpR": 0.10},
    "LipSuckUpperLeft": {"lipPress": 0.30, "upperLipUpL": 0.10},
    "LipSuckLowerRight": {"lipPress": 0.30, "lowerLipDownR": 0.10},
    "LipSuckLowerLeft": {"lipPress": 0.30, "lowerLipDownL": 0.10},
    "LipSuckCornerRight": {"pucker": 0.25, "frownR": 0.20},
    "LipSuckCornerLeft": {"pucker": 0.25, "frownL": 0.20},

    "LipFunnelUpperRight": {"pucker": 0.35},
    "LipFunnelUpperLeft": {"pucker": 0.35},
    "LipFunnelLowerRight": {"pucker": 0.35},
    "LipFunnelLowerLeft": {"pucker": 0.35},
    "LipPuckerUpperRight": {"pucker": 0.55},
    "LipPuckerUpperLeft": {"pucker": 0.55},
    "LipPuckerLowerRight": {"pucker": 0.55},
    "LipPuckerLowerLeft": {"pucker": 0.55},

    "MouthUpperUpRight": {"upperLipUpR": 0.80},
    "MouthUpperUpLeft": {"upperLipUpL": 0.80},
    "MouthUpperDeepenRight": {"upperLipUpR": 0.50, "smileR": 0.15},
    "MouthUpperDeepenLeft": {"upperLipUpL": 0.50, "smileL": 0.15},
    "NoseSneerRight": {"noseSneerR": 1.0, "upperLipUpR": 0.15},
    "NoseSneerLeft": {"noseSneerL": 1.0, "upperLipUpL": 0.15},
    "MouthLowerDownRight": {"lowerLipDownR": 0.80},
    "MouthLowerDownLeft": {"lowerLipDownL": 0.80},
    "MouthUpperRight": {"upperLipUpR": 0.30},
    "MouthUpperLeft": {"upperLipUpL": 0.30},
    "MouthLowerRight": {"lowerLipDownR": 0.30},
    "MouthLowerLeft": {"lowerLipDownL": 0.30},

    "MouthCornerPullRight": {"smileR": 1.0},
    "MouthCornerPullLeft": {"smileL": 1.0},
    "MouthCornerSlantRight": {"smileR": 0.50, "upperLipUpR": 0.15},
    "MouthCornerSlantLeft": {"smileL": 0.50, "upperLipUpL": 0.15},
    "MouthFrownRight": {"frownR": 1.0},
    "MouthFrownLeft": {"frownL": 1.0},
    "MouthStretchRight": {"smileR": 0.30},
    "MouthStretchLeft": {"smileL": 0.30},
    "MouthDimpleRight": {"smileR": 0.45},
    "MouthDimpleLeft": {"smileL": 0.45},

    "MouthRaiserUpper": {"upperLipUpL": 0.35, "upperLipUpR": 0.35},
    "MouthRaiserLower": {"lowerLipDownL": 0.35, "lowerLipDownR": 0.35},
    "MouthPressRight": {"lipPress": 0.50, "frownR": 0.10},
    "MouthPressLeft": {"lipPress": 0.50, "frownL": 0.10},
    "MouthTightenerRight": {"lipPress": 0.35, "smileR": 0.15},
    "MouthTightenerLeft": {"lipPress": 0.35, "smileL": 0.15},
}

SHAPE_REGIONS = {
    "EyeSquintRight": ["eyelidR", "cheekR"],
    "EyeSquintLeft": ["eyelidL", "cheekL"],
    "EyeWideRight": ["eyelidR"],
    "EyeWideLeft": ["eyelidL"],
    "BrowPinchRight": ["browR", "forehead"],
    "BrowPinchLeft": ["browL", "forehead"],
    "BrowLowererRight": ["browR"],
    "BrowLowererLeft": ["browL"],
    "BrowInnerUpRight": ["browR", "forehead"],
    "BrowInnerUpLeft": ["browL", "forehead"],
    "BrowOuterUpRight": ["browR", "templeR"],
    "BrowOuterUpLeft": ["browL", "templeL"],
    "NasalDilationRight": ["nose"],
    "NasalDilationLeft": ["nose"],
    "NasalConstrictRight": ["nose"],
    "NasalConstrictLeft": ["nose"],
    "CheekSquintRight": ["cheekR", "eyelidR"],
    "CheekSquintLeft": ["cheekL", "eyelidL"],
    "CheekPuffRight": ["cheekR"],
    "CheekPuffLeft": ["cheekL"],
    "CheekSuckRight": ["cheekR"],
    "CheekSuckLeft": ["cheekL"],

    "JawOpen": ["jaw", "chin", "lowerLip", "mouthInterior"],
    "JawRight": ["jaw", "chin", "lowerLip"],
    "JawLeft": ["jaw", "chin", "lowerLip"],
    "JawForward": ["jaw", "chin", "lowerLip"],
    "JawClench": ["jaw", "chin", "lowerLip"],
    "JawMandibleRaise": ["jaw", "chin", "lowerLip"],
    "MouthClosed": ["upperLip", "lowerLip"],

    "LipSuckUpperRight": ["upperLip", "mouthCornerR"],
    "LipSuckUpperLeft": ["upperLip", "mouthCornerL"],
    "LipSuckLowerRight": ["lowerLip", "mouthCornerR"],
    "LipSuckLowerLeft": ["lowerLip", "mouthCornerL"],
    "LipSuckCornerRight": ["mouthCornerR", "upperLip", "lowerLip"],
    "LipSuckCornerLeft": ["mouthCornerL", "upperLip", "lowerLip"],
    "LipFunnelUpperRight": ["upperLip", "mouthCornerR"],
    "LipFunnelUpperLeft": ["upperLip", "mouthCornerL"],
    "LipFunnelLowerRight": ["lowerLip", "mouthCornerR"],
    "LipFunnelLowerLeft": ["lowerLip", "mouthCornerL"],
    "LipPuckerUpperRight": ["upperLip", "mouthCornerR"],
    "LipPuckerUpperLeft": ["upperLip", "mouthCornerL"],
    "LipPuckerLowerRight": ["lowerLip", "mouthCornerR"],
    "LipPuckerLowerLeft": ["lowerLip", "mouthCornerL"],

    "MouthUpperUpRight": ["upperLip"],
    "MouthUpperUpLeft": ["upperLip"],
    "MouthUpperDeepenRight": ["upperLip", "mouthCornerR"],
    "MouthUpperDeepenLeft": ["upperLip", "mouthCornerL"],
    "NoseSneerRight": ["nose", "upperLip"],
    "NoseSneerLeft": ["nose", "upperLip"],
    "MouthLowerDownRight": ["lowerLip", "chin"],
    "MouthLowerDownLeft": ["lowerLip", "chin"],
    "MouthUpperRight": ["upperLip"],
    "MouthUpperLeft": ["upperLip"],
    "MouthLowerRight": ["lowerLip"],
    "MouthLowerLeft": ["lowerLip"],
    "MouthCornerPullRight": ["mouthCornerR", "cheekR"],
    "MouthCornerPullLeft": ["mouthCornerL", "cheekL"],
    "MouthCornerSlantRight": ["mouthCornerR", "cheekR"],
    "MouthCornerSlantLeft": ["mouthCornerL", "cheekL"],
    "MouthFrownRight": ["mouthCornerR", "lowerLip"],
    "MouthFrownLeft": ["mouthCornerL", "lowerLip"],
    "MouthStretchRight": ["mouthCornerR", "cheekR"],
    "MouthStretchLeft": ["mouthCornerL", "cheekL"],
    "MouthDimpleRight": ["mouthCornerR", "cheekR"],
    "MouthDimpleLeft": ["mouthCornerL", "cheekL"],
    "MouthRaiserUpper": ["upperLip"],
    "MouthRaiserLower": ["lowerLip"],
    "MouthPressRight": ["upperLip", "lowerLip"],
    "MouthPressLeft": ["upperLip", "lowerLip"],
    "MouthTightenerRight": ["upperLip", "lowerLip", "mouthCornerR"],
    "MouthTightenerLeft": ["upperLip", "lowerLip", "mouthCornerL"],
}

# ---------------------------------------------------------------- 校验


def shape_names_from_header():
    text = (ROOT / "src/core/model/shape.hpp").read_text(encoding="utf-8")
    block = text.split("#define FGB_SHAPE_LIST(X)", 1)[1].split("// clang-format on", 1)[0]
    names = re.findall(r"X\((\w+)\)", block)
    if len(names) != 88:
        raise SystemExit(f"shape.hpp 里解析出 {len(names)} 个形状，预期 88")
    return names


def validate_map(all_shapes):
    unknown = sorted(set(SHAPE_MAP) - set(all_shapes))
    if unknown:
        raise SystemExit(f"SHAPE_MAP 含未知形状：{unknown}")
    for shape, weights in SHAPE_MAP.items():
        for morph in weights:
            if morph not in MORPH_NAMES:
                raise SystemExit(f"{shape} 映射到未定义的 morph：{morph}")
    for shape, regions in SHAPE_REGIONS.items():
        if shape not in SHAPE_MAP:
            raise SystemExit(f"{shape} 有区域但没有 morph 映射")
        for region in regions:
            if region not in REGIONS:
                raise SystemExit(f"{shape} 映射到未定义的区域：{region}")


# ---------------------------------------------------------------- 写出


def write_face_map(path, all_shapes, model_name, note, generated_by):
    numeric_only = [name for name in all_shapes if name not in SHAPE_MAP]
    data = {
        "version": 3,
        "model": model_name,
        "generated_by": generated_by,
        "note": note,
        "regions": REGIONS,
        "materials": MATERIALS,
        "morphs": [
            {"name": name, "shapes": {shape: weights[name]
                                      for shape, weights in SHAPE_MAP.items()
                                      if name in weights}}
            for name in MORPH_NAMES
        ],
        "shape_regions": SHAPE_REGIONS,
        "numeric_only": numeric_only,
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def write_fgbface(path, positions, normals, indices, regions, materials, colors,
                  morph_names, deltas):
    """写出 v3 资产。positions/deltas 单位与渲染器一致（米或缩放后的单位都行）。"""
    pos = np.ascontiguousarray(np.asarray(positions, dtype=np.float32))
    vertex_count = pos.shape[0]
    if vertex_count > 65535:
        raise SystemExit(f"u16 索引的顶点数上限是 65535（当前 {vertex_count}）")
    nrm = np.clip(np.round(np.asarray(normals, dtype=np.float64) * 127.0),
                  -127, 127).astype(np.int8)
    idx = np.ascontiguousarray(np.asarray(indices, dtype=np.uint32))
    if idx.size and int(idx.max()) >= vertex_count:
        raise SystemExit("索引越界")
    idx16 = np.ascontiguousarray(idx.astype(np.uint16))
    reg = np.ascontiguousarray(np.asarray(regions, dtype=np.uint32))
    mat = np.ascontiguousarray(np.asarray(materials, dtype=np.uint8))
    col = np.clip(np.round(np.asarray(colors, dtype=np.float64) * 255.0), 0, 255)
    col = np.ascontiguousarray(col.astype(np.uint8))
    if mat.shape[0] != vertex_count or reg.shape[0] != vertex_count:
        raise SystemExit("region/material 数量与顶点数不一致")
    if col.shape != (vertex_count, 3):
        raise SystemExit(f"colors 形状应为 ({vertex_count}, 3)，实际 {col.shape}")
    if not morph_names:
        raise SystemExit("至少需要一个 morph")

    sparse = []
    for delta in deltas:
        d = np.asarray(delta, dtype=np.float64).reshape(vertex_count, 3)
        peak = float(np.abs(d).max()) if d.size else 0.0
        scale = peak / 32000.0 if peak > 0 else 1.0
        if peak <= 0:
            sparse.append((np.float32(1.0), np.zeros(0, dtype=np.uint16),
                           np.zeros(0, dtype=np.int16)))
            continue
        q = np.clip(np.round(d / scale), -32767, 32767).astype(np.int32)
        touched = np.nonzero(np.abs(q).sum(axis=1))[0]
        sparse.append((np.float32(scale),
                       np.ascontiguousarray(touched.astype(np.uint16)),
                       np.ascontiguousarray(q[touched].reshape(-1).astype(np.int16))))

    out = bytearray()
    out += b"FGBF"
    out += struct.pack("<III", 3, 0, vertex_count)
    out += struct.pack("<IH", idx16.size, len(morph_names))
    out += struct.pack("<BB", len(REGIONS), len(MATERIALS))
    out += pos.tobytes()
    out += nrm.tobytes()
    out += idx16.tobytes()
    out += reg.tobytes()
    out += mat.tobytes()
    out += col.tobytes()
    for name in morph_names:
        raw = name.encode("utf-8")
        if len(raw) > 255:
            raise SystemExit(f"morph 名过长：{name}")
        out += bytes([len(raw)]) + raw
    for scale, index, value in sparse:
        out += struct.pack("<fI", float(scale), index.size)
        out += index.tobytes()
        out += value.tobytes()
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(bytes(out))
    return len(out)


def vertex_normals(positions, indices):
    """按面法线累积的平滑顶点法线（资产烘焙与位移后都适用）。"""
    pos = np.asarray(positions, dtype=np.float64)
    tri = np.asarray(indices, dtype=np.int64).reshape(-1, 3)
    fn = np.cross(pos[tri[:, 1]] - pos[tri[:, 0]], pos[tri[:, 2]] - pos[tri[:, 0]])
    nrm = np.zeros_like(pos)
    for k in range(3):
        np.add.at(nrm, tri[:, k], fn)
    length = np.linalg.norm(nrm, axis=1, keepdims=True)
    return nrm / np.maximum(length, 1e-12)
