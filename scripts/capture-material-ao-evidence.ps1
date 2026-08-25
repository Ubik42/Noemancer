param(
    [ValidateSet('Debug','Release')]
    [string]$Config = 'Release',
    [ValidateSet('direct3d12','vulkan')]
    [string]$GpuBackend = 'direct3d12',
    [string]$OutputRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-FileSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Evidence file is missing: $Path" }
    $hash = [string](Get-FileHash -LiteralPath $Path -Algorithm SHA256 -ErrorAction Stop).Hash
    if ($hash -notmatch '^[0-9A-Fa-f]{64}$') { throw "Invalid SHA-256 digest for '$Path'." }
    return $hash.ToLowerInvariant()
}

function Get-JsonField {
    param(
        [Parameter(Mandatory = $true)]$Object,
        [Parameter(Mandatory = $true)][string]$Path
    )

    $current = $Object
    foreach ($part in $Path.Split('.')) {
        if ($null -eq $current) { throw "JSON field '$Path' is missing." }
        $property = $current.PSObject.Properties[$part]
        if ($null -eq $property) { throw "JSON field '$Path' is missing." }
        $current = $property.Value
    }
    return $current
}

function Assert-Sidecar {
    param(
        [Parameter(Mandatory = $true)]$Sidecar,
        [Parameter(Mandatory = $true)][string]$Label
    )

    $pass = Get-JsonField -Object $Sidecar -Path 'pass'
    $dimensionsMatch = Get-JsonField -Object $Sidecar -Path 'dimensionsMatch'
    if ($pass -isnot [bool] -or -not $pass -or $dimensionsMatch -isnot [bool] -or -not $dimensionsMatch) {
        throw "$Label sidecar did not pass its image/dimension contract."
    }
    if ((Get-JsonField -Object $Sidecar -Path 'referenceContract.id') -isnot [string] -or
        (Get-JsonField -Object $Sidecar -Path 'referenceContract.id') -cne 'noemancer.commercial-raster-reference/1.8') {
        throw "$Label sidecar has the wrong reference contract id."
    }
    if ((Get-JsonField -Object $Sidecar -Path 'renderer.schemaVersion') -cne 'noemancer.renderer-status.v26' -or
        (Get-JsonField -Object $Sidecar -Path 'renderer.graph.schemaVersion') -cne 'noemancer.render-graph.v11' -or
        (Get-JsonField -Object $Sidecar -Path 'renderer.colorPipeline.ambientOcclusion.technique') -cne 'eight-direction-horizon/separable-bilateral/indirect-only') {
        throw "$Label sidecar did not satisfy the renderer/AO version contract."
    }
    $artifactContract = Get-JsonField -Object $Sidecar -Path 'renderer.device.artifactContract'
    if ((Get-JsonField -Object $artifactContract -Path 'code') -cne 'ok' -or
        (Get-JsonField -Object $artifactContract -Path 'schema') -cne 'noemancer.shader-artifact-manifest/0.1') {
        throw "$Label sidecar has an invalid shader artifact contract."
    }
    $manifestHash = [string](Get-JsonField -Object $artifactContract -Path 'manifestHash')
    if ($manifestHash -notmatch '^sha256:[0-9A-Fa-f]{64}$') {
        throw "$Label sidecar has an invalid shader manifest hash."
    }
    return $manifestHash.ToLowerInvariant()
}

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$runtime = Join-Path $repositoryRoot "build/windows-msvc-debug/src/runtime/$Config/noemancer.exe"
$analyzer = Join-Path $PSScriptRoot 'analyze-material-ao-quality.ps1'
$pwshCommand = Get-Command pwsh -CommandType Application -ErrorAction Stop | Select-Object -First 1
$pwsh = [string]$pwshCommand.Source
if ([string]::IsNullOrWhiteSpace($pwsh)) { $pwsh = [string]$pwshCommand.Path }
if ([string]::IsNullOrWhiteSpace($pwsh)) { throw 'Unable to resolve the pwsh executable path.' }
if (-not (Test-Path -LiteralPath $runtime -PathType Leaf)) {
    throw "Noemancer $Config runtime is missing. Build target noemancer through scripts/engine.ps1 first."
}
if (-not (Test-Path -LiteralPath $analyzer -PathType Leaf)) {
    throw "Material/AO analyzer is missing: $analyzer"
}
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $OutputRoot = Join-Path $repositoryRoot "generated/acceptance/material-ao-$GpuBackend-$stamp"
}
$OutputRoot = [System.IO.Path]::GetFullPath($OutputRoot)
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
if (Get-ChildItem -LiteralPath $OutputRoot -Force | Select-Object -First 1) {
    throw "Material/AO evidence output must be empty because receipts are immutable: $OutputRoot"
}

$enabledImage = Join-Path $OutputRoot 'ao-enabled.bmp'
$disabledImage = Join-Path $OutputRoot 'ao-disabled.bmp'
$contractPath = Join-Path $OutputRoot 'material-ao-contract.json'
$receiptPath = Join-Path $OutputRoot 'material-ao-quality-evidence.json'
$stdout = Join-Path $OutputRoot 'runtime.stdout.jsonl'
$stderr = Join-Path $OutputRoot 'runtime.stderr.log'
$commonArguments = @(
    'run', '--format', 'json', '--reference-scene', 'commercial-raster-v1',
    '--frames', '64', '--window-width', '1920', '--window-height', '1080',
    '--exposure', '1.0', '--render-scale', '1.0', '--gpu-backend', $GpuBackend
)
$enabledArguments = $commonArguments + @('--capture-frame', $enabledImage)
$disabledArguments = $commonArguments + @('--capture-frame', $disabledImage, '--disable-ambient-occlusion')

& $runtime @enabledArguments 1> $stdout 2> $stderr
if ($LASTEXITCODE -ne 0) { throw "AO-enabled capture failed with exit code $LASTEXITCODE. See $stderr" }
& $runtime @disabledArguments 1>> $stdout 2>> $stderr
if ($LASTEXITCODE -ne 0) { throw "AO-disabled capture failed with exit code $LASTEXITCODE. See $stderr" }

$enabledSidecarPath = "$enabledImage.quality.json"
$disabledSidecarPath = "$disabledImage.quality.json"
foreach ($path in @($enabledImage,$disabledImage,$enabledSidecarPath,$disabledSidecarPath)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Material/AO capture output is missing: $path" }
}
$enabledSidecar = Get-Content -LiteralPath $enabledSidecarPath -Raw | ConvertFrom-Json
$disabledSidecar = Get-Content -LiteralPath $disabledSidecarPath -Raw | ConvertFrom-Json
foreach ($sidecar in @($enabledSidecar,$disabledSidecar)) {
    if (-not $sidecar.pass -or -not $sidecar.dimensionsMatch -or
        $sidecar.referenceContract.id -ne 'noemancer.commercial-raster-reference/1.8' -or
        $sidecar.renderer.schemaVersion -ne 'noemancer.renderer-status.v26' -or
        $sidecar.renderer.graph.schemaVersion -ne 'noemancer.render-graph.v11' -or
        $sidecar.renderer.colorPipeline.ambientOcclusion.technique -ne 'eight-direction-horizon/separable-bilateral/indirect-only') {
        throw 'Material/AO capture did not satisfy the current renderer/reference contract.'
    }
}
if (-not [bool]$enabledSidecar.renderer.colorPipeline.ambientOcclusion.enabled -or
    [bool]$disabledSidecar.renderer.colorPipeline.ambientOcclusion.enabled) {
    throw 'Material/AO A/B captures did not preserve the requested AO states.'
}
$enabledSidecar.referenceContract | ConvertTo-Json -Depth 32 | Set-Content -LiteralPath $contractPath -Encoding utf8NoBOM

& $pwsh -NoLogo -NoProfile -File $analyzer -AoEnabledImage $enabledImage `
    -AoDisabledImage $disabledImage -ContractPath $contractPath -OutputPath $receiptPath | Out-Null
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $receiptPath -PathType Leaf)) {
    throw "Material/AO quality analysis failed with exit code $LASTEXITCODE. See $receiptPath"
}
$receipt = Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json
if ($receipt.schemaVersion -ne 'noemancer.material-ao-quality-evidence/0.1' -or -not $receipt.success) {
    throw 'Material/AO analyzer receipt did not satisfy its versioned contract.'
}

$manifest = [ordered]@{
    schemaVersion = 'noemancer.material-ao-capture-evidence/0.1'
    capturedAt = [DateTimeOffset]::UtcNow.ToString('o')
    configuration = $Config
    gpuBackend = $GpuBackend
    referenceId = $enabledSidecar.referenceContract.id
    enabledImage = [System.IO.Path]::GetFileName($enabledImage)
    enabledImageSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $enabledImage).Hash.ToLowerInvariant()
    disabledImage = [System.IO.Path]::GetFileName($disabledImage)
    disabledImageSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $disabledImage).Hash.ToLowerInvariant()
    qualityEvidence = [System.IO.Path]::GetFileName($receiptPath)
    qualityEvidenceSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $receiptPath).Hash.ToLowerInvariant()
    rendererSchemaVersion = $enabledSidecar.renderer.schemaVersion
    graphSchemaVersion = $enabledSidecar.renderer.graph.schemaVersion
    shaderManifestHash = $enabledSidecar.renderer.device.artifactContract.manifestHash
    commands = @(
        [ordered]@{ purpose = 'ao-enabled'; executable = $runtime; arguments = $enabledArguments }
        [ordered]@{ purpose = 'ao-disabled'; executable = $runtime; arguments = $disabledArguments }
        [ordered]@{ purpose = 'quality-analysis'; executable = $pwsh; arguments = @('-NoLogo','-NoProfile','-File',$analyzer,'-AoEnabledImage',$enabledImage,'-AoDisabledImage',$disabledImage,'-ContractPath',$contractPath,'-OutputPath',$receiptPath) }
    )
}
$manifestPath = Join-Path $OutputRoot 'material-ao-evidence.json'
$manifest | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $manifestPath -Encoding utf8NoBOM
Write-Output ($manifest | ConvertTo-Json -Depth 16 -Compress)
