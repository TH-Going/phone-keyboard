# Real-process smoke checks for the main window, tray activation and server.
# The historical filename is kept. No physical mouse or keyboard is moved.
param(
    [string]$Executable = (Join-Path $PSScriptRoot '..\release\PhoneKeyboardC.exe'),
    [string]$SnapshotDir = ''
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $Executable)) { throw "Build first: $Executable is missing" }
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class MainWindowProbe {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    private delegate bool EnumProc(IntPtr window, IntPtr parameter);
    [DllImport("user32.dll")] private static extern bool EnumWindows(EnumProc callback, IntPtr parameter);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetClassName(IntPtr window, StringBuilder name, int count);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetWindowText(IntPtr window, StringBuilder name, int count);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr window, uint message, IntPtr wparam, IntPtr lparam);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr window, int id);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window, out RECT rect);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr window);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr window, IntPtr dc, uint flags);
    [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
    public static IntPtr Find(int process, string className) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((window, parameter) => {
            uint owner;
            GetWindowThreadProcessId(window, out owner);
            if (owner != process) return true;
            var name = new StringBuilder(128);
            GetClassName(window, name, name.Capacity);
            if (name.ToString() != className) return true;
            found = window;
            return false;
        }, IntPtr.Zero);
        return found;
    }
    public static string Title(IntPtr window) {
        var text = new StringBuilder(128);
        GetWindowText(window, text, text.Capacity);
        return text.ToString();
    }
}
'@

$testId = [Guid]::NewGuid().ToString('N')
$statePath = Join-Path $env:TEMP "pk_window_$testId.txt"
$listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
$listener.Start()
$port = $listener.LocalEndpoint.Port
$listener.Stop()
$token = '0123456789abcdef0123456789abcdef'
$failures = 0
function Assert([bool]$Condition, [string]$Message) {
    if ($Condition) { Write-Host "  ok   $Message" }
    else { Write-Host "  FAIL $Message" -ForegroundColor Red; $script:failures++ }
}
function Read-State {
    if (-not (Test-Path -LiteralPath $statePath)) { return @{} }
    $values = @{}
    Get-Content -LiteralPath $statePath -Encoding utf8 | ForEach-Object {
        if ($_ -match '^([a-z_]+)=(.+)$') { $values[$matches[1]] = $matches[2] }
    }
    return $values
}
function Wait-State([scriptblock]$Predicate) {
    $deadline = (Get-Date).AddSeconds(5)
    do {
        $values = Read-State
        if (& $Predicate $values) { return $values }
        Start-Sleep -Milliseconds 50
    } while ((Get-Date) -lt $deadline)
    return $values
}
function Click-Control([int]$Id) {
    $control = [MainWindowProbe]::GetDlgItem($script:window, $Id)
    if ($control -eq [IntPtr]::Zero) { throw "Missing control $Id" }
    [MainWindowProbe]::PostMessage($control, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
}
function Save-Window([string]$Name) {
    if (-not $SnapshotDir) { return }
    $rect = [MainWindowProbe+RECT]::new()
    [MainWindowProbe]::GetWindowRect($script:window, [ref]$rect) | Out-Null
    $bitmap = [System.Drawing.Bitmap]::new($rect.Right - $rect.Left, $rect.Bottom - $rect.Top)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $dc = $graphics.GetHdc()
    try {
        if (-not [MainWindowProbe]::PrintWindow($script:window, $dc, 2)) { throw 'Window capture failed' }
    } finally { $graphics.ReleaseHdc($dc); $graphics.Dispose() }
    try { $bitmap.Save((Join-Path $SnapshotDir "$Name.png"), [System.Drawing.Imaging.ImageFormat]::Png) }
    finally { $bitmap.Dispose() }
}

$dpiContext = [MainWindowProbe]::SetThreadDpiAwarenessContext([IntPtr](-4))
$process = $null
$tray = [IntPtr]::Zero
try {
    $process = Start-Process -WindowStyle Hidden -FilePath $Executable -PassThru -ArgumentList @(
        '--allow-multi', '--dry-run', '--state-file', $statePath, '--quit-after', '45000',
        '--port', "$port", '--token', $token
    )
    $state = Wait-State { param($s) $s['visible'] -eq '1' }
    Assert ($state['visible'] -eq '1') 'normal startup shows the main window'
    $script:window = [MainWindowProbe]::Find($process.Id, 'PhoneKeyboardCard')
    $tray = [MainWindowProbe]::Find($process.Id, 'PhoneKeyboardTray')
    if ($window -eq [IntPtr]::Zero -or $tray -eq [IntPtr]::Zero) { throw 'Application windows not found' }
    Assert ([MainWindowProbe]::Title($window) -eq '手键') 'native window title is 手键'
    $rect = [MainWindowProbe+RECT]::new()
    [MainWindowProbe]::GetWindowRect($window, [ref]$rect) | Out-Null
    $dpi = [MainWindowProbe]::GetDpiForWindow($window)
    Assert (($rect.Right - $rect.Left) -eq [math]::Floor((282 * $dpi + 48) / 96) -and
            ($rect.Bottom - $rect.Top) -eq [math]::Floor((338 * $dpi + 48) / 96)) 'native size matches 282 by 338 at the monitor DPI'
    Save-Window 'main-window'
    Click-Control 1001
    $state = Wait-State { param($s) $s['settings'] -eq '1' }
    Assert ($state['settings'] -eq '1') 'clicking the settings icon displays the sliders'
    Save-Window 'settings-window'
    Click-Control 1004
    $state = Wait-State { param($s) $s['settings'] -eq '0' }
    Assert ($state['settings'] -eq '0') 'Done returns to the QR page'
    Click-Control 1002
    $state = Wait-State { param($s) $s['visible'] -eq '0' }
    Assert ($state['visible'] -eq '0' -and -not $process.HasExited) 'the close icon hides the window and keeps the process running'
    $response = Invoke-WebRequest -Uri "http://127.0.0.1:$port/c/$token" -TimeoutSec 5
    Assert ($response.StatusCode -eq 200) 'phone controller is still served while the main window is closed'

    [MainWindowProbe]::PostMessage($tray, 0x8001, [IntPtr]::Zero, [IntPtr](0x10000 -bor 0x406)) | Out-Null
    [MainWindowProbe]::PostMessage($tray, 0x8001, [IntPtr]::Zero, [IntPtr](0x10000 -bor 0x200)) | Out-Null
    Start-Sleep -Milliseconds 600
    Assert ((Read-State)['visible'] -eq '0') 'tray hover callbacks do not reopen the closed window'
    [MainWindowProbe]::PostMessage($tray, 0x8001, [IntPtr]::Zero, [IntPtr](0x10000 -bor 0x400)) | Out-Null
    $state = Wait-State { param($s) $s['visible'] -eq '1' }
    Assert ($state['visible'] -eq '1') 'tray selection reopens the real window'
    [MainWindowProbe]::PostMessage($tray, 0x8001, [IntPtr]::Zero, [IntPtr](0x10000 -bor 0x407)) | Out-Null
    Start-Sleep -Milliseconds 600
    Assert ((Read-State)['visible'] -eq '1') 'leaving the tray keeps the reopened window visible'
    [MainWindowProbe]::PostMessage($window, 0x10, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    $state = Wait-State { param($s) $s['visible'] -eq '0' }
    Assert ($state['visible'] -eq '0' -and -not $process.HasExited) 'window-manager close also minimizes to the tray'
    [MainWindowProbe]::PostMessage($tray, 0x8004, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    $state = Wait-State { param($s) $s['visible'] -eq '1' }
    Assert ($state['visible'] -eq '1') 'the single-instance activation message reopens the window'
} finally {
    if ($process -and -not $process.HasExited) {
        if ($tray -ne [IntPtr]::Zero) {
            [MainWindowProbe]::PostMessage($tray, 0x8005, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
        }
        if (-not $process.WaitForExit(5000)) { Stop-Process -Id $process.Id -Force }
    }
    [MainWindowProbe]::SetThreadDpiAwarenessContext($dpiContext) | Out-Null
    Remove-Item -LiteralPath $statePath, "$statePath.tmp" -Force -ErrorAction SilentlyContinue
}
if ($failures) { throw "$failures main-window check(s) failed" }
Write-Host 'main-window smoke checks passed'
