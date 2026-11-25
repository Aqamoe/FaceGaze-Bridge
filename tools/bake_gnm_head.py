#!/usr/bin/env python3
"""从 Google GNM Head 烘焙校准页用的 `.fgbface` v3 头模。

为什么要换资产（ADR-0021 P4）：原来 `tools/gen_face_model.py` 出的是一颗
程序化椭球头，结构能认，但下颌、唇形、眼睑、鼻翼都是手工高斯堆出来的，
"调 lo/hi 时脸怎么变"看不出真实比例。GNM Head 是 Google 2026 年开源的
统计式参数化人头（Apache-2.0，可商用可再分发）：17.8k 顶点的扫描级中性头，
自带眼球/牙齿/舌头与 383 维表情基，正好可以当"风格化写实"的底模。

做法：中性头取模板网格（剔除不透明着色会遮挡虹膜的外层角膜壳，保留皮肤、
眼球内层、口腔内衬、上下牙齿/牙龈与舌头）；32 个 morph 用解剖拓扑掩码
（`upper_lip`/`lower_lip` 唇环、眼睑拓扑环、颞下颌关节铰链）构造期望位移场，
以带权重的高斯先验最小二乘（MAP）投影到 GNM 统计基，下齿与舌头随下颌刚性同步
转动并沿牙弓微缩留出安全间隙，使口型自然且无穿模或嘴角黑斑。

    python tools/bake_gnm_head.py                      # 写 ui/face/
    python tools/bake_gnm_head.py --preview out/       # 另出一张 morph 检查图

权重（53 MB，不随仓库分发）默认缓存到 `tools/.cache/gnm_head.npz`，
首次运行自动从 Hugging Face 下载；也可用 `--weights` 指定。
烘焙产物约 ~1.57 MiB，进仓库与安装包。
"""
import argparse
import sys
import urllib.request
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import face_contract as C  # noqa: E402

ROOT = C.ROOT
FACE_DIR = C.FACE_DIR
WEIGHTS_URL = "https://huggingface.co/google/gnm-v3/resolve/main/v3_0/gnm_head.npz"
CACHE = ROOT / "tools" / ".cache" / "gnm_head.npz"

# 渲染器空间（米 → 单位 1 的头高 ≈ 2.0）：与 ui/face.js 的相机预设配套
TARGET_HEIGHT = 2.02
EPS = 1e-9


# ---------------------------------------------------------------- 权重


def ensure_weights(path):
    if path.exists():
        return path
    path.parent.mkdir(parents=True, exist_ok=True)
    print(f"下载 GNM 权重（53 MB）→ {path}")
    tmp = path.with_suffix(".part")
    with urllib.request.urlopen(WEIGHTS_URL, timeout=600) as response, open(tmp, "wb") as handle:
        total = int(response.headers.get("Content-Length") or 0)
        done = 0
        while True:
            chunk = response.read(1 << 20)
            if not chunk:
                break
            handle.write(chunk)
            done += len(chunk)
            if total:
                print(f"\r  {done / 1048576:6.1f} / {total / 1048576:.1f} MiB", end="")
        print()
    tmp.replace(path)
    return path


class Gnm:
    def __init__(self, path):
        data = np.load(path)
        self.P = data["template_vertex_positions"].astype(np.float64)
        self.TRI = data["triangles"].astype(np.int64)
        self.E = data["expression_basis"].astype(np.float64)
        self.names = [str(n) for n in data["expression_names"]]
        groups = data["vertex_groups"].astype(np.float64)
        self.G = {str(n): groups[i] for i, n in enumerate(data["vertex_group_names"])}
        # 仅剔除眼球外层角膜壳：真实渲染里角膜是透明的，不透明着色会盖住虹膜；
        # 牙齿/牙龈/舌头保留下来，配合刚性下颌随动与牙弓安全余量避免穿模。
        drop = self.G["eye_exteriors"] > 0.5
        if drop.any():
            keep = ~drop
            remap = np.full(len(self.P), -1, dtype=np.int64)
            remap[keep] = np.arange(int(keep.sum()))
            tri = self.TRI[np.all(keep[self.TRI], axis=1)]
            self.TRI = remap[tri]
            self.P = self.P[keep]
            self.E = self.E[:, keep, :]
            self.G = {name: values[keep] for name, values in self.G.items()}
        self.blocks = {
            "lower": [i for i, n in enumerate(self.names) if n.startswith("lower_face")],
            "left_eye": [i for i, n in enumerate(self.names) if n.startswith("left_eye")],
            "right_eye": [i for i, n in enumerate(self.names) if n.startswith("right_eye")],
            "tongue": [i for i, n in enumerate(self.names) if n.startswith("tongue")],
        }
        self.adjacency()

    def adjacency(self):
        tri = self.TRI
        edges = np.concatenate([tri[:, [0, 1]], tri[:, [1, 2]], tri[:, [2, 0]]])
        edges = np.concatenate([edges, edges[:, ::-1]])
        self.edges = edges
        self.degree = np.maximum(np.bincount(edges[:, 0], minlength=len(self.P)), 1)
        adj = [[] for _ in range(len(self.P))]
        for u, v in edges:
            adj[u].append(v)
        self.adj = adj

    def smooth(self, field, iterations=2, mix=0.5):
        w = np.asarray(field, dtype=np.float64).copy()
        for _ in range(iterations):
            acc = np.zeros_like(w)
            np.add.at(acc, self.edges[:, 0], w[self.edges[:, 1]])
            w = (1.0 - mix) * w + mix * acc / self.degree
        return w

    def topo_distance(self, seed_mask, allow_mask=None, max_dist=20):
        """在允许的网格子集上按边拓扑求顶点最短环距。"""
        dist = np.full(len(self.P), max_dist, dtype=np.int32)
        q = list(np.nonzero(seed_mask)[0])
        for i in q:
            dist[i] = 0
        head = 0
        while head < len(q):
            u = q[head]
            head += 1
            du = dist[u]
            if du >= max_dist:
                continue
            for v in self.adj[u]:
                if allow_mask is not None and not allow_mask[v]:
                    continue
                if dist[v] > du + 1:
                    dist[v] = du + 1
                    q.append(v)
        return dist


def smoothstep(edge0, edge1, x):
    t = np.clip((x - edge0) / (edge1 - edge0 + EPS), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def gauss(x, sigma):
    return np.exp(-((x / sigma) ** 2))


# ---------------------------------------------------------------- 解剖锚点


class Anchors:
    def __init__(self, g):
        self.g = g
        P, G = g.P, g.G
        # 必须使用精确唇组 upper_lip/lower_lip（145+145 顶点），而非覆盖半张下半脸的
        # *_lip_region，否则口角锚点会外扩 10mm 到脸颊、唇线中点下移 5mm。
        self.lip_up_mask = G["upper_lip"] > 0.5
        self.lip_lo_mask = G["lower_lip"] > 0.5
        self.lip_mask = self.lip_up_mask | self.lip_lo_mask
        self.skin_mask = G["skin"] > 0.5
        self.skin_ext_mask = G["skin_exterior"] > 0.5
        self.sock_mask = G["mouth_sock"] > 0.5
        self.chin_mask = G["chin_region"] > 0.5

        self.lip_ring = g.topo_distance(self.sock_mask, allow_mask=self.skin_mask, max_dist=16)
        self.eye_ring = g.topo_distance(G["eye_sockets"] > 0.5, allow_mask=self.skin_mask, max_dist=16)
        self.sock_ring = g.topo_distance(self.skin_ext_mask, allow_mask=self.skin_mask, max_dist=16)

        inner_lip = self.lip_mask & (self.lip_ring == 1)
        self.mouth_y = float(P[inner_lip, 1].mean())
        self.mouth_z = float(P[inner_lip, 2].mean())
        self.corner_x = float(np.abs(P[self.lip_mask, 0]).max())
        corners = self.lip_mask & (np.abs(P[:, 0]) > self.corner_x - 0.0025)
        self.corner_y = float(P[corners, 1].mean())
        self.corner_z = float(P[corners, 2].mean())
        self.chin_y = float(P[self.chin_mask, 1].min())
        self.nose_y = float(P[G["nose_region"] > 0.5, 1].min())
        self.ear_y = float(P[G["ears"] > 0.5, 1].mean())

        self.eye = {}
        self.eye_radius = 0.0145
        for side, key in (("L", "left_eye"), ("R", "right_eye")):
            verts = P[(G[key] > 0.5) & (G["eye_interiors"] > 0.5)]
            self.eye[side] = verts.mean(0)
            self.eye_radius = float(np.abs(verts[:, 1] - self.eye[side][1]).max())
        self.eye_y = 0.5 * (self.eye["L"][1] + self.eye["R"][1])

        self.brow = {}
        self.brow_poly = {}
        for side, key in (("L", "left_brow_region"), ("R", "right_brow_region")):
            pts = P[G[key] > 0.5]
            self.brow[side] = pts.mean(0)
            # 沿真实眉弓拟合二次曲线 y(|x|)，替代平直水平线
            self.brow_poly[side] = np.polyfit(np.abs(pts[:, 0]), pts[:, 1], 2)

        self.scale = TARGET_HEIGHT / float(P[:, 1].max() - P[:, 1].min())
        self.center = np.array([0.0, 0.5 * (P[:, 1].max() + P[:, 1].min()), 0.0])


# ---------------------------------------------------------------- 权重场


class Fields:
    """按 GNM 顶点组与解剖锚点构造平滑的 0..1 权重场。"""

    def __init__(self, g, a):
        self.g = g
        self.a = a
        P, G = g.P, g.G
        self.x, self.y, self.z = P[:, 0], P[:, 1], P[:, 2]
        self.skin = (G["skin"] > 0.5).astype(np.float64)
        self.skin_ext = (G["skin_exterior"] > 0.5).astype(np.float64)
        self.lip_up = (G["upper_lip"] > 0.5).astype(np.float64)
        self.lip_lo = (G["lower_lip"] > 0.5).astype(np.float64)
        self.lip_all = np.clip(self.lip_up + self.lip_lo, 0, 1)
        self.ut = G["upper_teeth_and_gums"] > 0.5
        self.lt = G["lower_teeth_and_gums"] > 0.5
        self.tng = G["tongue"] > 0.5
        self.inner = np.clip(G["mouth_sock"], 0, 1)
        self.forehead = np.clip(G["forehead_region"], 0, 1)
        self.cheek = {"L": np.clip(G["left_cheek_region"], 0, 1),
                      "R": np.clip(G["right_cheek_region"], 0, 1)}
        self.brow = {"L": np.clip(G["left_brow_region"], 0, 1),
                     "R": np.clip(G["right_brow_region"], 0, 1)}
        self.orbital = {"L": np.clip(G["left_orbital_region"], 0, 1),
                        "R": np.clip(G["right_orbital_region"], 0, 1)}
        self.infra = {"L": np.clip(G["left_infraorbital_region"], 0, 1),
                      "R": np.clip(G["right_infraorbital_region"], 0, 1)}
        self.zyg = {"L": np.clip(G["left_zygomatic_region"], 0, 1),
                    "R": np.clip(G["right_zygomatic_region"], 0, 1)}
        self.nose = np.clip(G["nose_region"], 0, 1)
        self.front = smoothstep(-0.020, 0.040, self.z)

    def side(self, s, width=0.010):
        return smoothstep(-width, width, self.x * s)

    def jaw(self):
        a = self.a
        mouth_split = a.mouth_y + (a.corner_y - a.mouth_y) * smoothstep(0.0, a.corner_x, np.abs(self.x))
        below = smoothstep(mouth_split + 0.003, mouth_split - 0.006, self.y)
        below = np.where(a.lip_lo_mask, 1.0, np.where(a.lip_up_mask, 0.0, below))
        chin_fade = smoothstep(a.chin_y - 0.028, a.chin_y + 0.008, self.y)
        w = below * chin_fade * self.front * self.skin_ext
        w = self.g.smooth(w, 2)
        w = np.where(a.lip_lo_mask, np.maximum(w, 0.92), np.where(a.lip_up_mask, np.minimum(w, 0.03), w))
        return np.clip(w, 0.0, 1.0)


# ---------------------------------------------------------------- 位移场与拟合


def rot_axis(p, angle, pivot, axis=0):
    """绕通过 pivot 的坐标轴旋转，angle 可为逐顶点数组。"""
    q = p - pivot
    out = q.copy()
    c = np.cos(angle)
    s = np.sin(angle)
    if axis == 0:  # x 轴：下颌开合
        out[:, 1] = c * q[:, 1] - s * q[:, 2]
        out[:, 2] = s * q[:, 1] + c * q[:, 2]
    elif axis == 1:  # y 轴：左右摆动
        out[:, 0] = c * q[:, 0] + s * q[:, 2]
        out[:, 2] = -s * q[:, 0] + c * q[:, 2]
    else:
        out[:, 0] = c * q[:, 0] - s * q[:, 1]
        out[:, 1] = s * q[:, 0] + c * q[:, 1]
    return out + pivot


def fit(g, target, weights, blocks, lam=4e-4, max_scale=1.6, min_scale=0.65, lam_diag=0.0):
    """带顶点权重与高斯先验的统计基最小二乘投影（MAP）。

    仅在需要约束的关键解剖部位施加大权重，周围脸颊/鼻唇沟留较小权重让 GNM
    统计协方差自然跟随，避免"周围皮肤强行置零"把嘟嘴、挑眉或微笑幅度压扁。
    """
    if isinstance(blocks, str):
        blocks = (blocks,)
    dims = sorted({i for block in blocks for i in g.blocks[block]})
    basis = g.E[dims].reshape(len(dims), -1)  # (K, 3V)
    w3 = np.repeat(weights, 3)
    bw = basis * w3[None, :]
    gram = bw @ basis.T
    if lam_diag > 0.0:
        gram += lam_diag * np.diag(np.diag(gram))
    gram += lam * np.eye(len(dims))
    alpha = np.linalg.solve(gram, bw @ target.reshape(-1))
    fitted = (alpha[:, None, None] * g.E[dims]).sum(0)

    hi = (weights > 0.35) & (np.linalg.norm(target, axis=1) > 1e-6)
    if hi.any():
        target_rms = float(np.sqrt((target[hi] ** 2).mean()))
        fit_rms = float(np.sqrt((fitted[hi] ** 2).mean()))
        if fit_rms > 1e-12:
            factor = float(np.clip(target_rms / fit_rms, min_scale, max_scale))
            fitted *= factor
    return fitted


def build_morphs(g, a, f):
    """生成并投影 32 个 morph 的稀疏位移场（米，GNM 原始坐标）。"""
    x, y, z = f.x, f.y, f.z
    n = len(g.P)
    hinge = np.array([0.0, a.ear_y + 0.008, 0.012])

    base_w = np.full(n, 0.08, dtype=np.float64)
    base_w[y < a.chin_y - 0.022] = 0.8
    base_w[z < 0.02] = 0.8
    base_w[~a.skin_ext_mask] = 0.0

    deltas = {}

    # 1. 下颌开合（附带真实颞下颌关节前滑分量）；下牙/舌头同轴刚性随动且角度略收，杜绝穿下巴
    jaw_w = f.jaw()
    jaw_angle = 0.25
    jaw_slide = 0.007
    d = rot_axis(g.P, jaw_angle * jaw_w, hinge, 0) - g.P
    d[:, 2] += jaw_slide * jaw_w
    d[:, 1] -= 0.0020 * f.lip_lo
    d[:, 1] += 0.0012 * f.lip_up
    w = base_w.copy()
    w[a.lip_up_mask] = 4.0
    w[a.lip_lo_mask] = 2.5
    w[a.chin_mask] = 1.5
    w[g.G["nose_region"] > 0.5] = 2.0
    w[(g.G["upper_lip_region"] > 0.5) & ~a.lip_lo_mask] = 1.2
    w[~a.skin_ext_mask] = 0.0
    fitted = fit(g, d, w, "lower", lam=1.5e-4, max_scale=1.35)
    d_lt = rot_axis(g.P, 0.22, hinge, 0) - g.P
    d_lt[:, 2] += jaw_slide * 0.85
    d_tng = rot_axis(g.P, 0.16, hinge, 0) - g.P
    d_tng[:, 2] += jaw_slide * 0.65
    fitted[f.lt] = d_lt[f.lt]
    fitted[f.tng] = d_tng[f.tng]
    fitted[f.ut] = 0.0
    deltas["jawOpen"] = fitted

    # 2. 下颌左右/前伸
    for name, s in (("jawLeft", 1.0), ("jawRight", -1.0)):
        d = np.stack([s * 0.0065 * jaw_w, -0.0005 * jaw_w, np.zeros_like(x)], axis=1)
        w = base_w.copy()
        w[a.lip_lo_mask | a.chin_mask] = 2.0
        w[a.lip_up_mask | (g.G["nose_region"] > 0.5)] = 2.5
        w[~a.skin_ext_mask] = 0.0
        fitted = fit(g, d, w, "lower", lam=2e-4, max_scale=1.3)
        fitted[f.lt] = np.array([s * 0.0052, -0.0004, 0.0])
        fitted[f.tng] = np.array([s * 0.0040, -0.0003, 0.0])
        fitted[f.ut] = 0.0
        deltas[name] = fitted

    d = np.stack([np.zeros_like(x), np.zeros_like(x), 0.0055 * jaw_w], axis=1)
    w = base_w.copy()
    w[a.lip_lo_mask | a.chin_mask] = 2.0
    w[a.lip_up_mask | (g.G["nose_region"] > 0.5)] = 2.5
    w[~a.skin_ext_mask] = 0.0
    fitted = fit(g, d, w, "lower", lam=2e-4, max_scale=1.3)
    fitted[f.lt] = np.array([0.0, 0.0, 0.0045])
    fitted[f.tng] = np.array([0.0, 0.0, 0.0035])
    fitted[f.ut] = 0.0
    deltas["jawForward"] = fitted

    # 3. 微笑与撇嘴：使用尺度自适应正则（lam_diag）避免低频整体抬升基盖掉真实嘴角弧度，
    # 并让牵引场平滑延伸到嘴角外侧颊面，消除嘴角竖沟与中线露缝。
    for name, s in (("smileL", 1.0), ("smileR", -1.0),
                    ("frownL", 1.0), ("frownR", -1.0)):
        is_smile = name.startswith("smile")
        ramp = smoothstep(0.004, a.corner_x, s * x)
        if is_smile:
            corner_gauss = np.exp(-((x - s * (a.corner_x + 0.003)) / 0.020) ** 2 - ((y - (a.corner_y + 0.002)) / 0.016) ** 2)
            smile_field = g.smooth(corner_gauss * smoothstep(14.0, 2.0, a.lip_ring.astype(np.float64)), 2)
            dx = s * 0.0092 * smile_field * (0.25 + 0.75 * ramp)
            dy = 0.0124 * smile_field * ramp
            dz = -0.0052 * smile_field * ramp
            cheek = g.smooth(gauss(y - (a.corner_y + 0.020), 0.022) * (f.cheek["L" if s > 0 else "R"] + 0.6 * f.zyg["L" if s > 0 else "R"]), 2)
            dy += 0.0044 * cheek
            dz += 0.0018 * cheek
            d = np.stack([dx, dy, dz], axis=1)
            mid_lo = a.lip_lo_mask & (np.abs(x) < 0.006)
            mid_up = a.lip_up_mask & (np.abs(x) < 0.006)
            w = base_w.copy()
            w[a.lip_mask & (x * s >= 0.004)] = 3.2
            w[(a.lip_ring <= 11) & (x * s >= 0.010) & a.skin_ext_mask] = 2.2
            w[mid_lo] = 3.2
            w[mid_up] = 3.0
            w[a.chin_mask & (np.abs(x) < 0.018)] = 2.4
            w[(f.cheek["L" if s > 0 else "R"] > 0.3) & a.skin_ext_mask] = 1.4
            w[(x * s < -0.008) & a.skin_ext_mask] = 1.8
            w[g.G["nose_region"] > 0.5] = 2.0
            w[~a.skin_ext_mask] = 0.0
            fitted = fit(g, d, w, "lower", lam=4.5e-5, lam_diag=2e-3, max_scale=1.35)
        else:
            near_corner = np.exp(-((x - s * a.corner_x) / 0.015) ** 2 - ((y - a.corner_y) / 0.012) ** 2)
            lip_field = g.smooth(near_corner * smoothstep(10.0, 1.0, a.lip_ring.astype(np.float64)), 2)
            dx = -s * 0.0014 * lip_field * ramp
            dy = -0.0100 * lip_field * (0.15 + 0.85 * ramp)
            dz = -0.0015 * lip_field * ramp
            d = np.stack([dx, dy, dz], axis=1)
            mid_up = a.lip_up_mask & (np.abs(x) < 0.007)
            mid_lo = a.lip_lo_mask & (np.abs(x) < 0.007)
            d[mid_up] = np.array([0.0, -0.0006, 0.0])
            d[mid_lo] = np.array([0.0, 0.0004, 0.0])
            w = base_w.copy()
            w[a.lip_mask & (x * s >= 0.004)] = 3.0
            w[(a.lip_ring <= 7) & (x * s >= 0.008) & a.skin_ext_mask] = 2.0
            w[mid_up | mid_lo] = 2.8
            w[(x * s < -0.008) & a.skin_ext_mask] = 1.8
            w[g.G["nose_region"] > 0.5] = 1.8
            w[~a.skin_ext_mask] = 0.0
            fitted = fit(g, d, w, "lower", lam=5.0e-5, lam_diag=2e-3, max_scale=1.35)
        fitted[f.ut | f.lt | f.tng] = 0.0
        deltas[name] = fitted

    # 4. 嘟嘴与抿唇
    lip_band = smoothstep(6.5, 2.0, a.lip_ring.astype(np.float64)) * f.skin_ext
    cx = gauss(x, 0.022)
    inner_seal = np.where(a.lip_ring <= 2, -0.0014 * f.lip_up + 0.0014 * f.lip_lo, 0.0)
    outer_flare = np.where((a.lip_ring >= 3) & (a.lip_ring <= 5), 0.0012 * f.lip_up - 0.0015 * f.lip_lo, 0.0)
    d = np.stack([
        -x * 0.42 * lip_band,
        (inner_seal + outer_flare) * cx,
        0.0105 * lip_band * cx,
    ], axis=1)
    w = base_w.copy()
    w[a.lip_mask] = 3.8
    w[(a.lip_ring <= 6) & a.skin_ext_mask] = 1.6
    w[g.G["nose_region"] > 0.5] = 1.5
    w[~a.skin_ext_mask] = 0.0
    fitted = fit(g, d, w, "lower", lam=4.0e-5, lam_diag=2e-3, max_scale=1.45)
    # 嘴角内收时略将口内衬前环后撤，避免穿透嘴角皮肤
    fitted[a.sock_mask & (a.sock_ring <= 2), 2] -= 0.0012
    fitted[f.ut | f.lt | f.tng] = 0.0
    deltas["pucker"] = fitted

    d = np.stack([
        np.zeros_like(x),
        (-0.0018 * f.lip_up + 0.0020 * f.lip_lo) * smoothstep(6.0, 2.0, a.lip_ring.astype(np.float64)),
        -0.0015 * f.lip_all,
    ], axis=1)
    w = base_w.copy()
    w[a.lip_mask] = 3.0
    w[g.G["nose_region"] > 0.5] = 1.5
    w[~a.skin_ext_mask] = 0.0
    fitted = fit(g, d, w, "lower", lam=3e-4, max_scale=1.3)
    fitted[f.ut | f.lt | f.tng] = 0.0
    deltas["lipPress"] = fitted

    # 5. 单侧提上唇与降下唇（暴露真牙，左右分离）
    for name, s in (("upperLipUpL", 1.0), ("upperLipUpR", -1.0)):
        arch = gauss(x - s * 0.011, 0.011)
        uw = g.smooth(f.lip_up * f.side(s, 0.006) * (0.45 + 0.55 * arch), 2)
        d = np.stack([np.zeros_like(x), 0.0068 * uw, 0.0016 * uw], axis=1)
        w = base_w.copy()
        w[a.lip_up_mask & (x * s > -0.004)] = 3.0
        w[a.lip_lo_mask] = 2.5
        w[(x * s < -0.008) & a.skin_ext_mask] = 1.5
        w[~a.skin_ext_mask] = 0.0
        fitted = fit(g, d, w, "lower", lam=2e-4, max_scale=1.45)
        fitted[f.ut | f.lt | f.tng] = 0.0
        deltas[name] = fitted

    for name, s in (("lowerLipDownL", 1.0), ("lowerLipDownR", -1.0)):
        arch = gauss(x - s * 0.011, 0.011)
        lw = g.smooth(f.lip_lo * f.side(s, 0.006) * (0.45 + 0.55 * arch), 2)
        d = np.stack([np.zeros_like(x), -0.0072 * lw, 0.0018 * lw], axis=1)
        w = base_w.copy()
        w[a.lip_lo_mask & (x * s > -0.004)] = 3.0
        w[a.lip_up_mask] = 2.5
        w[(x * s < -0.008) & a.skin_ext_mask] = 1.5
        w[~a.skin_ext_mask] = 0.0
        fitted = fit(g, d, w, "lower", lam=2e-4, max_scale=1.45)
        fitted[f.ut | f.lt | f.tng] = 0.0
        deltas[name] = fitted

    # 6. 眉眼脸颊鼻翼
    for side, s in (("L", 1.0), ("R", -1.0)):
        block = "left_eye" if s > 0 else "right_eye"
        brow = g.smooth(f.brow[side], 2)
        brow_fore = g.smooth(np.clip(brow + 0.65 * f.forehead * f.side(s, 0.012) * smoothstep(0.365, 0.315, y), 0, 1), 2)
        inner = g.smooth(gauss(x - s * 0.017, 0.015) * brow_fore, 2)
        outer = g.smooth(gauss(x - s * 0.048, 0.015) * brow_fore, 2)
        whole = g.smooth(brow_fore, 2)
        ew = np.full(n, 0.12, dtype=np.float64)
        ew[brow > 0.25] = 2.2
        ew[(f.forehead > 0.3) & (x * s > 0.0)] = 0.8
        ew[(x * s < 0.0) & a.skin_ext_mask] = 1.5
        ew[~a.skin_ext_mask] = 0.0
        deltas["browInnerUp" + side] = fit(
            g, np.stack([np.zeros_like(x), 0.0068 * inner, 0.0005 * inner], axis=1),
            ew, block, lam=1.5e-4, max_scale=1.45)
        deltas["browOuterUp" + side] = fit(
            g, np.stack([np.zeros_like(x), 0.0062 * outer, np.zeros_like(x)], axis=1),
            ew, block, lam=1.5e-4, max_scale=1.45)
        deltas["browDown" + side] = fit(
            g, np.stack([-s * 0.0012 * whole, -0.0055 * whole, 0.0012 * whole], axis=1),
            ew, block, lam=1.5e-4, max_scale=1.45)

        c = a.eye[side]
        near_eye = (x * s > 0.005) & (a.eye_ring <= 7) & a.skin_ext_mask
        lid_falloff = smoothstep(7.0, 1.0, a.eye_ring.astype(np.float64)) * (x * s > 0.005) * f.skin_ext
        upper_lid = g.smooth(lid_falloff * smoothstep(c[1] - 0.002, c[1] + 0.002, y), 2)
        lower_lid = g.smooth(lid_falloff * smoothstep(c[1] + 0.002, c[1] - 0.002, y), 2)
        lw_blink = np.full(n, 0.15, dtype=np.float64)
        lw_blink[near_eye] = 3.0
        lw_blink[brow > 0.5] = 1.0
        lw_blink[(x * s < 0.0) & a.skin_ext_mask] = 1.5
        lw_blink[~a.skin_ext_mask] = 0.0
        # 上睑下落时向前包覆眼球凸面（+Z），确保闭眼不透出巩膜
        d_blink = np.stack([
            np.zeros_like(x),
            -0.0092 * upper_lid + 0.0022 * lower_lid,
            0.0034 * upper_lid + 0.0007 * lower_lid,
        ], axis=1)
        deltas["eyeBlink" + side] = fit(g, d_blink, lw_blink, block, lam=1e-4, max_scale=1.5)
        d_wide = np.stack([
            np.zeros_like(x),
            0.0045 * upper_lid - 0.0018 * lower_lid,
            -0.0006 * upper_lid,
        ], axis=1)
        deltas["eyeWide" + side] = fit(g, d_wide, lw_blink, block, lam=1e-4, max_scale=1.5)

        squint = g.smooth(gauss(y - (c[1] - 0.015), 0.015) * (f.cheek[side] + f.infra[side]), 2)
        sq_w = base_w.copy()
        sq_w[near_eye | (f.cheek[side] > 0.3) | (f.infra[side] > 0.3)] = 2.0
        sq_w[a.lip_mask] = 1.5
        sq_w[(x * s < -0.005) & a.skin_ext_mask] = 1.5
        sq_w[~a.skin_ext_mask] = 0.0
        deltas["cheekSquint" + side] = fit(
            g, np.stack([np.zeros_like(x), 0.0028 * lower_lid + 0.0042 * squint, 0.0018 * squint], axis=1),
            sq_w, ("lower", block), lam=2e-4, max_scale=1.45)

        cheek = g.smooth(f.cheek[side] * smoothstep(0.018, 0.038, np.abs(x)), 2)
        cw = base_w.copy()
        cw[(f.cheek[side] > 0.2) & a.skin_ext_mask] = 2.5
        cw[a.lip_mask] = 2.0
        cw[(x * s < -0.005) & a.skin_ext_mask] = 1.5
        cw[~a.skin_ext_mask] = 0.0
        deltas["cheekPuff" + side] = fit(
            g, np.stack([s * 0.0055 * cheek, 0.0005 * cheek, 0.0028 * cheek], axis=1),
            cw, "lower", lam=2e-4, max_scale=1.4)
        deltas["cheekSuck" + side] = fit(
            g, np.stack([-s * 0.0042 * cheek, -0.0005 * cheek, -0.0016 * cheek], axis=1),
            cw, "lower", lam=2e-4, max_scale=1.4)

        nostril = g.smooth(gauss(np.abs(x) - 0.012, 0.008) * gauss(y - (a.nose_y + 0.008), 0.008)
                           * (f.nose > 0.15) * f.side(s, 0.006), 2)
        nw = base_w.copy()
        nw[(f.nose > 0.2) & (x * s > -0.002)] = 2.5
        nw[a.lip_lo_mask] = 2.0
        nw[(x * s < -0.006) & a.skin_ext_mask] = 1.5
        nw[~a.skin_ext_mask] = 0.0
        deltas["noseSneer" + side] = fit(
            g, np.stack([s * 0.0016 * nostril, 0.0048 * nostril, 0.0012 * nostril], axis=1),
            nw, "lower", lam=2e-4, max_scale=1.45)

    # 上牙与眼球保持刚性静止，软组织表情不连带下牙/舌头
    static = (g.G["upper_teeth_and_gums"] > 0.5) | (g.G["eye_interiors"] > 0.5)
    for name in deltas:
        deltas[name][static] = 0.0
    for name in deltas:
        if not name.startswith("jaw"):
            deltas[name][f.lt | f.tng] = 0.0

    return deltas


# ---------------------------------------------------------------- 区域与材质


def region_weight(g, a, f):
    x, y, z = f.x, f.y, f.z
    P, G = g.P, g.G
    out = {name: np.zeros(len(P)) for name in C.REGIONS}
    out["upperLip"] = np.where(a.lip_up_mask, 1.0, 0.0)
    out["lowerLip"] = np.where(a.lip_lo_mask, 1.0, 0.0)
    out["lipColorUpper"] = out["upperLip"]
    out["lipColorLower"] = out["lowerLip"]
    corner = smoothstep(a.corner_x - 0.007, a.corner_x - 0.002, np.abs(x)) * a.lip_mask
    out["mouthCornerL"] = corner * (x > 0)
    out["mouthCornerR"] = corner * (x < 0)
    out["mouthInterior"] = np.clip(f.inner + f.ut + f.lt + f.tng, 0, 1)
    out["chin"] = (np.clip(G["chin_region"], 0, 1) > 0.5).astype(np.float64)
    jaw = (f.skin_ext > 0) & (y < a.mouth_y - 0.004) & (y > a.chin_y + 0.004) & (np.abs(x) > 0.016)
    out["jaw"] = jaw.astype(np.float64)
    out["neck"] = ((f.skin_ext > 0) & (y < a.chin_y - 0.004)).astype(np.float64)
    out["forehead"] = (np.clip(G["forehead_region"], 0, 1) > 0.5).astype(np.float64)
    out["templeL"] = (np.clip(G["left_temple_region"], 0, 1) > 0.5).astype(np.float64)
    out["templeR"] = (np.clip(G["right_temple_region"], 0, 1) > 0.5).astype(np.float64)
    out["earL"] = ((np.clip(G["ears"], 0, 1) > 0.5) & (x > 0)).astype(np.float64)
    out["earR"] = ((np.clip(G["ears"], 0, 1) > 0.5) & (x < 0)).astype(np.float64)
    out["cheekL"] = (np.clip(G["left_cheek_region"], 0, 1) > 0.5).astype(np.float64)
    out["cheekR"] = (np.clip(G["right_cheek_region"], 0, 1) > 0.5).astype(np.float64)
    out["browL"] = (np.clip(G["left_brow_region"], 0, 1) > 0.5).astype(np.float64)
    out["browR"] = (np.clip(G["right_brow_region"], 0, 1) > 0.5).astype(np.float64)
    for side, key in (("L", "eyebrowL"), ("R", "eyebrowR")):
        bg = f.brow[side]
        curve_y = np.polyval(a.brow_poly[side], np.abs(x)) + 0.0012
        cond = (bg > 0.3) & (np.abs(y - curve_y) < 0.0055)
        cond &= (x > 0.004) if side == "L" else (x < -0.004)
        out[key] = cond.astype(np.float64)
    out["eyelidL"] = ((np.clip(G["left_orbital_region"], 0, 1) > 0.5) & (f.skin > 0) & (x > 0)).astype(np.float64)
    out["eyelidR"] = ((np.clip(G["right_orbital_region"], 0, 1) > 0.5) & (f.skin > 0) & (x < 0)).astype(np.float64)
    out["eyeWhiteL"] = ((np.clip(G["scleras"], 0, 1) > 0.5) & (x > 0)).astype(np.float64)
    out["eyeWhiteR"] = ((np.clip(G["scleras"], 0, 1) > 0.5) & (x < 0)).astype(np.float64)
    out["irisL"] = ((np.clip(G["irises"], 0, 1) > 0.5) & (x > 0)).astype(np.float64)
    out["irisR"] = ((np.clip(G["irises"], 0, 1) > 0.5) & (x < 0)).astype(np.float64)
    out["pupilL"] = ((np.clip(G["pupils"], 0, 1) > 0.5) & (x > 0)).astype(np.float64)
    out["pupilR"] = ((np.clip(G["pupils"], 0, 1) > 0.5) & (x < 0)).astype(np.float64)
    out["nose"] = (np.clip(G["nose_region"], 0, 1) > 0.5).astype(np.float64)
    nostril = (y < a.nose_y + 0.008) & (np.abs(x) > 0.005) & (np.abs(x) < 0.020) & (z > 0.130)
    out["nostrilL"] = (nostril & (x > 0)).astype(np.float64)
    out["nostrilR"] = (nostril & (x < 0)).astype(np.float64)
    return out


def build_materials_and_colors(g, a, f, base_P):
    """按精确拓扑环距生成材质 id 与逐顶点颜色（消除嘴角黑斑、红下巴与熊猫眼）。"""
    P, G = base_P, g.G
    x, y, z = P[:, 0], P[:, 1], P[:, 2]
    mat = np.full(len(P), C.MATERIAL_ID["skin"], dtype=np.uint8)

    lip_ring_f = a.lip_ring.astype(np.float64)
    lip_w = smoothstep(5.8, 3.2, lip_ring_f) * f.skin_ext
    corner_taper = smoothstep(a.corner_x + 0.001, a.corner_x - 0.006, np.abs(x))
    lip_w *= (0.58 + 0.42 * corner_taper)
    lip_up_w = lip_w * np.where(a.lip_up_mask, 1.0, np.where(a.lip_lo_mask, 0.0, smoothstep(a.mouth_y - 0.002, a.mouth_y + 0.002, y)))
    lip_lo_w = lip_w - lip_up_w

    mat[a.lip_mask & (a.lip_ring <= 5)] = C.MATERIAL_ID["lip"]
    # 口内衬最外环（sock_ring==1）与唇缘共边，归入温和湿润过渡，避免口角三角锯齿
    mat[(G["mouth_sock"] > 0.3) & (a.sock_ring >= 2)] = C.MATERIAL_ID["mouth"]
    mat[(G["mouth_sock"] > 0.3) & (a.sock_ring <= 1)] = C.MATERIAL_ID["lip"]
    mat[G["gums"] > 0.5] = C.MATERIAL_ID["gums"]
    mat[G["teeth"] > 0.5] = C.MATERIAL_ID["teeth"]
    mat[G["tongue"] > 0.5] = C.MATERIAL_ID["tongue"]
    mat[G["scleras"] > 0.5] = C.MATERIAL_ID["sclera"]
    mat[G["irises"] > 0.5] = C.MATERIAL_ID["iris"]
    mat[G["pupils"] > 0.5] = C.MATERIAL_ID["pupil"]

    brow_w = np.zeros(len(P), dtype=np.float64)
    for side in ("L", "R"):
        bg_raw = f.brow[side]
        bg = g.smooth(bg_raw, 2)
        sel = bg_raw > 0.25
        if not sel.any():
            continue
        ax = np.abs(x)
        curve_y = np.polyval(a.brow_poly[side], ax) + 0.0012
        sigma_y = 0.0052 - 0.0016 * smoothstep(0.015, 0.056, ax)
        cx_min = float(ax[sel].min())
        cx_max = float(ax[sel].max())
        x_taper = smoothstep(cx_min - 0.002, cx_min + 0.007, ax) * smoothstep(cx_max + 0.001, cx_max - 0.010, ax)
        w_side = bg * x_taper * gauss(y - curve_y, sigma_y)
        w_side *= (x > 0) if side == "L" else (x < 0)
        brow_w = np.maximum(brow_w, w_side)
    brow_w = g.smooth(brow_w, 1, mix=0.45)
    mat[brow_w > 0.45] = C.MATERIAL_ID["brow"]

    col = np.tile(np.array([0.855, 0.695, 0.610], dtype=np.float64), (len(P), 1))

    def mix(rgb, weight):
        w = np.clip(weight, 0.0, 1.0)[:, None]
        col[:] = col * (1.0 - w) + np.asarray(rgb, dtype=np.float64)[None, :] * w

    cheek_blush = np.clip(f.cheek["L"] + f.cheek["R"] + f.infra["L"] + f.infra["R"], 0, 1) * 0.14
    mix([0.865, 0.655, 0.585], cheek_blush)

    mix([0.925, 0.918, 0.895], np.clip(G["scleras"] * 1.6, 0, 1))
    for side, key in (("L", "left_eye"), ("R", "right_eye")):
        sc = (G["scleras"] > 0.5) & (G[key] > 0.5)
        if sc.any():
            c = a.eye[side]
            r = np.hypot(x - c[0], y - c[1])
            col *= (1.0 - smoothstep(0.005, 0.012, r) * 0.14 * sc)[:, None]

    iris_m = G["irises"] > 0.5
    mix([0.235, 0.320, 0.365], np.clip(G["irises"] * 1.6, 0, 1))
    for side, key in (("L", "left_eye"), ("R", "right_eye")):
        ir = iris_m & (G[key] > 0.5)
        if ir.any():
            c = a.eye[side]
            r = np.hypot(x - c[0], y - c[1])
            rmax, rmin = float(r[ir].max()), float(r[ir].min())
            limbal = smoothstep(rmin + 0.55 * (rmax - rmin), rmax, r) * 0.42 * ir
            col *= (1.0 - limbal)[:, None]
    mix([0.025, 0.025, 0.035], np.clip(G["pupils"] * 1.6, 0, 1))

    sock_depth = smoothstep(1.0, 4.8, a.sock_ring.astype(np.float64))
    sock_col = (1.0 - sock_depth[:, None]) * np.array([0.56, 0.34, 0.34]) + sock_depth[:, None] * np.array([0.11, 0.04, 0.05])
    sock_w = np.clip(G["mouth_sock"] * 1.8, 0, 1)[:, None]
    col[:] = col * (1.0 - sock_w) + sock_col * sock_w

    gums_ao = 0.58 + 0.42 * smoothstep(0.095, 0.124, z)
    teeth_ao = 0.62 + 0.38 * smoothstep(0.095, 0.125, z)
    tongue_ao = 0.45 + 0.55 * smoothstep(0.085, 0.118, z)
    gw = np.clip(G["gums"] * 1.6, 0, 1)[:, None]
    tw = np.clip(G["teeth"] * 1.6, 0, 1)[:, None]
    tnw = np.clip(G["tongue"] * 1.6, 0, 1)[:, None]
    col[:] = col * (1.0 - gw) + (np.array([0.70, 0.42, 0.43]) * gums_ao[:, None]) * gw
    col[:] = col * (1.0 - tw) + (np.array([0.90, 0.88, 0.83]) * teeth_ao[:, None]) * tw
    col[:] = col * (1.0 - tnw) + (np.array([0.66, 0.37, 0.39]) * tongue_ao[:, None]) * tnw

    mix([0.745, 0.510, 0.485], lip_up_w * 0.90)
    mix([0.805, 0.555, 0.525], lip_lo_w * 0.90)
    # 唇缝压暗仅作用在精确唇组（upper_lip|lower_lip）内缘，绝不外溢成嘴角胡须纹
    seam = np.where(a.lip_mask, smoothstep(2.4, 1.0, lip_ring_f) * 0.22, 0.0)
    col *= (1.0 - seam)[:, None]

    mix([0.39, 0.30, 0.24], np.clip(brow_w * 0.72, 0, 1))

    eye_ring_f = a.eye_ring.astype(np.float64)
    lash = np.where(a.skin_ext_mask, smoothstep(2.3, 1.0, eye_ring_f) * 0.26, 0.0)
    mix([0.52, 0.37, 0.35], (G["eye_sockets"] > 0.5).astype(np.float64) * 0.75)
    col *= (1.0 - lash)[:, None]

    return mat, col


# ---------------------------------------------------------------- 预览（可选）


def render_preview(positions, normals, colors, path, yaw=0.0, size=(320, 380)):
    from PIL import Image

    W, H = size
    target = np.array([0.0, 0.0, 0.0])
    dist = 3.1
    eye = target + np.array([np.sin(yaw) * dist, 0.05, np.cos(yaw) * dist])
    fwd = target - eye
    fwd /= np.linalg.norm(fwd)
    right = np.cross(fwd, np.array([0.0, 1.0, 0.0]))
    right /= np.linalg.norm(right)
    up = np.cross(right, fwd)
    cam = np.stack([(positions - eye) @ right, (positions - eye) @ up,
                    (positions - eye) @ fwd], axis=1)
    fl = 1.0 / np.tan(np.radians(15.0))
    sx = (cam[:, 0] / cam[:, 2] * fl / (W / H) * 0.5 + 0.5) * W
    sy = (0.5 - cam[:, 1] / cam[:, 2] * fl * 0.5) * H
    light = np.array([0.35, 0.45, 0.82])
    light /= np.linalg.norm(light)
    lam = np.clip(normals @ light, 0.0, 1.0)
    rgb = np.asarray(colors, dtype=np.float64) * (0.30 + 0.85 * lam[:, None])
    img = np.ones((H, W, 3))
    radius = 1
    for i in np.argsort(-cam[:, 2]):
        if cam[i, 2] <= 0.05:
            continue
        cx, cy = int(sx[i]), int(sy[i])
        if 0 <= cx < W and 0 <= cy < H:
            img[max(0, cy - radius):cy + radius + 1, max(0, cx - radius):cx + radius + 1] = np.clip(rgb[i], 0, 1)
    Image.fromarray((img * 255).astype(np.uint8)).save(path)


def write_contact_sheet(g, neutral, deltas, colors, out_dir):
    """把 32 个 morph 各渲染一帧拼成检查图（人眼验收用，不参与构建）。"""
    from PIL import Image

    out_dir.mkdir(parents=True, exist_ok=True)
    tiles = []
    base = out_dir / "morph-neutral.png"
    render_preview(neutral, C.vertex_normals(neutral, g.TRI), colors, base)
    tiles.append(Image.open(base))
    for name, delta in deltas.items():
        posed = neutral + delta
        p = out_dir / f"morph-{name}.png"
        render_preview(posed, C.vertex_normals(posed, g.TRI), colors, p)
        tiles.append(Image.open(p))
    cols = 6
    w, h = tiles[0].size
    rows = (len(tiles) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * w, rows * h), (255, 255, 255))
    for i, tile in enumerate(tiles):
        sheet.paste(tile, ((i % cols) * w, (i // cols) * h))
    sheet.save(out_dir / "morph-sheet.png")
    print(f"检查图：{out_dir / 'morph-sheet.png'}")


# ---------------------------------------------------------------- 主流程


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--weights", default=str(CACHE))
    parser.add_argument("--out", default=str(FACE_DIR))
    parser.add_argument("--model", default="head_v2.fgbface")
    parser.add_argument("--preview", default="")
    parser.add_argument("--no-download", action="store_true")
    args = parser.parse_args()

    weights = Path(args.weights)
    if not weights.exists():
        if args.no_download:
            raise SystemExit(f"缺少权重 {weights}（--no-download 生效，不下载）")
        weights = ensure_weights(weights)

    all_shapes = C.shape_names_from_header()
    C.validate_map(all_shapes)

    g = Gnm(weights)
    a = Anchors(g)
    f = Fields(g, a)
    deltas = build_morphs(g, a, f)

    # GNM 模板本身的上下唇已自然贴合；直接保持模板闭唇，仅沿牙弓中心微缩牙齿与舌头
    # 并把深处口腔内衬外扩成自然颊前庭，避免极端表情时与侧磨牙或嘴角皮肤穿模。
    base_P = g.P.copy()
    arch_center_z = 0.078
    for mask in (f.ut, f.lt):
        x_scale = 0.965 - 0.050 * smoothstep(0.014, 0.028, np.abs(base_P[mask, 0]))
        base_P[mask, 0] *= x_scale
        base_P[mask, 2] = arch_center_z + (base_P[mask, 2] - arch_center_z) * 0.958
    base_P[f.tng, 0] *= 0.95
    base_P[f.tng, 2] -= 0.0015
    sock_r12 = a.sock_mask & (a.sock_ring <= 2)
    base_P[sock_r12, 2] -= 0.0007
    deep_sock = a.sock_mask & (a.sock_ring >= 3) & (base_P[:, 2] < 0.115)
    base_P[deep_sock, 0] *= 1.08
    base_P[deep_sock, 1] = a.mouth_y + (base_P[deep_sock, 1] - a.mouth_y) * 1.08

    positions = (base_P - a.center) * a.scale
    deltas = {k: v * a.scale for k, v in deltas.items()}
    normals = C.vertex_normals(positions, g.TRI)
    regions = np.zeros(len(positions), dtype=np.uint32)
    weights_field = region_weight(g, a, f)
    for i, name in enumerate(C.REGIONS):
        regions |= (weights_field[name] > 0.5).astype(np.uint32) << i
    materials, colors = build_materials_and_colors(g, a, f, base_P)

    out_dir = Path(args.out)
    size = C.write_fgbface(
        out_dir / args.model,
        positions, normals, g.TRI.reshape(-1), regions, materials, colors,
        C.MORPH_NAMES, [deltas[name] for name in C.MORPH_NAMES])
    C.write_face_map(
        out_dir / "face_map.json", all_shapes, args.model,
        "Google GNM Head（Apache-2.0）烘焙的中性头 + 32 个 morph；区域名与 morph 名是稳定契约",
        "tools/bake_gnm_head.py")

    at_least = min(float(np.abs(d).max()) for d in deltas.values())
    print(f"vertices={len(positions)} triangles={len(g.TRI)} morphs={len(deltas)}")
    print(f"顶点数 {len(positions)} / 三角面 {len(g.TRI)} / 区域 {len(C.REGIONS)} / 材质 {len(C.MATERIALS)}")
    print(f"最小 morph 峰值位移 {at_least:.4f}（模型单位），最大 "
          f"{max(float(np.abs(d).max()) for d in deltas.values()):.4f}")
    print(f"wrote {out_dir / args.model} ({size / 1048576:.2f} MiB)")
    print(f"wrote {out_dir / 'face_map.json'}")

    if args.preview:
        write_contact_sheet(g, positions, deltas, colors, Path(args.preview))
    return 0


if __name__ == "__main__":
    sys.exit(main())
