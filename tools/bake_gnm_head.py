#!/usr/bin/env python3
"""从 Google GNM Head 烘焙校准页用的 `.fgbface` v2 头模。

为什么要换资产（ADR-0021 P4）：原来 `tools/gen_face_model.py` 出的是一颗
程序化椭球头，结构能认，但下颌、唇形、眼睑、鼻翼都是手工高斯堆出来的，
"调 lo/hi 时脸怎么变"看不出真实比例。GNM Head 是 Google 2026 年开源的
统计式参数化人头（Apache-2.0，可商用可再分发）：17.8k 顶点的扫描级中性头，
自带眼球/牙齿/舌头与 383 维表情基，正好可以当"风格化写实"的底模。

做法：中性头直接取模板网格；32 个 morph 不是手刷顶点，而是
"用几何目标场驱动统计基"——按解剖锚点写出期望位移场（下颌绕铰链旋转、
嘴角上提、眼睑闭合……），再在 GNM 对应区块里做带岭正则的最小二乘，
把目标场投影到统计基上。这样位移天然平滑、符合解剖，且不会出现手工高斯
在扫描网格上常见的"融化"感。

    python tools/bake_gnm_head.py                      # 写 ui/face/
    python tools/bake_gnm_head.py --preview out/       # 另出一张 morph 检查图

权重（53 MB，不随仓库分发）默认缓存到 `tools/.cache/gnm_head.npz`，
首次运行自动从 Hugging Face 下载；也可用 `--weights` 指定。
烘焙产物只有 ~1.3 MB，进仓库与安装包。
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
        # 只保留"看得见"的三部分：皮肤、眼球内层、口腔内衬。
        #   * 角膜壳：真实渲染里是透明的，不透明着色会盖住虹膜；
        #   * 牙齿/牙龈/舌头：它们是口腔里的独立壳体，下颌动作把它们带下来时会
        #     从唇面/下巴里穿出来（预览没有遮挡与透明，穿模非常明显）。
        # 剔掉后张嘴看到的是暗色口腔内衬，够用且干净。
        drop = (self.G["eye_exteriors"] > 0.5)
        for name in ("upper_teeth_and_gums", "lower_teeth_and_gums", "tongue"):
            drop |= (self.G[name] > 0.5)
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

    def smooth(self, field, iterations=2, mix=0.5):
        w = np.asarray(field, dtype=np.float64)
        for _ in range(iterations):
            acc = np.zeros_like(w)
            np.add.at(acc, self.edges[:, 0], w[self.edges[:, 1]])
            w = (1.0 - mix) * w + mix * acc / self.degree
        return w


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
        lip = np.clip(G["upper_lip_region"] + G["lower_lip_region"], 0, 1)
        lip_mask = lip > 0.5
        self.mouth_y = float(P[lip_mask][:, 1].mean())
        self.mouth_z = float(P[lip_mask][:, 2].mean())
        self.corner_x = float(np.abs(P[lip_mask][:, 0]).max())
        self.chin_y = float(P[G["chin_region"] > 0.5][:, 1].min())
        self.nose_y = float(P[G["nose_region"] > 0.5][:, 1].min())
        self.eye = {}
        for side, key in (("L", "left_eye"), ("R", "right_eye")):
            verts = P[G[key] > 0.5]
            self.eye[side] = verts.mean(0)
            self.eye_radius = float(np.abs(verts[:, 1] - self.eye[side][1]).max())
        self.brow = {}
        for side, key in (("L", "left_brow_region"), ("R", "right_brow_region")):
            self.brow[side] = P[G[key] > 0.5].mean(0)
        self.eye_y = 0.5 * (self.eye["L"][1] + self.eye["R"][1])
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
        self.lip_up = np.clip(G["upper_lip_region"], 0, 1)
        self.lip_lo = np.clip(G["lower_lip_region"], 0, 1)
        self.teeth_low = np.clip(G["lower_teeth_and_gums"], 0, 1)
        self.teeth_up = np.clip(G["upper_teeth_and_gums"], 0, 1)
        self.tongue = np.clip(G["tongue"], 0, 1)
        self.inner = np.clip(G["mouth_sock"], 0, 1)
        self.cheek = {"L": np.clip(G["left_cheek_region"], 0, 1),
                      "R": np.clip(G["right_cheek_region"], 0, 1)}
        self.brow = {"L": np.clip(G["left_brow_region"], 0, 1),
                     "R": np.clip(G["right_brow_region"], 0, 1)}
        self.orbital = {"L": np.clip(G["left_orbital_region"], 0, 1),
                        "R": np.clip(G["right_orbital_region"], 0, 1)}
        self.nose = np.clip(G["nose_region"], 0, 1)
        self.front = smoothstep(-0.030, 0.030, self.z)

    def side(self, s):
        return smoothstep(-0.010, 0.010, self.x * s)

    def jaw(self):
        g = self.g
        # 过渡带跨过唇线：上唇基本不动、下唇跟着下颌走，嘴才会"张开"。带子不能
        # 太窄——统计基表达不了硬折痕，会过冲成自穿模（牙齿从唇面里穿出来）。
        w = smoothstep(self.a.mouth_y + 0.010, self.a.mouth_y - 0.020, self.y)
        # 颏下与脖子跟着转会把脖子拉断：下颌底下 8 mm 之外不再跟随
        w *= smoothstep(self.a.chin_y - 0.012, self.a.chin_y + 0.016, self.y)
        w *= self.front
        w = np.maximum(w, self.teeth_low)
        w = np.maximum(w, 0.45 * self.tongue)
        # 口腔内衬（mouth_sock）的下半圈必须跟着下颌走，否则张嘴时会从嘴唇里
        # 顶出来一块——GNM 的内衬是一根连接上下唇的管子，不驱动就会留在原地
        sock = self.inner * smoothstep(self.a.mouth_y + 0.004, self.a.mouth_y - 0.010, self.y)
        w = np.maximum(w, sock)
        w = g.smooth(w, 2)
        keep = (self.skin > 0) | (self.teeth_low > 0.5) | (self.tongue > 0.5) | (self.inner > 0.5)
        return np.where(keep, np.clip(w, 0, 1), 0.0)


# ---------------------------------------------------------------- 位移场


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


def build_targets(g, a, f):
    """32 个 morph 的期望位移场（米，GNM 原始坐标），键与 face_contract 一致。"""
    x, y, z = f.x, f.y, f.z
    out = {}
    jaw_w = f.jaw()
    hinge = np.array([0.0, a.eye_y - 0.010, 0.004])

    # 绕铰链旋转会让下巴往后缩（真实下颌还有前伸分量），补一点向前的位移
    d = rot_axis(g.P, 0.28 * jaw_w, hinge, 0) - g.P
    d[:, 2] += 0.022 * jaw_w
    # 光靠旋转，嘴唇在闭合拓扑上会被拉成一层膜（看不到口腔）。显式给上下唇
    # 一个相反的分离量，统计基才会把它学成"张嘴"而不是"下半脸拉长"。
    part = gauss(y - a.mouth_y, 0.009) * jaw_w * np.sign(y - a.mouth_y)
    d[:, 1] += 0.0055 * f.lip_up - 0.0090 * f.lip_lo + 0.0025 * part
    out["jawOpen"] = d
    for name, s in (("jawLeft", 1.0), ("jawRight", -1.0)):
        out[name] = np.stack([s * 0.0060 * jaw_w, -0.0008 * jaw_w, np.zeros_like(x)], axis=1)
    out["jawForward"] = np.stack([np.zeros_like(x), np.zeros_like(x), 0.0050 * jaw_w], axis=1)

    # 嘴角：权重按"到嘴角锚点的距离"给，而不是按唇组的软权重——唇组在嘴角处
    # 权重本来就低，只看组权重会变成"嘴唇中间在动、嘴角不动"，笑不出来。
    for name, s in (("smileL", 1.0), ("smileR", -1.0),
                    ("frownL", 1.0), ("frownR", -1.0)):
        up = 1.0 if name.startswith("smile") else -1.0
        near = np.exp(-((x - s * a.corner_x) / 0.013) ** 2 - ((y - a.mouth_y) / 0.011) ** 2)
        near *= np.clip(f.lip_up + f.lip_lo + f.inner, 0, 1) > 0.12
        lip = g.smooth(near, 2)
        cheek = g.smooth(gauss(y - (a.mouth_y + 0.008), 0.018) * f.cheek["L" if s > 0 else "R"], 2)
        dx = s * 0.0070 * lip + s * 0.0018 * cheek
        dy = up * (0.0105 * lip + 0.0024 * cheek)
        dz = up * 0.0014 * lip
        out[name] = np.stack([dx, dy, dz], axis=1)

    lip_all = np.clip(f.lip_up + f.lip_lo, 0, 1)
    center = gauss(x, 0.020)
    out["pucker"] = np.stack([
        -x * 0.42 * lip_all * center,
        (f.lip_lo - f.lip_up) * 0.0035 * center,
        0.0055 * lip_all * center,
    ], axis=1)
    out["lipPress"] = np.stack([
        np.zeros_like(x),
        (f.lip_lo - f.lip_up) * 0.0032,
        -0.0012 * lip_all,
    ], axis=1)

    for name, s in (("upperLipUpL", 1.0), ("upperLipUpR", -1.0)):
        w = g.smooth(f.lip_up * f.side(s), 2)
        out[name] = np.stack([np.zeros_like(x), 0.0068 * w, 0.0006 * w], axis=1)
    for name, s in (("lowerLipDownL", 1.0), ("lowerLipDownR", -1.0)):
        w = g.smooth(f.lip_lo * f.side(s), 2)
        out[name] = np.stack([np.zeros_like(x), -0.0062 * w, 0.0006 * w], axis=1)

    for side, s in (("L", 1.0), ("R", -1.0)):
        brow = f.brow[side]
        inner = g.smooth(gauss(x - s * 0.016, 0.013) * brow, 2)
        outer = g.smooth(gauss(x - s * 0.052, 0.013) * brow, 2)
        whole = g.smooth(brow, 2)
        out["browInnerUp" + side] = np.stack([np.zeros_like(x), 0.0062 * inner, 0.0004 * inner], axis=1)
        out["browOuterUp" + side] = np.stack([np.zeros_like(x), 0.0058 * outer, np.zeros_like(x)], axis=1)
        out["browDown" + side] = np.stack([np.zeros_like(x), -0.0050 * whole, 0.0010 * whole], axis=1)

        # 眼睑：睑裂边缘位移最大，向眶缘衰减；上睑下移量约为睑裂高度
        c = a.eye[side]
        span = gauss(x - c[0], 0.017) * f.skin
        above = np.clip(y - (c[1] + 0.004), 0.0, None)
        below = np.clip((c[1] - 0.004) - y, 0.0, None)
        upper = g.smooth(gauss(above, 0.010) * span * (y > c[1]), 2)
        lower = g.smooth(gauss(below, 0.007) * span * (y < c[1]), 2)
        out["eyeBlink" + side] = np.stack([
            np.zeros_like(x), -0.0088 * upper + 0.0022 * lower, np.zeros_like(x)], axis=1)
        out["eyeWide" + side] = np.stack([
            np.zeros_like(x), 0.0042 * upper - 0.0016 * lower, np.zeros_like(x)], axis=1)
        squint = g.smooth(gauss(y - (c[1] - 0.016), 0.016) * f.cheek[side], 2)
        out["cheekSquint" + side] = np.stack([
            np.zeros_like(x), 0.0022 * lower + 0.0038 * squint,
            0.0012 * squint], axis=1)
        cheek = f.cheek[side]
        out["cheekPuff" + side] = np.stack([
            s * 0.0038 * cheek, 0.0006 * cheek, 0.0012 * cheek], axis=1)
        out["cheekSuck" + side] = np.stack([
            -s * 0.0034 * cheek, -0.0006 * cheek, -0.0010 * cheek], axis=1)

        nostril = g.smooth(gauss(np.abs(x) - 0.010, 0.007) * gauss(y - (a.nose_y + 0.006), 0.007)
                           * (f.nose > 0.2) * f.side(s), 2)
        out["noseSneer" + side] = np.stack([
            s * 0.0014 * nostril, 0.0042 * nostril, 0.0010 * nostril], axis=1)
    return out


# ---------------------------------------------------------------- 拟合


# GNM 的表情基按解剖分块：眉毛、眼睑、眶周都在左右眼区块里，下颌/唇/颊/鼻
# 在下半脸区块。选错区块拟合出来接近零（脸上一动不动），所以这里显式列出来。
BLOCK_OF = {name: "lower" for name in C.MORPH_NAMES}
for _side, _block in (("L", "left_eye"), ("R", "right_eye")):
    for _prefix in ("eyeBlink", "eyeWide", "browInnerUp", "browOuterUp", "browDown"):
        BLOCK_OF[_prefix + _side] = _block
    # 眯眼是"下睑上抬 + 颧部鼓起"，一块基描述不全
    BLOCK_OF["cheekSquint" + _side] = ("lower", _block)


def fit(g, target, blocks, lam=2e-3):
    """把目标位移场投影到 GNM 的统计表情基上（岭正则最小二乘）。

    blocks 可以给多个区块的并集（如"下睑+颧部"横跨眼区块与下半脸区块）。
    """
    if isinstance(blocks, str):
        blocks = (blocks,)
    dims = sorted({i for block in blocks for i in g.blocks[block]})
    basis = g.E[dims].reshape(len(dims), -1)          # (K, 3V)
    d = target.reshape(-1)
    gram = basis @ basis.T
    gram += lam * np.trace(gram) / len(dims) * np.eye(len(dims))
    alpha = np.linalg.solve(gram, basis @ d)
    fitted = (alpha[:, None, None] * g.E[dims]).sum(0)
    # 统计基的方向幅度有限，必要时整体缩放以贴近目标场的能量
    support = np.abs(d) > 1e-9
    if support.any():
        target_rms = float(np.sqrt(np.mean(d[support] ** 2)))
        fit_rms = float(np.sqrt(np.mean(fitted.reshape(-1)[support] ** 2)))
        if fit_rms > 1e-12:
            factor = float(np.clip(target_rms / fit_rms, 0.55, 2.2))
            fitted *= factor
    return fitted


# ---------------------------------------------------------------- 区域与材质


def region_weight(g, a, f):
    x, y, z = f.x, f.y, f.z
    P, G = g.P, g.G
    out = {name: np.zeros(len(P)) for name in C.REGIONS}
    lip = np.clip(f.lip_up + f.lip_lo, 0, 1)
    out["upperLip"] = np.clip(f.lip_up * 2.0, 0, 1)
    out["lowerLip"] = np.clip(f.lip_lo * 2.0, 0, 1)
    out["lipColorUpper"] = out["upperLip"]
    out["lipColorLower"] = out["lowerLip"]
    corner = smoothstep(a.corner_x - 0.009, a.corner_x - 0.002, np.abs(x)) * (lip > 0.25)
    out["mouthCornerL"] = corner * (x > 0)
    out["mouthCornerR"] = corner * (x < 0)
    out["mouthInterior"] = np.clip(f.inner, 0, 1)
    out["chin"] = np.clip(G["chin_region"], 0, 1) > 0.5
    jaw = (f.skin > 0) & (y < a.mouth_y - 0.004) & (y > a.chin_y + 0.004) & (np.abs(x) > 0.016)
    out["jaw"] = jaw.astype(np.float64)
    # 「仅数值」之外还得看得见哪块在动：脖子与下颌分开标，高亮不会糊成一整块
    out["neck"] = (f.skin > 0) & (y < a.chin_y - 0.004)
    out["forehead"] = np.clip(G["forehead_region"], 0, 1) > 0.5
    out["templeL"] = np.clip(G["left_temple_region"], 0, 1) > 0.5
    out["templeR"] = np.clip(G["right_temple_region"], 0, 1) > 0.5
    out["earL"] = (np.clip(G["ears"], 0, 1) > 0.5) & (x > 0)
    out["earR"] = (np.clip(G["ears"], 0, 1) > 0.5) & (x < 0)
    out["cheekL"] = np.clip(G["left_cheek_region"], 0, 1) > 0.5
    out["cheekR"] = np.clip(G["right_cheek_region"], 0, 1) > 0.5
    out["browL"] = np.clip(G["left_brow_region"], 0, 1) > 0.5
    out["browR"] = np.clip(G["right_brow_region"], 0, 1) > 0.5
    brow_band = gauss(y - (a.brow["L"][1] + 0.004), 0.006) * (np.abs(x) < 0.058) * (z > 0.085)
    out["eyebrowL"] = brow_band * (x > 0.004)
    out["eyebrowR"] = brow_band * (x < -0.004)
    out["eyelidL"] = (np.clip(G["left_orbital_region"], 0, 1) > 0.5) & (f.skin > 0) & (x > 0)
    out["eyelidR"] = (np.clip(G["right_orbital_region"], 0, 1) > 0.5) & (f.skin > 0) & (x < 0)
    out["eyeWhiteL"] = (np.clip(G["scleras"], 0, 1) > 0.5) & (x > 0)
    out["eyeWhiteR"] = (np.clip(G["scleras"], 0, 1) > 0.5) & (x < 0)
    out["irisL"] = (np.clip(G["irises"], 0, 1) > 0.5) & (x > 0)
    out["irisR"] = (np.clip(G["irises"], 0, 1) > 0.5) & (x < 0)
    out["pupilL"] = (np.clip(G["pupils"], 0, 1) > 0.5) & (x > 0)
    out["pupilR"] = (np.clip(G["pupils"], 0, 1) > 0.5) & (x < 0)
    out["nose"] = (np.clip(G["nose_region"], 0, 1) > 0.5)
    nostril = (y < a.nose_y + 0.008) & (np.abs(x) > 0.005) & (np.abs(x) < 0.020) & (z > 0.130)
    out["nostrilL"] = nostril * (x > 0)
    out["nostrilR"] = nostril * (x < 0)
    return out


def material_ids(g, a, f):
    P, G = g.P, g.G
    mat = np.full(len(P), C.MATERIAL_ID["skin"], dtype=np.uint8)
    x, y, z = f.x, f.y, f.z

    def put(name, condition):
        mat[condition] = C.MATERIAL_ID[name]

    # GNM 的唇区组比"嘴唇"本身大（含人中与颏唇沟），直接按组上色会得到一张
    # 盖住下半张脸的粉色大嘴；再用离唇线的距离裁一条真正的唇带。
    lip_all = np.clip(f.lip_up + f.lip_lo, 0, 1)
    band = np.where(y > a.mouth_y, y < a.mouth_y + 0.0105, y > a.mouth_y - 0.0125)
    lip = (lip_all > 0.45) & band
    # 眉毛没有毛发几何，只能染一条带；带子必须留在眉区组内，否则会爬上额头
    brow_group = np.clip(G["left_brow_region"] + G["right_brow_region"], 0, 1)
    brow = (gauss(y - (a.brow["L"][1] + 0.0015), 0.0032) * brow_group > 0.52) & (z > 0.090)
    # 口腔内衬与唇面在唇线处共用一圈顶点：内衬必须先刷，唇/眉后刷覆盖，
    # 否则唇线那一圈顶点被标成"口腔"，外唇的三角形会随机变暗，像破了洞。
    put("mouth", np.clip(G["mouth_sock"], 0, 1) > 0.3)
    put("gums", np.clip(G["gums"], 0, 1) > 0.5)
    put("teeth", np.clip(G["teeth"], 0, 1) > 0.5)
    put("tongue", np.clip(G["tongue"], 0, 1) > 0.5)
    put("sclera", np.clip(G["scleras"], 0, 1) > 0.5)
    put("iris", np.clip(G["irises"], 0, 1) > 0.5)
    put("pupil", np.clip(G["pupils"], 0, 1) > 0.5)
    # 鼻孔不做染色：鼻翼下方的顶点分布很散，染深会变成一层"脏点"，
    # 而鼻孔本身是几何凹陷，光照已经足够表达。区域掩码里保留 nostril，供高亮用。
    put("lip", lip)
    put("brow", brow)
    return mat


# 逐顶点基色（近似 sRGB，渲染器直接乘光照，所以数值偏亮）
PALETTE = {
    "skin": (0.860, 0.705, 0.625),
    "lip": (0.775, 0.520, 0.500),
    "brow": (0.320, 0.255, 0.205),
    "sclera": (0.930, 0.925, 0.900),
    "iris": (0.225, 0.310, 0.345),
    "pupil": (0.030, 0.030, 0.040),
    "mouth": (0.200, 0.070, 0.080),
}


def vertex_colors(g, a, f):
    """逐顶点颜色。

    材质 id 只能表达硬边界：眉毛、唇线在网格上没有对应的几何分界，按顶点刷材质
    会沿三角形锯齿化（远看是两排锯齿，近看是碎片）。改成逐顶点颜色后，边界由
    光栅化插值，成为 1–2 mm 的柔和过渡。
    """
    P, G = g.P, g.G
    x, y = f.x, f.y
    col = np.tile(np.array(PALETTE["skin"], dtype=np.float64), (len(P), 1))

    def mix(name, weight):
        w = np.clip(weight, 0.0, 1.0)[:, None]
        col[:] = col * (1.0 - w) + np.asarray(PALETTE[name], dtype=np.float64)[None, :] * w

    # 眼球与口腔内衬本身就是独立几何，边界由几何决定，按组权重上色即可
    mix("sclera", np.clip(G["scleras"] * 1.6, 0, 1))
    mix("iris", np.clip(G["irises"] * 1.6, 0, 1))
    mix("pupil", np.clip(G["pupils"] * 1.6, 0, 1))
    mix("mouth", np.clip(G["mouth_sock"] * 2.2, 0, 1))

    # 嘴唇：唇区软权重 × 椭圆范围（矩形带会让嘴看起来是被"涂"上去的方块）
    lip_w = np.clip(f.lip_up + f.lip_lo, 0, 1)
    dy = y - a.mouth_y
    half_y = np.where(dy > 0.0, 0.0135, 0.0155)
    u = np.sqrt((x / (0.88 * a.corner_x + 1e-6)) ** 2 + (dy / half_y) ** 2)
    band = smoothstep(1.0, 0.55, u)
    mix("lip", np.clip(lip_w * band * 1.2, 0, 1))
    # 唇缝：沿唇线压一条暗线——闭嘴时才是一条缝，而不是一整片粉色
    seam = smoothstep(0.0042, 0.0012, np.abs(dy)) * lip_w
    col *= (1.0 - 0.42 * seam)[:, None]

    # 眉毛：眉区权重 × 竖向高斯 × 横向端点收口（没有毛发几何，只能"画"）
    for key in ("L", "R"):
        bg = f.brow[key]
        if float(bg.max()) <= 0.0:
            continue
        sel = bg > 0.25
        if not sel.any():
            continue
        cy = float(np.average(P[:, 1], weights=bg))
        cx = float(np.average(P[sel][:, 0]))
        hx = float(np.abs(P[sel][:, 0] - cx).max()) + 1e-6
        window = smoothstep(0.95, 0.62, np.abs(x - cx) / hx)
        mix("brow", np.clip(bg * window * gauss(y - cy, 0.0042) * 1.05, 0, 1))

    # 眼裂阴影：离眼球最近的皮肤压暗一圈——睁眼时是一条眼线，闭眼时是睑缘；
    # 没有这一圈，眼皮看起来像一块肿起来的橡皮。
    eyes = (G["scleras"] > 0.5) | (G["irises"] > 0.5)
    lid = (G["left_orbital_region"] > 0.15) | (G["right_orbital_region"] > 0.15)
    if eyes.any() and lid.any():
        pts = P[eyes]
        idx = np.nonzero(lid)[0]
        near = np.linalg.norm(P[idx][:, None, :] - pts[None, :, :], axis=2).min(axis=1)
        shade = smoothstep(0.0080, 0.0020, near) * 0.34
        col[idx] *= (1.0 - shade)[:, None]
    return col


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


def write_contact_sheet(g, a, neutral, deltas, colors, out_dir):
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
    targets = build_targets(g, a, f)

    deltas = {}
    for name in C.MORPH_NAMES:
        deltas[name] = fit(g, targets[name], BLOCK_OF[name])

    # 上牙/牙龈与眼球是刚体：任何表情都不该让它们变形，否则统计基会把它们推进
    # 唇面里，渲染出来是牙齿从嘴唇穿出来的锯齿。这里统一钉住。
    static = ((g.G["upper_teeth_and_gums"] > 0.5) | (g.G["eye_interiors"] > 0.5))
    for name in C.MORPH_NAMES:
        deltas[name][static] = 0.0

    # GNM 模板的唇缝是微张的（口腔内衬会露出一点白）。校准页的中性脸应该闭嘴，
    # 所以把基础网格往"轻压唇"推一点；morph 位移仍以模板为基准，不受影响。
    closed = g.P + deltas["lipPress"] * 0.55
    # 口腔内衬整体后退 1.5 mm：它和唇面在唇线处几乎重合，张嘴时逐顶点材质边界会
    # 互相穿插成锯齿；后退一点让它稳定地留在唇后，只在张嘴时露出暗腔。
    inner_mask = np.clip(g.G["mouth_sock"], 0, 1) > 0.3
    closed[inner_mask] -= np.array([0.0, 0.0, 0.0015])
    positions = (closed - a.center) * a.scale
    deltas = {k: v * a.scale for k, v in deltas.items()}
    normals = C.vertex_normals(positions, g.TRI)
    regions = np.zeros(len(positions), dtype=np.uint32)
    weights_field = region_weight(g, a, f)
    for i, name in enumerate(C.REGIONS):
        regions |= (weights_field[name] > 0.5).astype(np.uint32) << i
    materials = material_ids(g, a, f)
    colors = vertex_colors(g, a, f)

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
        write_contact_sheet(g, a, positions, deltas, colors, Path(args.preview))
    return 0


if __name__ == "__main__":
    sys.exit(main())
