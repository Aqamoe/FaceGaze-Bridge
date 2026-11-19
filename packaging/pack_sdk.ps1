# 把 dist/sdk 打成模块开发者下载的 SDK 压缩包。
#
# 压缩包根目录是 FaceGazeBridge-SDK-<SDK 版本>/，里面就是 dist/sdk 的内容：
# ABI 头、docs/、samples/、CMakeLists.txt（fgb::sdk）。解压后可直接
# add_subdirectory，或作为 FetchContent 的源码目录。
#
# SDK 版本独立于软件版本（ADR-0048）：取自 sdk/fgb_sdk_version.h，
# 不跟随 CMakeLists.txt 的产品版本，软件发版不会让 SDK 包名变化。
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File packaging\pack_sdk.ps1
#   powershell -ExecutionPolicy Bypass -File packaging\pack_sdk.ps1 -SdkVersion 1.2.0
param(
    [string]$SdkVersion = "",
    [string]$DistDir = "dist",
    [string]$OutputDir = "dist-installer"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

# 从 SDK 头文件读版本；优先 dist/sdk（安装产物，和包内容一致），
# 还没 install 时回落到仓库里的 sdk/。
function Read-SdkVersion([string]$Directory) {
    $header = Join-Path $Directory "fgb_sdk_version.h"
    if (-not (Test-Path $header)) { return $null }
    $text = Get-Content $header -Raw
    $line = [regex]::Match($text, '#define\s+FGB_SDK_VERSION\s+"([0-9]+\.[0-9]+\.[0-9]+)"')
    if (-not $line.Success) { return $null }
    # MAJOR/MINOR/PATCH 与字符串不一致时，模块里两种写法会互相矛盾。
    $parts = foreach ($part in @("MAJOR", "MINOR", "PATCH")) {
        $m = [regex]::Match($text, "#define\s+FGB_SDK_VERSION_$part\s+([0-9]+)")
        if (-not $m.Success) { throw "$header 缺少 FGB_SDK_VERSION_$part" }
        $m.Groups[1].Value
    }
    $joined = $parts -join "."
    if ($joined -ne $line.Groups[1].Value) {
        throw "$header 的 MAJOR/MINOR/PATCH（$joined）与字符串 $($line.Groups[1].Value) 不一致"
    }
    return $line.Groups[1].Value
}

if (-not $SdkVersion) {
    $SdkVersion = Read-SdkVersion (Join-Path $root "dist/sdk")
    if (-not $SdkVersion) {
        $SdkVersion = Read-SdkVersion (Join-Path $root "sdk")
    }
    if (-not $SdkVersion) {
        throw "无法从 sdk/fgb_sdk_version.h 读取 SDK 版本"
    }
}
if (-not [System.IO.Path]::IsPathRooted($DistDir)) { $DistDir = Join-Path $root $DistDir }
if (-not [System.IO.Path]::IsPathRooted($OutputDir)) { $OutputDir = Join-Path $root $OutputDir }

$sdkDir = Join-Path $DistDir "sdk"
if (-not (Test-Path $sdkDir)) {
    throw "找不到 SDK 目录：$sdkDir；先运行 cmake --install"
}

# 显式传入的版本必须与包内头文件一致，否则压缩包名会和内容对不上。
$headerVersion = Read-SdkVersion $sdkDir
if ($headerVersion -and $headerVersion -ne $SdkVersion) {
    throw "参数 -SdkVersion $SdkVersion 与包内 fgb_sdk_version.h 的 $headerVersion 不一致"
}

$stageName = "FaceGazeBridge-SDK-$SdkVersion"
$stage = Join-Path $OutputDir $stageName
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Copy-Item -Path (Join-Path $sdkDir "*") -Destination $stage -Recurse -Force

# 发布前确认关键文件都进了包，避免 install 规则被改坏却只发一个空目录。
foreach ($file in @("fgb_module_api.h", "fgb_sdk_version.h", "README.md", "CMakeLists.txt", "docs/native-module-guide.md")) {
    if (-not (Test-Path (Join-Path $stage $file))) {
        throw "SDK 压缩包缺少 $file（检查 cmake --install 的 sdk/ 输出）"
    }
}

$zip = Join-Path $OutputDir "$stageName.zip"
Compress-Archive -Path $stage -DestinationPath $zip -Force
Remove-Item -Recurse -Force $stage

Write-Host "SDK：$zip（SDK 版本 $SdkVersion，$([math]::Round((Get-Item $zip).Length / 1KB, 1)) KB）"
