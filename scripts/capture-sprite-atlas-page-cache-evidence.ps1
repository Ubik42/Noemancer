[CmdletBinding()]
param(
    [string]$Project = 'D:\3D\NoemancerPlatformer',
    [string]$RuntimePath = (Join-Path $PSScriptRoot '..\build\windows-msvc-debug\src\runtime\Debug\noemancer.exe'),
    [string]$OutputPath = (Join-Path $PSScriptRoot '..\generated\acceptance\sprite-atlas-page-cache-evidence.json'),
    [ValidateRange(30, 900)]
    [int]$TimeoutSeconds = 180,
    [switch]$KeepStaging
)

$ErrorActionPreference = 'Stop'
$script:ExitCode = 0
$script:Issues = [System.Collections.Generic.List[object]]::new()
$script:StagePath = $null
$script:StageKept = $false
$script:SourceTreeBefore = $null
$script:SourceTreeAfter = $null
$script:Fixture = $null
$script:Run1 = $null
$script:Run2 = $null

function Add-Issue {
    param(
        [Parameter(Mandatory = $true)][string]$Code,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Message,
        [ValidateRange(1, 124)][int]$ExitCode = 1
    )
    [void]$script:Issues.Add([ordered]@{ code = $Code; message = $Message })
    if ($script:ExitCode -eq 0 -or $ExitCode -eq 7) { $script:ExitCode = $ExitCode }
}

function Write-Utf8Json {
    param([Parameter(Mandatory = $true)][string]$Path, [Parameter(Mandatory = $true)]$Value)
    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    $text = if ($Value -is [string]) { $Value } else { $Value | ConvertTo-Json -Depth 100 -Compress:$false }
    [IO.File]::WriteAllText($Path, $text + "`n", [Text.UTF8Encoding]::new($false))
}

function Read-Json {
    param([Parameter(Mandatory = $true)][string]$Path)
    return (Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json)
}

function Get-Sha256 {
    param([Parameter(Mandatory = $true)][string]$Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-TreeFingerprint {
    param([Parameter(Mandatory = $true)][string]$Root)
    $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd('\')
    $rows = [System.Collections.Generic.List[string]]::new()
    foreach ($file in (Get-ChildItem -LiteralPath $rootFull -Recurse -File -Force | Where-Object {
        $_.FullName -notmatch '\\.git(\\|$)' } | Sort-Object FullName)) {
        $relative = [IO.Path]::GetRelativePath($rootFull, $file.FullName).Replace('\', '/')
        [void]$rows.Add($relative + '|' + (Get-Sha256 -Path $file.FullName))
    }
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes(($rows -join "`n"))
    $digest = [Security.Cryptography.SHA256]::HashData($bytes)
    return [ordered]@{
        sha256 = ([BitConverter]::ToString($digest).Replace('-', '').ToLowerInvariant())
        fileCount = $rows.Count
    }
}

function Invoke-HiddenProcess {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [string]$InputText = '',
        [Parameter(Mandatory = $true)][int]$TimeoutSeconds
    )
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $FilePath
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    foreach ($argument in $Arguments) { [void]$info.ArgumentList.Add([string]$argument) }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $info
    $startedAt = [Diagnostics.Stopwatch]::GetTimestamp()
    if (-not $process.Start()) { throw "Unable to start $FilePath" }
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    if ($InputText) { $process.StandardInput.Write($InputText) }
    $process.StandardInput.Close()
    $timedOut = -not $process.WaitForExit($TimeoutSeconds * 1000)
    if ($timedOut) {
        try { $process.Kill($true) } catch { }
        $process.WaitForExit()
    }
    $result = [ordered]@{
        exitCode = if ($timedOut) { 124 } else { $process.ExitCode }
        timedOut = $timedOut
        stdout = $stdoutTask.GetAwaiter().GetResult()
        stderr = $stderrTask.GetAwaiter().GetResult()
        durationMs = [math]::Round(([Diagnostics.Stopwatch]::GetTimestamp() - $startedAt) * 1000.0 / [Diagnostics.Stopwatch]::Frequency, 2)
    }
    $process.Dispose()
    return $result
}

function Copy-ProjectToStage {
    param([Parameter(Mandatory = $true)][string]$Source, [Parameter(Mandatory = $true)][string]$Destination)
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    foreach ($entry in (Get-ChildItem -LiteralPath $Source -Force | Where-Object { $_.Name -ne '.git' })) {
        Copy-Item -LiteralPath $entry.FullName -Destination (Join-Path $Destination $entry.Name) -Recurse -Force
    }
}

function Write-AtlasPng {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [switch]$Changed
    )
    if (-not $script:DrawingLoaded) {
        try { Add-Type -AssemblyName System.Drawing.Common -ErrorAction Stop }
        catch { Add-Type -AssemblyName System.Drawing -ErrorAction Stop }
        $script:DrawingLoaded = $true
    }
    $parent = Split-Path -Parent $Path
    New-Item -ItemType Directory -Path $parent -Force | Out-Null
    $bitmap = $null
    try {
        $bitmap = [Drawing.Bitmap]::new(1024, 1024, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $colors = @(
            [Drawing.Color]::FromArgb(255, 35, 92, 180),
            [Drawing.Color]::FromArgb(255, 220, 90, 48),
            [Drawing.Color]::FromArgb(255, 46, 155, 91),
            [Drawing.Color]::FromArgb(255, 178, 73, 188)
        )
        for ($y = 0; $y -lt 1024; $y++) {
            for ($x = 0; $x -lt 1024; $x++) {
                $quadrant = ([int]($x -ge 512)) + (2 * [int]($y -ge 512))
                $color = $colors[$quadrant]
                if ($Changed -and $x -eq 64 -and $y -eq 64) {
                    $color = [Drawing.Color]::FromArgb(255, 255, 220, 32)
                }
                $bitmap.SetPixel($x, $y, $color)
            }
        }
        if (Test-Path -LiteralPath $Path -PathType Leaf) { Remove-Item -LiteralPath $Path -Force }
        $bitmap.Save($Path, [Drawing.Imaging.ImageFormat]::Png)
    } finally {
        if ($bitmap) { $bitmap.Dispose() }
    }
}

function New-SpriteFixture {
    param([Parameter(Mandatory = $true)][string]$Stage)
    $assetsRoot = Join-Path $Stage 'assets'
    if (-not (Test-Path -LiteralPath $assetsRoot -PathType Container)) {
        throw 'The staged project has no assets directory.'
    }
    $textureId = 'evidence.sprite-atlas.texture'
    $spriteId = 'evidence.sprite-atlas.source'
    $textureRelative = 'evidence/sprite-atlas-1024.png'
    $spriteRelative = 'evidence/sprite-atlas-4page.sprite.json'
    $texturePath = Join-Path $assetsRoot $textureRelative.Replace('/', '\')
    $spritePath = Join-Path $assetsRoot $spriteRelative.Replace('/', '\')
    Write-AtlasPng -Path $texturePath
    $frames = @(
        [ordered]@{ id = 'evidence.frame.0'; rect = @(0, 0, 512, 512); trimOffset = @(0, 0); sourceSize = @(512, 512); pivot = @(0.5, 0.5); collisionProfile = '' },
        [ordered]@{ id = 'evidence.frame.1'; rect = @(512, 0, 512, 512); trimOffset = @(0, 0); sourceSize = @(512, 512); pivot = @(0.5, 0.5); collisionProfile = '' },
        [ordered]@{ id = 'evidence.frame.2'; rect = @(0, 512, 512, 512); trimOffset = @(0, 0); sourceSize = @(512, 512); pivot = @(0.5, 0.5); collisionProfile = '' },
        [ordered]@{ id = 'evidence.frame.3'; rect = @(512, 512, 512, 512); trimOffset = @(0, 0); sourceSize = @(512, 512); pivot = @(0.5, 0.5); collisionProfile = '' }
    )
    $sprite = [ordered]@{
        schema = 'noemancer.sprite-asset/0.2'; assetId = $spriteId; textureAsset = $textureId
        textureSize = @(1024, 1024); pixelsPerUnit = 64; sampling = 'nearest'; alphaMode = 'cutout'
        frames = $frames
        clips = @([ordered]@{ id = 'evidence.clip'; looping = $true; frames = @(
            [ordered]@{ frame = 'evidence.frame.0'; durationMs = 100; event = '' },
            [ordered]@{ frame = 'evidence.frame.1'; durationMs = 100; event = '' },
            [ordered]@{ frame = 'evidence.frame.2'; durationMs = 100; event = '' },
            [ordered]@{ frame = 'evidence.frame.3'; durationMs = 100; event = '' }
        ) })
        provenance = [ordered]@{ sourceUri = $textureRelative; sourceSha256 = 'fixture-generated'; generator = 'sprite-atlas-page-cache-evidence'; license = 'project-original' }
    }
    Write-Utf8Json -Path $spritePath -Value $sprite

    $registryPath = Join-Path $assetsRoot 'registry.json'
    if (Test-Path -LiteralPath $registryPath -PathType Leaf) { $registry = Read-Json -Path $registryPath }
    else { $registry = [pscustomobject]@{ schema = 'noemancer.assets/0.1'; assets = @() } }
    $records = @($registry.assets | Where-Object { $_.id -ne $textureId -and $_.id -ne $spriteId })
    $records += [ordered]@{
        id = $textureId; displayName = 'Evidence Four-Page Atlas'; kind = 'Texture'; uri = 'asset://' + $textureRelative
        path = $textureRelative; license = 'project-original'; redistribution = 'project-only'; tags = @('evidence', 'sprite', 'atlas')
    }
    $records += [ordered]@{
        id = $spriteId; displayName = 'Evidence Four-Page Sprite Atlas'; kind = 'Sprite'; uri = 'asset://' + $spriteRelative
        path = $spriteRelative; license = 'project-original'; redistribution = 'project-only'; tags = @('evidence', 'sprite', 'atlas')
        dependencies = @($textureId)
    }
    $registry.schema = 'noemancer.assets/0.1'
    $registry.assets = @($records)
    Write-Utf8Json -Path $registryPath -Value $registry

    $projectManifestPath = Join-Path $Stage 'noemancer.project.json'
    if (-not (Test-Path -LiteralPath $projectManifestPath -PathType Leaf)) { throw 'The staged project has no noemancer.project.json.' }
    $projectManifest = Read-Json -Path $projectManifestPath
    $packaged = @($projectManifest.packagedAssets)
    if ($packaged -notcontains $spriteId) { $packaged += $spriteId }
    $projectManifest.packagedAssets = @($packaged)
    Write-Utf8Json -Path $projectManifestPath -Value $projectManifest
    return [ordered]@{
        spriteId = $spriteId; textureId = $textureId; spritePath = $spritePath; texturePath = $texturePath
        spriteRelative = $spriteRelative; textureRelative = $textureRelative; frameCount = 4; expectedPageCount = 4
        sourceHashBefore = Get-Sha256 -Path $texturePath
    }
}

function Get-ResponseById {
    param([Parameter(Mandatory = $true)]$Responses, [Parameter(Mandatory = $true)][string]$Id)
    return ($Responses | Where-Object { $_.id -eq $Id } | Select-Object -First 1)
}

function Invoke-ProjectToolSession {
    param(
        [Parameter(Mandatory = $true)][string]$ProjectRoot,
        [Parameter(Mandatory = $true)]$Requests
    )
    $lines = [System.Collections.Generic.List[string]]::new()
    foreach ($request in $Requests) { [void]$lines.Add(($request | ConvertTo-Json -Depth 100 -Compress)) }
    $raw = Invoke-HiddenProcess -FilePath $RuntimePath -Arguments @('serve', '--project', $ProjectRoot, '--format', 'jsonl') `
        -InputText (($lines -join "`n") + "`n") -TimeoutSeconds $TimeoutSeconds
    $responses = [System.Collections.Generic.List[object]]::new()
    foreach ($line in ($raw.stdout -split "`r?`n" | Where-Object { $_.Trim() })) {
        try { [void]$responses.Add(($line | ConvertFrom-Json)) }
        catch { Add-Issue -Code 'serve.invalid-jsonl' -Message $_.Exception.Message -ExitCode 4 }
    }
    return [ordered]@{ process = $raw; responses = @($responses) }
}

function Resolve-GeneratedUri {
    param([Parameter(Mandatory = $true)][string]$Stage, [Parameter(Mandatory = $true)][string]$Uri)
    $prefix = 'generated://'
    if (-not $Uri.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { return $null }
    $relativeText = $Uri.Substring($prefix.Length)
    if ([string]::IsNullOrWhiteSpace($relativeText) -or $relativeText.Contains('\')) { return $null }
    $segments = $relativeText.Split('/')
    if (@($segments | Where-Object { $_ -eq '' -or $_ -eq '.' -or $_ -eq '..' }).Count -gt 0) { return $null }
    $generatedRoot = [IO.Path]::GetFullPath((Join-Path $Stage 'generated')).TrimEnd('\') + '\'
    $candidate = [IO.Path]::GetFullPath((Join-Path $generatedRoot ($relativeText.Replace('/', '\'))))
    if (-not $candidate.StartsWith($generatedRoot, [StringComparison]::OrdinalIgnoreCase)) { return $null }
    return $candidate
}

function Read-CookRun {
    param(
        [Parameter(Mandatory = $true)][string]$Stage,
        [Parameter(Mandatory = $true)]$Plan,
        [Parameter(Mandatory = $true)]$ApplySession,
        [Parameter(Mandatory = $true)][string]$SpriteId
    )
    $manifestPath = Join-Path $Stage ('generated\cook-manifests\' + [string]$Plan.planId + '.json')
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        Add-Issue -Code 'cook.manifest-missing' -Message "Cook manifest was not produced: $manifestPath" -ExitCode 5
        return $null
    }
    $manifest = Read-Json -Path $manifestPath
    $parentOutput = @($manifest.outputs | Where-Object { $_.assetId -eq $SpriteId }) | Select-Object -First 1
    if ($null -eq $parentOutput) {
        Add-Issue -Code 'cook.parent-output-missing' -Message "Cook manifest has no Sprite Atlas output for $SpriteId." -ExitCode 5
        return $null
    }
    $metadataPath = Resolve-GeneratedUri -Stage $Stage -Uri ([string]$parentOutput.metadataUri)
    if ($null -eq $metadataPath -or -not (Test-Path -LiteralPath $metadataPath -PathType Leaf)) {
        Add-Issue -Code 'cook.metadata-missing' -Message 'Cook parent metadata URI did not resolve under generated/.' -ExitCode 5
        return $null
    }
    $metadata = Read-Json -Path $metadataPath
    $atlas = $metadata.importedMetadata.atlasArtifact
    if ($null -eq $atlas -or $atlas.kind -ne 'SpriteAtlas' -or $null -eq $atlas.authoringDocument) {
        Add-Issue -Code 'cook.atlas-manifest-invalid' -Message 'Cook metadata did not expose a SpriteAtlas manifest with authoringDocument.' -ExitCode 5
        return $null
    }
    $pageItems = @($atlas.pages.items)
    $pageRecords = @($atlas.pageArtifacts)
    if ($pageItems.Count -lt 2 -or $pageItems.Count -ne $pageRecords.Count) {
        Add-Issue -Code 'cook.multi-page-missing' -Message "Expected at least two complete Atlas pages; got $($pageItems.Count) items and $($pageRecords.Count) records." -ExitCode 5
        return $null
    }
    $pages = [System.Collections.Generic.List[object]]::new()
    foreach ($item in $pageItems) {
        $pageIndex = [int]$item.pageIndex
        $record = $pageRecords | Where-Object { [int]$_.pageIndex -eq $pageIndex } | Select-Object -First 1
        if ($null -eq $record) {
            Add-Issue -Code 'cook.page-record-missing' -Message "No page artifact record for page $pageIndex." -ExitCode 5
            continue
        }
        $payloadUri = [string]$record.payloadUri
        $payloadPath = Resolve-GeneratedUri -Stage $Stage -Uri $payloadUri
        $actualHash = $null
        $actualBytes = $null
        if ($null -ne $payloadPath -and (Test-Path -LiteralPath $payloadPath -PathType Leaf)) {
            $actualHash = Get-Sha256 -Path $payloadPath
            $actualBytes = (Get-Item -LiteralPath $payloadPath).Length
        }
        $declaredHash = ([string]$item.payload.fingerprint) -replace '^sha256:', ''
        if ($null -eq $actualHash -or $actualHash -ne $declaredHash -or [int64]$actualBytes -ne [int64]$record.payloadBytes) {
            Add-Issue -Code 'cook.page-identity-mismatch' -Message "Page $pageIndex payload identity or size does not match its manifest." -ExitCode 5
        }
        [void]$pages.Add([ordered]@{
            pageIndex = $pageIndex; assetId = [string]$item.assetId; cacheKey = [string]$item.cacheKey
            cacheHit = [bool]$item.cacheHit; cacheMiss = -not [bool]$item.cacheHit; rebuilt = [bool]$item.rebuilt
            contentFingerprint = [string]$item.contentFingerprint; layoutFingerprint = $null
            layoutIdentitySource = 'page-cache-key includes page_layout_fingerprint; page-level layout fingerprint is not emitted by the timing-free artifact JSON'
            payloadFingerprint = [string]$item.payload.fingerprint; payloadBytes = [int64]$item.payload.bytes
            payloadUri = $payloadUri; actualPayloadSha256 = $actualHash; actualPayloadBytes = $actualBytes
            durationMs = $null; durationAvailable = $false
        })
    }
    return [ordered]@{
        planId = [string]$Plan.planId; planContentHash = [string]$Plan.contentHash
        mainCacheUri = [string]$Plan.inputs[0].cacheUri; manifestPath = $manifestPath; metadataPath = $metadataPath
        apply = [ordered]@{ exitCode = $ApplySession.process.exitCode; timedOut = $ApplySession.process.timedOut; durationMs = $ApplySession.process.durationMs }
        cacheHits = $ApplySession.responses | Where-Object { $_.id -eq 'apply' } | Select-Object -First 1 | ForEach-Object { $_.response.result.cacheHits }
        cacheMisses = $ApplySession.responses | Where-Object { $_.id -eq 'apply' } | Select-Object -First 1 | ForEach-Object { $_.response.result.cacheMisses }
        atlas = [ordered]@{ schema = [string]$atlas.schema; kind = [string]$atlas.kind; layoutFingerprint = [string]$atlas.layout.fingerprint; bundleFingerprint = [string]$atlas.bundleFingerprint; dependencies = @($atlas.dependencies); authoringDocumentAssetId = [string]$atlas.authoringDocument.assetId }
        pages = @($pages)
        pageTiming = [ordered]@{ available = $false; reason = 'Sprite Atlas artifact JSON intentionally omits per-page timing; evidence records process duration and page identity without fabricating page timings.' }
        topLevelPageOutputCount = @($manifest.outputs | Where-Object { [string]$_.kind -eq 'SpriteAtlasPage' }).Count
    }
}

function Compare-CookRuns {
    param([Parameter(Mandatory = $true)]$First, [Parameter(Mandatory = $true)]$Second)
    if ($First.pages.Count -ne $Second.pages.Count) {
        Add-Issue -Code 'cache.page-count-changed' -Message 'The second Cook produced a different page count.' -ExitCode 5
        return
    }
    if ($First.atlas.bundleFingerprint -eq $Second.atlas.bundleFingerprint) {
        Add-Issue -Code 'cache.bundle-fingerprint-unchanged' -Message 'The full Sprite Atlas bundle fingerprint did not reflect the source pixel edit.' -ExitCode 5
    }
    if ($First.atlas.layoutFingerprint -ne $Second.atlas.layoutFingerprint) {
        Add-Issue -Code 'cache.layout-fingerprint-changed' -Message 'The source pixel edit unexpectedly changed the deterministic Atlas layout fingerprint.' -ExitCode 5
    }
    foreach ($firstPage in $First.pages) {
        $secondPage = $Second.pages | Where-Object { $_.pageIndex -eq $firstPage.pageIndex } | Select-Object -First 1
        if ($null -eq $secondPage) {
            Add-Issue -Code 'cache.page-missing-second-run' -Message "Page $($firstPage.pageIndex) is missing from the second Cook." -ExitCode 5
            continue
        }
        $changed = $firstPage.pageIndex -eq 0
        if ($changed) {
            if ($firstPage.contentFingerprint -eq $secondPage.contentFingerprint -or
                $firstPage.payloadFingerprint -eq $secondPage.payloadFingerprint -or
                $secondPage.cacheHit) {
                Add-Issue -Code 'cache.changed-page-reused' -Message 'The changed page was reported as a cache hit or retained its old identity.' -ExitCode 5
            }
            if ($firstPage.layoutFingerprint -ne $secondPage.layoutFingerprint) {
                Add-Issue -Code 'cache.changed-page-layout-drift' -Message 'The changed source pixel unexpectedly changed page layout identity.' -ExitCode 5
            }
        } else {
            if ($firstPage.contentFingerprint -ne $secondPage.contentFingerprint -or
                $firstPage.payloadFingerprint -ne $secondPage.payloadFingerprint -or
                $firstPage.cacheKey -ne $secondPage.cacheKey -or
                -not $secondPage.cacheHit) {
                Add-Issue -Code 'cache.unaffected-page-rebuilt' -Message "Unchanged page $($firstPage.pageIndex) did not reuse its page cache identity." -ExitCode 5
            }
        }
    }
}

$receiptPath = [IO.Path]::GetFullPath($OutputPath)
$artifactRoot = Join-Path ([IO.Path]::GetDirectoryName($receiptPath)) ([IO.Path]::GetFileNameWithoutExtension($receiptPath) + '-artifacts')
$commandRoot = Join-Path $artifactRoot 'commands'
$startedAt = [DateTime]::UtcNow
$fixture = $null

try {
    if (-not (Test-Path -LiteralPath $Project -PathType Container)) { Add-Issue -Code 'input.project-missing' -Message "Project directory does not exist: $Project" -ExitCode 2; throw 'input' }
    if (-not (Test-Path -LiteralPath $RuntimePath -PathType Leaf)) { Add-Issue -Code 'input.runtime-missing' -Message "Runtime executable does not exist: $RuntimePath" -ExitCode 2; throw 'input' }
    $script:SourceTreeBefore = Get-TreeFingerprint -Root $Project
    $script:StagePath = Join-Path ([IO.Path]::GetTempPath()) ('noemancer-sprite-atlas-cache-' + [guid]::NewGuid().ToString('N'))
    Copy-ProjectToStage -Source $Project -Destination $script:StagePath
    $stagedGenerated = Join-Path $script:StagePath 'generated'
    if (Test-Path -LiteralPath $stagedGenerated -PathType Container) { Remove-Item -LiteralPath $stagedGenerated -Recurse -Force }
    $fixture = New-SpriteFixture -Stage $script:StagePath

    $planSession1 = Invoke-ProjectToolSession -ProjectRoot $script:StagePath -Requests @(
        [ordered]@{ id = 'registry'; name = 'asset.registry'; arguments = [ordered]@{ refresh = $false } },
        [ordered]@{ id = 'plan'; name = 'asset.cook.plan'; arguments = [ordered]@{ assetIds = @($fixture.spriteId); targetProfile = 'windows-x64-debug' } }
    )
    $planResponse1 = Get-ResponseById -Responses $planSession1.responses -Id 'plan'
    $plan1 = if ($planResponse1) { $planResponse1.response.result } else { $null }
    if ($planSession1.process.exitCode -ne 0 -or $null -eq $plan1 -or $plan1.valid -ne $true) {
        Add-Issue -Code 'cook.first-plan-failed' -Message 'The first independent Runtime plan process did not return a valid Cook plan.' -ExitCode 4
        throw 'first-plan'
    }
    $applySession1 = Invoke-ProjectToolSession -ProjectRoot $script:StagePath -Requests @(
        [ordered]@{ id = 'registry'; name = 'asset.registry'; arguments = [ordered]@{ refresh = $false } },
        [ordered]@{ id = 'apply'; name = 'asset.cook.apply'; arguments = [ordered]@{ plan = $plan1; dryRun = $false } }
    )
    $applyResponse1 = Get-ResponseById -Responses $applySession1.responses -Id 'apply'
    if ($applySession1.process.exitCode -ne 0 -or $null -eq $applyResponse1 -or $applyResponse1.response.result.success -ne $true) {
        Add-Issue -Code 'cook.first-apply-failed' -Message 'The first independent Runtime apply process did not commit the Cook closure.' -ExitCode 4
        throw 'first-apply'
    }
    $script:Run1 = Read-CookRun -Stage $script:StagePath -Plan $plan1 -ApplySession $applySession1 -SpriteId $fixture.spriteId

    $fixture.sourceHashBefore = Get-Sha256 -Path $fixture.texturePath
    Write-AtlasPng -Path $fixture.texturePath -Changed
    $fixture.sourceHashAfter = Get-Sha256 -Path $fixture.texturePath
    if ($fixture.sourceHashBefore -eq $fixture.sourceHashAfter) {
        Add-Issue -Code 'fixture.source-edit-noop' -Message 'The second-run source pixel edit did not change the atlas source hash.' -ExitCode 5
        throw 'edit'
    }

    $planSession2 = Invoke-ProjectToolSession -ProjectRoot $script:StagePath -Requests @(
        [ordered]@{ id = 'registry'; name = 'asset.registry'; arguments = [ordered]@{ refresh = $false } },
        [ordered]@{ id = 'plan'; name = 'asset.cook.plan'; arguments = [ordered]@{ assetIds = @($fixture.spriteId); targetProfile = 'windows-x64-debug' } }
    )
    $planResponse2 = Get-ResponseById -Responses $planSession2.responses -Id 'plan'
    $plan2 = if ($planResponse2) { $planResponse2.response.result } else { $null }
    if ($planSession2.process.exitCode -ne 0 -or $null -eq $plan2 -or $plan2.valid -ne $true) {
        Add-Issue -Code 'cook.second-plan-failed' -Message 'The second independent Runtime plan process did not return a valid Cook plan.' -ExitCode 4
        throw 'second-plan'
    }
    $applySession2 = Invoke-ProjectToolSession -ProjectRoot $script:StagePath -Requests @(
        [ordered]@{ id = 'registry'; name = 'asset.registry'; arguments = [ordered]@{ refresh = $false } },
        [ordered]@{ id = 'apply'; name = 'asset.cook.apply'; arguments = [ordered]@{ plan = $plan2; dryRun = $false } }
    )
    $applyResponse2 = Get-ResponseById -Responses $applySession2.responses -Id 'apply'
    if ($applySession2.process.exitCode -ne 0 -or $null -eq $applyResponse2 -or $applyResponse2.response.result.success -ne $true) {
        Add-Issue -Code 'cook.second-apply-failed' -Message 'The second independent Runtime apply process did not commit the Cook closure.' -ExitCode 4
        throw 'second-apply'
    }
    $script:Run2 = Read-CookRun -Stage $script:StagePath -Plan $plan2 -ApplySession $applySession2 -SpriteId $fixture.spriteId
    if ($script:Run1 -and $script:Run2) { Compare-CookRuns -First $script:Run1 -Second $script:Run2 }
} catch {
    if ($script:ExitCode -eq 0) { Add-Issue -Code 'unexpected.failure' -Message $_.Exception.Message -ExitCode 1 }
} finally {
    try {
        if ($script:StagePath -and (Test-Path -LiteralPath $script:StagePath)) {
            if ($KeepStaging) { $script:StageKept = $true } else { Remove-Item -LiteralPath $script:StagePath -Recurse -Force }
        }
    } catch { Add-Issue -Code 'cleanup.failed' -Message $_.Exception.Message -ExitCode 7 }
    try { $script:SourceTreeAfter = Get-TreeFingerprint -Root $Project } catch { Add-Issue -Code 'source.hash-after.failed' -Message $_.Exception.Message -ExitCode 7 }
    if ($script:SourceTreeBefore -and $script:SourceTreeAfter -and $script:SourceTreeBefore.sha256 -ne $script:SourceTreeAfter.sha256) {
        Add-Issue -Code 'source.changed' -Message 'The source project tree changed during the page-cache evidence run.' -ExitCode 6
    }
    $quality = [ordered]@{
        schemaVersion = 'noemancer.sprite-atlas-page-cache-quality/0.1'; pass = ($script:Issues.Count -eq 0)
        source = [ordered]@{ project = [IO.Path]::GetFullPath($Project); treeBefore = $script:SourceTreeBefore; treeAfter = $script:SourceTreeAfter; unchanged = ($script:SourceTreeBefore -and $script:SourceTreeAfter -and $script:SourceTreeBefore.sha256 -eq $script:SourceTreeAfter.sha256) }
        fixture = $fixture
        run1 = $script:Run1; run2 = $script:Run2
        pageTiming = [ordered]@{ available = $false; reason = 'Current Sprite Atlas artifact JSON is timing-free per page; run-level process duration is recorded, and page duration is not fabricated.' }
        cacheMechanism = [ordered]@{ root = if ($script:StagePath) { 'generated/cook-cache/sprite-atlas-page-cache/sprite-atlas-pages-v1' } else { $null }; keyIncludes = @('source_page_fingerprint', 'page_layout_fingerprint', 'cook_recipe_fingerprint', 'profile_fingerprint', 'compression', 'worker_identity'); expected = 'Main Sprite recipe changes may create a new parent cache directory, while unchanged page identities remain reusable in the Registry page cache.' }
        scope = 'headless independent Runtime serve processes; source-to-Cook page cache identity only; no window, GPU timing or fabricated per-page timing'
    }
    try {
        Write-Utf8Json -Path (Join-Path $commandRoot 'quality.json') -Value $quality
        if ($script:Run1) { Write-Utf8Json -Path (Join-Path $commandRoot 'run-1.json') -Value $script:Run1 }
        if ($script:Run2) { Write-Utf8Json -Path (Join-Path $commandRoot 'run-2.json') -Value $script:Run2 }
    } catch { Add-Issue -Code 'quality.write-failed' -Message $_.Exception.Message -ExitCode 7 }
    $receipt = [ordered]@{
        schemaVersion = 'noemancer.sprite-atlas-page-cache-evidence/0.1'; status = if ($script:Issues.Count -eq 0) { 'passed' } else { 'failed' }
        pass = ($script:Issues.Count -eq 0); startedAtUtc = $startedAt.ToString('o'); completedAtUtc = [DateTime]::UtcNow.ToString('o')
        source = [ordered]@{ project = [IO.Path]::GetFullPath($Project); treeBefore = $script:SourceTreeBefore; treeAfter = $script:SourceTreeAfter; unchanged = ($script:SourceTreeBefore -and $script:SourceTreeAfter -and $script:SourceTreeBefore.sha256 -eq $script:SourceTreeAfter.sha256) }
        staging = [ordered]@{ path = if ($script:StageKept) { $script:StagePath } else { $null }; kept = $script:StageKept }
        budget = [ordered]@{ timeoutSeconds = $TimeoutSeconds; pageCountMinimum = 2; atlasSize = @(1024, 1024); frameSize = @(512, 512); changedFrame = 'evidence.frame.0' }
        runs = [ordered]@{ first = $script:Run1; second = $script:Run2 }
        artifacts = [ordered]@{ quality = [IO.Path]::GetRelativePath((Split-Path -Parent $receiptPath), (Join-Path $commandRoot 'quality.json')).Replace('\', '/'); runEvidence = [IO.Path]::GetRelativePath((Split-Path -Parent $receiptPath), $commandRoot).Replace('\', '/') }
        issues = @($script:Issues); exitCode = $script:ExitCode
    }
    try { Write-Utf8Json -Path $receiptPath -Value $receipt } catch { $script:ExitCode = 7; Write-Error $_.Exception.Message }
}

exit $script:ExitCode
