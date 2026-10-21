# Main-window controls, tray callbacks and configuration regression checks.
# Top-level presentation and the registry are replaced; no desktop input.
param([string]$SnapshotDir = '')
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build"
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath) | Select-Object -First 1
if (-not $vsPath) { throw "MSVC toolset not found" }
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
$objects = Get-ChildItem -LiteralPath (Join-Path $root 'src') -Filter *.c |
    Where-Object { $_.Name -notin @('main.c', 'card.c') } |
    ForEach-Object { '"{0}"' -f (Join-Path $build ($_.BaseName + '.obj')) }
if (-not $objects) { throw "Run build.ps1 before the hover state test" }

$source = Join-Path $PSScriptRoot "hover-state-test.c"
$object = Join-Path $build "hover-state-test.obj"
$executable = Join-Path $build "hover-state-test.exe"
$command = 'call "{0}" >nul && cl /nologo /std:c17 /utf-8 /W3 /O2 /MD /I"{1}" /Fo"{2}" /Fe:"{3}" "{4}" {5} /link /SUBSYSTEM:CONSOLE /INCREMENTAL:NO' -f $vcvars, $build, $object, $executable, $source, ($objects -join ' ')

try {
    & cmd.exe /d /c $command
    if ($LASTEXITCODE -ne 0) { throw "Hover state test compilation failed" }
    if ($SnapshotDir) { & $executable $SnapshotDir } else { & $executable }
    $testExitCode = $LASTEXITCODE
} finally {
    Remove-Item -LiteralPath $object, $executable -Force -ErrorAction SilentlyContinue
}
exit $testExitCode
