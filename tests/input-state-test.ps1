# Build first. SendInput and motion are replaced by the test, so this never
# moves the cursor or sends real keyboard / mouse button events.
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build"
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath) | Select-Object -First 1
if (-not $vsPath) { throw "MSVC toolset not found" }
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
$util = Join-Path $build "util.obj"
if (-not (Test-Path -LiteralPath $util)) { throw "Run build.ps1 before the input state test" }

$source = Join-Path $PSScriptRoot "input-state-test.c"
$object = Join-Path $build "input-state-test.obj"
$executable = Join-Path $build "input-state-test.exe"
$command = 'call "{0}" >nul && cl /nologo /std:c17 /utf-8 /W3 /O2 /MD /Fo"{1}" /Fe:"{2}" "{3}" "{4}" /link /SUBSYSTEM:CONSOLE /INCREMENTAL:NO' -f $vcvars, $object, $executable, $source, $util

try {
    & cmd.exe /d /c $command
    if ($LASTEXITCODE -ne 0) { throw "Input state test compilation failed" }
    & $executable
    $testExitCode = $LASTEXITCODE
} finally {
    Remove-Item -LiteralPath $object, $executable -Force -ErrorAction SilentlyContinue
}
exit $testExitCode
