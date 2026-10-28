@echo off
chcp 65001 >nul
cd /d "%~dp0"

echo ============================================================
echo   FaceGaze Bridge - VMC 输入（没有 VRCFT 模块时用这个）
echo ------------------------------------------------------------
echo   监听 UDP 39539，接收 VMC 协议（VSeeFace / VRChat OSC Trackers 等）
echo   输出到 VRChat OSC 127.0.0.1:9000
echo   这个窗口是引擎日志，关掉它 = 停止转发
echo ============================================================
echo.

"%~dp0fgb-cli.exe" run --source vmc --vmc-port 39539 --ui %*

if errorlevel 1 (
    echo.
    echo 引擎已退出（错误码 %errorlevel%）。
    pause
)
