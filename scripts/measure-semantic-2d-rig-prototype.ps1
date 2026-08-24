[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Debug',
    [string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $projectRoot 'generated\acceptance\semantic-2d-rig-prototype-current'
}
$engineScript = Join-Path $PSScriptRoot 'engine.ps1'
& $engineScript check -Config $Config `
    -Target noemancer_semantic_2d_rig_prototype_tests `
    -TestRegex '^noemancer\.semantic_2d_rig_prototype$'
if ($LASTEXITCODE -ne 0) { throw 'Semantic 2D Rig prototype build/test gate failed.' }

$testExecutable = Join-Path $projectRoot "build\windows-msvc-debug\tests\$Config\noemancer_semantic_2d_rig_prototype_tests.exe"
if (-not (Test-Path -LiteralPath $testExecutable -PathType Leaf)) {
    throw "Prototype test executable is missing: $testExecutable"
}
$lines = @(& $testExecutable 2>&1)
if ($LASTEXITCODE -ne 0) { throw ($lines -join [Environment]::NewLine) }
$jsonLine = $lines | Where-Object { $_ -is [string] -and $_.TrimStart().StartsWith('{') } | Select-Object -Last 1
if (-not $jsonLine) { throw 'Prototype test did not publish a JSON receipt.' }
$receipt = $jsonLine | ConvertFrom-Json
if (-not $receipt.success) { throw 'Prototype receipt did not report success.' }

$receipt | Add-Member -NotePropertyName buildConfig -NotePropertyValue $Config
$receipt | Add-Member -NotePropertyName revision -NotePropertyValue ((& git -C $projectRoot rev-parse HEAD).Trim())
$receipt | Add-Member -NotePropertyName fixtureRoot -NotePropertyValue 'tests/fixtures/semantic-2d-rig'
$receipt | Add-Member -NotePropertyName prototypeBoundary -NotePropertyValue `
    'Cook-side composition into Sprite 0.2 clips; no Runtime schema and no second animation runtime.'

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$target = Join-Path $OutputDirectory 'semantic-2d-rig-prototype-receipt.json'
$temporary = "$target.tmp"
$receipt | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $temporary -Encoding utf8NoBOM
Move-Item -LiteralPath $temporary -Destination $target -Force
Write-Output $target
