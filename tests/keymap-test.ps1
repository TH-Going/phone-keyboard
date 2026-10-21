# Compares the key table in src/input.c with the one in the Rust original
# (code/src-tauri/src/input.rs).
#
# Each mapping is reduced to a canonical line "Code=0xVV/ext" and the two
# sorted lists are compared, which avoids PowerShell hashtable/array quirks.

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$cPath = Join-Path $root "src\input.c"
$rsPath = Join-Path $root "..\code\src-tauri\src\input.rs"

if (-not (Test-Path -LiteralPath $rsPath)) {
    throw "Original input.rs not found: $rsPath"
}

function Get-RustMappings([string]$Path) {
    $list = [System.Collections.Generic.List[string]]::new()
    foreach ($line in Get-Content -LiteralPath $Path) {
        if ($line -match '"([A-Za-z0-9]+)"\s*=>\s*\((0x[0-9A-Fa-f]+),\s*(true|false)\)') {
            $code = $matches[1]
            $vk = [Convert]::ToInt32($matches[2], 16)
            $extended = $matches[3] -eq 'true'
            $list.Add(('{0}=0x{1:X2}/{2}' -f $code, $vk, $extended))
        }
    }
    return $list
}

function Get-CMappings([string]$Path) {
    $list = [System.Collections.Generic.List[string]]::new()
    foreach ($line in Get-Content -LiteralPath $Path) {
        if ($line -match '^\s*\{"([A-Za-z0-9]+)",\s*(0x[0-9A-Fa-f]+),\s*([01])\},') {
            $code = $matches[1]
            $vk = [Convert]::ToInt32($matches[2], 16)
            $extended = $matches[3] -eq '1'
            $list.Add(('{0}=0x{1:X2}/{2}' -f $code, $vk, $extended))
        }
    }
    return $list
}

$failures = 0
function Assert([bool]$Condition, [string]$Message) {
    if ($Condition) {
        Write-Host "  ok   $Message"
    } else {
        Write-Host "  FAIL $Message" -ForegroundColor Red
        $script:failures++
    }
}

$rust = Get-RustMappings $rsPath
$c = Get-CMappings $cPath

Write-Host "key tables"
Assert ($rust.Count -eq 104) "Rust table has 104 entries (got $($rust.Count))"
Assert ($c.Count -eq 104) "C table has 104 entries (got $($c.Count))"

$diff = @(Compare-Object -ReferenceObject @($rust | Sort-Object) -DifferenceObject @($c | Sort-Object))
Assert ($diff.Count -eq 0) "every mapping matches (code, virtual-key, extended flag)"
if ($diff.Count -gt 0) {
    $diff | Select-Object -First 10 | ForEach-Object { Write-Host "       $($_.SideIndicator) $($_.InputObject)" }
}

# A duplicate would silently shadow an earlier entry at lookup time.
$duplicates = @($c | Group-Object | Where-Object { $_.Count -gt 1 })
Assert ($duplicates.Count -eq 0) "no duplicate entries in the C table"

if ($failures -gt 0) {
    Write-Host "$failures check(s) failed" -ForegroundColor Red
    exit 1
}
Write-Host "key tables are identical"
exit 0
