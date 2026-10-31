# 把 dist/sdk 打成模块开发者下载的 SDK 压缩包。
#
# 压缩包根目录是 FaceGazeBridge-SDK-<version>/，里面就是 dist/sdk 的内容：
# ABI 头、docs/、samples/、CMakeLists.txt（fgb::sdk）。解压后可直接
# add_subdirectory，或作为 FetchContent 的源码目录。
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File packaging\pack_sdk.ps1 -Version 1.0.3
param(
    [string]$Version = "",
    [string]$DistDir = "dist",
    [string]$OutputDir = "dist-installer"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if (-not $Version) {
    $text = Get-Content (Join-Path $root "CMakeLists.txt") -Raw
    if ($text -match 'project\(FaceGazeBridge\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
        $Version = $Matches[1]
    } else {
        throw "无法从 CMakeLists.txt 读取版本"
    }
}
if (-not [System.IO.Path]::IsPathRooted($DistDir)) { $DistDir = Join-Path $root $DistDir }
if (-not [System.IO.Path]::IsPathRooted($OutputDir)) { $OutputDir = Join-Path $root $OutputDir }

$sdkDir = Join-Path $DistDir "sdk"
if (-not (Test-Path $sdkDir)) {
    throw "找不到 SDK 目录：$sdkDir；先运行 cmake --install"
}

$stageName = "FaceGazeBridge-SDK-$Version"
$stage = Join-Path $OutputDir $stageName
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Copy-Item -Path (Join-Path $sdkDir "*") -Destination $stage -Recurse -Force

# 发布前确认关键文件都进了包，避免 install 规则被改坏却只发一个空目录。
foreach ($file in @("fgb_module_api.h", "README.md", "CMakeLists.txt", "docs/native-module-guide.md")) {
    if (-not (Test-Path (Join-Path $stage $file))) {
        throw "SDK 压缩包缺少 $file（检查 cmake --install 的 sdk/ 输出）"
    }
}

$zip = Join-Path $OutputDir "$stageName.zip"
Compress-Archive -Path $stage -DestinationPath $zip -Force
Remove-Item -Recurse -Force $stage

Write-Host "SDK：$zip（$([math]::Round((Get-Item $zip).Length / 1KB, 1)) KB）"
