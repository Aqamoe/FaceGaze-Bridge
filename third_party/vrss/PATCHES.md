# VRSS provider 上游与补丁

- 上游：VRSS-Tuner（MIT）
- 固定提交：`487c1c6054e764e5c6ce2a2a75724195223d1bc2`（2026-10-27）
- 复制日期：2026-11-01
- 复制范围：`common-vrss-provider/` 的 provider 源码与头文件、
  `vrss-tuner/BaballoniaRelay.h`、`third_party/nvapi`、`third_party/openvr`

## 本地新增文件

- `provider/profile_area.h`：只写 NVIDIA VRChat profile 的 VRSS area 隐藏设置
  `0x00524254`（1–100%），用于 FGB 的"抗锯齿范围"滑块。只修改 area，
  不伪造 `allow`/`mode`，不碰其它 profile 值；写入后需要重启游戏。

## 本地补丁（升级上游时必须重放）

1. `provider.cpp`：`#include "../vrss-tuner/BaballoniaRelay.h"` → `"BaballoniaRelay.h"`。
2. `OpenVrFrameSource.h` / `export_openvr_geometry.cpp`：OpenVR 与
   `hmd_candidate_identity.h` 改为同目录相对路径。
3. `--lease <file>`：主循环每 250 ms 检查文件 mtime；超过 15 s 退出，
   让 FGB 崩溃时不留下一个 24 小时的提权进程。
4. `--csv 0`：默认不写 `gaze.csv`（90 Hz × 24 h 会把磁盘写满）。
5. `--experimental-contract <path>`：参数优先于环境变量。提权进程的环境变量
   继承不可靠，不能把启动成败押在它上面。
6. `--driver-audit 1 --contract-out <path>`：审计 PASS 后直接写 7 字段合同，
   并额外输出 `driverVersion=`。审计仍只读 vtable 地址，不调用私有方法。
7. `--set-vrss-area <percent>` / `--get-vrss-area 1`：显式读写 NVIDIA VRChat
   profile 的 VRSS area；`--set-vrss-area` 需要管理员，结果写 `--output/vrss-area.txt`。
   正常启动时可用 `--vrss-area <percent>` 在同一次 UAC 里应用范围。
8. `catch` 写 `run/error.txt`，首行是机器可读原因码
   （`E_DRIVER` / `E_ADMIN` / `E_RECONNECT` / `E_PORT` / `E_IDENTITY` / `E_GENERIC`）。
9. `--input-route` 宽字符串逐字符收窄，消除 C4244 警告（路由名均为 ASCII）。

## 许可

- 上游代码：`LICENSE`（MIT）。
- NVAPI：`nvapi/License.txt`（NVIDIA 许可），只随包分发头文件与导入库，
  不修改、不替换 NVIDIA 运行时二进制。
- OpenVR：`openvr/LICENSE`（BSD-3-Clause）。
