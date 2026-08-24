[CmdletBinding()]
param(
    [string]$Project = 'D:\3D\NoemancerPlatformer',
    [string]$RuntimePath = (Join-Path $PSScriptRoot '..\build\windows-msvc-debug\src\runtime\Debug\noemancer.exe'),
    [string]$OutputPath = (Join-Path $PSScriptRoot '..\generated\acceptance\sprite-tilemap-production-pressure-evidence.json'),
    [ValidateRange(64, 4096)]
    [int]$SpriteFrameCount = 2048,
    [ValidateRange(1, 256)]
    [int]$SpriteClipCount = 32,
    [ValidateRange(1, 64)]
    [int]$TilemapChunkColumns = 32,
    [ValidateRange(1, 64)]
    [int]$TilemapChunkRows = 32,
    [ValidateRange(4, 32)]
    [int]$TilemapChunkSize = 8,
    [ValidateRange(0, 32)]
    [int]$VisibleChunkRadius = 8,
    [ValidateRange(10, 900)]
    [int]$TimeoutSeconds = 120,
    [switch]$KeepStaging
)

$ErrorActionPreference = 'Stop'
$script:ExitCode = 0
$script:Issues = [System.Collections.Generic.List[object]]::new()
$script:StagePath = $null
$script:StageKept = $false
$script:SourceTreeBefore = $null
$script:SourceTreeAfter = $null

function Add-Issue {
    param(
        [Parameter(Mandatory = $true)][string]$Code,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Message,
        [int]$ExitCode = 1
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
        $_.FullName -notmatch '\\\.git(\\|$)' } | Sort-Object FullName)) {
        $relative = [IO.Path]::GetRelativePath($rootFull, $file.FullName).Replace('\', '/')
        [void]$rows.Add($relative + '|' + (Get-Sha256 -Path $file.FullName))
    }
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes(($rows -join "`n"))
    $digest = [Security.Cryptography.SHA256]::HashData($bytes)
    return [ordered]@{ sha256 = ([BitConverter]::ToString($digest).Replace('-', '').ToLowerInvariant()); fileCount = $rows.Count }
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
    return [ordered]@{
        exitCode = if ($timedOut) { 124 } else { $process.ExitCode }
        timedOut = $timedOut
        stdout = $stdoutTask.GetAwaiter().GetResult()
        stderr = $stderrTask.GetAwaiter().GetResult()
        durationMs = [math]::Round(([Diagnostics.Stopwatch]::GetTimestamp() - $startedAt) * 1000.0 / [Diagnostics.Stopwatch]::Frequency, 2)
    }
}

function Copy-ProjectToStage {
    param([Parameter(Mandatory = $true)][string]$Source, [Parameter(Mandatory = $true)][string]$Destination)
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    foreach ($entry in (Get-ChildItem -LiteralPath $Source -Force | Where-Object { $_.Name -ne '.git' })) {
        Copy-Item -LiteralPath $entry.FullName -Destination (Join-Path $Destination $entry.Name) -Recurse -Force
    }
}

function New-PressureSpriteFixture {
    param([Parameter(Mandatory = $true)][string]$Stage)
    $assetsRoot = Join-Path $Stage 'assets'
    $artRoot = Join-Path $assetsRoot 'art'
    $sourceRoot = Join-Path $artRoot 'source'
    New-Item -ItemType Directory -Path $sourceRoot -Force | Out-Null
    $sourceTexture = Join-Path $sourceRoot 'courier-action-source-v2.png'
    if (-not (Test-Path -LiteralPath $sourceTexture -PathType Leaf)) { throw 'The staging project has no valid PNG source texture.' }
    $atlasRelative = 'art/source/pressure-atlas.png'
    $atlasPath = Join-Path $assetsRoot $atlasRelative.Replace('/', '\')
    $frameSize = 32
    $atlasSize = 2048
    try { Add-Type -AssemblyName System.Drawing.Common -ErrorAction Stop }
    catch { Add-Type -AssemblyName System.Drawing -ErrorAction Stop }
    $sourceImage = $null
    $atlasImage = $null
    $graphics = $null
    try {
        $sourceImage = [Drawing.Image]::FromFile($sourceTexture)
        $atlasImage = [Drawing.Bitmap]::new($atlasSize, $atlasSize, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $graphics = [Drawing.Graphics]::FromImage($atlasImage)
        $graphics.Clear([Drawing.Color]::Transparent)
        $graphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
        $graphics.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::Half
        $graphics.DrawImage($sourceImage, 0, 0, $atlasSize, $atlasSize)
        $atlasImage.Save($atlasPath, [Drawing.Imaging.ImageFormat]::Png)
    } finally {
        if ($graphics) { $graphics.Dispose() }
        if ($atlasImage) { $atlasImage.Dispose() }
        if ($sourceImage) { $sourceImage.Dispose() }
    }
    $pngHeader = [IO.File]::ReadAllBytes($atlasPath)
    $actualWidth = [BitConverter]::ToUInt32([byte[]]@($pngHeader[19], $pngHeader[18], $pngHeader[17], $pngHeader[16]), 0)
    $actualHeight = [BitConverter]::ToUInt32([byte[]]@($pngHeader[23], $pngHeader[22], $pngHeader[21], $pngHeader[20]), 0)
    if ($actualWidth -ne $atlasSize -or $actualHeight -ne $atlasSize) { throw 'Generated pressure atlas dimensions do not match Sprite textureSize.' }
    $columns = [int]($atlasSize / $frameSize)
    if ($SpriteFrameCount -gt $columns * $columns -or $SpriteClipCount -gt $SpriteFrameCount) { throw 'Sprite fixture exceeds the bounded atlas or clip budget.' }
    $frames = [System.Collections.Generic.List[object]]::new()
    for ($index = 0; $index -lt $SpriteFrameCount; $index++) {
        $x = [int](($index % $columns) * $frameSize)
        $y = [int]([math]::Floor($index / $columns) * $frameSize)
        [void]$frames.Add([ordered]@{
            id = ('pressure.{0:D5}' -f $index); rect = @($x, $y, $frameSize, $frameSize)
            trimOffset = @(0, 0); sourceSize = @($frameSize, $frameSize); pivot = @(0.5, 0.5); collisionProfile = ''
        })
    }
    $clips = [System.Collections.Generic.List[object]]::new()
    $framesPerClip = [int][math]::Ceiling($SpriteFrameCount / [double]$SpriteClipCount)
    for ($clipIndex = 0; $clipIndex -lt $SpriteClipCount; $clipIndex++) {
        $clipFrames = [System.Collections.Generic.List[object]]::new()
        $first = $clipIndex * $framesPerClip
        $last = [math]::Min($SpriteFrameCount, $first + $framesPerClip)
        for ($frameIndex = $first; $frameIndex -lt $last; $frameIndex++) {
            [void]$clipFrames.Add([ordered]@{ frame = ('pressure.{0:D5}' -f $frameIndex); durationMs = 83; event = '' })
        }
        [void]$clips.Add([ordered]@{ id = ('pressure.clip.{0:D3}' -f $clipIndex); looping = $true; frames = @($clipFrames) })
    }
    $spriteId = 'sprite.pressure.long-sequence'
    $textureId = 'texture.pressure.atlas'
    $spriteRelative = 'art/pressure-long-sequence.sprite.json'
    $spritePath = Join-Path $assetsRoot $spriteRelative.Replace('/', '\')
    $sprite = [ordered]@{
        schema = 'noemancer.sprite-asset/0.2'; assetId = $spriteId; textureAsset = $textureId
        textureSize = @($atlasSize, $atlasSize); pixelsPerUnit = 64; sampling = 'nearest'; alphaMode = 'cutout'
        material = [ordered]@{ normalStrength = 0.0; emissiveColor = @(0.0, 0.0, 0.0); emissiveIntensity = 0.0; depthBias = 0.0 }
        frames = @($frames); clips = @($clips)
        provenance = [ordered]@{ sourceUri = $atlasRelative; sourceSha256 = 'fixture-generated'; generator = 'sprite-tilemap-production-pressure'; license = 'project-original' }
    }
    Write-Utf8Json -Path $spritePath -Value $sprite

    $registryPath = Join-Path $assetsRoot 'registry.json'
    $registry = Read-Json -Path $registryPath
    $records = @($registry.assets)
    if (@($records | Where-Object { $_.id -eq $spriteId -or $_.id -eq $textureId }).Count -gt 0) { throw 'Pressure fixture asset IDs already exist in the staging registry.' }
    $records += [ordered]@{
        id = $textureId; displayName = 'Pressure Atlas'; kind = 'Texture'; uri = 'asset://' + $atlasRelative
        path = $atlasRelative; license = 'project-original'; redistribution = 'project-only'; tags = @('sprite', 'atlas', 'pressure')
    }
    $records += [ordered]@{
        id = $spriteId; displayName = 'Long Sprite Sequence Pressure Fixture'; kind = 'Sprite'; uri = 'asset://' + $spriteRelative
        path = $spriteRelative; license = 'project-original'; redistribution = 'project-only'; tags = @('sprite', 'sequence', 'pressure')
        dependencies = @($textureId)
    }
    $registry.assets = @($records)
    Write-Utf8Json -Path $registryPath -Value $registry

    $manifestPath = Join-Path $Stage 'noemancer.project.json'
    $manifest = Read-Json -Path $manifestPath
    $packaged = @($manifest.packagedAssets)
    foreach ($id in @($textureId, $spriteId)) { if ($packaged -notcontains $id) { $packaged += $id } }
    # Keep the pressure package closure deliberately bounded.  The temporary
    # fixture scene below references only the generated Sprite; the Cook
    # dependency walk then closes the generated Atlas without compiling the
    # sample game's optional managed project or unrelated content.
    $manifest.packagedAssets = @()
    if ($null -ne $manifest.PSObject.Properties['scriptProject']) { $manifest.PSObject.Properties.Remove('scriptProject') }
    if ($null -ne $manifest.PSObject.Properties['hudDocument']) { $manifest.PSObject.Properties.Remove('hudDocument') }
    Write-Utf8Json -Path $manifestPath -Value $manifest
    $scenePath = Join-Path $Stage ([string]$manifest.startupScene).Replace('/', '\')
    Write-Utf8Json -Path $scenePath -Value ([ordered]@{
        schema = 'noemancer.scene/0.1'; sceneGuid = 'scene.sprite-tilemap-production-pressure'; name = 'Sprite Tilemap Production Pressure Fixture'
        entities = @(
            [ordered]@{ guid = 'entity.pressure.root'; name = 'Pressure Sprite'; parent = $null; components = [ordered]@{
                Transform = [ordered]@{ position = @(0, 0, 0); scale = @(1, 1, 1) }
                SpriteRenderer = [ordered]@{ spriteAsset = $spriteId; clip = ('pressure.clip.{0:D3}' -f 0); playbackSpeed = 1; playing = $true; flipX = $false; flipY = $false; sortingLayer = 'pressure'; sortingOrder = 0; visible = $true }
            } }
        )
    })
    return [ordered]@{
        spriteId = $spriteId; textureId = $textureId; spritePath = $spritePath; atlasPath = $atlasPath
        frameCount = $SpriteFrameCount; clipCount = $SpriteClipCount; clipFrameReferenceCount = $SpriteFrameCount
        atlasWidth = $atlasSize; atlasHeight = $atlasSize; frameWidth = $frameSize; frameHeight = $frameSize
        atlasCapacity = $columns * $columns; atlasOccupiedArea = $SpriteFrameCount * $frameSize * $frameSize
        atlasOccupancyRatio = [math]::Round(($SpriteFrameCount * $frameSize * $frameSize) / [double]($atlasSize * $atlasSize), 8)
        sourceBytes = (Get-Item -LiteralPath $atlasPath).Length; sourceSha256 = Get-Sha256 -Path $atlasPath
        # The minimized temporary scene is the sole package root.  Its Cook
        # dependency closure adds texture.pressure.atlas deterministically.
        packageAssetIds = @($spriteId)
    }
}

function Get-ToolDescriptor {
    param([Parameter(Mandatory = $true)]$Manifest, [Parameter(Mandatory = $true)][string]$Name)
    $tools = @($Manifest.tools)
    return ($tools | Where-Object { $_.name -eq $Name } | Select-Object -First 1)
}

function Invoke-ProjectTools {
    param([Parameter(Mandatory = $true)][string]$ProjectRoot, [Parameter(Mandatory = $true)]$Requests)
    $lines = [System.Collections.Generic.List[string]]::new()
    foreach ($request in $Requests) { [void]$lines.Add(($request | ConvertTo-Json -Depth 100 -Compress)) }
    $raw = Invoke-HiddenProcess -FilePath $RuntimePath -Arguments @('serve', '--project', $ProjectRoot, '--format', 'jsonl') -InputText (($lines -join "`n") + "`n") -TimeoutSeconds $TimeoutSeconds
    $responses = [System.Collections.Generic.List[object]]::new()
    foreach ($line in ($raw.stdout -split "`r?`n" | Where-Object { $_.Trim() })) {
        try { [void]$responses.Add(($line | ConvertFrom-Json)) } catch { Add-Issue -Code 'serve.invalid-jsonl' -Message $_.Exception.Message -ExitCode 4 }
    }
    return [ordered]@{ process = $raw; responses = @($responses) }
}

function Get-ResponseById {
    param([Parameter(Mandatory = $true)]$Responses, [Parameter(Mandatory = $true)][string]$Id)
    return ($Responses | Where-Object { $_.id -eq $Id } | Select-Object -First 1)
}

function Invoke-PressureCommand {
    param([Parameter(Mandatory = $true)]$ToolManifest)
    $descriptor = Get-ToolDescriptor -Manifest $ToolManifest -Name 'render.tilemap.pressure'
    if ($null -eq $descriptor) { Add-Issue -Code 'tilemap.command-unavailable' -Message 'render.tilemap.pressure is absent from the runtime tool manifest.' -ExitCode 4; return $null }
    $properties = $descriptor.inputSchema.properties
    $arguments = [ordered]@{
        chunkColumns = $TilemapChunkColumns; chunkRows = $TilemapChunkRows; chunkSize = $TilemapChunkSize; visibleChunkRadius = $VisibleChunkRadius
    }
    $sparse = $false
    if ($null -ne $properties -and $null -ne $properties.PSObject.Properties['occupiedCellsPerChunk']) {
        $arguments.chunkColumns = 64; $arguments.chunkRows = 64; $arguments.occupiedCellsPerChunk = 16; $sparse = $true
    } else {
        $arguments.chunkColumns = [math]::Min($TilemapChunkColumns, 32)
        $arguments.chunkRows = [math]::Min($TilemapChunkRows, 32)
    }
    $payload = $arguments | ConvertTo-Json -Compress
    $raw = Invoke-HiddenProcess -FilePath $RuntimePath -Arguments @('tool', 'call', 'render.tilemap.pressure', '--input', $payload) -TimeoutSeconds $TimeoutSeconds
    $report = $null
    try {
        $decoded = $raw.stdout | ConvertFrom-Json
        # `tool call` returns the normal Agent envelope.  Keep the evidence
        # report itself as the stable payload, while preserving the envelope
        # in the command sidecar below.
        $report = if ($null -ne $decoded.PSObject.Properties['result']) { $decoded.result } else { $decoded }
    } catch { Add-Issue -Code 'tilemap.invalid-json' -Message 'render.tilemap.pressure did not return JSON.' -ExitCode 4 }
    return [ordered]@{ process = $raw; report = $report; sparseRequested = $sparse; arguments = $arguments }
}

$receiptPath = [IO.Path]::GetFullPath($OutputPath)
$artifactRoot = Join-Path ([IO.Path]::GetDirectoryName($receiptPath)) ([IO.Path]::GetFileNameWithoutExtension($receiptPath) + '-artifacts')
$commandRoot = Join-Path $artifactRoot 'commands'
$startedAt = [DateTime]::UtcNow
$fixture = $null
$registry = $null
$cook = $null
$apply = $null
$inspect = $null
$tilemap = $null
$package = $null
$toolManifest = $null

try {
    if (-not (Test-Path -LiteralPath $Project -PathType Container)) { Add-Issue -Code 'input.project-missing' -Message "Project directory does not exist: $Project" -ExitCode 2; throw 'input' }
    if (-not (Test-Path -LiteralPath $RuntimePath -PathType Leaf)) { Add-Issue -Code 'input.runtime-missing' -Message "Runtime executable does not exist: $RuntimePath" -ExitCode 2; throw 'input' }
    $script:SourceTreeBefore = Get-TreeFingerprint -Root $Project
    $script:StagePath = Join-Path ([IO.Path]::GetTempPath()) ('noemancer-sprite-tilemap-pressure-' + [guid]::NewGuid().ToString('N'))
    Copy-ProjectToStage -Source $Project -Destination $script:StagePath
    $fixture = New-PressureSpriteFixture -Stage $script:StagePath

    $manifestRaw = Invoke-HiddenProcess -FilePath $RuntimePath -Arguments @('tools', 'list', '--format', 'json') -TimeoutSeconds $TimeoutSeconds
    if ($manifestRaw.exitCode -ne 0) { Add-Issue -Code 'tools-list.failed' -Message $manifestRaw.stderr -ExitCode 4; throw 'tools' }
    $toolManifest = $manifestRaw.stdout | ConvertFrom-Json

    $requests = @(
        [ordered]@{ id = 'registry'; name = 'asset.registry'; arguments = [ordered]@{ refresh = $false } },
        [ordered]@{ id = 'inspect'; name = 'asset.inspect'; arguments = [ordered]@{ assetId = $fixture.spriteId } },
        [ordered]@{ id = 'cook'; name = 'asset.cook.plan'; arguments = [ordered]@{ assetIds = @($fixture.packageAssetIds); targetProfile = 'windows-x64-debug' } }
    )
    $serve = Invoke-ProjectTools -ProjectRoot $script:StagePath -Requests $requests
    if ($serve.process.exitCode -ne 0) { Add-Issue -Code 'serve.failed' -Message ($serve.process.stderr.Trim()) -ExitCode 4; throw 'serve' }
    $registry = Get-ResponseById -Responses $serve.responses -Id 'registry'
    $inspect = Get-ResponseById -Responses $serve.responses -Id 'inspect'
    $cook = Get-ResponseById -Responses $serve.responses -Id 'cook'
    foreach ($result in @($registry, $inspect, $cook)) {
        if ($null -eq $result -or $result.exitCode -ne 0 -or $null -eq $result.response.result) { Add-Issue -Code 'serve.command-failed' -Message 'A project Agent command did not produce a successful result.' -ExitCode 4 }
    }
    $registryResult = if ($registry) { $registry.response.result } else { $null }
    $inspectResult = if ($inspect) { $inspect.response.result } else { $null }
    $cookResult = if ($cook) { $cook.response.result } else { $null }
    if ($null -eq $registryResult -or $registryResult.errorCount -ne 0 -or @($registryResult.assets | Where-Object { $_.id -eq $fixture.spriteId -and $_.available -eq $true }).Count -ne 1) { Add-Issue -Code 'registry.pressure-asset-missing' -Message 'Registry did not resolve the generated pressure Sprite.' -ExitCode 5 }
    if ($null -eq $inspectResult -or $inspectResult.valid -ne $true -or $inspectResult.renderPayload.frameCount -ne $fixture.frameCount -or
        $inspectResult.renderPayload.clipCount -ne $fixture.clipCount -or $inspectResult.renderPayload.production.valid -ne $true -or
        $inspectResult.renderPayload.production.totalClipFrameReferences -ne $fixture.clipFrameReferenceCount -or
        $inspectResult.renderPayload.production.atlas.occupiedArea -ne $fixture.atlasOccupiedArea) {
        Add-Issue -Code 'sprite.inspect-mismatch' -Message 'Sprite inspect did not preserve the generated frame/clip workload and production atlas metrics.' -ExitCode 5
    }
    if ($null -eq $cookResult -or $cookResult.valid -ne $true -or $cookResult.code -ne 'ok' -or @($cookResult.inputs | Where-Object { $_.assetId -eq $fixture.spriteId }).Count -ne 1 -or @($cookResult.inputs | Where-Object { $_.assetId -eq $fixture.textureId }).Count -ne 1) { Add-Issue -Code 'cook.plan-failed' -Message 'Cook plan did not close the project Sprite and Atlas dependency.' -ExitCode 5 }
    elseif ($cookResult.valid -eq $true) {
        # Package planning consumes the committed deterministic Cook manifest.
        # Apply it in a fresh attached session after refreshing the same staged
        # registry revision; no source project files are touched.
        $applyServe = Invoke-ProjectTools -ProjectRoot $script:StagePath -Requests @(
            [ordered]@{ id = 'apply-registry'; name = 'asset.registry'; arguments = [ordered]@{ refresh = $false } },
            [ordered]@{ id = 'apply'; name = 'asset.cook.apply'; arguments = [ordered]@{ plan = $cookResult; dryRun = $false } }
        )
        if ($applyServe.process.exitCode -ne 0) { Add-Issue -Code 'cook.apply-session-failed' -Message $applyServe.process.stderr.Trim() -ExitCode 4 }
        $apply = Get-ResponseById -Responses $applyServe.responses -Id 'apply'
        $applyResult = if ($apply) { $apply.response.result } else { $null }
        if ($null -eq $applyResult -or $applyResult.success -ne $true -or $applyResult.dryRun -ne $false -or $applyResult.code -ne 'ok') { Add-Issue -Code 'cook.apply-failed' -Message 'Cook plan was valid but no committed Cook manifest was produced.' -ExitCode 5 }
    }

    $tilemap = Invoke-PressureCommand -ToolManifest $toolManifest
    $report = if ($tilemap) { $tilemap.report } else { $null }
    if ($null -eq $report -or $tilemap.process.exitCode -ne 0 -or $report.valid -ne $true -or $report.code -ne 'ok') { Add-Issue -Code 'tilemap.pressure-failed' -Message 'Tilemap pressure command did not return a valid report.' -ExitCode 5 }
    elseif ([string]$report.schemaVersion -notmatch '^noemancer\.tilemap-pressure/0\.[2-9][0-9]*$') { Add-Issue -Code 'tilemap.schema-too-old' -Message "Unexpected Tilemap pressure schema: $($report.schemaVersion)" -ExitCode 5 }
    elseif ($null -eq $report.workload -or $report.workload.totalChunks -lt 1024 -or $null -eq $report.culling -or $null -eq $report.stableResidency -or [string]$report.scope -notmatch 'not-gpu-timing') { Add-Issue -Code 'tilemap.pressure-shape-invalid' -Message 'Tilemap pressure report lacks bounded workload, culling, stable residency, or non-GPU scope evidence.' -ExitCode 5 }

    $packagePath = Join-Path $artifactRoot 'package'
    try {
        $package = Invoke-HiddenProcess -FilePath $RuntimePath -Arguments @('package', '--project', $script:StagePath, '--output', $packagePath, '--target-profile', 'windows-x64-debug', '--dry-run', '--format', 'json') -TimeoutSeconds $TimeoutSeconds
        if ($package.exitCode -ne 0) { Add-Issue -Code 'package.dry-run-failed' -Message ($package.stderr.Trim()) -ExitCode 4 }
        else {
            try {
                $packageResult = $package.stdout | ConvertFrom-Json
                if ($packageResult.success -ne $true) { Add-Issue -Code 'package.dry-run-invalid' -Message 'Package dry-run returned success=false.' -ExitCode 5 }
            } catch { Add-Issue -Code 'package.invalid-json' -Message 'Package dry-run did not return JSON.' -ExitCode 4 }
        }
    } catch { Add-Issue -Code 'package.process-exception' -Message $_.Exception.Message -ExitCode 4 }

    Write-Utf8Json -Path (Join-Path $commandRoot 'tools.json') -Value $toolManifest
    if ($registry) { Write-Utf8Json -Path (Join-Path $commandRoot 'asset-registry.json') -Value $registry }
    if ($inspect) { Write-Utf8Json -Path (Join-Path $commandRoot 'asset-inspect.json') -Value $inspect }
    if ($cook) { Write-Utf8Json -Path (Join-Path $commandRoot 'asset-cook-plan.json') -Value $cook }
    if ($apply) { Write-Utf8Json -Path (Join-Path $commandRoot 'asset-cook-apply.json') -Value $apply }
    if ($tilemap) { Write-Utf8Json -Path (Join-Path $commandRoot 'tilemap-pressure.json') -Value $tilemap }
    if ($package) {
        $packageDocument = $null
        try { $packageDocument = $package.stdout | ConvertFrom-Json } catch { }
        Write-Utf8Json -Path (Join-Path $commandRoot 'package.json') -Value ([ordered]@{ process = $package; result = $packageDocument })
    }
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
        Add-Issue -Code 'source.changed' -Message 'The source project tree changed during the pressure run.' -ExitCode 6
    }
    $quality = [ordered]@{
        schemaVersion = 'noemancer.sprite-tilemap-production-pressure-quality/0.1'; pass = ($script:Issues.Count -eq 0)
        sprite = if ($fixture) { [ordered]@{ fixture = $fixture; production = if ($inspectResult) { $inspectResult.renderPayload.production } else { $null } } } else { $null }
        tilemap = if ($tilemap) { [ordered]@{ arguments = $tilemap.arguments; sparseRequested = $tilemap.sparseRequested; report = $tilemap.report } } else { $null }
        registry = if ($registry) { [ordered]@{ exitCode = $registry.exitCode; assetCount = if ($registry.response.result) { $registry.response.result.assetCount } else { $null }; errorCount = if ($registry.response.result) { $registry.response.result.errorCount } else { $null } } } else { $null }
        cook = if ($cook) { [ordered]@{ exitCode = $cook.exitCode; valid = if ($cook.response.result) { $cook.response.result.valid } else { $false }; inputCount = if ($cook.response.result) { @($cook.response.result.inputs).Count } else { 0 }; apply = if ($apply) { [ordered]@{ exitCode = $apply.exitCode; success = if ($apply.response.result) { $apply.response.result.success } else { $false }; artifactCount = if ($apply.response.result) { @($apply.response.result.artifacts).Count } else { 0 } } } else { $null } } } else { $null }
        package = if ($package) { [ordered]@{ exitCode = $package.exitCode; timedOut = $package.timedOut } } else { $null }
        scope = 'headless-source-registry-cook-package-and-deterministic-render-extraction; no GPU timing or fabricated GPU data'
    }
    try { Write-Utf8Json -Path (Join-Path $artifactRoot 'quality.json') -Value $quality } catch { Add-Issue -Code 'quality.write-failed' -Message $_.Exception.Message -ExitCode 7 }
    $receipt = [ordered]@{
        schemaVersion = 'noemancer.sprite-tilemap-production-pressure-evidence/0.1'; status = if ($script:Issues.Count -eq 0) { 'passed' } else { 'failed' }
        pass = ($script:Issues.Count -eq 0); startedAtUtc = $startedAt.ToString('o'); completedAtUtc = [DateTime]::UtcNow.ToString('o')
        source = [ordered]@{ project = [IO.Path]::GetFullPath($Project); treeBefore = $script:SourceTreeBefore; treeAfter = $script:SourceTreeAfter; unchanged = ($script:SourceTreeBefore -and $script:SourceTreeAfter -and $script:SourceTreeBefore.sha256 -eq $script:SourceTreeAfter.sha256) }
        staging = [ordered]@{ path = if ($script:StageKept) { $script:StagePath } else { $null }; kept = $script:StageKept }
        budgets = [ordered]@{ spriteFrameCount = $SpriteFrameCount; spriteClipCount = $SpriteClipCount; tilemapChunkColumns = $TilemapChunkColumns; tilemapChunkRows = $TilemapChunkRows; tilemapChunkSize = $TilemapChunkSize; visibleChunkRadius = $VisibleChunkRadius; timeoutSeconds = $TimeoutSeconds }
        artifacts = [ordered]@{ quality = [IO.Path]::GetRelativePath((Split-Path -Parent $receiptPath), (Join-Path $artifactRoot 'quality.json')).Replace('\', '/'); commands = [IO.Path]::GetRelativePath((Split-Path -Parent $receiptPath), $commandRoot).Replace('\', '/') }
        issues = @($script:Issues); exitCode = $script:ExitCode
    }
    try { Write-Utf8Json -Path $receiptPath -Value $receipt } catch { $script:ExitCode = 7; Write-Error $_.Exception.Message }
}

exit $script:ExitCode
