[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$docsRoot = Join-Path $projectRoot 'docs'
$errors = [System.Collections.Generic.List[string]]::new()

function Add-AuditError([string]$Message) {
    $errors.Add($Message)
}

$statePath = Join-Path $docsRoot 'current-state.json'
try {
    $state = Get-Content -LiteralPath $statePath -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($state.schemaVersion -ne 'noemancer.project-state/0.2') { Add-AuditError 'Unexpected project-state schemaVersion.' }
    if (-not $state.historyIsNonAuthoritative) { Add-AuditError 'Historical documents must be explicitly non-authoritative.' }
    if ($state.developmentMode.protocol -ne 'codex-goal-batched') { Add-AuditError 'Current development protocol must remain explicit.' }
    if ($state.developmentMode.delegation -ne 'proactive-up-to-three-luna-workers-for-independent-non-overlapping-lanes') {
        Add-AuditError 'Delegation must remain proactive, bounded, and orchestrator-owned.'
    }
    if ($state.developmentMode.integration -ne 'orchestrator-owned') { Add-AuditError 'Canonical integration must remain orchestrator-owned.' }
    foreach ($property in $state.authority.PSObject.Properties) {
        $authorityPath = Join-Path $projectRoot $property.Value
        if (-not (Test-Path -LiteralPath $authorityPath)) { Add-AuditError "Missing authority target: $($property.Value)" }
    }
} catch {
    Add-AuditError "current-state.json is invalid: $($_.Exception.Message)"
}

$lineBudgets = @{
    'docs/architecture.md' = 180
    'docs/development-plan.zh-CN.md' = 180
    'docs/first-acceptance-status.zh-CN.md' = 100
}
foreach ($entry in $lineBudgets.GetEnumerator()) {
    $path = Join-Path $projectRoot $entry.Key
    $lineCount = (Get-Content -LiteralPath $path -Encoding UTF8 | Measure-Object -Line).Lines
    if ($lineCount -gt $entry.Value) { Add-AuditError "$($entry.Key) exceeds its $($entry.Value)-line authority budget ($lineCount)." }
}

foreach ($research in Get-ChildItem (Join-Path $docsRoot 'research') -File -Filter '*.md') {
    if ($research.Name -eq 'README.md') { continue }
    $header = (Get-Content -LiteralPath $research.FullName -Encoding UTF8 | Select-Object -First 5) -join ' '
    if ($header -notmatch 'Historical|Superseded') { Add-AuditError "Research document lacks a historical marker: $($research.Name)" }
}

$authoritativeFiles = @(
    (Join-Path $projectRoot 'README.md'),
    (Join-Path $projectRoot 'AGENTS.md'),
    (Join-Path $docsRoot 'README.md'),
    (Join-Path $docsRoot 'architecture.md'),
    (Join-Path $docsRoot 'development-plan.zh-CN.md'),
    (Join-Path $docsRoot 'first-acceptance-status.zh-CN.md')
)
$retiredPhrases = @('下一主批进入 HostFXR', 'SDL AudioStream', 'core <- world <- runtime/editor/devtools`.', 'E2：真正的开发者脚本层（当前）')
foreach ($file in $authoritativeFiles) {
    $text = Get-Content -LiteralPath $file -Raw -Encoding UTF8
    foreach ($phrase in $retiredPhrases) {
        if ($text.Contains($phrase)) { Add-AuditError "Retired guidance remains in ${file}: $phrase" }
    }
}

$markdownFiles = @((Get-Item (Join-Path $projectRoot 'README.md')), (Get-Item (Join-Path $projectRoot 'AGENTS.md')))
$markdownFiles += Get-ChildItem $docsRoot -Recurse -File -Filter '*.md'
foreach ($file in $markdownFiles) {
    $text = Get-Content -LiteralPath $file.FullName -Raw -Encoding UTF8
    foreach ($match in [regex]::Matches($text, '\[[^\]]+\]\(([^)]+)\)')) {
        $link = $match.Groups[1].Value
        if ($link -match '^(https?://|mailto:|#)') { continue }
        $relativePath = ($link -split '#')[0]
        if ([string]::IsNullOrWhiteSpace($relativePath)) { continue }
        if (-not (Test-Path -LiteralPath (Join-Path $file.DirectoryName $relativePath))) {
            Add-AuditError "Broken local link in $($file.FullName): $link"
        }
    }
}

if ($errors.Count -gt 0) {
    $errors | ForEach-Object { Write-Error $_ }
    exit 2
}

Write-Output 'Governance audit passed: authorities, history markers, line budgets, retired guidance, and local links.'
