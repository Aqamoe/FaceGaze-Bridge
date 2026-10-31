# 一键发布：构建 → 单测 → dist → VRCFT 运行时 → 安装包 → SDK 包 → 校验和
# （可选 draft release）。
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File packaging\release.ps1
#   powershell -ExecutionPolicy Bypass -File packaging\release.ps1 -Publish
#   powershell -ExecutionPolicy Bypass -File packaging\release.ps1 -RuntimeZip D:\tmp\vrcft-runtime.zip
#
# 前提：VS 2022（含 C++ 工具链与 Redist）、Inno Setup 6、Python 3。
# 运行时默认从本机已安装的 VRCFT 抽取（tools/build_vrcft_runtime.py）；CI 没有
# VRCFT，用 -RuntimeZip 提供压缩包（根目录直接是 runtime 文件）。
# -Publish 需要 gh 已登录，会创建/更新 **draft** release，不会自动公开。
param(
    [string]$Version = "",
    [string]$BuildDir = "build/windows-vs2022",
    [string]$DistDir = "dist",
    [string]$OutputDir = "dist-installer",
    [string]$RuntimeSource = "",
    [string]$RuntimeZip = "",
    [string]$NotesFile = "",
    [switch]$SkipTests,
    [switch]$SkipRuntime,
    [switch]$Publish
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
$tag = "v$Version"

if ($Publish) {
    $dirty = & git status --porcelain
    if ($dirty) {
        throw "工作区不干净，先提交再 -Publish：`n$dirty"
    }
    if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
        throw "找不到 gh；安装 GitHub CLI 并 gh auth login"
    }
}

Write-Host "FaceGaze Bridge $Version"
Write-Host "  build  : $BuildDir"
Write-Host "  dist   : $DistDir"
Write-Host "  output : $OutputDir"

# 发布产物必须可复现：每次都从空的 dist 开始（runtime 会重新生成/解压）。
if (Test-Path $DistDir) { Remove-Item -Recurse -Force $DistDir }

& cmake -S . -B $BuildDir -G "Visual Studio 17 2022" -A x64 -DFGB_WARNINGS_AS_ERRORS=ON
if ($LASTEXITCODE -ne 0) { throw "CMake 配置失败" }

& cmake --build $BuildDir --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw "构建失败" }

if (-not $SkipTests) {
    & ctest --test-dir $BuildDir -C Release --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "单测失败" }
}

& cmake --install $BuildDir --config Release --prefix $DistDir
if ($LASTEXITCODE -ne 0) { throw "cmake --install 失败" }

$runtimeDest = Join-Path $DistDir "runtime/vrcft"
if ($RuntimeZip) {
    New-Item -ItemType Directory -Force -Path $runtimeDest | Out-Null
    Expand-Archive -Path $RuntimeZip -DestinationPath $runtimeDest -Force
    if (-not (Test-Path (Join-Path $runtimeDest "VRCFaceTracking.ModuleProcess.exe"))) {
        throw "运行时压缩包根目录必须直接包含 VRCFaceTracking.ModuleProcess.exe：$RuntimeZip"
    }
    Write-Host "运行时：已解压 $RuntimeZip"
} elseif (-not $SkipRuntime) {
    $pyArgs = @("tools/build_vrcft_runtime.py", "--dest", $runtimeDest)
    if ($RuntimeSource) { $pyArgs += @("--source", $RuntimeSource) }
    & python @pyArgs
    if ($LASTEXITCODE -ne 0) { throw "VRCFT 运行时抽取失败" }
}

& (Join-Path $PSScriptRoot "build_installer.ps1") `
    -Version $Version -DistDir $DistDir -OutputDir $OutputDir

$setup = Join-Path $OutputDir "FaceGazeBridge-Setup-$Version.exe"
if (-not (Test-Path $setup)) { throw "找不到安装包：$setup" }

& (Join-Path $PSScriptRoot "pack_sdk.ps1") `
    -Version $Version -DistDir $DistDir -OutputDir $OutputDir
$sdkZip = Join-Path $OutputDir "FaceGazeBridge-SDK-$Version.zip"
if (-not (Test-Path $sdkZip)) { throw "找不到 SDK 包：$sdkZip" }

# CI 没有 VRCFT 安装目录，把运行时压缩包作为 release 资产带出去，后续 tag
# 构建就能直接复用（release.yml 会优先下载它）。
$runtimeZipOut = Join-Path $OutputDir "vrcft-runtime.zip"
Compress-Archive -Path (Join-Path $runtimeDest "*") -DestinationPath $runtimeZipOut -Force

$sumFile = Join-Path $OutputDir "SHA256SUMS.txt"
$lines = @()
foreach ($file in @($setup, $runtimeZipOut, $sdkZip)) {
    $hash = (Get-FileHash $file -Algorithm SHA256).Hash.ToLower()
    $lines += "$hash  $([System.IO.Path]::GetFileName($file))"
}
Set-Content -Path $sumFile -Value $lines -Encoding ascii

Write-Host ""
Write-Host "安装包：$setup（$([math]::Round((Get-Item $setup).Length / 1MB, 1)) MB）"
Write-Host "运行时：$runtimeZipOut"
Write-Host "SDK：$sdkZip"
Write-Host "校验和：$sumFile"

if ($Publish) {
    $notesPath = Join-Path $OutputDir "release-notes-$Version.md"
    if ($NotesFile) {
        Copy-Item $NotesFile $notesPath -Force
    } else {
        $notes = @"
FaceGaze Bridge $Version

- 用户级安装（不需要管理员），内置 VRCFT 模块宿主运行时与 VC++ 运行库
- 安装包未签名，首次运行可能被 SmartScreen 拦截；请用 SHA256SUMS.txt 核对文件
- 模块、校准、输出设置保存在 %LOCALAPPDATA%\FaceGazeBridge
"@
        Set-Content -Path $notesPath -Value $notes -Encoding utf8
    }

    $assets = @($setup, $runtimeZipOut, $sdkZip, $sumFile)
    & gh release view $tag --json tagName 2>$null | Out-Null
    if ($LASTEXITCODE -eq 0) {
        Write-Host "更新已有 release $tag（draft 状态不变）"
        & gh release upload $tag @assets --clobber
        if ($LASTEXITCODE -ne 0) { throw "gh release upload 失败" }
        & gh release edit $tag --notes-file $notesPath
    } else {
        & gh release create $tag @assets --draft `
            --title "FaceGaze Bridge $Version" --notes-file $notesPath
        if ($LASTEXITCODE -ne 0) { throw "gh release create 失败" }
    }
    Write-Host "Release：https://github.com/Aqamoe/FaceGaze-Bridge/releases/tag/$tag"
}
