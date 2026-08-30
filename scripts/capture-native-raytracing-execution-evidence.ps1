param(
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Release',
    [string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $repositoryRoot 'generated\acceptance\native-raytracing-minimal-trace-current'
}

$runtimePath = Join-Path $repositoryRoot "build\windows-msvc-debug\src\runtime\$Config\noemancer.exe"
if (-not (Test-Path -LiteralPath $runtimePath -PathType Leaf)) {
    throw "Noemancer $Config runtime is missing. Build target 'noemancer' before capturing evidence."
}

$rawOutput = & $runtimePath rhi execute --format json 2>&1
$exitCode = $LASTEXITCODE
$textOutput = ($rawOutput | ForEach-Object { $_.ToString() }) -join "`n"
$execution = $textOutput | ConvertFrom-Json -Depth 100

if ($exitCode -ne 0 -or -not $execution.success) {
    throw "Native ray-tracing execution failed with exit code $exitCode.`n$textOutput"
}
if (-not $execution.aggregate.valid -or
    -not $execution.aggregate.nativeRhiReady -or
    -not $execution.aggregate.blasTlasRuntimeReady -or
    $execution.aggregate.rtgiReady) {
    throw 'Native ray-tracing aggregate violated the bounded readiness/non-RTGI contract.'
}
if ($execution.aggregate.backends.Count -ne 2) {
    throw 'Native ray-tracing evidence must contain exactly D3D12 and Vulkan receipts.'
}
foreach ($backend in $execution.aggregate.backends) {
    if (-not $backend.present -or $backend.receipt.state -ne 'ready' -or
        -not $backend.receipt.traceSubmitted -or
        -not $backend.receipt.traceCompleted -or
        -not $backend.receipt.gpuTimestampsValid) {
        throw "Backend '$($backend.backend)' did not publish a complete trace/timestamp receipt."
    }
}

$relativeRuntime = [System.IO.Path]::GetRelativePath($repositoryRoot, $runtimePath).Replace('\', '/')
$evidence = [ordered]@{
    schema = 'noemancer.native-raytracing-execution-evidence/0.1'
    capturedAtUtc = [DateTime]::UtcNow.ToString('o')
    configuration = $Config
    runtime = $relativeRuntime
    command = 'noemancer rhi execute --format json'
    exitCode = $exitCode
    execution = $execution
}

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$outputPath = Join-Path $OutputDirectory 'evidence.json'
$temporaryPath = "$outputPath.tmp"
$evidence | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $temporaryPath -Encoding utf8NoBOM
Move-Item -LiteralPath $temporaryPath -Destination $outputPath -Force
Write-Output $outputPath
