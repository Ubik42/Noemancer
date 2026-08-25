[CmdletBinding()]
param(
    [string]$Project = $(if ($env:NOEMANCER_PLATFORMER_PROJECT) { $env:NOEMANCER_PLATFORMER_PROJECT } else { Join-Path ([IO.Path]::GetPathRoot($PSScriptRoot)) '3D\NoemancerPlatformer' }),
    [string]$Configuration = 'Release',
    [int]$BudgetKiB = 64,
    [int]$ResidentBudgetKiB = 8192,
    [int]$FinalFrames = 16
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$evidence = Join-Path $root "generated\acceptance\texture-streaming-$stamp"
$package = Join-Path $evidence 'package'
$runtime = Join-Path $root "build\windows-msvc-debug\src\runtime\$Configuration\noemancer.exe"
New-Item -ItemType Directory -Force -Path $evidence | Out-Null

& (Join-Path $PSScriptRoot 'engine.ps1') build -Target noemancer -Config $Configuration
if ($LASTEXITCODE -ne 0) { throw 'Runtime build failed.' }

$packageReceipt = Join-Path $evidence 'package-receipt.jsonl'
& $runtime package --project $Project --output $package --target-profile windows-x64-release --format json |
    Set-Content -LiteralPath $packageReceipt -Encoding utf8
if ($LASTEXITCODE -ne 0) { throw 'Platformer validation package failed.' }

$profile = Get-Content -LiteralPath (Join-Path $package 'config\game-profile.json') -Raw | ConvertFrom-Json
$player = Join-Path $package (Join-Path 'bin' ([string]$profile.executable))
if (-not (Test-Path -LiteralPath $player)) { throw "Packaged Player is missing: $player" }

function Invoke-ResidencyStage {
    param([string]$Backend, [string]$Stage, [int]$Frames)
    $image = Join-Path $evidence "lumen-run-$Backend-$Stage.bmp"
    $stdout = Join-Path $evidence "runtime-$Backend-$Stage.stdout.jsonl"
    $stderr = Join-Path $evidence "runtime-$Backend-$Stage.stderr.log"
    & $player --format json --frames $Frames --capture-frame $image --window-width 1920 --window-height 1080 `
        --gpu-backend $Backend --texture-streaming-budget-kib $BudgetKiB `
        --texture-streaming-resident-budget-kib $ResidentBudgetKiB 2> $stderr |
        Set-Content -LiteralPath $stdout -Encoding utf8
    if ($LASTEXITCODE -ne 0) { throw "Packaged Player $Stage stage failed on $Backend." }
    $event = Get-Content -LiteralPath $stdout | ForEach-Object {
        try { $_ | ConvertFrom-Json } catch { $null }
    } | Where-Object event -eq 'render.scene.final' | Select-Object -Last 1
    if (-not $event) { throw "Renderer Status is missing for $Backend/$Stage." }
    $renderer = $event.message | ConvertFrom-Json
    $residency = $renderer.textureResidency
    $spriteResources = @($renderer.textureResources.resources | Where-Object owner -eq 'scene.sprite')
    if ($renderer.schemaVersion -ne 'noemancer.renderer-status.v27' -or
        $renderer.textureResources.schemaVersion -ne 'noemancer.texture-resource-table/0.1' -or
        $renderer.textureResources.pendingTransitions -ne 0 -or
        $residency.schemaVersion -ne 'noemancer.texture-residency/0.3' -or
        -not $residency.asyncFrameStreaming -or $residency.ktxTextures -lt 2 -or
        $residency.nativeCompressedTextures -lt 2 -or $residency.rgba8FallbackTextures -ne 0 -or
        $spriteResources.Count -lt 2 -or
        $renderer.sprites.missingTextureDraws -ne 0) {
        throw "Texture streaming base contract failed on $Backend/$Stage."
    }
    if ($Stage -eq 'early' -and
        ($residency.authoredMipLevelsUploaded -ge $residency.authoredMipLevelsTotal -or
         $residency.gpuAllocationBytesEstimate -ge $residency.fullChainBytesEstimate)) {
        throw "Early visibility did not preserve a partial residency state on $Backend."
    }
    if ($Stage -eq 'complete' -and
        ($residency.pendingLevels -ne 0 -or $residency.completedStreams -ne $residency.ktxTextures -or
         @($spriteResources | Where-Object resourceGeneration -gt 1).Count -lt 1)) {
        throw "Texture streams did not converge within $Frames frames on $Backend."
    }
    return [ordered]@{
        stage = $Stage
        frames = $Frames
        image = Split-Path -Leaf $image
        imageSha256 = (Get-FileHash -LiteralPath $image -Algorithm SHA256).Hash
        runtimeLog = Split-Path -Leaf $stdout
        ktxTextures = $residency.ktxTextures
        authoredMipLevelsTotal = $residency.authoredMipLevelsTotal
        authoredMipLevelsUploaded = $residency.authoredMipLevelsUploaded
        pendingLevels = $residency.pendingLevels
        completedStreams = $residency.completedStreams
        residentMipStarts = @($residency.streams | ForEach-Object residentMipStart)
        targetMipStarts = @($residency.streams | ForEach-Object targetMipStart)
        resourceGenerations = @($spriteResources | ForEach-Object resourceGeneration)
        gpuAllocationBytesEstimate = $residency.gpuAllocationBytesEstimate
        uploadedCopyBytesTotal = $residency.uploadedCopyBytesTotal
        missingTextureDraws = $renderer.sprites.missingTextureDraws
    }
}

function Invoke-PressureStage {
    param([string]$Backend)
    $pressureFrames = 72
    $pressureResidentKiB = 2300
    $image = Join-Path $evidence "lumen-run-$Backend-pressure.bmp"
    $stdout = Join-Path $evidence "runtime-$Backend-pressure.stdout.jsonl"
    $stderr = Join-Path $evidence "runtime-$Backend-pressure.stderr.log"
    & $player --format json --frames $pressureFrames --capture-frame $image --window-width 1920 --window-height 1080 `
        --gpu-backend $Backend --texture-streaming-budget-kib $BudgetKiB `
        --texture-streaming-resident-budget-kib $pressureResidentKiB `
        --texture-streaming-workload noemancer.texture-streaming.pressure/0.1 2> $stderr |
        Set-Content -LiteralPath $stdout -Encoding utf8
    if ($LASTEXITCODE -ne 0) { throw "Packaged Player pressure stage failed on $Backend." }
    $event = Get-Content -LiteralPath $stdout | ForEach-Object {
        try { $_ | ConvertFrom-Json } catch { $null }
    } | Where-Object event -eq 'render.scene.final' | Select-Object -Last 1
    if (-not $event) { throw "Renderer pressure status is missing for $Backend." }
    $renderer = $event.message | ConvertFrom-Json
    $residency = $renderer.textureResidency
    $spriteResources = @($renderer.textureResources.resources | Where-Object owner -eq 'scene.sprite')
    if ($renderer.schemaVersion -ne 'noemancer.renderer-status.v27' -or
        $renderer.textureResources.schemaVersion -ne 'noemancer.texture-resource-table/0.1' -or
        $renderer.textureResources.pendingTransitions -ne 0 -or
        $residency.schemaVersion -ne 'noemancer.texture-residency/0.3' -or
        $residency.workload -ne 'noemancer.texture-streaming.pressure/0.1' -or
        $residency.evictionsTotal -lt 1 -or $residency.reuploadsTotal -lt 1 -or
        $spriteResources.Count -lt 2 -or @($spriteResources | Where-Object resourceGeneration -gt 1).Count -lt 1 -or
        $residency.gpuAllocationBytesEstimate -gt ($pressureResidentKiB * 1024) -or
        $residency.overBudget -or $renderer.sprites.missingTextureDraws -ne 0) {
        throw "Texture pressure/eviction contract failed on $Backend."
    }
    return [ordered]@{
        frames = $pressureFrames
        residentBudgetBytes = $pressureResidentKiB * 1024
        image = Split-Path -Leaf $image
        imageSha256 = (Get-FileHash -LiteralPath $image -Algorithm SHA256).Hash
        runtimeLog = Split-Path -Leaf $stdout
        gpuAllocationBytesEstimate = $residency.gpuAllocationBytesEstimate
        evictionsTotal = $residency.evictionsTotal
        reuploadsTotal = $residency.reuploadsTotal
        resourceGenerations = @($spriteResources | ForEach-Object resourceGeneration)
        bytesReleasedTotalEstimate = $residency.bytesReleasedTotalEstimate
        missingTextureDraws = $renderer.sprites.missingTextureDraws
    }
}

$backendEvidence = @()
foreach ($backend in @('direct3d12', 'vulkan')) {
    $backendEvidence += [ordered]@{
        backend = $backend
        early = Invoke-ResidencyStage -Backend $backend -Stage early -Frames 1
        complete = Invoke-ResidencyStage -Backend $backend -Stage complete -Frames $FinalFrames
        pressure = Invoke-PressureStage -Backend $backend
    }
}

$manifest = [ordered]@{
    schemaVersion = 'noemancer.texture-streaming-evidence/0.3'
    capturedAt = [DateTimeOffset]::UtcNow.ToString('o')
    configuration = $Configuration
    project = (Resolve-Path -LiteralPath $Project).Path
    package = 'package'
    runtimeSha256 = (Get-FileHash -LiteralPath $runtime -Algorithm SHA256).Hash
    packageReceipt = Split-Path -Leaf $packageReceipt
    policy = [ordered]@{
        artifact = 'KTX2/Basis'
        formatSelection = 'SDL_GPU device query; BC7 then RGBA8'
        uploadOrder = 'initial authored tail, then transaction-safe one-mip physical tier transitions'
        visibility = 'tail visible immediately; authored resident mip is rebased to GPU mip zero'
        scheduler = 'screen footprint, importance, authored priority, visibility aging, hysteresis and resident budget'
        budgetBytesPerFrame = $BudgetKiB * 1024
        residentBudgetBytes = $ResidentBudgetKiB * 1024
        asyncFrameStreaming = $true
    }
    backends = $backendEvidence
    pass = $true
}
$manifestPath = Join-Path $evidence 'texture-streaming-evidence.json'
$manifest | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $manifestPath -Encoding utf8
[pscustomobject]@{ Success = $true; Evidence = $manifestPath; Package = $package }
