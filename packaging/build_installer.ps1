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
#
# VC++ 运行库：exe 是动态 CRT 链接，干净 Windows 上不保证有。脚本会把
# msvcp140.dll / vcruntime140.dll / vcruntime140_1.dll 复制到 dist 根目录和
# runtime\vrcft（模块宿主进程），这样安装包不依赖用户装 VC 再分发包。
# 找不到运行库时用 -CrtDir 指定，或 -SkipCrtDeploy 跳过。
param(
    [string]$Version = "",
    [string]$DistDir = "",
    [string]$OutputDir = "",
    [string]$CrtDir = "",
    [switch]$SkipCrtDeploy
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
# ISCC 按 .iss 所在目录解析相对路径，这里统一转成基于仓库根的绝对路径。
if (-not [System.IO.Path]::IsPathRooted($DistDir)) { $DistDir = Join-Path $root $DistDir }
if (-not [System.IO.Path]::IsPathRooted($OutputDir)) { $OutputDir = Join-Path $root $OutputDir }
if ($CrtDir -and -not [System.IO.Path]::IsPathRooted($CrtDir)) { $CrtDir = Join-Path $root $CrtDir }

if (-not (Test-Path (Join-Path $DistDir "FaceGaze Bridge.exe"))) {
    throw "找不到 $DistDir\FaceGaze Bridge.exe；先运行 cmake --install build/release --config Release --prefix $DistDir"
}
if (-not (Test-Path (Join-Path $DistDir "runtime\vrcft"))) {
    Write-Warning "dist 里缺少 runtime\vrcft；安装包将依赖用户本机安装的 VRCFT。"
}

function Find-CrtDir {
    param([string]$Explicit)
    if ($Explicit) {
        if (Test-Path (Join-Path $Explicit "vcruntime140.dll")) { return $Explicit }
        throw "指定的 CrtDir 里没有 vcruntime140.dll：$Explicit"
    }
    $roots = @()
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $install = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath 2>$null
        if ($install) { $roots += (Join-Path $install "VC\Redist\MSVC") }
    }
    $roots += @(
        (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\2019\BuildTools\VC\Redist\MSVC"),
        (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\2019\Community\VC\Redist\MSVC"),
        (Join-Path ${env:ProgramFiles} "Microsoft Visual Studio\2022\BuildTools\VC\Redist\MSVC"),
        (Join-Path ${env:ProgramFiles} "Microsoft Visual Studio\2022\Community\VC\Redist\MSVC")
    )
    foreach ($root in $roots) {
        if (-not (Test-Path $root)) { continue }
        foreach ($versionDir in (Get-ChildItem $root -Directory | Sort-Object Name -Descending)) {
            $x64 = Join-Path $versionDir.FullName "x64"
            if (-not (Test-Path $x64)) { continue }
            $crt = Get-ChildItem $x64 -Directory -Filter "Microsoft.VC*.CRT" |
                Where-Object { Test-Path (Join-Path $_.FullName "vcruntime140.dll") } |
                Select-Object -First 1
            if ($crt) { return $crt.FullName }
        }
    }
    throw "找不到 MSVC x64 运行库（VC\Redist\MSVC\<ver>\x64\Microsoft.VC*.CRT）；用 -CrtDir 指定，或加 -SkipCrtDeploy"
}

if (-not $SkipCrtDeploy) {
    $crt = Find-CrtDir $CrtDir
    $targets = @($DistDir)
    $runtimeDir = Join-Path $DistDir "runtime\vrcft"
    if (Test-Path $runtimeDir) { $targets += $runtimeDir }
    foreach ($target in $targets) {
        foreach ($dll in @("msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll")) {
            Copy-Item (Join-Path $crt $dll) (Join-Path $target $dll) -Force
        }
    }
    Write-Host "VC 运行库：$crt"
    Write-Host "  → $($targets -join ' ; ')"
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
