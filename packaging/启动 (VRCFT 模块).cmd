@echo off
chcp 65001 >nul
cd /d "%~dp0"

echo ============================================================
echo   FaceGaze Bridge - 接管 VRCFT 模块
echo ------------------------------------------------------------
echo   1. 需要先完全关闭 VRCFT 本体（模块的沙箱端口是独占的）
echo   2. 需要 VRChat 已在运行，且 OSC 已开启（默认 127.0.0.1:9000）
echo   3. 这个窗口是引擎日志，关掉它 = 停止追踪
echo ============================================================
echo.

"%~dp0fgb-cli.exe" run --source vrcft --ui %*

if errorlevel 1 (
    echo.
    echo 引擎已退出（错误码 %errorlevel%）。常见原因看 docs 里的排障，
    echo 或先把界面上的"模块"页打开看它报了什么。
    pause
)
