[CmdletBinding()]
param(
    [string]$OutputRoot
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$manifestPath = Join-Path $repositoryRoot 'assets/local-test/mixamo/manifest.json'
$manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
$fixture = $manifest.assets | Where-Object { $_.path -eq 'rumba-dancing-02.fbx' } | Select-Object -First 1
if ($null -eq $fixture) {
    throw 'The local Mixamo manifest does not declare rumba-dancing-02.fbx.'
}
$fixturePath = Join-Path (Split-Path -Parent $manifestPath) $fixture.path
if (-not (Test-Path -LiteralPath $fixturePath -PathType Leaf)) {
    throw "The optional local Mixamo fixture is unavailable: $fixturePath"
}
$fixtureInfo = Get-Item -LiteralPath $fixturePath
if ($fixtureInfo.Length -ne [int64]$fixture.bytes) {
    throw "The local Mixamo fixture byte count does not match its manifest: $($fixtureInfo.Length) != $($fixture.bytes)."
}
$fixtureSha256 = (Get-FileHash -LiteralPath $fixturePath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($fixtureSha256 -ne [string]$fixture.sha256) {
    throw 'The local Mixamo fixture SHA-256 does not match its manifest.'
}

if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $repositoryRoot ('generated/acceptance/animation-compression-' +
        (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
$OutputRoot = [System.IO.Path]::GetFullPath($OutputRoot)
New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null
$evidencePath = Join-Path $OutputRoot 'animation-compression-evidence.json'

& (Join-Path $repositoryRoot 'scripts/engine.ps1') build -Config Release `
    -Target noemancer_animation_compression_evidence_tests
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$executable = Join-Path $repositoryRoot `
    'build/windows-msvc-debug/tests/Release/noemancer_animation_compression_evidence_tests.exe'
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "The Release animation compression evidence executable is missing: $executable"
}

$previousEvidencePath = $env:NOEMANCER_ANIMATION_COMPRESSION_EVIDENCE_PATH
try {
    $env:NOEMANCER_ANIMATION_COMPRESSION_EVIDENCE_PATH = $evidencePath
    & $executable
    $probeExitCode = $LASTEXITCODE
} finally {
    $env:NOEMANCER_ANIMATION_COMPRESSION_EVIDENCE_PATH = $previousEvidencePath
}
if ($probeExitCode -eq 77) {
    throw 'The animation compression evidence fixture became unavailable after manifest validation.'
}
if ($probeExitCode -ne 0) { exit $probeExitCode }
if (-not (Test-Path -LiteralPath $evidencePath -PathType Leaf)) {
    throw 'The animation compression evidence executable did not write its JSON artifact.'
}

$evidence = Get-Content -LiteralPath $evidencePath -Raw -Encoding UTF8 | ConvertFrom-Json
if ($evidence.schemaVersion -ne 'noemancer.animation-compression-evidence/0.1') {
    throw "Unexpected animation compression evidence schema: $($evidence.schemaVersion)"
}
if (-not $evidence.pass) {
    throw 'The animation compression evidence contract failed.'
}
$baselineCompression = $evidence.compile.baseline.compression
$candidateCompression = $evidence.compile.candidate.compression
$runtimeArchiveReduction = if ([double]$baselineCompression.selectedRuntimeArchiveBytes -gt 0.0) {
    1.0 - ([double]$candidateCompression.selectedRuntimeArchiveBytes /
        [double]$baselineCompression.selectedRuntimeArchiveBytes)
} else { 0.0 }
$candidateP95Ratio = if ([double]$evidence.timing.baseline.p95 -gt 0.0) {
    [double]$evidence.timing.candidate.p95 / [double]$evidence.timing.baseline.p95
} else { 0.0 }
$evidence | Add-Member -NotePropertyName source -NotePropertyValue ([ordered]@{
    manifest = 'assets/local-test/mixamo/manifest.json'
    path = 'assets/local-test/mixamo/rumba-dancing-02.fbx'
    bytes = [int64]$fixture.bytes
    sha256 = 'sha256:' + $fixtureSha256
    redistribution = 'local-only; evidence input is not packaged'
}) -Force
$evidence | Add-Member -NotePropertyName decision -NotePropertyValue ([ordered]@{
    productionDefault = 'ozz_runtime_baseline'
    hierarchicalKeyReductionPromoted = $false
    runtimeArchiveReductionFraction = $runtimeArchiveReduction
    candidateSamplingP95Ratio = $candidateP95Ratio
    rationale = 'Candidate key reduction is diagnostic-only until it proves smaller runtime archives and no sampling regression on representative clips.'
    nextCompressionCandidate = 'ACL only after a pinned-license same-contract A/B; not an established dependency.'
}) -Force
$sourceRevision = (& git -C $repositoryRoot rev-parse HEAD).Trim()
$sourceDirty = [bool](& git -C $repositoryRoot status --porcelain)
$evidence | Add-Member -NotePropertyName provenance -NotePropertyValue ([ordered]@{
    sourceRevision = $sourceRevision
    sourceDirty = $sourceDirty
    configuration = 'Release'
    fixtureManifest = 'assets/local-test/mixamo/manifest.json'
    fixtureSha256 = 'sha256:' + $fixtureSha256
    generationCommand = 'scripts/measure-animation-compression.ps1'
}) -Force
$evidence | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $evidencePath -Encoding UTF8

Write-Output $evidencePath
