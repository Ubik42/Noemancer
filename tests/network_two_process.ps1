param(
    [Parameter(Mandatory = $true)]
    [string]$Runtime
)

$ErrorActionPreference = 'Stop'
$listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
$listener.Start()
$port = ([System.Net.IPEndPoint]$listener.LocalEndpoint).Port
$listener.Stop()
$serverOutput = [System.IO.Path]::GetTempFileName()
$serverError = [System.IO.Path]::GetTempFileName()
$server = $null

try {
    $server = Start-Process -FilePath $Runtime `
        -ArgumentList @('network-server', '--port', "$port", '--sessions', '1', '--timeout-ms', '5000', '--format', 'json') `
        -RedirectStandardOutput $serverOutput -RedirectStandardError $serverError `
        -PassThru -WindowStyle Hidden

    $clientText = $null
    for ($attempt = 0; $attempt -lt 30; ++$attempt) {
        Start-Sleep -Milliseconds 100
        $candidate = & $Runtime network-client --host 127.0.0.1 --port $port `
            --peer-id client.ctest --payload-bytes 768 --timeout-ms 1000 --format json 2>&1
        if ($LASTEXITCODE -eq 0) {
            $clientText = $candidate
            break
        }
        if ($server.HasExited) { break }
    }
    if ($null -eq $clientText) { throw 'The independent client process could not establish a session.' }
    if (-not $server.WaitForExit(7000)) { throw 'The independent server process did not stop after its session budget.' }
    $server.WaitForExit()
    $server.Refresh()
    $client = $clientText | ConvertFrom-Json
    $serverResult = Get-Content -LiteralPath $serverOutput -Raw | ConvertFrom-Json
    if (-not $client.success -or -not $serverResult.success -or
        -not $client.reliableControl -or -not $client.unreliableState -or
        $client.statePayloadBytes -ne 768 -or $serverResult.completedSessions -ne 1 -or
        $serverResult.sessions[0].peerId -ne 'client.ctest') {
        throw 'Two-process reliable-control/unreliable-state evidence is incomplete.'
    }
    Write-Output "Two-process network session passed on 127.0.0.1:$port"
} finally {
    if ($null -ne $server -and -not $server.HasExited) { $server.Kill() }
    Remove-Item -LiteralPath $serverOutput -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $serverError -Force -ErrorAction SilentlyContinue
}
