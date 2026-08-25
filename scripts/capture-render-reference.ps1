param(
    [ValidateSet('Debug','Release')]
    [string]$Config = 'Release',
    [ValidateSet('direct3d12','vulkan')]
    [string]$GpuBackend = 'direct3d12',
    [string]$OutputRoot,
    [switch]$DisableAmbientOcclusion
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$runtime = Join-Path $repositoryRoot "build/windows-msvc-debug/src/runtime/$Config/noemancer.exe"
if (-not (Test-Path -LiteralPath $runtime -PathType Leaf)) {
    throw "Noemancer $Config runtime is missing. Build target noemancer through scripts/engine.ps1 first."
}
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $OutputRoot = Join-Path $repositoryRoot "generated/acceptance/raster-reference-$stamp"
}
$OutputRoot = [System.IO.Path]::GetFullPath($OutputRoot)
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
if (Get-ChildItem -LiteralPath $OutputRoot -Force | Select-Object -First 1) {
    throw "Reference evidence output must be empty because receipts are immutable: $OutputRoot"
}
$image = Join-Path $OutputRoot 'commercial-raster-v1-8.bmp'
$stdout = Join-Path $OutputRoot 'runtime.stdout.jsonl'
$stderr = Join-Path $OutputRoot 'runtime.stderr.log'
$performanceEvidence = Join-Path $OutputRoot 'performance-evidence.json'
$bloomEvidence = Join-Path $OutputRoot 'bloom-quality-evidence.json'
$colorContract = Join-Path $OutputRoot 'color-response-contract.json'
$colorEvidence = Join-Path $OutputRoot 'color-response-quality-evidence.json'
$pwsh = (Get-Command pwsh -ErrorAction Stop).Source
$bloomAnalyzer = Join-Path $PSScriptRoot 'analyze-bloom-quality.ps1'
$colorAnalyzer = Join-Path $PSScriptRoot 'analyze-color-response-quality.ps1'
$arguments = @(
    'run', '--format', 'json',
    '--reference-scene', 'commercial-raster-v1',
    '--capture-frame', $image,
    '--frames', '64',
    '--window-width', '1920', '--window-height', '1080',
    '--exposure', '1.0', '--render-scale', '1.0',
    '--gpu-backend', $GpuBackend
)
if ($DisableAmbientOcclusion) { $arguments += '--disable-ambient-occlusion' }
& $runtime @arguments 1> $stdout 2> $stderr
if ($LASTEXITCODE -ne 0) { throw "Reference capture failed with exit code $LASTEXITCODE. See $stderr" }
$sidecarPath = "$image.quality.json"
if (-not (Test-Path -LiteralPath $image -PathType Leaf) -or
    -not (Test-Path -LiteralPath $sidecarPath -PathType Leaf)) {
    throw 'Reference capture did not produce the image and quality sidecar.'
}
$sidecar = Get-Content -LiteralPath $sidecarPath -Raw | ConvertFrom-Json
if (-not $sidecar.pass -or -not $sidecar.dimensionsMatch -or
    $sidecar.referenceContract.id -ne 'noemancer.commercial-raster-reference/1.8' -or
    $sidecar.renderer.schemaVersion -ne 'noemancer.renderer-status.v27' -or
    $sidecar.renderer.graph.schemaVersion -ne 'noemancer.render-graph.v11' -or
    $sidecar.renderer.colorPipeline.toneMapper -ne 'ACES-RRT-ODT-fit/matrix' -or
    $sidecar.renderer.colorPipeline.toneMapInput -ne 'scene-linear-after-grading' -or
    $sidecar.renderer.colorPipeline.displayGamut -ne 'rec709-bounded' -or
    $sidecar.renderer.colorPipeline.outputEncoding -ne 'explicit-sRGB-transfer/RGBA8-UNORM' -or
    $sidecar.renderer.colorPipeline.ambientOcclusion.technique -ne 'eight-direction-horizon/separable-bilateral/indirect-only' -or
    [bool]$sidecar.renderer.colorPipeline.ambientOcclusion.enabled -eq [bool]$DisableAmbientOcclusion -or
    $sidecar.renderer.colorPipeline.bloom.technique -ne 'four-level-dual-filter' -or
    [int]$sidecar.renderer.colorPipeline.bloom.levels -ne 4 -or
    $sidecar.renderer.textureResources.schemaVersion -ne 'noemancer.texture-resource-table/0.1' -or
    [int]$sidecar.renderer.textureResources.pendingTransitions -ne 0 -or
    [int]$sidecar.renderer.builtInPrimitives.sphere.triangles -lt 1024 -or
    [int]$sidecar.renderer.importedTextures -lt 5 -or
    [int]$sidecar.renderer.materialFeatureCounts.normalMapped -lt 3 -or
    [int]$sidecar.renderer.materialFeatureCounts.metallicRoughnessMapped -lt 3 -or
    [int]$sidecar.renderer.materialFeatureCounts.occlusionMapped -lt 3 -or
    [int]$sidecar.renderer.materialFeatureCounts.emissiveMapped -lt 3 -or
    [int]$sidecar.renderer.materialFeatureCounts.alphaMasked -lt 1 -or
    [int]$sidecar.renderer.materialFeatureCounts.alphaBlended -lt 1 -or
    [int]$sidecar.renderer.materialFeatureCounts.doubleSided -lt 2 -or
    [int]$sidecar.renderer.shadow.cascadesAvailable -ne 4 -or
    [int]$sidecar.renderer.shadow.cascadesCached -ne 4 -or
    [long]$sidecar.renderer.shadow.cacheMissesTotal -lt 4 -or
    [long]$sidecar.renderer.shadow.cacheHitsTotal -lt 4 -or
    -not $sidecar.renderer.localShadow.enabled -or
    [int]$sidecar.renderer.localShadow.selectedLights -lt 2 -or
    [int]$sidecar.renderer.localShadow.pointLights -lt 1 -or
    [int]$sidecar.renderer.localShadow.spotLights -lt 1 -or
    [int]$sidecar.renderer.localShadow.facesAvailable -lt 7 -or
    [int]$sidecar.renderer.localShadow.facesCached -lt 7 -or
    [long]$sidecar.renderer.localShadow.cacheMissesTotal -lt 7 -or
    [long]$sidecar.renderer.localShadow.cacheHitsTotal -lt 7) {
    throw 'Reference capture failed its versioned quality contract.'
}
& $pwsh -NoLogo -NoProfile -File $bloomAnalyzer -ImagePath $image `
    -ReceiptPath $bloomEvidence -ExpectedWidth 1920 -ExpectedHeight 1080 | Out-Null
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $bloomEvidence -PathType Leaf)) {
    throw "Bloom quality analysis failed with exit code $LASTEXITCODE. See $bloomEvidence"
}
$bloom = Get-Content -LiteralPath $bloomEvidence -Raw | ConvertFrom-Json
if ($bloom.schemaVersion -ne 'noemancer.bloom-quality-evidence/0.1' -or -not $bloom.success) {
    throw 'Bloom quality evidence did not satisfy its versioned contract.'
}
$sidecar.referenceContract | ConvertTo-Json -Depth 32 | Set-Content -LiteralPath $colorContract -Encoding utf8NoBOM
& $pwsh -NoLogo -NoProfile -File $colorAnalyzer -ImagePath $image `
    -ContractPath $colorContract -OutputPath $colorEvidence | Out-Null
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $colorEvidence -PathType Leaf)) {
    throw "Color-response quality analysis failed with exit code $LASTEXITCODE. See $colorEvidence"
}
$color = Get-Content -LiteralPath $colorEvidence -Raw | ConvertFrom-Json
if ($color.schemaVersion -ne 'noemancer.color-response-quality-evidence/0.1' -or -not $color.success) {
    throw 'Color-response quality evidence did not satisfy its versioned contract.'
}
$performanceArguments = @(
    'run', '--format', 'json',
    '--reference-scene', 'commercial-raster-v1',
    '--performance-evidence', $performanceEvidence,
    '--performance-workload', 'noemancer.commercial-raster-reference/1.8',
    '--performance-warmup-frames', '32', '--performance-sample-frames', '120',
    '--window-width', '1920', '--window-height', '1080',
    '--exposure', '1.0', '--render-scale', '1.0',
    '--gpu-backend', $GpuBackend
)
if ($DisableAmbientOcclusion) { $performanceArguments += '--disable-ambient-occlusion' }
& $runtime @performanceArguments 1>> $stdout 2>> $stderr
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $performanceEvidence -PathType Leaf)) {
    throw "Reference performance run failed with exit code $LASTEXITCODE. See $stderr"
}
$performance = Get-Content -LiteralPath $performanceEvidence -Raw | ConvertFrom-Json
$cpuFrameP95 = [double]$performance.cpu.frameTime.p95
$cpuBudget = [double]$sidecar.referenceContract.quality.cpuFrameP95MillisecondsMax
if ($cpuFrameP95 -gt $cpuBudget) {
    throw "Reference CPU frame p95 $cpuFrameP95 ms exceeds the $cpuBudget ms contract."
}
$manifest = [ordered]@{
    schemaVersion = 'noemancer.render-reference-evidence/0.1'
    referenceId = $sidecar.referenceContract.id
    capturedAt = [DateTimeOffset]::UtcNow.ToString('o')
    configuration = $Config
    gpuBackend = $GpuBackend
    ambientOcclusionEnabled = -not [bool]$DisableAmbientOcclusion
    image = [System.IO.Path]::GetFileName($image)
    imageSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $image).Hash
    qualitySidecar = [System.IO.Path]::GetFileName($sidecarPath)
    qualitySidecarSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $sidecarPath).Hash
    bloomQualityEvidence = [System.IO.Path]::GetFileName($bloomEvidence)
    bloomQualityEvidenceSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $bloomEvidence).Hash
    colorResponseContract = [System.IO.Path]::GetFileName($colorContract)
    colorResponseContractSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $colorContract).Hash
    colorResponseQualityEvidence = [System.IO.Path]::GetFileName($colorEvidence)
    colorResponseQualityEvidenceSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $colorEvidence).Hash
    performanceEvidence = [System.IO.Path]::GetFileName($performanceEvidence)
    performanceEvidenceSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $performanceEvidence).Hash
    cpuFrameP95Milliseconds = $cpuFrameP95
    cpuFrameP95MillisecondsMax = $cpuBudget
    runtimeSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $runtime).Hash
    commands = @(
        [ordered]@{ purpose = 'golden-capture'; executable = $runtime; arguments = $arguments }
        [ordered]@{ purpose = 'bloom-quality-analysis'; executable = $pwsh; arguments = @('-NoLogo', '-NoProfile', '-File', $bloomAnalyzer, '-ImagePath', $image, '-ReceiptPath', $bloomEvidence, '-ExpectedWidth', 1920, '-ExpectedHeight', 1080) }
        [ordered]@{ purpose = 'color-response-quality-analysis'; executable = $pwsh; arguments = @('-NoLogo', '-NoProfile', '-File', $colorAnalyzer, '-ImagePath', $image, '-ContractPath', $colorContract, '-OutputPath', $colorEvidence) }
        [ordered]@{ purpose = 'cpu-performance'; executable = $runtime; arguments = $performanceArguments }
    )
    pass = $true
}
$manifestPath = Join-Path $OutputRoot 'reference-evidence.json'
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding utf8
[pscustomobject]@{
    Success = $true
    Evidence = $manifestPath
    Image = $image
    Quality = $sidecarPath
    BloomQuality = $bloomEvidence
    ColorResponseQuality = $colorEvidence
}
