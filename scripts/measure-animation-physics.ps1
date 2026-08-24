param(
    [ValidateSet('Debug','Release')]
    [string]$Config = 'Release',
    [ValidateSet('direct3d12','vulkan')]
    [string]$GpuBackend = 'direct3d12',
    [ValidateRange(0,10000)]
    [int]$WarmupFrames = 60,
    [ValidateRange(60,10000)]
    [int]$SampleFrames = 120,
    [string]$OutputRoot
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$runtime = Join-Path $repositoryRoot "build/windows-msvc-debug/src/runtime/$Config/noemancer.exe"
if (-not (Test-Path -LiteralPath $runtime -PathType Leaf)) {
    throw "Noemancer $Config runtime is missing. Build target noemancer through scripts/engine.ps1 first."
}
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $repositoryRoot ('generated/acceptance/animation-physics-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
$OutputRoot = [System.IO.Path]::GetFullPath($OutputRoot)
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
$performancePath = Join-Path $OutputRoot 'performance-evidence.json'
$imagePath = Join-Path $OutputRoot 'animation-physics.bmp'
$stdoutPath = Join-Path $OutputRoot 'runtime.stdout.jsonl'
$stderrPath = Join-Path $OutputRoot 'runtime.stderr.log'
$performanceArguments = @(
    'run','--animation-physics-stress','--format','json',
    '--performance-evidence',$performancePath,'--performance-workload','noemancer.animation-physics/0.1',
    '--performance-warmup-frames',$WarmupFrames,'--performance-sample-frames',$SampleFrames,
    '--window-width','1920','--window-height','1080','--gpu-backend',$GpuBackend
)
& $runtime @performanceArguments 1> $stdoutPath 2> $stderrPath
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $performancePath -PathType Leaf)) {
    throw "Animation/physics performance run failed with exit code $LASTEXITCODE. See $stderrPath"
}
$performance = Get-Content -LiteralPath $performancePath -Raw | ConvertFrom-Json
$skinning = $performance.renderer.submission.skinning
if ($performance.workload.id -ne 'noemancer.animation-physics/0.1' -or
    [int]$performance.workload.resolution.width -ne 1920 -or [int]$performance.workload.resolution.height -ne 1080 -or
    [int]$skinning.renderInstances -ne 64 -or [int]$skinning.drawItems -lt 64 -or
    [int]$skinning.jointMatrices -lt 3200 -or [int]$performance.renderer.visibleRenderables -lt 321) {
    throw 'Animation/physics workload did not exercise its deterministic production contract.'
}
$cpuP95 = [double]$performance.cpu.frameTime.p95
if ($cpuP95 -gt 8.0) { throw "Animation/physics CPU frame p95 $cpuP95 ms exceeds the 8.0 ms baseline budget." }
$captureArguments = @(
    'run','--animation-physics-stress','--format','json','--capture-frame',$imagePath,'--frames','64',
    '--window-width','1920','--window-height','1080','--gpu-backend',$GpuBackend
)
& $runtime @captureArguments 1>> $stdoutPath 2>> $stderrPath
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $imagePath -PathType Leaf) -or
    -not (Test-Path -LiteralPath "$imagePath.quality.json" -PathType Leaf)) {
    throw "Animation/physics visual capture failed with exit code $LASTEXITCODE. See $stderrPath"
}
$quality = Get-Content -LiteralPath "$imagePath.quality.json" -Raw | ConvertFrom-Json
if (-not $quality.pass -or -not $quality.dimensionsMatch) { throw 'Animation/physics visual capture failed its image quality contract.' }
$manifest = [ordered]@{
    schemaVersion = 'noemancer.animation-physics-evidence/0.1'
    workloadId = 'noemancer.animation-physics/0.1'
    capturedAt = [DateTimeOffset]::UtcNow.ToString('o')
    configuration = $Config
    gpuBackend = $GpuBackend
    contract = [ordered]@{ animatedActors=64; dynamicBodies=256; width=1920; height=1080; cpuFrameP95MillisecondsMax=8.0 }
    result = [ordered]@{
        cpuFrameMeanMilliseconds=[double]$performance.cpu.frameTime.mean
        cpuFrameP95Milliseconds=$cpuP95
        simulationMeanMilliseconds=[double]$performance.cpu.simulation.mean
        simulationP95Milliseconds=[double]$performance.cpu.simulation.p95
        renderExtractMeanMilliseconds=[double]$performance.cpu.renderExtract.mean
        skinnedRenderInstances=[int]$skinning.renderInstances
        skinningJointMatrices=[int]$skinning.jointMatrices
        visibleRenderables=[int]$performance.renderer.visibleRenderables
    }
    artifacts = [ordered]@{
        performance='performance-evidence.json'
        performanceSha256=(Get-FileHash -Algorithm SHA256 -LiteralPath $performancePath).Hash
        image='animation-physics.bmp'
        imageSha256=(Get-FileHash -Algorithm SHA256 -LiteralPath $imagePath).Hash
        quality='animation-physics.bmp.quality.json'
        runtimeSha256=(Get-FileHash -Algorithm SHA256 -LiteralPath $runtime).Hash
    }
    commands = @(
        [ordered]@{ purpose='performance'; executable=$runtime; arguments=$performanceArguments }
        [ordered]@{ purpose='visual'; executable=$runtime; arguments=$captureArguments }
    )
    pass = $true
}
$manifestPath = Join-Path $OutputRoot 'animation-physics-evidence.json'
$manifest | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $manifestPath -Encoding utf8
[pscustomobject]@{ Success=$true; Evidence=$manifestPath; CpuFrameP95Milliseconds=$cpuP95; Image=$imagePath }
