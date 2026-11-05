#!/usr/bin/env python3
"""生成 ui/param_desc.json —— 参数页卡片的人话说明（中文 / English / 日本語）。

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
# 显式条目：名字 → (中文, English, 日本語)
# 写法统一成"部位 动作"两段，和形状表里的 `左眼 眯眼` / `左目 細目` 同一口径。
# ---------------------------------------------------------------------------
EXPLICIT = {
    # ---- v2 派生：简化形状 ----
    "BrowUpLeft": ("左眉 上抬", "Left brow up", "左眉 上げ"),
    "BrowUpRight": ("右眉 上抬", "Right brow up", "右眉 上げ"),
    "BrowDownLeft": ("左眉 下压", "Left brow down", "左眉 下げ"),
    "BrowDownRight": ("右眉 下压", "Right brow down", "右眉 下げ"),
    "MouthSmileLeft": ("左嘴角 微笑", "Left mouth corner smile", "左口角 笑み"),
    "MouthSmileRight": ("右嘴角 微笑", "Right mouth corner smile", "右口角 笑み"),
    "MouthSadLeft": ("左嘴角 下垂", "Left mouth corner sad", "左口角 下げ"),
    "MouthSadRight": ("右嘴角 下垂", "Right mouth corner sad", "右口角 下げ"),
    # ---- v2 派生：眼追 ----
    "EyeX": ("双眼 水平注视", "Both eyes horizontal gaze", "両目 水平視線"),
    "EyeY": ("双眼 垂直注视", "Both eyes vertical gaze", "両目 垂直視線"),
    "EyeLeftX": ("左眼 水平注视", "Left eye horizontal gaze", "左目 水平視線"),
    "EyeLeftY": ("左眼 垂直注视", "Left eye vertical gaze", "左目 垂直視線"),
    "EyeRightX": ("右眼 水平注视", "Right eye horizontal gaze", "右目 水平視線"),
    "EyeRightY": ("右眼 垂直注视", "Right eye vertical gaze", "右目 垂直視線"),
    # ---- v2 派生：瞳孔 ----
    "PupilDilation": ("瞳孔 放大（毫米）", "Pupil dilation (mm)", "瞳孔 拡大（mm）"),
    "PupilDiameter": ("双眼 瞳孔直径", "Both pupils diameter", "両目 瞳孔径"),
    "PupilDiameterLeft": ("左眼 瞳孔直径", "Left pupil diameter", "左目 瞳孔径"),
    "PupilDiameterRight": ("右眼 瞳孔直径", "Right pupil diameter", "右目 瞳孔径"),
    # ---- v2 派生：睁闭 / 眼睑 ----
    "EyeOpenLeft": ("左眼 睁开度", "Left eye openness", "左目 開き"),
    "EyeOpenRight": ("右眼 睁开度", "Right eye openness", "右目 開き"),
    "EyeOpen": ("双眼 睁开度", "Both eyes openness", "両目 開き"),
    "EyeClosedLeft": ("左眼 闭合度", "Left eye closed", "左目 閉じ"),
    "EyeClosedRight": ("右眼 闭合度", "Right eye closed", "右目 閉じ"),
    "EyeClosed": ("双眼 闭合度", "Both eyes closed", "両目 閉じ"),
    "EyeLidLeft": ("左眼 眼睑（含睁大）", "Left eyelid (with wide)", "左目 まぶた（見開き込み）"),
    "EyeLidRight": ("右眼 眼睑（含睁大）", "Right eyelid (with wide)", "右目 まぶた（見開き込み）"),
    "EyeLid": ("双眼 眼睑（含睁大）", "Both eyelids (with wide)", "両目 まぶた（見開き込み）"),
    "EyeWide": ("双眼 睁大", "Both eyes wide", "両目 見開き"),
    "EyeSquint": ("双眼 眯眼", "Both eyes squint", "両目 細目"),
    "EyesSquint": ("双眼 眯眼（旧名）", "Both eyes squint (legacy alias)", "両目 細目（旧名）"),
    # ---- v2 派生：眉毛 ----
    "BrowUp": ("双眉 上抬", "Both brows up", "両眉 上げ"),
    "BrowDown": ("双眉 下压", "Both brows down", "両眉 下げ"),
    "BrowInnerUp": ("双眉 内侧上抬", "Both brows inner up", "両眉 内側上げ"),
    "BrowOuterUp": ("双眉 外侧上抬", "Both brows outer up", "両眉 外側上げ"),
    "BrowExpression": ("双眉 表情强弱", "Both brows expression", "両眉 表情の強さ"),
    "BrowExpressionLeft": ("左眉 表情强弱", "Left brow expression", "左眉 表情の強さ"),
    "BrowExpressionRight": ("右眉 表情强弱", "Right brow expression", "右眉 表情の強さ"),
    # ---- v2 派生：下颌 / 脸颊 ----
    "JawX": ("下颌 左右", "Jaw left and right", "顎 左右"),
    "JawZ": ("下颌 前后", "Jaw forward and back", "顎 前後"),
    "CheekSquint": ("双颊 眯起", "Both cheeks squint", "両頬 上がり"),
    "CheekSuck": ("双颊 内吸", "Both cheeks suck", "両頬 吸い込み"),
    "CheekPuffSuck": ("双颊 鼓与吸", "Both cheeks puff and suck", "両頬 膨らませ・吸い込み"),
    "CheekPuffSuckLeft": ("左颊 鼓与吸", "Left cheek puff and suck", "左頬 膨らませ・吸い込み"),
    "CheekPuffSuckRight": ("右颊 鼓与吸", "Right cheek puff and suck", "右頬 膨らませ・吸い込み"),
    # ---- v2 派生：嘴唇 ----
    "MouthX": ("嘴唇 左右", "Mouth left and right", "唇 左右"),
    "MouthUpperX": ("上唇 左右", "Upper lip left and right", "上唇 左右"),
    "MouthLowerX": ("下唇 左右", "Lower lip left and right", "下唇 左右"),
    "LipSuck": ("双唇 内吸", "Both lips suck", "両唇 吸い込み"),
    "LipSuckUpper": ("上唇 内吸", "Upper lip suck", "上唇 吸い込み"),
    "LipSuckLower": ("下唇 内吸", "Lower lip suck", "下唇 吸い込み"),
    "LipFunnel": ("双唇 漏斗", "Both lips funnel", "両唇 漏斗"),
    "LipFunnelUpper": ("上唇 漏斗", "Upper lip funnel", "上唇 漏斗"),
    "LipFunnelLower": ("下唇 漏斗", "Lower lip funnel", "下唇 漏斗"),
    "LipPucker": ("双唇 撅起", "Both lips pucker", "両唇 すぼめ"),
    "LipPuckerUpper": ("上唇 撅起", "Upper lip pucker", "上唇 すぼめ"),
    "LipPuckerLower": ("下唇 撅起", "Lower lip pucker", "下唇 すぼめ"),
    "LipPuckerLeft": ("左嘴角 撅起", "Left mouth corner pucker", "左口角 すぼめ"),
    "LipPuckerRight": ("右嘴角 撅起", "Right mouth corner pucker", "右口角 すぼめ"),
    "LipSuckFunnelUpper": ("上唇 吸与漏斗", "Upper lip suck vs funnel", "上唇 吸い込み・漏斗"),
    "LipSuckFunnelLower": ("下唇 吸与漏斗", "Lower lip suck vs funnel", "下唇 吸い込み・漏斗"),
    "LipSuckFunnelUpperLeft": (
        "左上唇 吸与漏斗", "Upper left lip suck vs funnel", "左上唇 吸い込み・漏斗"),
    "LipSuckFunnelUpperRight": (
        "右上唇 吸与漏斗", "Upper right lip suck vs funnel", "右上唇 吸い込み・漏斗"),
    "LipSuckFunnelLowerLeft": (
        "左下唇 吸与漏斗", "Lower left lip suck vs funnel", "左下唇 吸い込み・漏斗"),
    "LipSuckFunnelLowerRight": (
        "右下唇 吸与漏斗", "Lower right lip suck vs funnel", "右下唇 吸い込み・漏斗"),
    "MouthUpperUp": ("上唇 上抬", "Upper lip up", "上唇 上げ"),
    "MouthLowerDown": ("下唇 下拉", "Lower lip down", "下唇 下げ"),
    "MouthOpen": ("嘴唇 张开", "Mouth open", "唇 開き"),
    "MouthStretch": ("嘴唇 横拉", "Mouth stretch", "唇 横引き"),
    "MouthTightener": ("嘴唇 收紧", "Mouth tightener", "唇 引き締め"),
    "MouthPress": ("嘴唇 压紧", "Mouth press", "唇 押さえ"),
    "MouthDimple": ("嘴角 酒窝", "Mouth dimple", "口角 えくぼ"),
    "NoseSneer": ("鼻子 皱起", "Nose sneer", "鼻 しわ寄せ"),
    "MouthTightenerStretch": (
        "嘴唇 收紧与横拉", "Mouth tightener vs stretch", "唇 引き締め・横引き"),
    "MouthTightenerStretchLeft": (
        "左嘴角 收紧与横拉", "Left mouth tightener vs stretch", "左口角 引き締め・横引き"),
    "MouthTightenerStretchRight": (
        "右嘴角 收紧与横拉", "Right mouth tightener vs stretch", "右口角 引き締め・横引き"),
    "MouthCornerY": ("双嘴角 上下", "Both mouth corners up and down", "両口角 上下"),
    "MouthCornerYLeft": ("左嘴角 上下", "Left mouth corner up and down", "左口角 上下"),
    "MouthCornerYRight": ("右嘴角 上下", "Right mouth corner up and down", "右口角 上下"),
    "SmileFrown": ("双嘴角 笑与皱眉", "Both corners smile vs frown", "両口角 笑み・しかめ"),
    "SmileFrownLeft": ("左嘴角 笑与皱眉", "Left corner smile vs frown", "左口角 笑み・しかめ"),
    "SmileFrownRight": ("右嘴角 笑与皱眉", "Right corner smile vs frown", "右口角 笑み・しかめ"),
    "SmileSad": ("双嘴角 笑与难过", "Both corners smile vs sad", "両口角 笑み・悲しみ"),
    "SmileSadLeft": ("左嘴角 笑与难过", "Left corner smile vs sad", "左口角 笑み・悲しみ"),
    "SmileSadRight": ("右嘴角 笑与难过", "Right corner smile vs sad", "右口角 笑み・悲しみ"),
    # ---- v2 派生：舌头 ----
    "TongueX": ("舌头 左右", "Tongue left and right", "舌 左右"),
    "TongueY": ("舌头 上下", "Tongue up and down", "舌 上下"),
    "TongueArchY": ("舌头 卷曲", "Tongue arch", "舌 反り"),
    "TongueShape": ("舌头 平与窄", "Tongue flat vs squish", "舌 平ら・細め"),
    # ---- v1 legacy：眼追 ----
    "EyesX": ("双眼 水平注视（旧版）", "Both eyes horizontal gaze (legacy)", "両目 水平視線（旧）"),
    "EyesY": ("双眼 垂直注视（旧版）", "Both eyes vertical gaze (legacy)", "両目 垂直視線（旧）"),
    "LeftEyeX": ("左眼 水平注视（旧版）", "Left eye horizontal gaze (legacy)", "左目 水平視線（旧）"),
    "LeftEyeY": ("左眼 垂直注视（旧版）", "Left eye vertical gaze (legacy)", "左目 垂直視線（旧）"),
    "RightEyeX": ("右眼 水平注视（旧版）", "Right eye horizontal gaze (legacy)", "右目 水平視線（旧）"),
    "RightEyeY": ("右眼 垂直注视（旧版）", "Right eye vertical gaze (legacy)", "右目 垂直視線（旧）"),
    "LeftEyeWiden": ("左眼 睁大（旧版）", "Left eye widen (legacy)", "左目 見開き（旧）"),
    "RightEyeWiden": ("右眼 睁大（旧版）", "Right eye widen (legacy)", "右目 見開き（旧）"),
    "EyeWiden": ("双眼 睁大（旧版）", "Both eyes widen (legacy)", "両目 見開き（旧）"),
    "LeftEyeSqueeze": ("左眼 眯紧", "Left eye squeeze", "左目 細め"),
    "RightEyeSqueeze": ("右眼 眯紧", "Right eye squeeze", "右目 細め"),
    "EyesSqueeze": ("双眼 眯紧", "Both eyes squeeze", "両目 細め"),
    "EyesDilation": ("瞳孔 放大（旧版）", "Pupil dilation (legacy)", "瞳孔 拡大（旧）"),
    "EyesPupilDiameter": (
        "双眼 瞳孔直径（旧版）", "Both pupils diameter (legacy)", "両目 瞳孔径（旧）"),
    "LeftEyeLid": ("左眼 睑开度", "Left eyelid openness", "左目 まぶた開き"),
    "RightEyeLid": ("右眼 睑开度", "Right eyelid openness", "右目 まぶた開き"),
    "CombinedEyeLid": ("双眼 睑开度", "Both eyelids openness", "両目 まぶた開き"),
    "LeftEyeLidExpanded": (
        "左眼 睑开度（含睁大）", "Left eyelid openness (with wide)",
        "左目 まぶた開き（見開き込み）"),
    "RightEyeLidExpanded": (
        "右眼 睑开度（含睁大）", "Right eyelid openness (with wide)",
        "右目 まぶた開き（見開き込み）"),
    "EyeLidExpanded": (
        "双眼 睑开度（含睁大）", "Both eyelids openness (with wide)",
        "両目 まぶた開き（見開き込み）"),
    "LeftEyeLidExpandedSqueeze": (
        "左眼 睑开度（含眯紧）", "Left eyelid (with squeeze)", "左目 まぶた（細め込み）"),
    "RightEyeLidExpandedSqueeze": (
        "右眼 睑开度（含眯紧）", "Right eyelid (with squeeze)", "右目 まぶた（細め込み）"),
    "EyeLidExpandedSqueeze": (
        "双眼 睑开度（含眯紧）", "Both eyelids (with squeeze)", "両目 まぶた（細め込み）"),
    "LeftEyeWidenToggle": ("左眼 睁大开关", "Left eye widen toggle", "左目 見開きスイッチ"),
    "RightEyeWidenToggle": ("右眼 睁大开关", "Right eye widen toggle", "右目 見開きスイッチ"),
    "EyesWidenToggle": ("双眼 睁大开关", "Both eyes widen toggle", "両目 見開きスイッチ"),
    "LeftEyeSqueezeToggle": ("左眼 眯紧开关", "Left eye squeeze toggle", "左目 細めスイッチ"),
    "RightEyeSqueezeToggle": ("右眼 眯紧开关", "Right eye squeeze toggle", "右目 細めスイッチ"),
    "EyesSqueezeToggle": ("双眼 眯紧开关", "Both eyes squeeze toggle", "両目 細めスイッチ"),
    # ---- v1 legacy：Quest Pro 眉毛/脸颊/嘴（旧聚合名）----
    "BrowsInnerUp": ("双眉 内侧上抬（旧版）", "Both brows inner up (legacy)", "両眉 内側上げ（旧）"),
    "BrowsOuterUp": ("双眉 外侧上抬（旧版）", "Both brows outer up (legacy)", "両眉 外側上げ（旧）"),
    "BrowsDown": ("双眉 下压（旧版）", "Both brows down (legacy)", "両眉 下げ（旧）"),
    "CheeksSquint": ("双颊 眯起（旧版）", "Both cheeks squint (legacy)", "両頬 上がり（旧）"),
    "MouthRaiserUpper": ("上唇 抬起", "Upper lip raiser", "上唇 上げ"),
    "MouthRaiserLower": ("下唇 抬起", "Lower lip raiser", "下唇 上げ"),
    # ---- v1 legacy：SRanipal 专属名 ----
    "MouthApeShape": ("嘴 猿形", "Mouth ape shape", "口 猿口"),
    "MouthPout": ("嘴 撅起", "Mouth pout", "口 すぼめ"),
    "MouthUpperOverturn": ("上唇 外翻", "Upper lip overturn", "上唇 外反"),
    "MouthLowerOverturn": ("下唇 外翻", "Lower lip overturn", "下唇 外反"),
    "MouthUpperInside": ("上唇 内侧", "Upper lip inside", "上唇 内側"),
    "MouthLowerInside": ("下唇 内侧", "Lower lip inside", "下唇 内側"),
    "MouthLowerOverlay": ("下唇 覆盖", "Lower lip overlay", "下唇 オーバーレイ"),
    "TongueLongStep1": ("舌头 伸出（第一段）", "Tongue out (step 1)", "舌 突出（1段目）"),
    "TongueLongStep2": ("舌头 伸出（第二段）", "Tongue out (step 2)", "舌 突出（2段目）"),
    "TongueUpRightMorph": ("舌尖 右上", "Tongue tip up right", "舌尖 右上"),
    "TongueUpLeftMorph": ("舌尖 左上", "Tongue tip up left", "舌尖 左上"),
    "TongueDownRightMorph": ("舌尖 右下", "Tongue tip down right", "舌尖 右下"),
    "TongueDownLeftMorph": ("舌尖 左下", "Tongue tip down left", "舌尖 左下"),
    "TongueSteps": ("舌头 伸出级数", "Tongue out steps", "舌 突出段数"),
    # ---- v1 legacy：合并规则里的"只有部位"的名字，只能显式写 ----
    "MouthUpper": ("上唇", "Upper lip", "上唇"),
    "MouthLower": ("下唇", "Lower lip", "下唇"),
    "PuffSuck": ("双颊 鼓与吸（旧版）", "Both cheeks puff and suck (legacy)", "両頬 膨らませ・吸い込み（旧）"),
    "PuffSuckLeft": ("左颊 鼓与吸（旧版）", "Left cheek puff and suck (legacy)", "左頬 膨らませ・吸い込み（旧）"),
    "PuffSuckRight": ("右颊 鼓与吸（旧版）", "Right cheek puff and suck (legacy)", "右頬 膨らませ・吸い込み（旧）"),
    "JawOpenForward": ("下颌 前伸张开", "Jaw open and forward", "顎 前突して開く"),
    "MouthUpperInsideOverturn": ("上唇内侧 外翻", "Upper lip inside overturn", "上唇内側 外反"),
    "MouthLowerInsideOverturn": ("下唇内侧 外翻", "Lower lip inside overturn", "下唇内側 外反"),
}

# ---------------------------------------------------------------------------
# 组合名：v1 的 91 条合并规则（LipShapeMerger.MergedShapes）。
# 名字是 `<部位><修饰>` 的机械拼接，例如 MouthUpperUpRightPout。
# 先按最长前缀认出部位，剩下的尾巴查修饰表。
# ---------------------------------------------------------------------------
BASES = [
    ("MouthUpperUpRight", ("右上唇", "upper right lip", "右上唇")),
    ("MouthUpperUpLeft", ("左上唇", "upper left lip", "左上唇")),
    ("MouthLowerDownRight", ("右下唇", "lower right lip", "右下唇")),
    ("MouthLowerDownLeft", ("左下唇", "lower left lip", "左下唇")),
    ("MouthUpperUp", ("上唇", "upper lip", "上唇")),
    ("MouthLowerDown", ("下唇", "lower lip", "下唇")),
    ("MouthUpper", ("上唇", "upper lip", "上唇")),
    ("MouthLower", ("下唇", "lower lip", "下唇")),
    ("SmileRight", ("右嘴角", "right mouth corner", "右口角")),
    ("SmileLeft", ("左嘴角", "left mouth corner", "左口角")),
    ("Smile", ("嘴角", "mouth corner", "口角")),
    ("PuffRight", ("右颊", "right cheek", "右頬")),
    ("PuffLeft", ("左颊", "left cheek", "左頬")),
    ("Puff", ("脸颊", "cheek", "頬")),
    ("JawOpenForward", ("下颌", "jaw", "顎")),
    ("JawOpen", ("下颌", "jaw", "顎")),
    ("MouthUpperInsideOverturn", ("上唇内侧", "upper lip inside", "上唇内側")),
    ("MouthLowerInsideOverturn", ("下唇内侧", "lower lip inside", "下唇内側")),
]

MODS = [
    ("RightUpperOverturn", ("右上外翻", "upper right overturn", "右上外反")),
    ("LeftUpperOverturn", ("左上外翻", "upper left overturn", "左上外反")),
    ("RightLowerOverturn", ("右下外翻", "lower right overturn", "右下外反")),
    ("LeftLowerOverturn", ("左下外翻", "lower left overturn", "左下外反")),
    ("RightUpperInside", ("右上内侧", "upper right inside", "右上内側")),
    ("RightLowerInside", ("右下内侧", "lower right inside", "右下内側")),
    ("LeftUpperInside", ("左上内侧", "upper left inside", "左上内側")),
    ("LeftLowerInside", ("左下内侧", "lower left inside", "左下内側")),
    ("UpperOverturn", ("上唇外翻", "upper overturn", "上唇外反")),
    ("LowerOverturn", ("下唇外翻", "lower overturn", "下唇外反")),
    ("UpperInside", ("上缘内侧", "upper inner edge", "上縁内側")),
    ("LowerInside", ("下缘内侧", "lower inner edge", "下縁内側")),
    ("Overturn", ("外翻", "overturn", "外反")),
    ("Inside", ("内侧", "inside", "内側")),
    ("Overlay", ("覆盖", "overlay", "オーバーレイ")),
    ("PuffRight", ("右颊鼓起", "right cheek puff", "右頬 膨らませ")),
    ("PuffLeft", ("左颊鼓起", "left cheek puff", "左頬 膨らませ")),
    ("Puff", ("鼓起", "puff", "膨らませ")),
    ("Ape", ("猿形", "ape", "猿口")),
    ("Pout", ("撅起", "pout", "すぼめ")),
    ("Suck", ("内吸", "suck", "吸い込み")),
    ("Forward", ("前伸", "forward", "前突")),
    ("Steps", ("级数", "steps", "段数")),
]


def compose(name):
    for base, (zh_base, en_base, ja_base) in BASES:
        if not name.startswith(base):
            continue
        tail = name[len(base):]
        if tail == "":
            return None  # 纯部位名（如 JawX）必须显式写，别拼
        for mod, (zh_mod, en_mod, ja_mod) in MODS:
            if tail == mod:
                return f"{zh_base} {zh_mod}", f"{en_base} {en_mod}", f"{ja_base} {ja_mod}"
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
            zh, en, ja = EXPLICIT[name]
        elif name in shapes:
            continue
        else:
            composed = compose(name)
            if not composed:
                missing.append(name)
                continue
            zh, en, ja = composed
        table[name] = {"zh": zh, "en": en, "ja": ja}

    if missing:
        print("缺少说明的参数（请补 EXPLICIT 或 BASES/MODS）：", file=sys.stderr)
        for name in missing:
            print("  " + name, file=sys.stderr)
        return 1

    # 基础形状的日语在 shapes.json 里；两边都得齐，否则界面上会出现"中文混日文"
    shapes_ja = [name for name in names if name in base and not shapes.get(name, {}).get("ja")]
    if shapes_ja:
        print("ui/shapes.json 缺日语名称：", file=sys.stderr)
        for name in shapes_ja:
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
            "参数页卡片的一句话说明（zh / en / ja）。键是逻辑参数名（不带 v2/ 前缀），"
            "由 tools/gen_param_desc.py 生成，不要手改。基础 88 个形状不在这里——"
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
