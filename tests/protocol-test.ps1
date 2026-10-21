# Protocol test for the pure C build.
#
# Starts the app headless (no tray, dry-run input), then checks:
#   - GET /c/<token>          -> 200 + CSP + HTML body
#   - GET /c/<wrong>          -> 404
#   - GET /assets/controller.css|js -> 200
#   - WebSocket /ws/<token>   -> handshake + text frame accepted
#   - WebSocket /ws/<wrong>   -> rejected
#
# The app runs with --dry-run so no keyboard or mouse input is injected.

$ErrorActionPreference = "Stop"

$exe = Join-Path $PSScriptRoot "..\release\PhoneKeyboardC.exe"
$log = Join-Path $env:TEMP "pk_protocol.log"
$port = 47831
$token = "0123456789abcdef0123456789abcdef"

if (-not (Test-Path -LiteralPath $exe)) {
    throw "Build the app first: $exe is missing"
}
Remove-Item $log -ErrorAction SilentlyContinue

function Invoke-RawHttp([string]$Path) {
    $client = [System.Net.Sockets.TcpClient]::new("127.0.0.1", $script:port)
    try {
        $client.ReceiveTimeout = 5000
        $stream = $client.GetStream()
        $request = "GET $Path HTTP/1.1`r`nHost: 127.0.0.1:$script:port`r`nConnection: close`r`n`r`n"
        $bytes = [System.Text.Encoding]::ASCII.GetBytes($request)
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush()
        $reader = [System.IO.StreamReader]::new($stream, [System.Text.Encoding]::UTF8)
        return $reader.ReadToEnd()
    } finally {
        $client.Close()
    }
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

$process = Start-Process -FilePath $exe -PassThru -ArgumentList @(
    '--no-tray', '--dry-run', '--allow-multi',
    '--port', "$port", '--token', $token, '--log', $log
)

try {
    # Readiness: poll the listening port instead of the log file, which the app
    # keeps open for writing.
    $ready = $false
    $deadline = (Get-Date).AddSeconds(10)
    while (-not $ready -and (Get-Date) -lt $deadline) {
        try {
            $probe = [System.Net.Sockets.TcpClient]::new()
            $probe.Connect("127.0.0.1", $port)
            $probe.Close()
            $ready = $true
        } catch {
            Start-Sleep -Milliseconds 100
        }
    }
    if (-not $ready) {
        throw "The app never started listening on port $port"
    }

    Write-Host "HTTP"
    $page = Invoke-RawHttp "/c/$token"
    Assert ($page -match '^HTTP/1.1 200 OK') "GET /c/<token> returns 200"
    Assert ($page -match 'Content-Security-Policy: default-src') "page carries the CSP header"
    Assert ($page -match 'Referrer-Policy: no-referrer') "page carries the referrer policy"
    Assert ($page -match '<!doctype html>') "page body is the controller HTML"
    Assert ($page -match 'Content-Type: text/html; charset=utf-8') "page content type"

    $wrong = Invoke-RawHttp "/c/ffffffffffffffffffffffffffffffff"
    Assert ($wrong -match '^HTTP/1.1 404 Not Found') "GET /c/<wrong token> returns 404"

    $css = Invoke-RawHttp "/assets/controller.css"
    Assert ($css -match '^HTTP/1.1 200 OK') "GET /assets/controller.css returns 200"
    Assert ($css -match 'Content-Type: text/css') "stylesheet content type"

    $js = Invoke-RawHttp "/assets/controller.js"
    Assert ($js -match '^HTTP/1.1 200 OK') "GET /assets/controller.js returns 200"
    Assert ($js -match 'Content-Type: text/javascript') "script content type"

    $missing = Invoke-RawHttp "/nope"
    Assert ($missing -match '^HTTP/1.1 404 Not Found') "unknown path returns 404"

    Write-Host "WebSocket"
    $socket = [System.Net.WebSockets.ClientWebSocket]::new()
    $cancel = [System.Threading.CancellationTokenSource]::new()
    $uri = [Uri]"ws://127.0.0.1:$port/ws/$token"
    $socket.ConnectAsync($uri, $cancel.Token).GetAwaiter().GetResult() | Out-Null
    Assert ($socket.State -eq [System.Net.WebSockets.WebSocketState]::Open) "handshake with a valid token succeeds"

    $payload = [System.Text.Encoding]::UTF8.GetBytes('{"t":"release"}')
    $segment = [System.ArraySegment[byte]]::new($payload)
    $socket.SendAsync($segment, [System.Net.WebSockets.WebSocketMessageType]::Text, $true,
                      $cancel.Token).GetAwaiter().GetResult() | Out-Null
    Start-Sleep -Milliseconds 150
    Assert ($socket.State -eq [System.Net.WebSockets.WebSocketState]::Open) "text frame accepted, socket still open"

    $socket.CloseAsync([System.Net.WebSockets.WebSocketCloseStatus]::NormalClosure, "done",
                       $cancel.Token).GetAwaiter().GetResult() | Out-Null
    Assert ($socket.State -eq [System.Net.WebSockets.WebSocketState]::Closed) "clean close"
    $socket.Dispose()

    $rejected = $false
    $bad = [System.Net.WebSockets.ClientWebSocket]::new()
    $badCancel = [System.Threading.CancellationTokenSource]::new()
    try {
        $bad.ConnectAsync([Uri]"ws://127.0.0.1:$port/ws/ffffffffffffffffffffffffffffffff",
                          $badCancel.Token).GetAwaiter().GetResult() | Out-Null
    } catch {
        $rejected = $true
    } finally {
        $bad.Dispose()
    }
    Assert $rejected "handshake with a wrong token is rejected"

    Start-Sleep -Milliseconds 300
} finally {
    if (-not $process.HasExited) {
        $process.Kill()
        $process.WaitForExit()
    }
}

# The log is only readable once the app has exited.
$logText = if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log -Raw } else { "" }
Assert ($logText -match 'ws connected') "server logged the accepted connection"
Assert ($logText -match 'ws rejected') "server logged the rejected connection"

if ($failures -gt 0) {
    Write-Host "$failures check(s) failed" -ForegroundColor Red
    exit 1
}
Write-Host "all protocol checks passed"
exit 0
