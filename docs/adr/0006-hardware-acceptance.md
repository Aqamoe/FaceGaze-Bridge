# ADR-0006 · 首轮真机验收记录

- 状态：通过
- 日期：2026-10-29 01:51–01:52
- 参与：PICO 头显（PICO Connect 串流，已戴）+ VRChat + FaceGaze Bridge

## 目的

M2（模块兼容）与 M3（参数层）此前都只验证到"数据到了"或"包发出去了"，
从未验证过"人脸上的表情真的动起来"。这次补上这一环。

## 拓扑

```
PICO 头显 ──串流──> PICO Connect ──UDP 29765──> VRCFTPicoModule（.NET）
                                                      │  VRCFT IPC（AF EC 00 8D）
                                                      ▼
                                          fgb-cli（C++，扮演 VRCFT Core）
                                                      │  OSC
                                                      ▼
                                            VRChat :9000
```

VRCFT 本体全程关闭，由 `fgb-cli run --source vrcft` 直接驱动官方 `ModuleProcess.exe`。

## 结果

- 握手 → GetSupported → Init → Status(Active) → 100 Hz 拉取，全程零告警
- 约 115 fps 稳定拉流，`late=0`
- 头像解析：`Avatar-B`，546 个声明参数中命中 52 个，30 个走二进制编码，156 条 OSC 目标
- **用户目视确认：眨眼、张嘴、笑、抬眉均随动**

## 过程中暴露的问题

第一次跑绑错了头像（`Avatar-A` 而非 `Avatar-B`）。原因是当时用"最近修改的
配置文件"判断当前头像，而**加载头像不更新 mtime**。改读 VRChat 日志的
`Saving Avatar Data:` 后修正，详见 ADR-0004 的"两次踩坑后定稿"。

## 结论与遗留

- M2 + M3 在真实硬件上闭环，可以开始做 M3 剩余的校准 / 滤波器 / UI
- 情绪幅度、延迟手感这类主观项**没有量化**。下一步应引入录制回放（M4）来做可对比的回归
- 本次只验证了 v2 参数路径；v1 legacy 与 `/tracking/eye/*` 原生通道未在真机验证
