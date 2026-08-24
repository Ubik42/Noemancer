[CmdletBinding()]
param(
    [string]$ProjectRoot = $(if ($env:NOEMANCER_PLATFORMER_PROJECT) { $env:NOEMANCER_PLATFORMER_PROJECT } else { Join-Path ([IO.Path]::GetPathRoot($PSScriptRoot)) '3D\NoemancerPlatformer' }),
    [string]$AcceptancePath = '',
    [ValidateSet('Debug', 'Release')][string]$Config = 'Debug',
    [string]$OutputRoot = ''
)

# Hidden acceptance only: the executable owns ProjectHybridPixelAuthoring and
# HybridPixelProfilePanel. This script owns the disposable copy and integrity
# boundary; it never opens the Editor or edits the source project.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$engine = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$issues = [System.Collections.Generic.List[object]]::new()
function Full([string]$p) { [IO.Path]::GetFullPath($p) }
function Hash([string]$p) { (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.ToLowerInvariant() }
function TextHash([string]$text) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { ([BitConverter]::ToString($sha.ComputeHash([Text.UTF8Encoding]::new($false).GetBytes($text)))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}
function TreeHash([string]$root) {
    $base = (Full $root).TrimEnd('\') + '\'; $rows = [System.Collections.Generic.List[string]]::new()
    foreach ($file in (Get-ChildItem -LiteralPath $root -File -Recurse -Force | Sort-Object FullName)) {
        if ($file.FullName -match '\\.git\\|\\bin\\|\\obj\\|\\generated\\') { continue }
        [void]$rows.Add($file.FullName.Substring($base.Length).Replace('\', '/') + "`t" + (Hash $file.FullName))
    }
    TextHash (($rows -join "`n") + "`n")
}
function Add-Issue([string]$code, [string]$message) { [void]$issues.Add([ordered]@{ code = $code; message = $message }) }
function Write-Json([string]$path, $value) {
    [IO.Directory]::CreateDirectory((Split-Path -Parent $path)) | Out-Null
    [IO.File]::WriteAllText($path, (($value | ConvertTo-Json -Depth 100) + "`n"), [Text.UTF8Encoding]::new($false))
}
function Run([string]$file, [string[]]$arguments, [string]$work) {
    Push-Location $work
    try {
        $text = (& $file @arguments 2>&1 | Out-String)
        [pscustomobject]@{ exitCode = $LASTEXITCODE; stdout = $text; stderr = '' }
    } finally { Pop-Location }
}
function Copy-Project([string]$source, [string]$destination) {
    [IO.Directory]::CreateDirectory($destination) | Out-Null
    foreach ($entry in (Get-ChildItem -LiteralPath $source -Force)) {
        if ($entry.Name -in @('.git', 'bin', 'obj', 'generated')) { continue }
        Copy-Item -LiteralPath $entry.FullName -Destination (Join-Path $destination $entry.Name) -Recurse -Force
    }
}

$success = $false; $exitCode = 3; $output = ''; $stage = ''; $sourceManifest = ''
$beforeManifestHash = ''; $afterManifestHash = ''; $beforeTreeHash = ''; $afterTreeHash = ''
$acceptance = $null; $acceptanceReceipt = $null
try {
    $source = Full $ProjectRoot; $sourceManifest = Join-Path $source 'noemancer.project.json'
    if (-not (Test-Path -LiteralPath $source -PathType Container) -or -not (Test-Path -LiteralPath $sourceManifest -PathType Leaf)) {
        Add-Issue 'invocation.project-missing' "Project or manifest does not exist: $source"; $exitCode = 2
    } else {
        $beforeManifestHash = Hash $sourceManifest; $beforeTreeHash = TreeHash $source
        if ([string]::IsNullOrWhiteSpace($OutputRoot)) { $output = Join-Path $engine 'generated/acceptance/hybrid-pixel-profile-authoring' } else { $output = Full $OutputRoot }
        [IO.Directory]::CreateDirectory($output) | Out-Null
        $stage = Join-Path $output 'project'
        if (Test-Path -LiteralPath $stage) { $stage = Join-Path $output ('project-' + [Guid]::NewGuid().ToString('N')) }
        Copy-Project $source $stage
        $stageManifest = Join-Path $stage 'noemancer.project.json'
        $manifest = Get-Content -LiteralPath $stageManifest -Raw | ConvertFrom-Json -Depth 100
        $inputBefore = ($manifest.inputActions | ConvertTo-Json -Depth 100 -Compress)
        if (-not $manifest.PSObject.Properties['futureProjectField']) { Add-Member -InputObject $manifest -MemberType NoteProperty -Name futureProjectField -Value ([ordered]@{ preserve = $true; owner = 'hybrid-profile-authoring-acceptance' }) }
        $unknownBefore = ($manifest.futureProjectField | ConvertTo-Json -Depth 100 -Compress)
        $manifest.schema = 'noemancer.project/0.2'
        $profileFixture = [ordered]@{ schema = 'noemancer.hybrid-pixel-profile/0.1'; profileId = 'hybrid.fixture'; enabled = $true; virtualWidth = 320; virtualHeight = 180; pixelsPerUnit = 16.0; integerScaling = $true; snapCamera = $true; snapSprites = $true; presentationFilter = 'nearest' }
        if ($manifest.PSObject.Properties['hybridPixelProfile']) { $manifest.hybridPixelProfile = $profileFixture }
        else { Add-Member -InputObject $manifest -MemberType NoteProperty -Name hybridPixelProfile -Value $profileFixture }
        Write-Json $stageManifest $manifest
        $staged = Get-Content -LiteralPath $stageManifest -Raw | ConvertFrom-Json -Depth 100
        $fixturePass = [string]$staged.schema -eq 'noemancer.project/0.2' -and ($staged.inputActions | ConvertTo-Json -Depth 100 -Compress) -eq $inputBefore -and ($staged.futureProjectField | ConvertTo-Json -Depth 100 -Compress) -eq $unknownBefore
        if (-not $fixturePass) { Add-Issue 'fixture.preservation-failed' 'Staging profile injection changed inputActions or futureProjectField.' }

        if ([string]::IsNullOrWhiteSpace($AcceptancePath)) {
            $leaf = if ($Config -eq 'Debug') { 'Debug' } else { 'Release' }
            $dir = Join-Path $engine ("build/windows-msvc-debug/tests/{0}" -f $leaf)
            $candidates = @('noemancer_hybrid_pixel_profile_authoring_acceptance.exe', 'hybrid_pixel_profile_authoring_acceptance.exe')
            foreach ($name in $candidates) { $candidate = Join-Path $dir $name; if (Test-Path -LiteralPath $candidate -PathType Leaf) { $AcceptancePath = $candidate; break } }
        }
        if ([string]::IsNullOrWhiteSpace($AcceptancePath) -or -not (Test-Path -LiteralPath $AcceptancePath -PathType Leaf)) {
            Add-Issue 'acceptance.target-missing' 'The built hybrid_pixel_profile_authoring_acceptance target is unavailable.'
        } else {
            $receiptPath = Join-Path $output 'acceptance-receipt.json'
            $acceptance = Run -file (Full $AcceptancePath) -arguments @('--project-root', $stage, '--receipt', $receiptPath) -work $engine
            if (Test-Path -LiteralPath $receiptPath -PathType Leaf) { $acceptanceReceipt = Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json -Depth 100 }
            if ($null -eq $acceptanceReceipt -or $acceptance.exitCode -ne 0 -or $acceptanceReceipt.success -ne $true) { Add-Issue 'acceptance.failed' 'The real Project/Panel acceptance target did not pass.' }
        }
        $afterManifestHash = Hash $sourceManifest; $afterTreeHash = TreeHash $source
        if ($beforeManifestHash -ne $afterManifestHash) { Add-Issue 'source.manifest-polluted' 'Source manifest changed.' }
        if ($beforeTreeHash -ne $afterTreeHash) { Add-Issue 'source.tree-polluted' 'Source project tree changed.' }
        $success = $issues.Count -eq 0 -and $null -ne $acceptanceReceipt -and $acceptanceReceipt.success -eq $true
        $exitCode = if ($success) { 0 } else { 3 }
    }
} catch { Add-Issue 'verifier.unexpected' $_.Exception.Message; $exitCode = 1 }
if ([string]::IsNullOrWhiteSpace($output)) { $output = Join-Path $engine 'generated/acceptance/hybrid-pixel-profile-authoring'; [IO.Directory]::CreateDirectory($output) | Out-Null }
$receipt = if ($null -ne $acceptanceReceipt) { $acceptanceReceipt } else { [ordered]@{ schemaVersion = 'noemancer.hybrid-pixel-profile-authoring-acceptance/0.1'; success = $false } }
$receipt | Add-Member -MemberType NoteProperty -Name verifier -Value ([ordered]@{ success = $success; sourceManifestSha256Before = $beforeManifestHash; sourceManifestSha256After = $afterManifestHash; sourceTreeSha256Before = $beforeTreeHash; sourceTreeSha256After = $afterTreeHash; stagingProject = $stage; issues = @($issues.ToArray()) }) -Force
$receiptPath = Join-Path $output 'hybrid-pixel-profile-authoring-receipt.json'; Write-Json $receiptPath $receipt
Write-Output $(if ($success) { 'hybrid-pixel-profile-authoring: pass' } else { 'hybrid-pixel-profile-authoring: noninteractive failure' })
Write-Output ('receipt: ' + $receiptPath)
exit $exitCode
