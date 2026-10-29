# 构建 FaceGaze Bridge 安装包（需要 Inno Setup 6 的 ISCC.exe）。
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File packaging\build_installer.ps1
#   powershell -ExecutionPolicy Bypass -File packaging\build_installer.ps1 `
#       -DistDir D:\temp\dist -OutputDir D:\temp\installer
#
# 默认读取仓库根目录的 release 安装产物 dist/。dist 里必须有
# runtime/vrcft（由 tools/build_vrcft_runtime.py 生成），否则安装包在用户机上
# 无法加载模块——脚本会给出警告但继续，方便先验证安装器本身。
param(
    [string]$Version = "",
    [string]$DistDir = "",
    [string]$OutputDir = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $Version) {
    $text = Get-Content (Join-Path $root "CMakeLists.txt") -Raw
    if ($text -match 'project\(FaceGazeBridge\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
        $Version = $Matches[1]
    } else {
        throw "无法从 CMakeLists.txt 读取版本"
    }
}
if (-not $DistDir) {
    $DistDir = Join-Path $root "dist"
}
if (-not $OutputDir) {
    $OutputDir = Join-Path $root "dist-installer"
}

if (-not (Test-Path (Join-Path $DistDir "FaceGaze Bridge.exe"))) {
    throw "找不到 $DistDir\FaceGaze Bridge.exe；先运行 cmake --install build/release --config Release --prefix $DistDir"
}
if (-not (Test-Path (Join-Path $DistDir "runtime\vrcft"))) {
    Write-Warning "dist 里缺少 runtime\vrcft；安装包将依赖用户本机安装的 VRCFT。"
}

$iscc = @(
    (Join-Path ${env:ProgramFiles(x86)} "Inno Setup 6\ISCC.exe"),
    (Join-Path $env:LOCALAPPDATA "Programs\Inno Setup 6\ISCC.exe"),
    (Join-Path $env:ProgramFiles "Inno Setup 6\ISCC.exe")
) | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
if (-not $iscc) {
    $iscc = (Get-Command ISCC.exe -ErrorAction SilentlyContinue).Source
}
if (-not $iscc) {
    throw "找不到 ISCC.exe；安装 Inno Setup 6（winget install JRSoftware.InnoSetup）"
}

Write-Host "FaceGaze Bridge $Version"
Write-Host "  dist   : $DistDir"
Write-Host "  output : $OutputDir"
& $iscc "/DAppVersion=$Version" "/DDistDir=$DistDir" "/DOutputDir=$OutputDir" `
    (Join-Path $PSScriptRoot "facegazebridge.iss")
if ($LASTEXITCODE -ne 0) {
    throw "ISCC 失败（退出码 $LASTEXITCODE）"
}

$setup = Join-Path $OutputDir "FaceGazeBridge-Setup-$Version.exe"
if (-not (Test-Path $setup)) {
    throw "ISCC 成功但找不到 $setup"
}
Write-Host "安装包：$setup（$([math]::Round((Get-Item $setup).Length / 1MB, 1)) MB）"
