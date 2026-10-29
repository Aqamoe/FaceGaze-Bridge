#!/usr/bin/env python3
"""生成 3D 校准预览用的 .fgbface 模型与 face_map.json。

为什么是程序化模型：
    路线 A（自有风格化写实头）需要美术产出，暂时没有。这里的头是一个可控的
    风格化占位模型：结构、区域、morph 名与映射表就是正式契约，之后换美术资产
    时只需要重新导出同格式、同名 morph/区域，页面和渲染器不用改。

用法：
    python tools/gen_face_model.py            # 写到 ui/face/
    python tools/gen_face_model.py --out <目录>

格式（little-endian，见 ADR-0021）：
    magic "FGBF" | version u32 | flags u32
    vertex_count u32 | index_count u32 | morph_count u16
    region_count u8 | reserved u8
    positions f32[3N]
    normals   i8[3N]
    indices   u32[M]
    region_mask u32[N]
    morph_names u8 len + utf8 * K
    morph_deltas f16[3N*K]
"""
import argparse
import json
import math
import re
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# ---------------------------------------------------------------- 数学小工具

def clamp(x, lo, hi):
    return lo if x < lo else hi if x > hi else x


def smoothstep(edge0, edge1, x):
    if edge0 == edge1:
        return 0.0 if x < edge0 else 1.0
    t = clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def gauss(x, sigma):
    return math.exp(-(x / sigma) ** 2)


def norm3(v):
    n = math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])
    if n < 1e-12:
        return (0.0, 0.0, 0.0)
    return (v[0] / n, v[1] / n, v[2] / n)


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


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


def bits(*names):
    value = 0
    for name in names:
        value |= REGION_BIT[name]
    return value


# ---------------------------------------------------------------- 网格

class Mesh:
    def __init__(self):
        self.pos = []
        self.nrm = []
        self.region = []
        self.part = []
        self.uv = []
        self.idx = []

    def vertex(self, p, n, region, part, uv=(0.0, 0.0)):
        self.pos.append((float(p[0]), float(p[1]), float(p[2])))
        self.nrm.append(norm3(n))
        self.region.append(region)
        self.part.append(part)
        self.uv.append((float(uv[0]), float(uv[1])))
        return len(self.pos) - 1

    def tri(self, a, b, c):
        self.idx.extend((a, b, c))

    def quad(self, a, b, c, d):
        self.tri(a, b, c)
        self.tri(a, c, d)

    def add_ellipsoid(self, center, radii, region, part, lon=20, lat=12, axis_z=0.0):
        cx, cy, cz = center
        rx, ry, rz = radii
        rows = []
        for j in range(lat + 1):
            phi = -math.pi / 2 + math.pi * j / lat
            ring = []
            for i in range(lon):
                theta = 2 * math.pi * i / lon
                x = rx * math.cos(phi) * math.sin(theta)
                y = ry * math.sin(phi)
                z = rz * math.cos(phi) * math.cos(theta)
                p = (cx + x, cy + y, cz + z)
                n = (x / (rx * rx), y / (ry * ry), z / (rz * rz))
                ring.append(self.vertex(p, n, region, part, (i / lon, j / lat)))
            rows.append(ring)
        for j in range(lat):
            for i in range(lon):
                a = rows[j][i]
                b = rows[j][(i + 1) % lon]
                c = rows[j + 1][(i + 1) % lon]
                d = rows[j + 1][i]
                self.quad(a, b, c, d)


def rotate_x(p, angle):
    c, s = math.cos(angle), math.sin(angle)
    return (p[0], c * p[1] - s * p[2], s * p[1] + c * p[2])


# ---------------------------------------------------------------- 头部曲面

HEAD_RX, HEAD_RY, HEAD_RZ = 0.72, 1.0, 0.80
EYE_CENTER = (0.25, 0.10, 0.600)
EYE_RADIUS = 0.112
EYE_LID_RADIUS = 0.126
MOUTH_Y = -0.235
MOUTH_Z = 0.735


def head_point(theta, phi):
    ct, st = math.cos(theta), math.sin(theta)
    cp, sp = math.cos(phi), math.sin(phi)
    x = HEAD_RX * st * cp
    y = HEAD_RY * sp
    z = HEAD_RZ * ct * cp

    sx = 1.0 if x >= 0 else -1.0
    front = ct * cp

    if z < 0:
        z *= 1.08  # 后脑更饱满，纯椭球看起来像橄榄

    # 下颌收窄：从耳下开始往下巴收
    jaw = smoothstep(0.0, -0.9, y)
    x *= 1.0 - 0.36 * (jaw ** 1.4)

    if front > 0:
        ax = abs(x)
        # 下巴前突
        z += 0.110 * gauss(ax, 0.20) * gauss(y + 0.60, 0.16)
        # 眉弓
        z += 0.030 * gauss(y - 0.27, 0.09) * (1.0 - 0.55 * gauss(x, 0.06))
        # 眼窝
        z -= 0.078 * gauss(ax - 0.25, 0.165) * gauss(y - 0.10, 0.115)
        # 鼻梁与鼻头
        z += 0.055 * gauss(x, 0.052) * smoothstep(-0.02, 0.18, y) * smoothstep(0.30, 0.22, y)
        z += 0.140 * gauss(x, 0.075) * gauss(y + 0.155, 0.050)
        # 人中 / 嘴缝
        z -= 0.040 * gauss(x, 0.16) * gauss(y - MOUTH_Y + 0.005, 0.035)
        # 颧骨
        x += sx * 0.030 * gauss(ax - 0.36, 0.16) * gauss(y + 0.02, 0.18)
    return (x, y, z)


def head_normal(theta, phi):
    d = 1e-3
    p0 = head_point(theta, phi)
    dt = sub(head_point(theta + d, phi), head_point(theta - d, phi))
    dp = sub(head_point(theta, phi + d), head_point(theta, phi - d))
    n = cross(dt, dp)
    radial = (p0[0] / HEAD_RX, p0[1] / HEAD_RY, p0[2] / HEAD_RZ)
    if dot(n, radial) < 0:
        n = (-n[0], -n[1], -n[2])
    return norm3(n)


def head_regions(x, y, z):
    names = []
    ax = abs(x)
    if y > 0.30 and z > 0.05:
        names.append("forehead")
    if 0.15 < y < 0.31 and z > 0.45:
        names.append("browL" if x >= 0 else "browR")
    if ax < 0.13 and -0.28 < y < 0.26 and z > 0.52:
        names.append("nose")
    if ax > 0.18 and -0.20 < y < 0.10 and z > 0.28:
        names.append("cheekL" if x >= 0 else "cheekR")
    if ax < 0.23 and -0.31 < y < -0.16 and z > 0.52:
        names.append("upperLip" if y > MOUTH_Y else "lowerLip")
    if 0.15 < ax < 0.26 and -0.32 < y < -0.17 and z > 0.35:
        names.append("mouthCornerL" if x >= 0 else "mouthCornerR")
    if ax < 0.20 and -0.64 < y < -0.30 and z > 0.30:
        names.append("chin")
    # 只标下颌缘与下巴两侧；整块下半脸都标蓝会像戴了口罩
    if -0.62 < y < -0.12 and z > -0.02 and (ax > 0.22 or y < -0.40):
        names.append("jaw")
    if ax > 0.50 and 0.02 < y < 0.42 and z < 0.35:
        names.append("templeL" if x >= 0 else "templeR")
    return bits(*names)


def build_head(mesh, lon=96, lat=56):
    rows = []
    for j in range(lat):
        phi = -math.pi / 2 + math.pi * (j + 1) / (lat + 1)
        ring = []
        for i in range(lon):
            theta = 2 * math.pi * i / lon
            p = head_point(theta, phi)
            ring.append(mesh.vertex(p, head_normal(theta, phi),
                                    head_regions(*p), "head", (i / lon, (j + 1) / (lat + 1))))
        rows.append(ring)
    for j in range(lat - 1):
        for i in range(lon):
            a = rows[j][i]
            b = rows[j][(i + 1) % lon]
            c = rows[j + 1][(i + 1) % lon]
            d = rows[j + 1][i]
            mesh.quad(a, b, c, d)

    top = mesh.vertex((0, HEAD_RY, 0.0), (0, 1, 0), bits("forehead"), "head", (0.5, 1.0))
    bottom = mesh.vertex((0, -HEAD_RY, 0.0), (0, -1, 0), bits("jaw"), "head", (0.5, 0.0))
    for i in range(lon):
        a = rows[-1][i]
        b = rows[-1][(i + 1) % lon]
        mesh.tri(top, a, b)
        c = rows[0][i]
        d = rows[0][(i + 1) % lon]
        mesh.tri(bottom, d, c)


def build_neck(mesh, lon=24, rings=10):
    top_y, bottom_y = -0.62, -1.72
    center_z = -0.04
    rows = []
    for j in range(rings + 1):
        t = j / rings
        y = top_y + (bottom_y - top_y) * t
        r = 0.30 + 0.08 * t
        ring = []
        for i in range(lon):
            a = 2 * math.pi * i / lon
            x = r * math.cos(a)
            z = center_z + 0.82 * r * math.sin(a)
            ring.append(mesh.vertex((x, y, z), (math.cos(a), 0, math.sin(a)),
                                    bits("neck"), "neck", (i / lon, t)))
        rows.append(ring)
    for j in range(rings):
        for i in range(lon):
            mesh.quad(rows[j][i], rows[j][(i + 1) % lon],
                      rows[j + 1][(i + 1) % lon], rows[j + 1][i])
    center = mesh.vertex((0, bottom_y, center_z), (0, -1, 0), bits("neck"),
                         "neck", (0.5, 1.0))
    last = rows[-1]
    for i in range(lon):
        mesh.tri(center, last[i], last[(i + 1) % lon])


def build_ears(mesh):
    for side, sx in (("L", 1.0), ("R", -1.0)):
        mesh.add_ellipsoid((sx * 0.705, 0.02, -0.02), (0.048, 0.135, 0.095),
                           bits("ear" + side), "ear" + side, lon=12, lat=10)


def surface_theta_phi(x, y):
    phi = math.asin(clamp(y, -0.95, 0.95))
    denom = HEAD_RX * max(math.cos(phi), 0.2)
    theta = math.asin(clamp(x / denom, -0.99, 0.99))
    return theta, phi


def build_eyebrow(mesh, side, sx):
    # 眉毛做成贴在皮肤上的独立薄片：如果直接把头部 brow 区域染深，
    # 会变成一条横贯额头的黑带（第一版就是这样）。
    columns = 12
    rows = []
    for i in range(columns + 1):
        t = i / columns
        x = sx * (0.10 + 0.26 * t)
        arch = math.sin(math.pi * t)
        y = 0.238 + 0.020 * arch
        pair = []
        for dy, v in ((0.012, 1.0), (-0.012, 0.0)):
            theta, phi = surface_theta_phi(x, y + dy)
            p = head_point(theta, phi)
            n = head_normal(theta, phi)
            q = (p[0] + n[0] * 0.004, p[1] + n[1] * 0.004, p[2] + n[2] * 0.004)
            pair.append(mesh.vertex(q, n, bits("brow" + side, "eyebrow" + side),
                                    "eyebrow" + side, (t, v)))
        rows.append(pair)
    for i in range(columns):
        b0, t0 = rows[i]
        b1, t1 = rows[i + 1]
        mesh.quad(b0, b1, t1, t0)


def build_nostrils(mesh):
    for side, sx in (("L", 1.0), ("R", -1.0)):
        mesh.add_ellipsoid((sx * 0.052, -0.195, 0.850), (0.026, 0.016, 0.012),
                           bits("nostril" + side), "nostril" + side, lon=10, lat=6)


def build_eye(mesh, side, sx):
    center = (sx * EYE_CENTER[0], EYE_CENTER[1], EYE_CENTER[2])
    mesh.add_ellipsoid(center, (EYE_RADIUS, EYE_RADIUS, EYE_RADIUS),
                       bits("eyeWhite" + side), "eye" + side, lon=20, lat=14)

    def cap(radius, max_angle, region, part, rings=3, lon=20):
        center_idx = mesh.vertex((center[0], center[1], center[2] + radius),
                                 (0, 0, 1), bits(region), part)
        rows = []
        for j in range(1, rings + 1):
            a = max_angle * j / rings
            ring = []
            for i in range(lon):
                b = 2 * math.pi * i / lon
                d = (math.sin(a) * math.cos(b), math.sin(a) * math.sin(b), math.cos(a))
                p = (center[0] + radius * d[0], center[1] + radius * d[1],
                     center[2] + radius * d[2])
                ring.append(mesh.vertex(p, d, bits(region), part))
            rows.append(ring)
        for i in range(lon):
            mesh.tri(center_idx, rows[0][i], rows[0][(i + 1) % lon])
        for j in range(rings - 1):
            for i in range(lon):
                mesh.quad(rows[j][i], rows[j][(i + 1) % lon],
                          rows[j + 1][(i + 1) % lon], rows[j + 1][i])

    cap(EYE_RADIUS + 0.0015, math.radians(26), "iris" + side, "iris" + side)
    cap(EYE_RADIUS + 0.0030, math.radians(12), "pupil" + side, "pupil" + side, rings=2)


def build_lid(mesh, side, sx, upper):
    center = (sx * EYE_CENTER[0], EYE_CENTER[1], EYE_CENTER[2])
    nu, nv = 16, 6
    rows = []
    for j in range(nv + 1):
        v = j / nv
        ring = []
        for i in range(nu + 1):
            u = -1.0 + 2.0 * i / nu
            if upper:
                margin = norm3((u * 0.55, 0.16, 1.0))
                top = norm3((u * 0.55, 0.95, 0.35))
            else:
                margin = norm3((u * 0.55, -0.48, 1.0))
                top = norm3((u * 0.55, -0.95, 0.30))
            d = norm3(tuple(margin[k] * (1 - v) + top[k] * v for k in range(3)))
            p = (center[0] + EYE_LID_RADIUS * d[0],
                 center[1] + EYE_LID_RADIUS * d[1],
                 center[2] + EYE_LID_RADIUS * d[2])
            ring.append(mesh.vertex(p, d, bits("eyelid" + side),
                                    ("upperLid" if upper else "lowerLid") + side,
                                    (u, v)))
        rows.append(ring)
    for j in range(nv):
        for i in range(nu):
            mesh.quad(rows[j][i], rows[j][i + 1], rows[j + 1][i + 1], rows[j + 1][i])


def build_mouth(mesh):
    # 上下唇各一根沿嘴形走的管子；比在头部曲面上做细密网格容易控制形状
    for upper, y0, radius in ((True, MOUTH_Y + 0.030, 0.019),
                              (False, MOUTH_Y - 0.030, 0.021)):
        nu, nv = 16, 8
        rows = []
        for i in range(nu + 1):
            u = -1.0 + 2.0 * i / nu
            cx = u * 0.170
            cy = y0 + 0.012 * u * u
            if upper:
                cy -= 0.010 * gauss(u, 0.22)  # 唇珠：上唇中间略凹
            else:
                cy -= 0.006 * (1.0 - u * u)   # 下唇中间更饱满
            cz = MOUTH_Z - 0.005 * u * u
            ring = []
            for j in range(nv + 1):
                v = 2 * math.pi * j / nv
                n = (0.0, math.sin(v), math.cos(v))
                p = (cx, cy + radius * n[1], cz + radius * n[2])
                region = bits("upperLip" if upper else "lowerLip",
                              "lipColorUpper" if upper else "lipColorLower")
                if abs(u) > 0.78:
                    region |= bits("mouthCornerL" if u > 0 else "mouthCornerR")
                ring.append(mesh.vertex(p, n, region,
                                        "lipUpper" if upper else "lipLower", (u, j / nv)))
            rows.append(ring)
        for i in range(nu):
            for j in range(nv):
                a = rows[i][j]
                b = rows[i + 1][j]
                c = rows[i + 1][(j + 1) % (nv + 1)]
                d = rows[i][(j + 1) % (nv + 1)]
                mesh.quad(a, b, c, d)

    # 口腔：连接上下唇内缘的深色带子。闭嘴时只有一条缝，张嘴时下缘随
    # 下唇下移、上缘固定，看起来就是口腔，而不是一块贴在脸上的黑板。
    nx, ny = 14, 3
    rows = []
    for j in range(ny + 1):
        v = j / ny
        y = -0.230 - 0.012 * v
        ring = []
        for i in range(nx + 1):
            u = -1.0 + 2.0 * i / nx
            ring.append(mesh.vertex((u * 0.145, y, MOUTH_Z + 0.017), (0, 0, 1),
                                    bits("mouthInterior"), "interior", (u, v)))
        rows.append(ring)
    for j in range(ny):
        for i in range(nx):
            mesh.quad(rows[j][i], rows[j][i + 1], rows[j + 1][i + 1], rows[j + 1][i])


# ---------------------------------------------------------------- morph

def lower_face_weight(y):
    return smoothstep(0.04, -0.38, y)


def side_weight(x, sign):
    return smoothstep(-0.02, 0.12, x * sign)


def head_brow_weight(x, y, z, side):
    if not (0.10 < y < 0.38 and z > 0.40):
        return 0.0
    return side_weight(x, side) if side else 1.0


def morph_delta(mesh, i, name):
    p = mesh.pos[i]
    x, y, z = p
    part = mesh.part[i]
    region = mesh.region[i]
    ax = abs(x)
    side = 1.0 if x >= 0 else -1.0

    def has(r):
        return (region & REGION_BIT[r]) != 0

    lower = part in ("head", "lipLower", "interior") or has("jaw") or has("chin")
    if name == "jawOpen":
        if part in ("head", "lipLower", "interior"):
            w = lower_face_weight(y) * smoothstep(-0.55, 0.15, z)
            if part == "interior":
                # 带子上缘固定、下缘跟着下唇走；v 存的是 0(上)..1(下)
                w = mesh.uv[i][1]
            if w > 0:
                # 头和下唇分开处理：头只轻微下沉，下唇大幅下移并略向前，
                # 否则下颌曲面会盖住嘴唇，正面看起来像"两个红点"。
                if part == "head":
                    return (-0.004 * w, -0.055 * w, -0.008 * w)
                if part == "lipLower":
                    return (0.0, -0.120 * w, 0.010 * w)
                return (0.0, -0.120 * w, 0.0)
    elif name in ("jawLeft", "jawRight"):
        if part in ("head", "lipLower", "interior"):
            w = lower_face_weight(y) * smoothstep(-0.55, 0.15, z)
            if part == "interior":
                w = 1.0
            if w > 0:
                s = 1.0 if name == "jawLeft" else -1.0
                return (0.045 * s * w, 0.0, 0.0)
    elif name == "jawForward":
        if part in ("head", "lipLower", "interior"):
            w = lower_face_weight(y) * smoothstep(-0.55, 0.15, z)
            if part == "interior":
                w = 1.0
            if w > 0:
                return (0.0, 0.0, 0.050 * w)
    elif name in ("smileL", "smileR"):
        s = 1.0 if name == "smileL" else -1.0
        cx = 0.20 * s
        g = gauss(x - cx, 0.14) * gauss(y + 0.23, 0.11) * gauss(z - MOUTH_Z, 0.16)
        if part in ("head", "lipUpper", "lipLower", "interior") and g > 0.01:
            return (0.020 * s * g, 0.035 * g, 0.0)
        cheek = gauss(ax - 0.34, 0.14) * gauss(y + 0.03, 0.16)
        if part == "head" and has("cheekL" if s > 0 else "cheekR") and cheek > 0.01:
            return (0.0, 0.014 * cheek, 0.0)
    elif name in ("frownL", "frownR"):
        s = 1.0 if name == "frownL" else -1.0
        cx = 0.20 * s
        g = gauss(x - cx, 0.14) * gauss(y + 0.23, 0.11) * gauss(z - MOUTH_Z, 0.16)
        if part in ("head", "lipUpper", "lipLower") and g > 0.01:
            return (-0.010 * s * g, -0.030 * g, 0.0)
    elif name == "pucker":
        g = gauss(x, 0.20) * gauss(y - MOUTH_Y, 0.10) * gauss(z - MOUTH_Z, 0.10)
        if part in ("lipUpper", "lipLower", "interior") and g > 0.01:
            return (-x * 0.32 * g, 0.0, 0.035 * g)
    elif name == "lipPress":
        if part == "lipUpper":
            return (0.0, -0.012, 0.0)
        if part == "lipLower":
            return (0.0, 0.012, 0.0)
        if part == "head" and (has("upperLip") or has("lowerLip")):
            return (0.0, (0.006 if has("lowerLip") else -0.006), 0.0)
    elif name in ("upperLipUpL", "upperLipUpR"):
        s = 1.0 if name == "upperLipUpL" else -1.0
        w = side_weight(x, s)
        if part == "lipUpper" or (part == "head" and has("upperLip")):
            return (0.0, 0.024 * w, 0.0)
    elif name in ("lowerLipDownL", "lowerLipDownR"):
        s = 1.0 if name == "lowerLipDownL" else -1.0
        w = side_weight(x, s)
        if part == "lipLower" or (part == "head" and has("lowerLip")):
            return (0.0, -0.024 * w, 0.0)
    elif name in ("browInnerUpL", "browInnerUpR", "browOuterUpL",
                  "browOuterUpR", "browDownL", "browDownR"):
        if part != "head":
            return None
        s = 1.0 if name.endswith("L") else -1.0
        if name.startswith("browInner"):
            band = gauss(ax - 0.14, 0.13) * gauss(y - 0.24, 0.075) * smoothstep(0.40, 0.62, z)
            return (0.0, 0.032 * side_weight(x, s) * band, 0.0)
        if name.startswith("browOuter"):
            band = gauss(ax - 0.34, 0.11) * gauss(y - 0.25, 0.075) * smoothstep(0.40, 0.62, z)
            return (0.0, 0.028 * side_weight(x, s) * band, 0.0)
        band = gauss(ax - 0.24, 0.18) * gauss(y - 0.22, 0.09) * smoothstep(0.40, 0.62, z)
        return (0.0, -0.027 * side_weight(x, s) * band, 0.008 * band)
    elif name in ("eyeBlinkL", "eyeBlinkR", "eyeWideL", "eyeWideR"):
        s = 1.0 if name.endswith("L") else -1.0
        suffix = "L" if s > 0 else "R"
        if part not in ("upperLid" + suffix, "lowerLid" + suffix):
            return None
        u, v = mesh.uv[i]
        center = (s * EYE_CENTER[0], EYE_CENTER[1], EYE_CENTER[2])
        d = norm3(sub(p, center))
        if name.startswith("eyeBlink"):
            if part.startswith("upperLid"):
                angle = -1.10 * ((1.0 - v) ** 1.25)
            else:
                angle = 0.12 * ((1.0 - v) ** 1.25)
        else:
            if part.startswith("upperLid"):
                angle = 0.38 * ((1.0 - v) ** 1.25)
            else:
                angle = -0.16 * ((1.0 - v) ** 1.25)
        nd = rotate_x(d, angle)
        np_ = (center[0] + EYE_LID_RADIUS * nd[0],
               center[1] + EYE_LID_RADIUS * nd[1],
               center[2] + EYE_LID_RADIUS * nd[2])
        return sub(np_, p)
    elif name in ("cheekSquintL", "cheekSquintR"):
        s = 1.0 if name.endswith("L") else -1.0
        if part.startswith("lowerLid") and part.endswith("L" if s > 0 else "R"):
            u, v = mesh.uv[i]
            center = (s * EYE_CENTER[0], EYE_CENTER[1], EYE_CENTER[2])
            d = norm3(sub(p, center))
            nd = rotate_x(d, 0.22 * ((1.0 - v) ** 1.25))
            np_ = (center[0] + EYE_LID_RADIUS * nd[0],
                   center[1] + EYE_LID_RADIUS * nd[1],
                   center[2] + EYE_LID_RADIUS * nd[2])
            return sub(np_, p)
        if part == "head":
            band = gauss(ax - 0.30, 0.13) * gauss(y + 0.01, 0.13) * smoothstep(0.30, 0.55, z)
            return (0.0, 0.020 * side_weight(x, s) * band, 0.0)
    elif name in ("cheekPuffL", "cheekPuffR", "cheekSuckL", "cheekSuckR"):
        if part != "head":
            return None
        s = 1.0 if name.endswith("L") else -1.0
        band = gauss(ax - 0.34, 0.15) * gauss(y + 0.03, 0.17) * smoothstep(0.25, 0.50, z)
        amount = 0.035 if name.startswith("cheekPuff") else -0.030
        return (s * amount * band, 0.0, 0.010 if name.startswith("cheekPuff") else -0.006)
    elif name in ("noseSneerL", "noseSneerR"):
        if part != "head":
            return None
        s = 1.0 if name.endswith("L") else -1.0
        band = gauss(ax - 0.075, 0.055) * gauss(y + 0.13, 0.07) * smoothstep(0.50, 0.65, z)
        return (0.010 * s * band, 0.020 * band, 0.0)
    return None


def build_morphs(mesh, morph_names):
    deltas = []
    for name in morph_names:
        out = []
        for i in range(len(mesh.pos)):
            d = morph_delta(mesh, i, name)
            out.append(d if d is not None else (0.0, 0.0, 0.0))
        deltas.append(out)
    return deltas


# ---------------------------------------------------------------- 映射表

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

def write_fgbface(path, mesh, deltas):
    n = len(mesh.pos)
    m = len(mesh.idx)
    k = len(deltas)
    out = bytearray()
    out += b"FGBF"
    out += struct.pack("<III", 1, 0, n)
    out += struct.pack("<IHBB", m, k, len(REGIONS), 0)
    for p in mesh.pos:
        out += struct.pack("<fff", *p)
    for v in mesh.nrm:
        out += struct.pack("<bbb", *(int(round(clamp(c, -1.0, 1.0) * 127)) for c in v))
    for index in mesh.idx:
        out += struct.pack("<I", index)
    for region in mesh.region:
        out += struct.pack("<I", region)
    for name in MORPH_NAMES:
        raw = name.encode("utf-8")
        if len(raw) > 255:
            raise SystemExit(f"morph 名过长：{name}")
        out += bytes([len(raw)]) + raw
    for delta_set in deltas:
        for d in delta_set:
            out += struct.pack("<eee", *d)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(out)
    return len(out)


def write_face_map(path, all_shapes):
    numeric_only = [name for name in all_shapes if name not in SHAPE_MAP]
    data = {
        "version": 1,
        "model": "head_v1.fgbface",
        "generated_by": "tools/gen_face_model.py",
        "note": "程序化风格化占位头模；区域名与 morph 名是稳定契约，换正式资产时保持不变",
        "regions": REGIONS,
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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", default=str(ROOT / "ui" / "face"))
    args = parser.parse_args()
    out_dir = Path(args.out)

    all_shapes = shape_names_from_header()
    validate_map(all_shapes)

    mesh = Mesh()
    build_head(mesh)
    build_neck(mesh)
    build_ears(mesh)
    for side, sx in (("L", 1.0), ("R", -1.0)):
        build_eye(mesh, side, sx)
        build_lid(mesh, side, sx, True)
        build_lid(mesh, side, sx, False)
        build_eyebrow(mesh, side, sx)
    build_mouth(mesh)
    build_nostrils(mesh)

    deltas = build_morphs(mesh, MORPH_NAMES)
    size = write_fgbface(out_dir / "head_v1.fgbface", mesh, deltas)
    write_face_map(out_dir / "face_map.json", all_shapes)

    mapped = len(all_shapes) - len([n for n in all_shapes if n not in SHAPE_MAP])
    print(f"vertices={len(mesh.pos)} indices={len(mesh.idx)} morphs={len(MORPH_NAMES)}")
    print(f"mapped shapes={mapped}/{len(all_shapes)}")
    print(f"wrote {out_dir / 'head_v1.fgbface'} ({size / 1024 / 1024:.2f} MiB)")
    print(f"wrote {out_dir / 'face_map.json'}")


if __name__ == "__main__":
    main()
