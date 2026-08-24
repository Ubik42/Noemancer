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
    [switch]$KeepStaging,
    [switch]$CommitPackage,
    [switch]$ValidatePlayer,
    [ValidateRange(1, 600)]
    [int]$PlayerFrames = 256,
    [ValidateRange(60, 600)]
    [int]$PlayerSampleFrames = 60
)

$ErrorActionPreference = 'Stop'
$script:ExitCode = 0
$script:Issues = [System.Collections.Generic.List[object]]::new()
$script:StagePath = $null
$script:StageKept = $false
$script:SourceTreeBefore = $null
$script:SourceTreeAfter = $null
$script:PackageCommitRequested = ($CommitPackage -or $ValidatePlayer)

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
                # The primitive keeps the optional hidden GPU Player probe
                # focused on a real renderable entity while the SpriteRenderer
                # remains the workload under test.  It is built-in and adds no
                # source asset to the Cook/package closure.
                MeshRenderer = [ordered]@{ meshAsset = 'asset.primitive.cube'; castsShadows = $true; receivesShadows = $true; visible = $true }
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

function Test-Ktx2Payload {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    $bytes = [IO.File]::ReadAllBytes($Path)
    $magic = [byte[]]@(0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A)
    if ($bytes.Length -lt $magic.Length) { return $false }
    for ($index = 0; $index -lt $magic.Length; $index++) {
        if ($bytes[$index] -ne $magic[$index]) { return $false }
    }
    return $true
}

function Test-PackagedSpriteAtlasClosure {
    param(
        [Parameter(Mandatory = $true)][string]$PackageRoot,
        [Parameter(Mandatory = $true)][string]$SpriteAssetId
    )
    $report = [ordered]@{
        packageRoot = $PackageRoot
        registryPath = $null
        parent = $null
        pages = @()
        pagePayloads = @()
        sourceAtlasFiles = @()
        valid = $false
    }
    $registryPath = Join-Path $PackageRoot 'content/assets/registry.json'
    $report.registryPath = $registryPath
    if (-not (Test-Path -LiteralPath $registryPath -PathType Leaf)) {
        Add-Issue -Code 'package.registry-missing' -Message "Committed package has no content/assets/registry.json: $registryPath" -ExitCode 5
        return $report
    }
    $registryDocument = $null
    try { $registryDocument = Read-Json -Path $registryPath }
    catch {
        Add-Issue -Code 'package.registry-invalid-json' -Message "Committed package registry is not valid JSON: $($_.Exception.Message)" -ExitCode 5
        return $report
    }
    if ($null -eq $registryDocument.assets) {
        Add-Issue -Code 'package.registry-assets-missing' -Message 'Committed package registry does not contain an assets array.' -ExitCode 5
        return $report
    }
    $records = @($registryDocument.assets)
    $parent = $records | Where-Object { $_.id -eq $SpriteAssetId } | Select-Object -First 1
    if ($null -eq $parent -or [string]$parent.kind -ne 'SpriteAtlas') {
        Add-Issue -Code 'package.atlas-parent-missing' -Message "Committed package registry has no SpriteAtlas parent for $SpriteAssetId." -ExitCode 5
    } else {
        $report.parent = [ordered]@{ id = [string]$parent.id; kind = [string]$parent.kind; path = [string]$parent.path; sourceAssetId = [string]$parent.sourceAssetId }
    }
    $pages = @($records | Where-Object {
        $_.kind -eq 'SpriteAtlasPage' -and (
            $_.sourceAssetId -eq $SpriteAssetId -or [string]$_.id -like ($SpriteAssetId + '.atlas.page.*'))
    })
    if ($pages.Count -eq 0) {
        Add-Issue -Code 'package.atlas-pages-missing' -Message "Committed package registry has no SpriteAtlasPage outputs for $SpriteAssetId." -ExitCode 5
    }
    # Registry paths are intentionally relative to content/assets (the same
    # root used by the generated package manifest), not the broader content
    # directory.  Keeping this explicit catches a page that is listed but not
    # actually present in the committed closure.
    $contentRoot = [IO.Path]::GetFullPath((Join-Path $PackageRoot 'content/assets'))
    $pageReports = [System.Collections.Generic.List[object]]::new()
    foreach ($page in $pages) {
        $relativePath = [string]$page.path
        $payloadPath = if ($relativePath) { Join-Path $contentRoot $relativePath } else { '' }
        $payloadValid = ($relativePath -and (Test-Path -LiteralPath $payloadPath -PathType Leaf) -and
            ([IO.Path]::GetExtension($relativePath) -ieq '.ktx2') -and (Test-Ktx2Payload -Path $payloadPath))
        $actualHash = if ($payloadValid) { 'sha256:' + (Get-Sha256 -Path $payloadPath) } else { $null }
        $hashMatches = ($payloadValid -and [string]$page.contentHash -eq $actualHash)
        $pageReport = [ordered]@{
            id = [string]$page.id; kind = [string]$page.kind; path = $relativePath
            payloadExists = [bool](Test-Path -LiteralPath $payloadPath -PathType Leaf)
            ktx2 = $payloadValid; expectedHash = [string]$page.contentHash
            actualHash = $actualHash; hashMatches = $hashMatches
        }
        [void]$pageReports.Add($pageReport)
        if (-not $payloadValid) {
            Add-Issue -Code 'package.atlas-page-payload-missing' -Message "SpriteAtlasPage $($page.id) does not resolve to a valid KTX2 payload inside the package." -ExitCode 5
        } elseif (-not $hashMatches) {
            Add-Issue -Code 'package.atlas-page-hash-mismatch' -Message "SpriteAtlasPage $($page.id) payload hash does not match the packaged registry." -ExitCode 5
        }
    }
    $report.pages = @($pageReports)
    $sourceFiles = @(Get-ChildItem -LiteralPath $PackageRoot -Recurse -File -Force | Where-Object {
        $_.Name -ieq 'pressure-atlas.png'
    })
    $report.sourceAtlasFiles = @($sourceFiles | ForEach-Object { [IO.Path]::GetRelativePath($PackageRoot, $_.FullName).Replace('\', '/') })
    if ($sourceFiles.Count -gt 0) {
        Add-Issue -Code 'package.source-atlas-included' -Message 'Committed package closure contains the source pressure-atlas.png.' -ExitCode 5
    }
    $report.valid = ($null -ne $report.parent -and $pages.Count -gt 0 -and
        @($pageReports | Where-Object { -not $_.ktx2 -or -not $_.hashMatches }).Count -eq 0 -and
        $sourceFiles.Count -eq 0)
    return $report
}

function Test-PlayerAtlasBindingOutput {
    param(
        [Parameter(Mandatory = $true)]$ProcessResult,
        [Parameter(Mandatory = $true)][string]$Label
    )
    $bindingLines = @($ProcessResult.stdout -split "`r?`n" | Where-Object { $_ -match 'sprite\.atlas-binding' })
    # The runtime event envelope records successful atlas registration as
    # message.code="ok" (failed registrations use an explicit success=false
    # field).  Accept both representations so the evidence remains stable
    # across human/JSON logger versions without accepting an unrelated line.
    $successLines = @($bindingLines | Where-Object { $_ -match 'success.*true' -or $_ -match 'code.*ok' })
    $report = [ordered]@{
        label = $Label; exitCode = $ProcessResult.exitCode; timedOut = $ProcessResult.timedOut
        bindingLineCount = $bindingLines.Count; success = ($successLines.Count -gt 0)
    }
    if ($ProcessResult.timedOut -or $ProcessResult.exitCode -ne 0) {
        Add-Issue -Code ("player.$Label-failed") -Message "$Label Player process failed or timed out (exitCode=$($ProcessResult.exitCode))." -ExitCode 5
    }
    if ($successLines.Count -eq 0) {
        Add-Issue -Code ("player.$Label-atlas-binding-missing") -Message "$Label Player stdout did not contain a successful sprite.atlas-binding event." -ExitCode 5
    }
    return $report
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
$packageRegistry = $null
$packagePath = $null
$playerExecutable = $null
$playerHeadless = $null
$playerD3D12 = $null
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

    $packageDirectoryName = if ($script:PackageCommitRequested) { 'package-commit' } else { 'package' }
    $packagePath = Join-Path $artifactRoot $packageDirectoryName
    $packageArguments = @(
        'package', '--project', $script:StagePath, '--output', $packagePath,
        '--target-profile', 'windows-x64-debug'
    )
    if (-not $script:PackageCommitRequested) { $packageArguments += '--dry-run' }
    $packageArguments += '--format'
    $packageArguments += 'json'
    try {
        $package = Invoke-HiddenProcess -FilePath $RuntimePath -Arguments $packageArguments -TimeoutSeconds $TimeoutSeconds
        if ($package.exitCode -ne 0) {
            $packageCode = if ($script:PackageCommitRequested) { 'package.commit-failed' } else { 'package.dry-run-failed' }
            Add-Issue -Code $packageCode -Message ($package.stderr.Trim()) -ExitCode 4
        } else {
            try {
                $packageResult = $package.stdout | ConvertFrom-Json
                if ($packageResult.success -ne $true) {
                    $packageCode = if ($script:PackageCommitRequested) { 'package.commit-invalid' } else { 'package.dry-run-invalid' }
                    Add-Issue -Code $packageCode -Message 'Package command returned success=false.' -ExitCode 5
                } elseif ($script:PackageCommitRequested -and -not (Test-Path -LiteralPath $packagePath -PathType Container)) {
                    Add-Issue -Code 'package.commit-output-missing' -Message "Package reported success but did not create its output directory: $packagePath" -ExitCode 5
                } elseif ($script:PackageCommitRequested) {
                    $packageRegistry = Test-PackagedSpriteAtlasClosure -PackageRoot $packagePath -SpriteAssetId $fixture.spriteId
                }
            } catch {
                $packageCode = if ($script:PackageCommitRequested) { 'package.invalid-json' } else { 'package.invalid-json' }
                Add-Issue -Code $packageCode -Message 'Package command did not return JSON.' -ExitCode 4
            }
        }
    } catch {
        $packageCode = if ($script:PackageCommitRequested) { 'package.process-exception' } else { 'package.process-exception' }
        Add-Issue -Code $packageCode -Message $_.Exception.Message -ExitCode 4
    }

    if ($ValidatePlayer) {
        if (-not $script:PackageCommitRequested -or $null -eq $package -or $package.exitCode -ne 0 -or
            -not (Test-Path -LiteralPath $packagePath -PathType Container) -or $null -eq $packageRegistry -or
            $packageRegistry.valid -ne $true) {
            Add-Issue -Code 'player.package-not-ready' -Message 'Player validation requires a successfully committed package with a valid SpriteAtlas closure.' -ExitCode 5
        } else {
            $profilePath = Join-Path $packagePath 'config/game-profile.json'
            $packagedExecutables = @(Get-ChildItem -LiteralPath (Join-Path $packagePath 'bin') -Filter '*.exe' -File -ErrorAction SilentlyContinue)
            if (-not (Test-Path -LiteralPath $profilePath -PathType Leaf)) {
                Add-Issue -Code 'player.profile-missing' -Message "Committed package has no game profile: $profilePath" -ExitCode 5
            } elseif ($packagedExecutables.Count -eq 0) {
                Add-Issue -Code 'player.executable-missing' -Message "Committed package has no executable under bin: $packagePath" -ExitCode 5
            } else {
                $playerExecutable = $packagedExecutables | Select-Object -First 1 -ExpandProperty FullName
                try {
                    $playerHeadless = Invoke-HiddenProcess -FilePath $playerExecutable -Arguments @(
                        'player', '--profile', $profilePath, '--headless', '--frames', [string]$PlayerFrames, '--format', 'json'
                    ) -TimeoutSeconds $TimeoutSeconds
                    $playerHeadlessReport = Test-PlayerAtlasBindingOutput -ProcessResult $playerHeadless -Label 'headless'
                } catch {
                    Add-Issue -Code 'player.headless-process-exception' -Message $_.Exception.Message -ExitCode 5
                }
                try {
                    $performanceEvidencePath = Join-Path $artifactRoot 'player-d3d12-performance.json'
                    $playerD3D12 = Invoke-HiddenProcess -FilePath $playerExecutable -Arguments @(
                        'player', '--profile', $profilePath, '--gpu-backend', 'direct3d12',
                        '--performance-evidence', $performanceEvidencePath, '--performance-hidden',
                        '--performance-warmup-frames', '1', '--performance-sample-frames', [string]$PlayerSampleFrames,
                        '--format', 'json'
                    ) -TimeoutSeconds $TimeoutSeconds
                    $playerD3D12Report = Test-PlayerAtlasBindingOutput -ProcessResult $playerD3D12 -Label 'hidden-d3d12'
                } catch {
                    Add-Issue -Code 'player.hidden-d3d12-process-exception' -Message $_.Exception.Message -ExitCode 5
                }
            }
        }
    }

    Write-Utf8Json -Path (Join-Path $commandRoot 'tools.json') -Value $toolManifest
    if ($registry) { Write-Utf8Json -Path (Join-Path $commandRoot 'asset-registry.json') -Value $registry }
    if ($inspect) { Write-Utf8Json -Path (Join-Path $commandRoot 'asset-inspect.json') -Value $inspect }
    if ($cook) { Write-Utf8Json -Path (Join-Path $commandRoot 'asset-cook-plan.json') -Value $cook }
    if ($apply) { Write-Utf8Json -Path (Join-Path $commandRoot 'asset-cook-apply.json') -Value $apply }
    if ($tilemap) { Write-Utf8Json -Path (Join-Path $commandRoot 'tilemap-pressure.json') -Value $tilemap }
    if ($package) {
        $packageDocument = $null
        try { $packageDocument = $package.stdout | ConvertFrom-Json } catch { }
        Write-Utf8Json -Path (Join-Path $commandRoot 'package.json') -Value ([ordered]@{
            process = $package; result = $packageDocument; commitRequested = $script:PackageCommitRequested; output = $packagePath
        })
    }
    if ($packageRegistry) { Write-Utf8Json -Path (Join-Path $commandRoot 'package-registry.json') -Value $packageRegistry }
    if ($playerHeadless) { Write-Utf8Json -Path (Join-Path $commandRoot 'player-headless.json') -Value ([ordered]@{ process = $playerHeadless; report = $playerHeadlessReport }) }
    if ($playerD3D12) { Write-Utf8Json -Path (Join-Path $commandRoot 'player-hidden-d3d12.json') -Value ([ordered]@{ process = $playerD3D12; report = $playerD3D12Report }) }
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
        package = if ($package) {
            [ordered]@{
                exitCode = $package.exitCode; timedOut = $package.timedOut; commitRequested = $script:PackageCommitRequested
                output = $packagePath; registry = $packageRegistry
            }
        } else { $null }
        player = if ($ValidatePlayer) {
            [ordered]@{
                executable = $playerExecutable; frames = $PlayerFrames; sampleFrames = $PlayerSampleFrames
                headless = if ($playerHeadless) { $playerHeadlessReport } else { $null }
                hiddenD3D12 = if ($playerD3D12) { $playerD3D12Report } else { $null }
            }
        } else { $null }
        scope = if ($ValidatePlayer) {
            'hidden-source-registry-cook-committed-package-headless-player-hidden-d3d12-and-deterministic-render-extraction; no fabricated GPU data'
        } else { 'headless-source-registry-cook-package-and-deterministic-render-extraction; no GPU timing or fabricated GPU data' }
    }
    try { Write-Utf8Json -Path (Join-Path $artifactRoot 'quality.json') -Value $quality } catch { Add-Issue -Code 'quality.write-failed' -Message $_.Exception.Message -ExitCode 7 }
    $receipt = [ordered]@{
        schemaVersion = 'noemancer.sprite-tilemap-production-pressure-evidence/0.1'; status = if ($script:Issues.Count -eq 0) { 'passed' } else { 'failed' }
        pass = ($script:Issues.Count -eq 0); startedAtUtc = $startedAt.ToString('o'); completedAtUtc = [DateTime]::UtcNow.ToString('o')
        source = [ordered]@{ project = [IO.Path]::GetFullPath($Project); treeBefore = $script:SourceTreeBefore; treeAfter = $script:SourceTreeAfter; unchanged = ($script:SourceTreeBefore -and $script:SourceTreeAfter -and $script:SourceTreeBefore.sha256 -eq $script:SourceTreeAfter.sha256) }
        staging = [ordered]@{ path = if ($script:StageKept) { $script:StagePath } else { $null }; kept = $script:StageKept }
        budgets = [ordered]@{
            spriteFrameCount = $SpriteFrameCount; spriteClipCount = $SpriteClipCount; tilemapChunkColumns = $TilemapChunkColumns
            tilemapChunkRows = $TilemapChunkRows; tilemapChunkSize = $TilemapChunkSize; visibleChunkRadius = $VisibleChunkRadius
            timeoutSeconds = $TimeoutSeconds; commitPackage = [bool]$CommitPackage; validatePlayer = [bool]$ValidatePlayer
            playerFrames = $PlayerFrames; playerSampleFrames = $PlayerSampleFrames
        }
        artifacts = [ordered]@{
            quality = [IO.Path]::GetRelativePath((Split-Path -Parent $receiptPath), (Join-Path $artifactRoot 'quality.json')).Replace('\', '/')
            commands = [IO.Path]::GetRelativePath((Split-Path -Parent $receiptPath), $commandRoot).Replace('\', '/')
            package = if ($packagePath) { [IO.Path]::GetRelativePath((Split-Path -Parent $receiptPath), $packagePath).Replace('\', '/') } else { $null }
        }
        issues = @($script:Issues); exitCode = $script:ExitCode
    }
    try { Write-Utf8Json -Path $receiptPath -Value $receipt } catch { $script:ExitCode = 7; Write-Error $_.Exception.Message }
}

exit $script:ExitCode
