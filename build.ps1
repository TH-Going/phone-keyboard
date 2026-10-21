$ErrorActionPreference = "Stop"

# Builds the pure C port with the MSVC toolchain that ships with the Visual
# Studio Build Tools. Output: release\PhoneKeyboardC.exe (single file, no
# WebView2 dependency).

$root = $PSScriptRoot
$src = Join-Path $root "src"
$build = Join-Path $root "build"
$release = Join-Path $root "release"

New-Item -ItemType Directory -Force -Path $build, $release | Out-Null

& (Join-Path $root "tools\embed.ps1") -WebDir (Join-Path $root "web") -OutFile (Join-Path $build "web_assets.h")

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw "vswhere.exe not found. Install Visual Studio Build Tools with the C++ workload."
}
$vsPath = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath) | Select-Object -First 1
if (-not $vsPath) {
    throw "No MSVC toolset found. Install 'Desktop development with C++'."
}
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path -LiteralPath $vcvars)) {
    throw "vcvars64.bat not found under $vsPath"
}

$sources = Get-ChildItem -LiteralPath $src -Filter *.c | Sort-Object Name | ForEach-Object {
    '"{0}"' -f $_.FullName
}

$compileFlags = @(
    "/nologo",
    "/std:c17",
    "/utf-8",
    "/W3",
    "/O2",
    "/MD",
    "/GS",
    "/D_CRT_SECURE_NO_WARNINGS",
    "/DNDEBUG",
    "/DWIN32_LEAN_AND_MEAN",
    "/DUNICODE",
    "/D_UNICODE",
    "/I""$src""",
    "/I""$build"""
) -join " "

$exe = Join-Path $release "PhoneKeyboardC.exe"
$batch = @"
@echo off
setlocal
call "$vcvars" >nul
if errorlevel 1 exit /b 1
cd /d "$build"
rc /nologo /fo app.res "$root\res\app.rc"
if errorlevel 1 exit /b 1
cl $compileFlags /Fo"$build\\" /Fd"$build\\" $($sources -join ' ') app.res /Fe:"$exe" /link /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /INCREMENTAL:NO
exit /b %errorlevel%
"@

$batchPath = Join-Path $build "build.bat"
Set-Content -LiteralPath $batchPath -Value $batch -Encoding ASCII

Push-Location $root
try {
    & cmd.exe /c $batchPath
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed with exit code $LASTEXITCODE"
    }
} finally {
    Pop-Location
}

if (-not (Test-Path -LiteralPath $exe)) {
    throw "Build finished without producing $exe"
}
$size = [math]::Round((Get-Item -LiteralPath $exe).Length / 1KB, 1)
Write-Host "Portable build: $exe ($size KB)"
