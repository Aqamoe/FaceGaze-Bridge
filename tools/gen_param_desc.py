#!/usr/bin/env python3
"""生成 ui/param_desc.json —— 参数页卡片的人话说明（中文 / English）。

为什么需要生成而不是手写 JSON：参数目录是数据驱动的（88 基础形状 + 88 v2 派生 +
189 条 v1 legacy），v1 的 91 条合并规则名是 `<部位><修饰>` 的机械组合，手写 365 条
既写不完也维护不了。这里只维护"词表 + 少量显式条目"，其余按规则拼。

- 基础 88 个形状的名称在 ui/shapes.json 里（校准页也用），本脚本**不重复**它们，
  参数页查不到时会回落到那份表，所以两边永远一致。
- 新增参数忘了写说明时，本脚本会失败退出（tests/test_params.cpp 也会兜一层）。

用法：
    python tools/gen_param_desc.py            # 写 ui/param_desc.json
    python tools/gen_param_desc.py --check    # 只检查覆盖，不写文件
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
UI = ROOT / "ui"

# ---------------------------------------------------------------------------
# 显式条目：名字 → (中文, English)
# 中文写法统一成"部位 动作"两段，和形状表里的 `左眼 眯眼` 同一口径。
# ---------------------------------------------------------------------------
EXPLICIT = {
    # ---- v2 派生：简化形状 ----
    "BrowUpLeft": ("左眉 上抬", "Left brow up"),
    "BrowUpRight": ("右眉 上抬", "Right brow up"),
    "BrowDownLeft": ("左眉 下压", "Left brow down"),
    "BrowDownRight": ("右眉 下压", "Right brow down"),
    "MouthSmileLeft": ("左嘴角 微笑", "Left mouth corner smile"),
    "MouthSmileRight": ("右嘴角 微笑", "Right mouth corner smile"),
    "MouthSadLeft": ("左嘴角 下垂", "Left mouth corner sad"),
    "MouthSadRight": ("右嘴角 下垂", "Right mouth corner sad"),
    # ---- v2 派生：眼追 ----
    "EyeX": ("双眼 水平注视", "Both eyes horizontal gaze"),
    "EyeY": ("双眼 垂直注视", "Both eyes vertical gaze"),
    "EyeLeftX": ("左眼 水平注视", "Left eye horizontal gaze"),
    "EyeLeftY": ("左眼 垂直注视", "Left eye vertical gaze"),
    "EyeRightX": ("右眼 水平注视", "Right eye horizontal gaze"),
    "EyeRightY": ("右眼 垂直注视", "Right eye vertical gaze"),
    # ---- v2 派生：瞳孔 ----
    "PupilDilation": ("瞳孔 放大（毫米）", "Pupil dilation (mm)"),
    "PupilDiameter": ("双眼 瞳孔直径", "Both pupils diameter"),
    "PupilDiameterLeft": ("左眼 瞳孔直径", "Left pupil diameter"),
    "PupilDiameterRight": ("右眼 瞳孔直径", "Right pupil diameter"),
    # ---- v2 派生：睁闭 / 眼睑 ----
    "EyeOpenLeft": ("左眼 睁开度", "Left eye openness"),
    "EyeOpenRight": ("右眼 睁开度", "Right eye openness"),
    "EyeOpen": ("双眼 睁开度", "Both eyes openness"),
    "EyeClosedLeft": ("左眼 闭合度", "Left eye closed"),
    "EyeClosedRight": ("右眼 闭合度", "Right eye closed"),
    "EyeClosed": ("双眼 闭合度", "Both eyes closed"),
    "EyeLidLeft": ("左眼 眼睑（含睁大）", "Left eyelid (with wide)"),
    "EyeLidRight": ("右眼 眼睑（含睁大）", "Right eyelid (with wide)"),
    "EyeLid": ("双眼 眼睑（含睁大）", "Both eyelids (with wide)"),
    "EyeWide": ("双眼 睁大", "Both eyes wide"),
    "EyeSquint": ("双眼 眯眼", "Both eyes squint"),
    "EyesSquint": ("双眼 眯眼（旧名）", "Both eyes squint (legacy alias)"),
    # ---- v2 派生：眉毛 ----
    "BrowUp": ("双眉 上抬", "Both brows up"),
    "BrowDown": ("双眉 下压", "Both brows down"),
    "BrowInnerUp": ("双眉 内侧上抬", "Both brows inner up"),
    "BrowOuterUp": ("双眉 外侧上抬", "Both brows outer up"),
    "BrowExpression": ("双眉 表情强弱", "Both brows expression"),
    "BrowExpressionLeft": ("左眉 表情强弱", "Left brow expression"),
    "BrowExpressionRight": ("右眉 表情强弱", "Right brow expression"),
    # ---- v2 派生：下颌 / 脸颊 ----
    "JawX": ("下颌 左右", "Jaw left and right"),
    "JawZ": ("下颌 前后", "Jaw forward and back"),
    "CheekSquint": ("双颊 眯起", "Both cheeks squint"),
    "CheekSuck": ("双颊 内吸", "Both cheeks suck"),
    "CheekPuffSuck": ("双颊 鼓与吸", "Both cheeks puff and suck"),
    "CheekPuffSuckLeft": ("左颊 鼓与吸", "Left cheek puff and suck"),
    "CheekPuffSuckRight": ("右颊 鼓与吸", "Right cheek puff and suck"),
    # ---- v2 派生：嘴唇 ----
    "MouthX": ("嘴唇 左右", "Mouth left and right"),
    "MouthUpperX": ("上唇 左右", "Upper lip left and right"),
    "MouthLowerX": ("下唇 左右", "Lower lip left and right"),
    "LipSuck": ("双唇 内吸", "Both lips suck"),
    "LipSuckUpper": ("上唇 内吸", "Upper lip suck"),
    "LipSuckLower": ("下唇 内吸", "Lower lip suck"),
    "LipFunnel": ("双唇 漏斗", "Both lips funnel"),
    "LipFunnelUpper": ("上唇 漏斗", "Upper lip funnel"),
    "LipFunnelLower": ("下唇 漏斗", "Lower lip funnel"),
    "LipPucker": ("双唇 撅起", "Both lips pucker"),
    "LipPuckerUpper": ("上唇 撅起", "Upper lip pucker"),
    "LipPuckerLower": ("下唇 撅起", "Lower lip pucker"),
    "LipPuckerLeft": ("左嘴角 撅起", "Left mouth corner pucker"),
    "LipPuckerRight": ("右嘴角 撅起", "Right mouth corner pucker"),
    "LipSuckFunnelUpper": ("上唇 吸与漏斗", "Upper lip suck vs funnel"),
    "LipSuckFunnelLower": ("下唇 吸与漏斗", "Lower lip suck vs funnel"),
    "LipSuckFunnelUpperLeft": ("左上唇 吸与漏斗", "Upper left lip suck vs funnel"),
    "LipSuckFunnelUpperRight": ("右上唇 吸与漏斗", "Upper right lip suck vs funnel"),
    "LipSuckFunnelLowerLeft": ("左下唇 吸与漏斗", "Lower left lip suck vs funnel"),
    "LipSuckFunnelLowerRight": ("右下唇 吸与漏斗", "Lower right lip suck vs funnel"),
    "MouthUpperUp": ("上唇 上抬", "Upper lip up"),
    "MouthLowerDown": ("下唇 下拉", "Lower lip down"),
    "MouthOpen": ("嘴唇 张开", "Mouth open"),
    "MouthStretch": ("嘴唇 横拉", "Mouth stretch"),
    "MouthTightener": ("嘴唇 收紧", "Mouth tightener"),
    "MouthPress": ("嘴唇 压紧", "Mouth press"),
    "MouthDimple": ("嘴角 酒窝", "Mouth dimple"),
    "NoseSneer": ("鼻子 皱起", "Nose sneer"),
    "MouthTightenerStretch": ("嘴唇 收紧与横拉", "Mouth tightener vs stretch"),
    "MouthTightenerStretchLeft": ("左嘴角 收紧与横拉", "Left mouth tightener vs stretch"),
    "MouthTightenerStretchRight": ("右嘴角 收紧与横拉", "Right mouth tightener vs stretch"),
    "MouthCornerY": ("双嘴角 上下", "Both mouth corners up and down"),
    "MouthCornerYLeft": ("左嘴角 上下", "Left mouth corner up and down"),
    "MouthCornerYRight": ("右嘴角 上下", "Right mouth corner up and down"),
    "SmileFrown": ("双嘴角 笑与皱眉", "Both corners smile vs frown"),
    "SmileFrownLeft": ("左嘴角 笑与皱眉", "Left corner smile vs frown"),
    "SmileFrownRight": ("右嘴角 笑与皱眉", "Right corner smile vs frown"),
    "SmileSad": ("双嘴角 笑与难过", "Both corners smile vs sad"),
    "SmileSadLeft": ("左嘴角 笑与难过", "Left corner smile vs sad"),
    "SmileSadRight": ("右嘴角 笑与难过", "Right corner smile vs sad"),
    # ---- v2 派生：舌头 ----
    "TongueX": ("舌头 左右", "Tongue left and right"),
    "TongueY": ("舌头 上下", "Tongue up and down"),
    "TongueArchY": ("舌头 卷曲", "Tongue arch"),
    "TongueShape": ("舌头 平与窄", "Tongue flat vs squish"),
    # ---- v1 legacy：眼追 ----
    "EyesX": ("双眼 水平注视（旧版）", "Both eyes horizontal gaze (legacy)"),
    "EyesY": ("双眼 垂直注视（旧版）", "Both eyes vertical gaze (legacy)"),
    "LeftEyeX": ("左眼 水平注视（旧版）", "Left eye horizontal gaze (legacy)"),
    "LeftEyeY": ("左眼 垂直注视（旧版）", "Left eye vertical gaze (legacy)"),
    "RightEyeX": ("右眼 水平注视（旧版）", "Right eye horizontal gaze (legacy)"),
    "RightEyeY": ("右眼 垂直注视（旧版）", "Right eye vertical gaze (legacy)"),
    "LeftEyeWiden": ("左眼 睁大（旧版）", "Left eye widen (legacy)"),
    "RightEyeWiden": ("右眼 睁大（旧版）", "Right eye widen (legacy)"),
    "EyeWiden": ("双眼 睁大（旧版）", "Both eyes widen (legacy)"),
    "LeftEyeSqueeze": ("左眼 眯紧", "Left eye squeeze"),
    "RightEyeSqueeze": ("右眼 眯紧", "Right eye squeeze"),
    "EyesSqueeze": ("双眼 眯紧", "Both eyes squeeze"),
    "EyesDilation": ("瞳孔 放大（旧版）", "Pupil dilation (legacy)"),
    "EyesPupilDiameter": ("双眼 瞳孔直径（旧版）", "Both pupils diameter (legacy)"),
    "LeftEyeLid": ("左眼 睑开度", "Left eyelid openness"),
    "RightEyeLid": ("右眼 睑开度", "Right eyelid openness"),
    "CombinedEyeLid": ("双眼 睑开度", "Both eyelids openness"),
    "LeftEyeLidExpanded": ("左眼 睑开度（含睁大）", "Left eyelid openness (with wide)"),
    "RightEyeLidExpanded": ("右眼 睑开度（含睁大）", "Right eyelid openness (with wide)"),
    "EyeLidExpanded": ("双眼 睑开度（含睁大）", "Both eyelids openness (with wide)"),
    "LeftEyeLidExpandedSqueeze": ("左眼 睑开度（含眯紧）", "Left eyelid (with squeeze)"),
    "RightEyeLidExpandedSqueeze": ("右眼 睑开度（含眯紧）", "Right eyelid (with squeeze)"),
    "EyeLidExpandedSqueeze": ("双眼 睑开度（含眯紧）", "Both eyelids (with squeeze)"),
    "LeftEyeWidenToggle": ("左眼 睁大开关", "Left eye widen toggle"),
    "RightEyeWidenToggle": ("右眼 睁大开关", "Right eye widen toggle"),
    "EyesWidenToggle": ("双眼 睁大开关", "Both eyes widen toggle"),
    "LeftEyeSqueezeToggle": ("左眼 眯紧开关", "Left eye squeeze toggle"),
    "RightEyeSqueezeToggle": ("右眼 眯紧开关", "Right eye squeeze toggle"),
    "EyesSqueezeToggle": ("双眼 眯紧开关", "Both eyes squeeze toggle"),
    # ---- v1 legacy：Quest Pro 眉毛/脸颊/嘴（旧聚合名）----
    "BrowsInnerUp": ("双眉 内侧上抬（旧版）", "Both brows inner up (legacy)"),
    "BrowsOuterUp": ("双眉 外侧上抬（旧版）", "Both brows outer up (legacy)"),
    "BrowsDown": ("双眉 下压（旧版）", "Both brows down (legacy)"),
    "CheeksSquint": ("双颊 眯起（旧版）", "Both cheeks squint (legacy)"),
    "MouthRaiserUpper": ("上唇 抬起", "Upper lip raiser"),
    "MouthRaiserLower": ("下唇 抬起", "Lower lip raiser"),
    # ---- v1 legacy：SRanipal 专属名 ----
    "MouthApeShape": ("嘴 猿形", "Mouth ape shape"),
    "MouthPout": ("嘴 撅起", "Mouth pout"),
    "MouthUpperOverturn": ("上唇 外翻", "Upper lip overturn"),
    "MouthLowerOverturn": ("下唇 外翻", "Lower lip overturn"),
    "MouthUpperInside": ("上唇 内侧", "Upper lip inside"),
    "MouthLowerInside": ("下唇 内侧", "Lower lip inside"),
    "MouthLowerOverlay": ("下唇 覆盖", "Lower lip overlay"),
    "TongueLongStep1": ("舌头 伸出（第一段）", "Tongue out (step 1)"),
    "TongueLongStep2": ("舌头 伸出（第二段）", "Tongue out (step 2)"),
    "TongueUpRightMorph": ("舌尖 右上", "Tongue tip up right"),
    "TongueUpLeftMorph": ("舌尖 左上", "Tongue tip up left"),
    "TongueDownRightMorph": ("舌尖 右下", "Tongue tip down right"),
    "TongueDownLeftMorph": ("舌尖 左下", "Tongue tip down left"),
    "TongueSteps": ("舌头 伸出级数", "Tongue out steps"),
    # ---- v1 legacy：合并规则里的"只有部位"的名字，只能显式写 ----
    "MouthUpper": ("上唇", "Upper lip"),
    "MouthLower": ("下唇", "Lower lip"),
    "PuffSuck": ("双颊 鼓与吸（旧版）", "Both cheeks puff and suck (legacy)"),
    "PuffSuckLeft": ("左颊 鼓与吸（旧版）", "Left cheek puff and suck (legacy)"),
    "PuffSuckRight": ("右颊 鼓与吸（旧版）", "Right cheek puff and suck (legacy)"),
    "JawOpenForward": ("下颌 前伸张开", "Jaw open and forward"),
    "MouthUpperInsideOverturn": ("上唇内侧 外翻", "Upper lip inside overturn"),
    "MouthLowerInsideOverturn": ("下唇内侧 外翻", "Lower lip inside overturn"),
}

# ---------------------------------------------------------------------------
# 组合名：v1 的 91 条合并规则（LipShapeMerger.MergedShapes）。
# 名字是 `<部位><修饰>` 的机械拼接，例如 MouthUpperUpRightPout。
# 先按最长前缀认出部位，剩下的尾巴查修饰表。
# ---------------------------------------------------------------------------
BASES = [
    ("MouthUpperUpRight", ("右上唇", "upper right lip")),
    ("MouthUpperUpLeft", ("左上唇", "upper left lip")),
    ("MouthLowerDownRight", ("右下唇", "lower right lip")),
    ("MouthLowerDownLeft", ("左下唇", "lower left lip")),
    ("MouthUpperUp", ("上唇", "upper lip")),
    ("MouthLowerDown", ("下唇", "lower lip")),
    ("MouthUpper", ("上唇", "upper lip")),
    ("MouthLower", ("下唇", "lower lip")),
    ("SmileRight", ("右嘴角", "right mouth corner")),
    ("SmileLeft", ("左嘴角", "left mouth corner")),
    ("Smile", ("嘴角", "mouth corner")),
    ("PuffRight", ("右颊", "right cheek")),
    ("PuffLeft", ("左颊", "left cheek")),
    ("Puff", ("脸颊", "cheek")),
    ("JawOpenForward", ("下颌", "jaw")),
    ("JawOpen", ("下颌", "jaw")),
    ("MouthUpperInsideOverturn", ("上唇内侧", "upper lip inside")),
    ("MouthLowerInsideOverturn", ("下唇内侧", "lower lip inside")),
]

MODS = [
    ("RightUpperOverturn", ("右上外翻", "upper right overturn")),
    ("LeftUpperOverturn", ("左上外翻", "upper left overturn")),
    ("RightLowerOverturn", ("右下外翻", "lower right overturn")),
    ("LeftLowerOverturn", ("左下外翻", "lower left overturn")),
    ("RightUpperInside", ("右上内侧", "upper right inside")),
    ("RightLowerInside", ("右下内侧", "lower right inside")),
    ("LeftUpperInside", ("左上内侧", "upper left inside")),
    ("LeftLowerInside", ("左下内侧", "lower left inside")),
    ("UpperOverturn", ("上唇外翻", "upper overturn")),
    ("LowerOverturn", ("下唇外翻", "lower overturn")),
    ("UpperInside", ("上缘内侧", "upper inner edge")),
    ("LowerInside", ("下缘内侧", "lower inner edge")),
    ("Overturn", ("外翻", "overturn")),
    ("Inside", ("内侧", "inside")),
    ("Overlay", ("覆盖", "overlay")),
    ("PuffRight", ("右颊鼓起", "right cheek puff")),
    ("PuffLeft", ("左颊鼓起", "left cheek puff")),
    ("Puff", ("鼓起", "puff")),
    ("Ape", ("猿形", "ape")),
    ("Pout", ("撅起", "pout")),
    ("Suck", ("内吸", "suck")),
    ("Forward", ("前伸", "forward")),
    ("Steps", ("级数", "steps")),
]


def compose(name):
    for base, (zh_base, en_base) in BASES:
        if not name.startswith(base):
            continue
        tail = name[len(base):]
        if tail == "":
            return None  # 纯部位名（如 JawX）必须显式写，别拼
        for mod, (zh_mod, en_mod) in MODS:
            if tail == mod:
                return f"{zh_base} {zh_mod}", f"{en_base} {en_mod}"
        return None
    return None


def catalog_names():
    """按 catalog 的展开顺序返回 (逻辑名, 去掉 v2/ 前缀的短名)。"""
    shape_hpp = (ROOT / "src/core/model/shape.hpp").read_text(encoding="utf-8-sig")
    block = shape_hpp.split("#define FGB_SHAPE_LIST(X)")[1].split("enum class Shape")[0]
    base = re.findall(r"X\((\w+)\)", block)

    v2 = (ROOT / "src/params/v2_params.cpp").read_text(encoding="utf-8-sig")
    composite = re.findall(r'P\("(\w+)"', v2)

    v1 = (ROOT / "src/params/v1_params.cpp").read_text(encoding="utf-8-sig")
    eye = re.findall(r'[PB]\("(\w+)"', v1.split("kLegacyEye[] = {")[1].split("\n};")[0])
    sranipal = re.findall(r'"(\w+)"', v1.split("kSRanipalNames[kSRanipalCount] = {")[1].split("};")[0])
    rules = re.findall(r'\{"(\w+)"', (ROOT / "src/params/v1_lip_tables.inc").read_text(encoding="utf-8-sig"))

    names = []
    for name in base + composite + sranipal + rules + eye:
        if name not in names:
            names.append(name)
    return names, set(base)


def main():
    check_only = "--check" in sys.argv
    shapes = json.loads((UI / "shapes.json").read_text(encoding="utf-8"))["shapes"]
    names, base = catalog_names()

    table = {}
    missing = []
    for name in names:
        if name in base:
            continue  # 基础形状在 shapes.json 里，参数页会回落过去
        if name in EXPLICIT:
            zh, en = EXPLICIT[name]
            table[name] = {"zh": zh, "en": en}
            continue
        if name in shapes:
            continue
        composed = compose(name)
        if composed:
            zh, en = composed
            table[name] = {"zh": zh, "en": en}
            continue
        missing.append(name)

    if missing:
        print("缺少说明的参数（请补 EXPLICIT 或 BASES/MODS）：", file=sys.stderr)
        for name in missing:
            print("  " + name, file=sys.stderr)
        return 1

    covered = len(base) + len(table)
    if covered < len(names):
        print(f"覆盖不全：{covered}/{len(names)}", file=sys.stderr)
        return 1
    print(f"参数 {len(names)} 个：基础形状 {len(base)}（沿用 shapes.json）+ 本表 {len(table)}")
    if check_only:
        return 0

    payload = {
        "version": 1,
        "note": (
            "参数页卡片的一句话说明。键是逻辑参数名（不带 v2/ 前缀），由 "
            "tools/gen_param_desc.py 生成，不要手改。基础 88 个形状不在这里——"
            "它们的名字与说明在 shapes.json（校准页共用），参数页查不到时会回落过去。"
        ),
        "params": table,
    }
    (UI / "param_desc.json").write_text(
        json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print("已写入 ui/param_desc.json")
    return 0


if __name__ == "__main__":
    sys.exit(main())
