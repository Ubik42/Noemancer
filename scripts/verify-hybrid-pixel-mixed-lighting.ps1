[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$ProjectRoot = $(if ($env:NOEMANCER_PLATFORMER_PROJECT) { $env:NOEMANCER_PLATFORMER_PROJECT } else { Join-Path ([IO.Path]::GetPathRoot($PSScriptRoot)) '3D\NoemancerPlatformer' }),
    [string]$RuntimePath = '',
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Release',
    [ValidateRange(1, 60)]
    # Allow the packaged KTX2 mip-tail streamer to reach the authored detail
    # tier before visual evidence is captured. Three frames proved semantic
    # startup but produced a deliberately coarse tail-only image.
    [int]$Frames = 16,
    [ValidateRange(10, 600)]
    [int]$TimeoutSeconds = 180,
    [ValidateRange(640, 7680)]
    [int]$WindowWidth = 1280,
    [ValidateRange(360, 4320)]
    [int]$WindowHeight = 720,
    [ValidateSet('vfx-post', 'production')]
    [string]$Contract = 'vfx-post',
    [string]$OutputRoot = ''
)

# Hidden acceptance contract for the active frontier:
#   hybrid-pixel.pixel-aligned-vfx-and-controlled-post
#
# The script owns only a temporary Lumen Run copy and generated evidence.  It
# deliberately does not build or mutate the native engine, shaders, source
# project, or authoritative documentation.  Exit codes are stable:
#   0 = all package, Render World, Renderer Status and dual-backend captures pass
#   2 = invalid invocation (project/runtime/output/config)
#   3 = bounded acceptance contract failure
#   1 = unexpected verifier failure

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:EngineRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$script:Issues = [System.Collections.Generic.List[object]]::new()
$script:MaxIssues = 192
$script:MaxCapturedBytes = 4MB
$script:OutputRootPath = $null
$script:DiagnosticsRoot = $null
$script:TemporaryProjectRoot = $null
$script:SourceManifestHashBefore = ''
$script:SourceManifestHashAfter = ''
$script:SourceProjectHashBefore = ''
$script:SourceProjectHashAfter = ''
$script:ExpectedProfile = $null
$script:Fixture = $null
$script:NativeBoundary = $null
$script:SourceHeadless = $null
$script:Package = $null
$script:PackageHeadless = $null
$script:PackageClosure = $null
$script:SourceCaptures = [System.Collections.Generic.List[object]]::new()
$script:PackageCaptures = [System.Collections.Generic.List[object]]::new()
$script:Comparisons = $null
$script:VirtualWidth = 320
$script:VirtualHeight = 180
$script:PixelsPerUnit = 16.0
$script:TargetProfile = 'windows-x64-release'
$script:Backends = @('direct3d12', 'vulkan')
$script:ProductionContract = $Contract -eq 'production'
$script:ProductionEvidence = $null
$script:PerformanceEvidence = [System.Collections.Generic.List[object]]::new()

if ($script:ProductionContract) {
    . (Join-Path $PSScriptRoot 'lib\hybrid-pixel-production-fixture.ps1')
    . (Join-Path $PSScriptRoot 'lib\hybrid-pixel-production-evidence.ps1')
}

function Get-PropertyValue {
    param(
        [AllowNull()]$Object,
        [Parameter(Mandatory = $true)][string]$Name
    )
    if ($null -eq $Object) { return $null }
    if ($Object -is [System.Collections.IDictionary] -and $Object.Contains($Name)) { return $Object[$Name] }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}

function Add-Issue {
    param(
        [Parameter(Mandatory = $true)][string]$Code,
        [Parameter(Mandatory = $true)][string]$Stage,
        [Parameter(Mandatory = $true)][string]$Message,
        [string]$Path = ''
    )
    if ($script:Issues.Count -ge $script:MaxIssues) { return }
    $issue = [ordered]@{ code = $Code; stage = $Stage; message = $Message }
    if (-not [string]::IsNullOrWhiteSpace($Path)) { $issue.path = $Path }
    [void]$script:Issues.Add([pscustomobject]$issue)
}

function Get-FullPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    return [IO.Path]::GetFullPath($Path)
}

function Get-RelativeArtifactPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    if ($null -eq $script:OutputRootPath) { return $Path }
    try {
        $relative = [IO.Path]::GetRelativePath($script:OutputRootPath, $Path)
        if ($relative -eq '..' -or $relative.StartsWith('..' + [IO.Path]::DirectorySeparatorChar) -or
            $relative.StartsWith('..' + [IO.Path]::AltDirectorySeparatorChar)) { return $Path }
        return $relative.Replace('\', '/')
    }
    catch { return $Path }
}

function Get-Sha256 {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return '' }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-TextSha256 {
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Text)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [Text.UTF8Encoding]::new($false).GetBytes($Text)
        return ([BitConverter]::ToString($algorithm.ComputeHash($bytes)) -replace '-', '').ToLowerInvariant()
    }
    finally { $algorithm.Dispose() }
}

function Write-Utf8 {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Text
    )
    $parent = Split-Path -Parent $Path
    if (-not [string]::IsNullOrWhiteSpace($parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    [IO.File]::WriteAllText($Path, $Text, [Text.UTF8Encoding]::new($false))
}

function Read-JsonFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Stage,
        [Parameter(Mandatory = $true)][string]$Label
    )
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        Add-Issue -Code 'json.missing' -Stage $Stage -Path (Get-RelativeArtifactPath $Path) -Message "$Label is missing."
        return $null
    }
    try {
        $raw = [IO.File]::ReadAllText($Path)
        if ([string]::IsNullOrWhiteSpace($raw)) { throw 'JSON is empty.' }
        return ConvertFrom-Json -InputObject $raw -Depth 100
    }
    catch {
        Add-Issue -Code 'json.invalid' -Stage $Stage -Path (Get-RelativeArtifactPath $Path) -Message "$Label is invalid JSON: $($_.Exception.Message)"
        return $null
    }
}

function Get-ProjectTreeHash {
    param(
        [Parameter(Mandatory = $true)][string]$ProjectPath,
        [Parameter(Mandatory = $true)][string]$Stage
    )
    if (-not (Test-Path -LiteralPath $ProjectPath -PathType Container)) { return '' }
    $ignoredPattern = '(?i)[\\/](?:\.git|bin|obj|generated)[\\/]'
    $maximumFiles = 4096
    $maximumBytes = 512MB
    try {
        $files = @(
            Get-ChildItem -LiteralPath $ProjectPath -Recurse -File -Force -ErrorAction Stop |
                Where-Object { $_.FullName -notmatch $ignoredPattern } |
                Sort-Object FullName
        )
        if ($files.Count -gt $maximumFiles) {
            Add-Issue -Code 'project.hash-file-limit' -Stage $Stage -Message "Project hash contains $($files.Count) files; the bounded limit is $maximumFiles."
            return ''
        }
        $totalBytes = [uint64]0
        $lines = [System.Collections.Generic.List[string]]::new()
        foreach ($file in $files) {
            $totalBytes += [uint64]$file.Length
            if ($totalBytes -gt $maximumBytes) {
                Add-Issue -Code 'project.hash-byte-limit' -Stage $Stage -Message "Project hash exceeds the bounded $maximumBytes byte limit."
                return ''
            }
            $relative = [IO.Path]::GetRelativePath($ProjectPath, $file.FullName).Replace('\', '/')
            [void]$lines.Add("$relative|$($file.Length)|$(Get-Sha256 $file.FullName)")
        }
        return Get-TextSha256 ($lines -join "`n")
    }
    catch {
        Add-Issue -Code 'project.hash-failed' -Stage $Stage -Message "Could not hash project tree: $($_.Exception.Message)"
        return ''
    }
}

function Copy-ProjectSnapshot {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination
    )
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    $ignored = @('.git', 'bin', 'obj', 'generated')
    foreach ($child in @(Get-ChildItem -LiteralPath $Source -Force)) {
        if ($ignored -contains $child.Name) { continue }
        Copy-Item -LiteralPath $child.FullName -Destination (Join-Path $Destination $child.Name) -Recurse -Force
    }
}

function Resolve-SafeRelativePath {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$RelativePath,
        [Parameter(Mandatory = $true)][string]$Stage,
        [Parameter(Mandatory = $true)][string]$Field
    )
    if ([string]::IsNullOrWhiteSpace($RelativePath) -or [IO.Path]::IsPathRooted($RelativePath) -or
        $RelativePath -match '(^|[\\/])\.\.(?:[\\/]|$)') {
        Add-Issue -Code 'path.unsafe' -Stage $Stage -Path $Field -Message "Path '$RelativePath' is not a safe relative path."
        return $null
    }
    $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    $pathFull = [IO.Path]::GetFullPath((Join-Path $Root $RelativePath))
    if (-not $pathFull.StartsWith($rootFull, [StringComparison]::OrdinalIgnoreCase)) {
        Add-Issue -Code 'path.escape' -Stage $Stage -Path $Field -Message "Path '$RelativePath' escapes '$Root'."
        return $null
    }
    return $pathFull
}

function Limit-Text {
    param([AllowNull()][string]$Text)
    if ($null -eq $Text) { $Text = '' }
    $encoding = [Text.UTF8Encoding]::new($false)
    $bytes = $encoding.GetByteCount($Text)
    if ($bytes -le $script:MaxCapturedBytes) { return [ordered]@{ text = $Text; bytes = $bytes; truncated = $false } }
    $characters = [Math]::Min($Text.Length, $script:MaxCapturedBytes)
    while ($characters -gt 0 -and $encoding.GetByteCount($Text.Substring(0, $characters)) -gt $script:MaxCapturedBytes) { $characters-- }
    $limited = if ($characters -gt 0) { $Text.Substring(0, $characters) } else { '' }
    return [ordered]@{ text = $limited; bytes = $bytes; truncated = $true }
}

function Quote-CommandToken {
    param([Parameter(Mandatory = $true)][string]$Token)
    if ($Token -notmatch '[\s"]') { return $Token }
    return '"' + ($Token -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1') + '"'
}

function ConvertTo-CommandLine {
    param(
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )
    return ((@($Executable) + @($Arguments)) | ForEach-Object { Quote-CommandToken ([string]$_) }) -join ' '
}

function Invoke-BoundedProcess {
    param(
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [Parameter(Mandatory = $true)][string]$StdoutPath,
        [Parameter(Mandatory = $true)][string]$StderrPath,
        [Parameter(Mandatory = $true)][string]$Stage
    )
    $started = [DateTimeOffset]::UtcNow
    $stdoutText = ''
    $stderrText = ''
    $exitCode = -1
    $timedOut = $false
    $startError = ''
    $process = $null
    try {
        $info = [Diagnostics.ProcessStartInfo]::new()
        $info.FileName = $Executable
        $info.WorkingDirectory = $WorkingDirectory
        $info.UseShellExecute = $false
        $info.CreateNoWindow = $true
        $info.RedirectStandardOutput = $true
        $info.RedirectStandardError = $true
        if ($null -ne $script:DiagnosticsRoot) { $info.Environment['NOEMANCER_DIAGNOSTICS_DIR'] = $script:DiagnosticsRoot }
        foreach ($argument in $Arguments) { [void]$info.ArgumentList.Add([string]$argument) }
        $process = [Diagnostics.Process]::new()
        $process.StartInfo = $info
        if (-not $process.Start()) { throw 'Process.Start returned false.' }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            $timedOut = $true
            try { $process.Kill($true) } catch { try { $process.Kill() } catch { } }
            try { [void]$process.WaitForExit(5000) } catch { }
        }
        try { $stdoutText = $stdoutTask.GetAwaiter().GetResult() } catch { $stdoutText = '' }
        try { $stderrText = $stderrTask.GetAwaiter().GetResult() } catch { $stderrText = '' }
        if ($process.HasExited) { $exitCode = $process.ExitCode }
    }
    catch { $startError = $_.Exception.Message }
    finally { if ($null -ne $process) { $process.Dispose() } }

    $stdoutLimited = Limit-Text $stdoutText
    $stderrLimited = Limit-Text $stderrText
    Write-Utf8 -Path $StdoutPath -Text $stdoutLimited.text
    Write-Utf8 -Path $StderrPath -Text $stderrLimited.text
    if ($stdoutLimited.truncated -or $stderrLimited.truncated) { Add-Issue -Code 'process.output-truncated' -Stage $Stage -Message 'A bounded process log exceeded the receipt capture limit.' }
    if (-not [string]::IsNullOrWhiteSpace($startError)) { Add-Issue -Code 'process.start-failed' -Stage $Stage -Message $startError }
    if ($timedOut) { Add-Issue -Code 'process.timeout' -Stage $Stage -Message "The process exceeded the $TimeoutSeconds second timeout." }
    return [ordered]@{
        stage = $Stage; executable = $Executable; arguments = @($Arguments)
        commandLine = ConvertTo-CommandLine -Executable $Executable -Arguments $Arguments
        workingDirectory = $WorkingDirectory; exitCode = $exitCode; timedOut = $timedOut
        durationMs = [int]([DateTimeOffset]::UtcNow.Subtract($started).TotalMilliseconds)
        stdoutText = $stdoutLimited.text; stderrText = $stderrLimited.text
        stdoutPath = $StdoutPath; stderrPath = $StderrPath
        stdoutBytes = $stdoutLimited.bytes; stderrBytes = $stderrLimited.bytes
    }
}

function Get-ProcessEvidence {
    param([Parameter(Mandatory = $true)]$ProcessResult)
    return [ordered]@{
        stage = $ProcessResult.stage; executable = $ProcessResult.executable; arguments = @($ProcessResult.arguments)
        commandLine = $ProcessResult.commandLine; workingDirectory = $ProcessResult.workingDirectory
        exitCode = $ProcessResult.exitCode; timedOut = $ProcessResult.timedOut; durationMs = $ProcessResult.durationMs
        stdout = [ordered]@{ path = Get-RelativeArtifactPath $ProcessResult.stdoutPath; bytes = $ProcessResult.stdoutBytes; sha256 = Get-Sha256 $ProcessResult.stdoutPath }
        stderr = [ordered]@{ path = Get-RelativeArtifactPath $ProcessResult.stderrPath; bytes = $ProcessResult.stderrBytes; sha256 = Get-Sha256 $ProcessResult.stderrPath }
    }
}

function Get-JsonLogEvents {
    param([Parameter(Mandatory = $true)][string]$Text)
    $events = [System.Collections.Generic.List[object]]::new()
    foreach ($line in ($Text -split "`r?`n")) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        try { $envelope = ConvertFrom-Json -InputObject $line -Depth 100 } catch { continue }
        $eventName = [string](Get-PropertyValue $envelope 'event')
        if ([string]::IsNullOrWhiteSpace($eventName)) { continue }
        $message = [string](Get-PropertyValue $envelope 'message')
        $payload = Get-PropertyValue $envelope 'payload'
        if ($null -eq $payload -and -not [string]::IsNullOrWhiteSpace($message)) {
            try { $payload = ConvertFrom-Json -InputObject $message -Depth 100 } catch { }
        }
        [void]$events.Add([pscustomobject]@{
            event = $eventName; level = [string](Get-PropertyValue $envelope 'level')
            payload = $payload; message = $message
        })
    }
    return @($events)
}

function Get-LastEvent {
    param([Parameter(Mandatory = $true)]$Events, [Parameter(Mandatory = $true)][string]$Name)
    $matches = @($Events | Where-Object { $_.event -eq $Name })
    if ($matches.Count -eq 0) { return $null }
    return $matches[$matches.Count - 1]
}

function New-RegistryRecord {
    param(
        [Parameter(Mandatory = $true)][string]$Id,
        [Parameter(Mandatory = $true)][string]$DisplayName,
        [Parameter(Mandatory = $true)][string]$Kind,
        [Parameter(Mandatory = $true)][string]$RelativePath,
        [Parameter(Mandatory = $true)][string[]]$Tags,
        [string[]]$Dependencies = @()
    )
    $uri = 'asset://' + $RelativePath.Replace('\', '/')
    $record = [ordered]@{
        id = $Id; displayName = $DisplayName; kind = $Kind; uri = $uri
        path = $RelativePath.Replace('\', '/'); license = 'project-original'; redistribution = 'project-only'
        tags = @($Tags)
    }
    if ($Dependencies.Count -gt 0) { $record.dependencies = @($Dependencies) }
    return [pscustomobject]$record
}

function New-SpriteDocument {
    param(
        [Parameter(Mandatory = $true)][string]$AssetId,
        [Parameter(Mandatory = $true)][string]$ShadingModel,
        [Parameter(Mandatory = $true)][bool]$ReceivesShadows,
        [Parameter(Mandatory = $true)][bool]$CastsShadows
    )
    return [ordered]@{
        schema = 'noemancer.sprite-asset/0.2'; assetId = $AssetId
        textureAsset = 'texture.hybrid.pixel.base'; textureSize = @(1448, 1086)
        pixelsPerUnit = 16; sampling = 'nearest'; alphaMode = 'cutout'
        material = [ordered]@{
            normalTextureAsset = 'texture.hybrid.pixel.normal'
            emissiveMaskTextureAsset = 'texture.hybrid.pixel.emissive'
            depthTextureAsset = 'texture.hybrid.pixel.depth'
            normalStrength = 0.85; emissiveColor = @(1.0, 0.16, 0.04)
            emissiveIntensity = 0.35; depthBias = 0.0005; shadingModel = $ShadingModel
            metallic = 0.15; roughness = 0.45; receivesShadows = $ReceivesShadows; castsShadows = $CastsShadows
        }
        frames = @([ordered]@{
            id = 'idle.0'; rect = @(0, 0, 1448, 1086); trimOffset = @(0, 0)
            sourceSize = @(1448, 1086); pivot = @(0.5, 0.5); collisionProfile = ''
        })
        clips = @([ordered]@{
            id = 'idle'; looping = $true; frames = @([ordered]@{ frame = 'idle.0'; durationMs = 1000; event = '' })
        })
        provenance = [ordered]@{
            sourceUri = 'assets/art/source/hybrid-pixel-mixed-lighting/base.png'
            sourceSha256 = 'fixture'; generator = 'hybrid-pixel-mixed-lighting-acceptance'; license = 'project-original'
        }
    }
}

function New-AcceptanceScene {
    $entities = [System.Collections.Generic.List[object]]::new()
    [void]$entities.Add([ordered]@{ guid = 'entity.hybrid.root'; name = 'Hybrid Pixel Mixed Lighting'; parent = $null; components = [ordered]@{ Transform = [ordered]@{ position = @(0, 0, 0) } } })
    [void]$entities.Add([ordered]@{ guid = 'entity.hybrid.camera'; name = 'Acceptance Camera'; parent = 'entity.hybrid.root'; components = [ordered]@{
        Transform = [ordered]@{ position = @(0, 0, 18) }
        Camera = [ordered]@{ target = @(0, 0, 0); verticalFovDegrees = 45; nearClip = 0.1; farClip = 100; primary = $true; projection = 'orthographic'; orthographicHeight = 11.25 }
    } })
    [void]$entities.Add([ordered]@{ guid = 'entity.hybrid.sun'; name = 'Directional Acceptance Sun'; parent = 'entity.hybrid.root'; components = [ordered]@{
        DirectionalLight = [ordered]@{ direction = @(-0.45, -1, -0.25); color = @(0.72, 0.84, 1.0); intensity = 0.65; ambientIntensity = 0.08; castsShadows = $true }
    } })
    [void]$entities.Add([ordered]@{ guid = 'entity.hybrid.point-light'; name = 'Point Acceptance Light'; parent = 'entity.hybrid.root'; components = [ordered]@{
        Transform = [ordered]@{ position = @(0, 4, 4) }
        LocalLight = [ordered]@{ kind = 'point'; color = @(1.0, 0.55, 0.3); luminousPowerLumens = 1200; rangeMeters = 12; direction = @(0, -1, 0); innerConeDegrees = 25; outerConeDegrees = 35; sourceRadiusMeters = 0.1; castsShadows = $true }
    } })
    [void]$entities.Add([ordered]@{ guid = 'entity.hybrid.spot-light'; name = 'Spot Acceptance Light'; parent = 'entity.hybrid.root'; components = [ordered]@{
        Transform = [ordered]@{ position = @(0, 5, 3) }
        LocalLight = [ordered]@{ kind = 'spot'; color = @(0.3, 0.55, 1.0); luminousPowerLumens = 1400; rangeMeters = 14; direction = @(0, -1, -0.2); innerConeDegrees = 12; outerConeDegrees = 28; sourceRadiusMeters = 0.15; castsShadows = $true }
    } })
    [void]$entities.Add([ordered]@{ guid = 'entity.hybrid.ground'; name = 'Acceptance Ground'; parent = 'entity.hybrid.root'; components = [ordered]@{
        Transform = [ordered]@{ position = @(0, -3, 0); scale = @(9, 0.25, 2) }
        MeshRenderer = [ordered]@{ meshAsset = 'asset.primitive.cube'; visible = $true; castsShadows = $true; receivesShadows = $true }
        PbrMaterial = [ordered]@{ baseColor = @(0.025, 0.07, 0.09); metallic = 0.18; roughness = 0.62; emissiveColor = @(0.02, 0.18, 0.22); emissiveIntensity = 0.08 }
    } })
    [void]$entities.Add([ordered]@{ guid = 'entity.hybrid.mesh'; name = 'Acceptance 3D Sphere'; parent = 'entity.hybrid.root'; components = [ordered]@{
        Transform = [ordered]@{ position = @(0, -0.2, -0.8); scale = @(1.3, 1.3, 1.3) }
        MeshRenderer = [ordered]@{ meshAsset = 'asset.primitive.sphere'; visible = $true; castsShadows = $true; receivesShadows = $true }
        PbrMaterial = [ordered]@{ baseColor = @(0.95, 0.2, 0.08); metallic = 0.06; roughness = 0.36; emissiveColor = @(0.04, 0.3, 0.8); emissiveIntensity = 0.35 }
    } })
    $spriteRows = @(
        @{ guid = 'entity.hybrid.sprite.lit.cast.receive'; name = 'Lit Cast Receive'; asset = 'sprite.hybrid.pixel.lit.cast.receive'; position = @(-5.0, 1.0, 0); order = 0 },
        @{ guid = 'entity.hybrid.sprite.lit.no-shadow'; name = 'Lit No Shadow'; asset = 'sprite.hybrid.pixel.lit.no-shadow'; position = @(-1.7, 1.0, 0); order = 1 },
        @{ guid = 'entity.hybrid.sprite.unlit.cast.receive'; name = 'Unlit Cast Receive'; asset = 'sprite.hybrid.pixel.unlit.cast.receive'; position = @(1.7, 1.0, 0); order = 2 },
        @{ guid = 'entity.hybrid.sprite.unlit.no-shadow'; name = 'Unlit No Shadow'; asset = 'sprite.hybrid.pixel.unlit.no-shadow'; position = @(5.0, 1.0, 0); order = 3 }
    )
    foreach ($row in $spriteRows) {
        [void]$entities.Add([ordered]@{ guid = $row.guid; name = $row.name; parent = 'entity.hybrid.root'; components = [ordered]@{
            Transform = [ordered]@{ position = $row.position; scale = @(0.38, 0.38, 0.38) }
            SpriteRenderer = [ordered]@{ spriteAsset = $row.asset; clip = 'idle'; playbackSpeed = 1; playing = $false; flipX = $false; flipY = $false; sortingLayer = 'acceptance'; sortingOrder = $row.order; visible = $true }
        } })
    }
    return [ordered]@{ schema = 'noemancer.scene/0.1'; sceneGuid = 'scene.hybrid-pixel-mixed-lighting'; name = 'Hybrid Pixel Mixed Lighting Acceptance'; entities = @($entities) }
}

function Inject-AcceptanceFixture {
    param([Parameter(Mandatory = $true)][string]$ProjectPath)
    $manifestPath = Join-Path $ProjectPath 'noemancer.project.json'
    $registryPath = Join-Path $ProjectPath 'assets\registry.json'
    $manifest = Read-JsonFile -Path $manifestPath -Stage 'fixture' -Label 'Copied project manifest'
    $registry = Read-JsonFile -Path $registryPath -Stage 'fixture' -Label 'Copied Asset Registry'
    if ($null -eq $manifest -or $null -eq $registry) { return $null }

    $sourcePath = Join-Path $ProjectPath 'assets\art\source\courier-action-source-v2.png'
    if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
        Add-Issue -Code 'fixture.source-missing' -Stage 'fixture' -Message 'The copied source Lumen Run PNG is missing.'
        return $null
    }
    $fixtureRelativeRoot = 'art/source/hybrid-pixel-mixed-lighting'
    $fixtureSourceRoot = Join-Path $ProjectPath ('assets\' + $fixtureRelativeRoot)
    New-Item -ItemType Directory -Path $fixtureSourceRoot -Force | Out-Null
    $texturePaths = [ordered]@{
        base = "$fixtureRelativeRoot/base.png"; normal = "$fixtureRelativeRoot/normal.png"
        emissive = "$fixtureRelativeRoot/emissive.png"; depth = "$fixtureRelativeRoot/depth.png"
    }
    foreach ($relative in $texturePaths.Values) {
        $destination = Join-Path $ProjectPath ('assets\' + $relative.Replace('/', '\'))
        Copy-Item -LiteralPath $sourcePath -Destination $destination -Force
    }

    $records = @((Get-PropertyValue $registry 'assets'))
    $ids = @(
        'texture.hybrid.pixel.base', 'texture.hybrid.pixel.normal', 'texture.hybrid.pixel.emissive', 'texture.hybrid.pixel.depth',
        'sprite.hybrid.pixel.lit.cast.receive', 'sprite.hybrid.pixel.lit.no-shadow',
        'sprite.hybrid.pixel.unlit.cast.receive', 'sprite.hybrid.pixel.unlit.no-shadow'
    )
    $textureRecords = @(
        (New-RegistryRecord 'texture.hybrid.pixel.base' 'Hybrid Pixel Base' 'Texture' $texturePaths.base @('sprite', 'base-color', 'srgb'))
        (New-RegistryRecord 'texture.hybrid.pixel.normal' 'Hybrid Pixel Normal' 'Texture' $texturePaths.normal @('sprite', 'normal', 'linear'))
        (New-RegistryRecord 'texture.hybrid.pixel.emissive' 'Hybrid Pixel Emissive Mask' 'Texture' $texturePaths.emissive @('sprite', 'emissive', 'mask', 'linear'))
        (New-RegistryRecord 'texture.hybrid.pixel.depth' 'Hybrid Pixel Height Depth' 'Texture' $texturePaths.depth @('sprite', 'depth', 'linear'))
    )
    foreach ($record in $textureRecords) { $records += $record }
    $spriteMatrix = @(
        @{ id = 'sprite.hybrid.pixel.lit.cast.receive'; name = 'Hybrid Pixel Lit Cast Receive'; shading = 'lit'; receive = $true; cast = $true },
        @{ id = 'sprite.hybrid.pixel.lit.no-shadow'; name = 'Hybrid Pixel Lit No Shadow'; shading = 'lit'; receive = $false; cast = $false },
        @{ id = 'sprite.hybrid.pixel.unlit.cast.receive'; name = 'Hybrid Pixel Unlit Cast Receive'; shading = 'unlit'; receive = $true; cast = $true },
        @{ id = 'sprite.hybrid.pixel.unlit.no-shadow'; name = 'Hybrid Pixel Unlit No Shadow'; shading = 'unlit'; receive = $false; cast = $false }
    )
    $spriteRoot = Join-Path $ProjectPath 'assets\art\hybrid-pixel-mixed-lighting'
    New-Item -ItemType Directory -Path $spriteRoot -Force | Out-Null
    foreach ($row in $spriteMatrix) {
        $fileName = ($row.id + '.sprite.json')
        $relative = 'art/hybrid-pixel-mixed-lighting/' + $fileName
        $filePath = Join-Path $ProjectPath ('assets\' + $relative.Replace('/', '\'))
        $document = New-SpriteDocument -AssetId $row.id -ShadingModel $row.shading -ReceivesShadows:$row.receive -CastsShadows:$row.cast
        Write-Utf8 -Path $filePath -Text (($document | ConvertTo-Json -Depth 100) + "`n")
        $dependencies = @('texture.hybrid.pixel.base', 'texture.hybrid.pixel.normal', 'texture.hybrid.pixel.emissive', 'texture.hybrid.pixel.depth')
        $records += New-RegistryRecord $row.id $row.name 'Sprite' $relative @('sprite', 'hybrid-pixel', $row.shading) $dependencies
    }
    $seen = @{}
    foreach ($record in $records) {
        $recordId = [string](Get-PropertyValue $record 'id')
        if ($seen.ContainsKey($recordId)) {
            Add-Issue -Code 'fixture.registry-duplicate' -Stage 'fixture' -Message "Fixture registry would duplicate asset '$recordId'."
        }
        $seen[$recordId] = $true
    }
    $registry.schema = 'noemancer.assets/0.1'
    $registry.assets = @($records)
    Write-Utf8 -Path $registryPath -Text (($registry | ConvertTo-Json -Depth 100) + "`n")

    $sceneRelative = 'scenes/hybrid-pixel-mixed-lighting.scene.json'
    $scenePath = Join-Path $ProjectPath ($sceneRelative.Replace('/', '\'))
    Write-Utf8 -Path $scenePath -Text ((New-AcceptanceScene | ConvertTo-Json -Depth 100) + "`n")

    $manifest.schema = 'noemancer.project/0.2'
    $manifest.startupScene = $sceneRelative
    $hybridPixelProfile = [ordered]@{
        schema = 'noemancer.hybrid-pixel-profile/0.1'; profileId = 'lumen-run-hybrid-pixel-mixed-lighting'; enabled = $true
        virtualWidth = $script:VirtualWidth; virtualHeight = $script:VirtualHeight; pixelsPerUnit = $script:PixelsPerUnit
        integerScaling = $true; snapCamera = $true; snapSprites = $true; presentationFilter = 'nearest'
    }
    if($null -eq $manifest.PSObject.Properties['hybridPixelProfile']) {
        $manifest | Add-Member -NotePropertyName hybridPixelProfile -NotePropertyValue $hybridPixelProfile
    } else {
        $manifest.hybridPixelProfile = $hybridPixelProfile
    }
    $packagedAssets = @((Get-PropertyValue $manifest 'packagedAssets'))
    foreach ($id in $ids) { if ($packagedAssets -notcontains $id) { $packagedAssets += $id } }
    $manifest.packagedAssets = @($packagedAssets)
    Write-Utf8 -Path $manifestPath -Text (($manifest | ConvertTo-Json -Depth 100) + "`n")
    return [ordered]@{
        profile = $manifest.hybridPixelProfile; sceneRelative = $sceneRelative; scenePath = $scenePath
        textureIds = @('texture.hybrid.pixel.base', 'texture.hybrid.pixel.normal', 'texture.hybrid.pixel.emissive', 'texture.hybrid.pixel.depth')
        spriteIds = @($spriteMatrix | ForEach-Object { $_.id })
        expectedAssetIds = @($ids)
        spriteMatrix = @($spriteMatrix)
        meshAssets = @('asset.primitive.cube', 'asset.primitive.sphere')
        localLightKinds = @('point', 'spot')
    }
}

function Test-ProjectNativeBoundary {
    param([Parameter(Mandatory = $true)][string]$ProjectPath)
    $nativeExtensions = @('.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp', '.hxx', '.inl', '.ixx', '.m', '.mm', '.cmake', '.vcxproj')
    $ignoredPattern = '(?i)[\\/](?:\.git|bin|obj|generated)[\\/]'
    $nativeFiles = @(
        Get-ChildItem -LiteralPath $ProjectPath -Recurse -File -Force -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -notmatch $ignoredPattern -and ($nativeExtensions -contains $_.Extension.ToLowerInvariant() -or $_.Name -eq 'CMakeLists.txt') } |
            ForEach-Object { $_.FullName }
    )
    foreach ($file in $nativeFiles) { Add-Issue -Code 'native.game-source' -Stage 'nativeBoundary' -Path (Get-RelativeArtifactPath $file) -Message 'Copied Lumen Run contains native C/C++ or native build source.' }
    $referenceExtensions = @('.cs', '.csproj', '.props', '.targets', '.sln', '.cmake', '.vcxproj')
    $referencePatterns = @(
        '(?i)\b(?:DllImport|LibraryImport|UnmanagedCallersOnly|NativeLibrary|Marshal\.GetDelegateForFunctionPointer)\b',
        '(?i)\b(?:cmake_minimum_required|add_subdirectory|target_link_libraries|add_library|add_executable)\b',
        '(?i)(?:src[\\/]engine|src[\\/]runtime|src[\\/]editor|noemancer[._-]?native)'
    )
    $references = [System.Collections.Generic.List[object]]::new()
    foreach ($file in @(Get-ChildItem -LiteralPath $ProjectPath -Recurse -File -Force -ErrorAction SilentlyContinue)) {
        if ($file.FullName -match $ignoredPattern -or $referenceExtensions -notcontains $file.Extension.ToLowerInvariant() -or $file.Length -gt 2MB) { continue }
        try { $text = [IO.File]::ReadAllText($file.FullName) } catch { continue }
        foreach ($pattern in $referencePatterns) {
            $match = [regex]::Match($text, $pattern)
            if (-not $match.Success) { continue }
            $line = ($text.Substring(0, $match.Index) -split "`r?`n").Count
            $item = [ordered]@{ file = Get-RelativeArtifactPath $file.FullName; line = $line; sample = $match.Value }
            [void]$references.Add([pscustomobject]$item)
            Add-Issue -Code 'native.game-reference' -Stage 'nativeBoundary' -Path $item.file -Message "Copied project references native engine/build code at line ${line}."
            break
        }
    }
    return [ordered]@{ pass = ($nativeFiles.Count -eq 0 -and $references.Count -eq 0); nativeFileCount = $nativeFiles.Count; nativeFiles = @($nativeFiles | ForEach-Object { Get-RelativeArtifactPath $_ }); nativeReferenceCount = $references.Count; nativeReferences = @($references) }
}

function Assert-Scalar {
    param(
        [AllowNull()]$Actual,
        [AllowNull()]$Expected,
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Stage
    )
    if ($null -eq $Actual -or [string]$Actual -ne [string]$Expected) {
        Add-Issue -Code 'contract.value-mismatch' -Stage $Stage -Path $Path -Message "Expected '$Expected', got '$Actual'."
        return $false
    }
    return $true
}

function Assert-AtLeast {
    param(
        [AllowNull()]$Actual,
        [Parameter(Mandatory = $true)][double]$Minimum,
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Stage
    )
    $number = 0.0
    $valid = $null -ne $Actual -and [double]::TryParse(([string]$Actual), [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$number)
    if (-not $valid -or $number -lt $Minimum) {
        Add-Issue -Code 'contract.count-too-small' -Stage $Stage -Path $Path -Message "Expected at least $Minimum, got '$Actual'."
        return $false
    }
    return $true
}

function Assert-NonNegative {
    param(
        [AllowNull()]$Actual,
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Stage
    )
    $number = 0.0
    $valid = $null -ne $Actual -and [double]::TryParse(([string]$Actual), [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$number)
    if (-not $valid -or $number -lt 0) {
        Add-Issue -Code 'contract.count-invalid' -Stage $Stage -Path $Path -Message "Expected a non-negative numeric count, got '$Actual'."
        return $false
    }
    return $true
}

function Get-RenderWorldCandidate {
    param(
        [Parameter(Mandatory = $true)]$Events,
        [Parameter(Mandatory = $true)][AllowNull()]$Status,
        [Parameter(Mandatory = $true)][string]$Stage
    )
    $eventsToInspect = @('render.world.final', 'render.world', 'render.scene.final', 'render.scene')
    foreach ($eventName in $eventsToInspect) {
        foreach ($event in @($Events | Where-Object { $_.event -eq $eventName })) {
            $payload = $event.payload
            if ($null -eq $payload) { continue }
            if ([string](Get-PropertyValue $payload 'schemaVersion') -eq 'noemancer.render-world.v15') { return $payload }
            $world = Get-PropertyValue $payload 'renderWorld'
            if ($null -ne $world -and [string](Get-PropertyValue $world 'schemaVersion') -eq 'noemancer.render-world.v15') { return $world }
            $world = Get-PropertyValue $payload 'render_world'
            if ($null -ne $world -and [string](Get-PropertyValue $world 'schemaVersion') -eq 'noemancer.render-world.v15') { return $world }
        }
    }
    $statusWorld = Get-PropertyValue $Status 'renderWorld'
    if ($null -ne $statusWorld -and [string](Get-PropertyValue $statusWorld 'schemaVersion') -eq 'noemancer.render-world.v15') { return $statusWorld }
    Add-Issue -Code 'render-world.evidence-missing' -Stage $Stage -Path '/renderWorld/schemaVersion' -Message 'No Render World v15 snapshot was published by the hidden run.'
    return $null
}

function Test-RenderWorldContract {
    param(
        [Parameter(Mandatory = $true)][AllowNull()]$World,
        [Parameter(Mandatory = $true)][string]$Stage
    )
    $before = $script:Issues.Count
    if ($null -eq $World) { return [ordered]@{ pass = $false; schemaVersion = ''; spriteCount = 0; instanceCount = 0; localLightCount = 0; fingerprint = '' } }
    Assert-Scalar (Get-PropertyValue $World 'schemaVersion') 'noemancer.render-world.v15' '/schemaVersion' $Stage | Out-Null
    $sprites = @((Get-PropertyValue $World 'sprites'))
    $instances = @((Get-PropertyValue $World 'instances'))
    $localLights = @((Get-PropertyValue $World 'localLights'))
    $vfxParticles = @((Get-PropertyValue $World 'vfxParticles'))
    $directional = Get-PropertyValue $World 'directionalLight'
    if ($null -eq (Get-PropertyValue $World 'sprites')) { Add-Issue -Code 'render-world.sprites-missing' -Stage $Stage -Path '/sprites' -Message 'Render World v15 must expose sprites.' }
    if ($null -eq (Get-PropertyValue $World 'instances')) { Add-Issue -Code 'render-world.instances-missing' -Stage $Stage -Path '/instances' -Message 'Render World v15 must expose 3D instances.' }
    if ($null -eq (Get-PropertyValue $World 'localLights')) { Add-Issue -Code 'render-world.local-lights-missing' -Stage $Stage -Path '/localLights' -Message 'Render World v15 must expose local lights.' }
    if ($null -eq $directional) { Add-Issue -Code 'render-world.directional-missing' -Stage $Stage -Path '/directionalLight' -Message 'Render World v15 must expose the directional light.' }
    Assert-AtLeast $vfxParticles.Count 1 '/vfxParticles' $Stage | Out-Null
    foreach ($particle in $vfxParticles) {
        $policy = Get-PropertyValue $particle 'spritePolicy'
        Assert-Scalar (Get-PropertyValue $policy 'pixelAlignment') 'profile' '/vfxParticles/spritePolicy/pixelAlignment' $Stage | Out-Null
        Assert-Scalar (Get-PropertyValue $policy 'sizeQuantization') 'profile' '/vfxParticles/spritePolicy/sizeQuantization' $Stage | Out-Null
        Assert-Scalar (Get-PropertyValue $policy 'sampling') 'profile' '/vfxParticles/spritePolicy/sampling' $Stage | Out-Null
    }
    Assert-AtLeast $localLights.Count 2 '/localLights' $Stage | Out-Null
    if ($null -ne $directional) { Assert-Scalar (Get-PropertyValue $directional 'castsShadows') $true '/directionalLight/castsShadows' $Stage | Out-Null }
    foreach ($kind in $script:Fixture.localLightKinds) {
        $matching = @($localLights | Where-Object { [string](Get-PropertyValue $_ 'kind') -eq $kind })
        if ($matching.Count -eq 0) { Add-Issue -Code 'render-world.local-light-kind-missing' -Stage $Stage -Path '/localLights' -Message "Render World is missing a '$kind' light." }
        else { Assert-Scalar (Get-PropertyValue $matching[0] 'castsShadows') $true "/localLights/$kind/castsShadows" $Stage | Out-Null }
    }
    $worldSprites = @{}
    foreach ($sprite in $sprites) { $worldSprites[[string](Get-PropertyValue $sprite 'entityId')] = $sprite }
    foreach ($row in $script:Fixture.spriteMatrix) {
        $entityId = switch ([string]$row.id) {
            'sprite.hybrid.pixel.lit.cast.receive' { 'entity.hybrid.sprite.lit.cast.receive' }
            'sprite.hybrid.pixel.lit.no-shadow' { 'entity.hybrid.sprite.lit.no-shadow' }
            'sprite.hybrid.pixel.unlit.cast.receive' { 'entity.hybrid.sprite.unlit.cast.receive' }
            default { 'entity.hybrid.sprite.unlit.no-shadow' }
        }
        if (-not $worldSprites.ContainsKey($entityId)) {
            Add-Issue -Code 'render-world.sprite-missing' -Stage $Stage -Path '/sprites' -Message "Render World is missing '$entityId'."
            continue
        }
        $sprite = $worldSprites[$entityId]
        Assert-Scalar (Get-PropertyValue $sprite 'spriteAsset') $row.id "/sprites/$entityId/spriteAsset" $Stage | Out-Null
        Assert-Scalar (Get-PropertyValue $sprite 'visible') $true "/sprites/$entityId/visible" $Stage | Out-Null
        $material = Get-PropertyValue $sprite 'material'
        if ($null -eq $material) { Add-Issue -Code 'render-world.sprite-material-missing' -Stage $Stage -Path "/sprites/$entityId/material" -Message 'Sprite material channels are missing.'; continue }
        foreach ($mapping in @(
            @{ field = 'normalTextureAsset'; value = 'texture.hybrid.pixel.normal' },
            @{ field = 'emissiveMaskTextureAsset'; value = 'texture.hybrid.pixel.emissive' },
            @{ field = 'depthTextureAsset'; value = 'texture.hybrid.pixel.depth' }
        )) { Assert-Scalar (Get-PropertyValue $material $mapping.field) $mapping.value "/sprites/$entityId/material/$($mapping.field)" $Stage | Out-Null }
        Assert-Scalar (Get-PropertyValue $material 'shadingModel') $row.shading "/sprites/$entityId/material/shadingModel" $Stage | Out-Null
        Assert-Scalar (Get-PropertyValue $material 'receivesShadows') $row.receive "/sprites/$entityId/material/receivesShadows" $Stage | Out-Null
        Assert-Scalar (Get-PropertyValue $material 'castsShadows') $row.cast "/sprites/$entityId/material/castsShadows" $Stage | Out-Null
        Assert-AtLeast (Get-PropertyValue $material 'normalStrength') 0.1 "/sprites/$entityId/material/normalStrength" $Stage | Out-Null
        Assert-AtLeast (Get-PropertyValue $material 'emissiveIntensity') 0.1 "/sprites/$entityId/material/emissiveIntensity" $Stage | Out-Null
        Assert-AtLeast (Get-PropertyValue $material 'depthBias') 0.0 "/sprites/$entityId/material/depthBias" $Stage | Out-Null
    }
    $meshInstances = @($instances | Where-Object {
        [string](Get-PropertyValue $_ 'meshAsset') -in $script:Fixture.meshAssets -and
        $null -eq (Get-PropertyValue $_ 'vfx')
    })
    if ($meshInstances.Count -lt 2) { Add-Issue -Code 'render-world.3d-mesh-missing' -Stage $Stage -Path '/instances' -Message 'Render World must contain both cube ground and sphere 3D mesh instances.' }
    foreach ($mesh in $meshInstances) {
        Assert-Scalar (Get-PropertyValue $mesh 'visible') $true "/instances/$([string](Get-PropertyValue $mesh 'entityId'))/visible" $Stage | Out-Null
        if (-not $script:ProductionContract -or [string](Get-PropertyValue $mesh 'entityId') -eq 'entity.hybrid.mesh') {
            Assert-Scalar (Get-PropertyValue $mesh 'castsShadows') $true "/instances/$([string](Get-PropertyValue $mesh 'entityId'))/castsShadows" $Stage | Out-Null
            Assert-Scalar (Get-PropertyValue $mesh 'receivesShadows') $true "/instances/$([string](Get-PropertyValue $mesh 'entityId'))/receivesShadows" $Stage | Out-Null
        }
    }
    $fingerprintObject = [ordered]@{
        schemaVersion = Get-PropertyValue $World 'schemaVersion'
        directional = [ordered]@{ entityId = Get-PropertyValue $directional 'entityId'; castsShadows = Get-PropertyValue $directional 'castsShadows' }
        localLights = @($localLights | Sort-Object { [string](Get-PropertyValue $_ 'entityId') } | ForEach-Object { [ordered]@{ entityId = Get-PropertyValue $_ 'entityId'; kind = Get-PropertyValue $_ 'kind'; castsShadows = Get-PropertyValue $_ 'castsShadows' } })
        sprites = @($sprites | Where-Object { [string](Get-PropertyValue $_ 'entityId') -like 'entity.hybrid.sprite.*' } | Sort-Object { [string](Get-PropertyValue $_ 'entityId') } | ForEach-Object { $m = Get-PropertyValue $_ 'material'; [ordered]@{ entityId = Get-PropertyValue $_ 'entityId'; spriteAsset = Get-PropertyValue $_ 'spriteAsset'; shadingModel = Get-PropertyValue $m 'shadingModel'; receivesShadows = Get-PropertyValue $m 'receivesShadows'; castsShadows = Get-PropertyValue $m 'castsShadows'; normal = Get-PropertyValue $m 'normalTextureAsset'; emissive = Get-PropertyValue $m 'emissiveMaskTextureAsset'; depth = Get-PropertyValue $m 'depthTextureAsset' } })
        meshes = @($meshInstances | Sort-Object { [string](Get-PropertyValue $_ 'entityId') } | ForEach-Object { [ordered]@{ entityId = Get-PropertyValue $_ 'entityId'; meshAsset = Get-PropertyValue $_ 'meshAsset'; castsShadows = Get-PropertyValue $_ 'castsShadows'; receivesShadows = Get-PropertyValue $_ 'receivesShadows' } })
        vfx = [ordered]@{ count = $vfxParticles.Count; policies = @($vfxParticles | ForEach-Object { $p = Get-PropertyValue $_ 'spritePolicy'; "$(Get-PropertyValue $p 'pixelAlignment')/$(Get-PropertyValue $p 'sizeQuantization')/$(Get-PropertyValue $p 'sampling')" } | Sort-Object -Unique) }
    }
    $fingerprint = Get-TextSha256 (($fingerprintObject | ConvertTo-Json -Depth 30 -Compress))
    return [ordered]@{ pass = ($script:Issues.Count -eq $before); schemaVersion = Get-PropertyValue $World 'schemaVersion'; spriteCount = $sprites.Count; instanceCount = $instances.Count; localLightCount = $localLights.Count; fingerprint = $fingerprint; snapshot = $fingerprintObject }
}

function Test-RendererStatusContract {
    param(
        [Parameter(Mandatory = $true)][AllowNull()]$Status,
        [Parameter(Mandatory = $true)][string]$Stage
    )
    $before = $script:Issues.Count
    if ($null -eq $Status) { Add-Issue -Code 'renderer.status-missing' -Stage $Stage -Message 'Renderer Status payload is missing.'; return [ordered]@{ pass = $false; fingerprint = '' } }
    Assert-Scalar (Get-PropertyValue $Status 'schemaVersion') 'noemancer.renderer-status.v27' '/schemaVersion' $Stage | Out-Null
    $hybrid = Get-PropertyValue $Status 'hybridPixel'
    if ($null -eq $hybrid) { Add-Issue -Code 'renderer.hybrid-pixel-missing' -Stage $Stage -Path '/hybridPixel' -Message 'Renderer Status does not expose Hybrid Pixel evidence.' }
    else {
        Assert-Scalar (Get-PropertyValue $hybrid 'active') $true '/hybridPixel/active' $Stage | Out-Null
        $profile = Get-PropertyValue $hybrid 'profile'
        if ($null -eq $profile) { Add-Issue -Code 'renderer.hybrid-profile-missing' -Stage $Stage -Path '/hybridPixel/profile' -Message 'Hybrid Pixel profile projection is missing.' }
        else {
            Assert-Scalar (Get-PropertyValue $profile 'schema') 'noemancer.hybrid-pixel-profile/0.1' '/hybridPixel/profile/schema' $Stage | Out-Null
            Assert-Scalar (Get-PropertyValue $profile 'profileId') $script:ExpectedProfile.profileId '/hybridPixel/profile/profileId' $Stage | Out-Null
            Assert-Scalar (Get-PropertyValue $profile 'virtualWidth') $script:VirtualWidth '/hybridPixel/profile/virtualWidth' $Stage | Out-Null
            Assert-Scalar (Get-PropertyValue $profile 'virtualHeight') $script:VirtualHeight '/hybridPixel/profile/virtualHeight' $Stage | Out-Null
            Assert-Scalar (Get-PropertyValue $profile 'pixelsPerUnit') $script:PixelsPerUnit '/hybridPixel/profile/pixelsPerUnit' $Stage | Out-Null
            Assert-Scalar (Get-PropertyValue $profile 'presentationFilter') 'nearest' '/hybridPixel/profile/presentationFilter' $Stage | Out-Null
        }
        $projection = Get-PropertyValue $hybrid 'projection'
        if ($null -ne $projection) { Assert-Scalar (Get-PropertyValue $projection 'valid') $true '/hybridPixel/projection/valid' $Stage | Out-Null }
        $vfxPolicy = Get-PropertyValue $hybrid 'vfx'
        if ($null -eq $vfxPolicy) { Add-Issue -Code 'renderer.hybrid-vfx-policy-missing' -Stage $Stage -Path '/hybridPixel/vfx' -Message 'Hybrid Pixel VFX policy is missing.' }
        else {
            Assert-Scalar (Get-PropertyValue $vfxPolicy 'sameGpuLifecycle') $true '/hybridPixel/vfx/sameGpuLifecycle' $Stage | Out-Null
            Assert-Scalar (Get-PropertyValue $vfxPolicy 'centerSnap') $true '/hybridPixel/vfx/centerSnap' $Stage | Out-Null
            Assert-Scalar (Get-PropertyValue $vfxPolicy 'sizeQuantization') $true '/hybridPixel/vfx/sizeQuantization' $Stage | Out-Null
            Assert-Scalar (Get-PropertyValue $vfxPolicy 'renderExtent') 'virtual' '/hybridPixel/vfx/renderExtent' $Stage | Out-Null
        }
        $postPolicy = Get-PropertyValue $hybrid 'postProcess'
        if ($null -eq $postPolicy) { Add-Issue -Code 'renderer.hybrid-post-policy-missing' -Stage $Stage -Path '/hybridPixel/postProcess' -Message 'Hybrid Pixel post-process policy is missing.' }
        else {
            Assert-Scalar (Get-PropertyValue $postPolicy 'temporalHistory') 'disabled' '/hybridPixel/postProcess/temporalHistory' $Stage | Out-Null
            Assert-Scalar (Get-PropertyValue $postPolicy 'autoExposure') 'locked-unity' '/hybridPixel/postProcess/autoExposure' $Stage | Out-Null
            Assert-Scalar (Get-PropertyValue $postPolicy 'presentation') 'nearest-integer' '/hybridPixel/postProcess/presentation' $Stage | Out-Null
        }
    }
    Assert-Scalar (Get-PropertyValue (Get-PropertyValue $Status 'vfxGpu') 'abi') 'structured-particle-gpu-lifecycle-indirect/0.7' '/vfxGpu/abi' $Stage | Out-Null
    $colorPipeline = Get-PropertyValue $Status 'colorPipeline'
    Assert-Scalar (Get-PropertyValue (Get-PropertyValue $colorPipeline 'autoExposure') 'enabled') $false '/colorPipeline/autoExposure/enabled' $Stage | Out-Null
    Assert-Scalar (Get-PropertyValue (Get-PropertyValue $colorPipeline 'autoExposure') 'mode') 'locked-unity' '/colorPipeline/autoExposure/mode' $Stage | Out-Null
    Assert-Scalar (Get-PropertyValue (Get-PropertyValue $colorPipeline 'bloom') 'resolutionDomain') 'virtual-grid' '/colorPipeline/bloom/resolutionDomain' $Stage | Out-Null
    Assert-Scalar (Get-PropertyValue (Get-PropertyValue $colorPipeline 'ambientOcclusion') 'resolutionDomain') 'virtual-grid' '/colorPipeline/ambientOcclusion/resolutionDomain' $Stage | Out-Null
    $sprites = Get-PropertyValue $Status 'sprites'
    if ($null -eq $sprites) { Add-Issue -Code 'renderer.sprite-evidence-missing' -Stage $Stage -Path '/sprites' -Message 'Renderer Status does not expose Sprite mixed-material evidence.' }
    else {
        Assert-Scalar (Get-PropertyValue $sprites 'pipelineCreated') $true '/sprites/pipelineCreated' $Stage | Out-Null
        # textureCount is the renderer's deduplicated base-sprite texture
        # counter, not the number of authored material channels.  In this
        # fixture it is three (two source sprites plus the hybrid base), while
        # the authoritative four-channel proof lives in textureResources.
        Assert-NonNegative (Get-PropertyValue $sprites 'textureCount') '/sprites/textureCount' $Stage | Out-Null
        if ($script:ProductionContract) {
            Assert-AtLeast (Get-PropertyValue $sprites 'instancesSubmitted') 4 '/sprites/instancesSubmitted' $Stage | Out-Null
            Assert-AtLeast (Get-PropertyValue $sprites 'litInstances') 2 '/sprites/litInstances' $Stage | Out-Null
            Assert-AtLeast (Get-PropertyValue $sprites 'unlitInstances') 2 '/sprites/unlitInstances' $Stage | Out-Null
        } else {
            Assert-Scalar (Get-PropertyValue $sprites 'instancesSubmitted') 4 '/sprites/instancesSubmitted' $Stage | Out-Null
            Assert-Scalar (Get-PropertyValue $sprites 'litInstances') 2 '/sprites/litInstances' $Stage | Out-Null
            Assert-Scalar (Get-PropertyValue $sprites 'unlitInstances') 2 '/sprites/unlitInstances' $Stage | Out-Null
        }
        # shadowReceivers counts receivers that reach the directional lit
        # path; unlit sprites can author receivesShadows but are intentionally
        # excluded from this aggregate.  Render World v15 checks the exact
        # authored receive matrix below, so only require a valid counter here.
        Assert-NonNegative (Get-PropertyValue $sprites 'shadowReceivers') '/sprites/shadowReceivers' $Stage | Out-Null
        if ($script:ProductionContract) {
            Assert-AtLeast (Get-PropertyValue $sprites 'authoredShadowCasters') 2 '/sprites/authoredShadowCasters' $Stage | Out-Null
        } else {
            Assert-Scalar (Get-PropertyValue $sprites 'authoredShadowCasters') 2 '/sprites/authoredShadowCasters' $Stage | Out-Null
        }
        Assert-Scalar (Get-PropertyValue $sprites 'missingMaterialTextures') 0 '/sprites/missingMaterialTextures' $Stage | Out-Null
        $channels = @((Get-PropertyValue $sprites 'materialChannels'))
        foreach ($channel in @('normal', 'emissive-mask', 'height-depth')) { if ($channels -notcontains $channel) { Add-Issue -Code 'renderer.sprite-channel-missing' -Stage $Stage -Path '/sprites/materialChannels' -Message "Sprite material channel '$channel' is absent." } }
        $lightingPath = [string](Get-PropertyValue $sprites 'lightingPath')
        if ($lightingPath -notmatch 'directional' -or $lightingPath -notmatch 'point-spot' -or $lightingPath -notmatch 'shadow') { Add-Issue -Code 'renderer.sprite-lighting-path-invalid' -Stage $Stage -Path '/sprites/lightingPath' -Message 'Sprite status does not attest to shared directional/local/shadow resources.' }
        $resources = Get-PropertyValue (Get-PropertyValue $Status 'textureResources') 'resources'
        if ($null -eq $resources) {
            Add-Issue -Code 'renderer.sprite-texture-resources-missing' -Stage $Stage -Path '/textureResources/resources' -Message 'Texture Resource Table evidence is required to prove the four Sprite material channels were loaded.'
        }
        else {
            $requiredTextures = @(
                @{ id = 'texture.hybrid.pixel.base'; semantic = 'sprite-base-color-srgb' },
                @{ id = 'texture.hybrid.pixel.normal'; semantic = 'sprite-linear-data' },
                @{ id = 'texture.hybrid.pixel.emissive'; semantic = 'sprite-linear-data' },
                @{ id = 'texture.hybrid.pixel.depth'; semantic = 'sprite-linear-data' }
            )
            foreach ($requiredTexture in $requiredTextures) {
                $matches = @($resources | Where-Object { [string](Get-PropertyValue $_ 'stableId') -eq $requiredTexture.id -and [bool](Get-PropertyValue $_ 'available') })
                if ($matches.Count -eq 0) {
                    Add-Issue -Code 'renderer.sprite-texture-not-resident' -Stage $Stage -Path '/textureResources/resources' -Message "Loaded Texture Resource Table evidence is missing '$($requiredTexture.id)'."
                    continue
                }
                Assert-Scalar (Get-PropertyValue $matches[0] 'semantic') $requiredTexture.semantic "/textureResources/resources/$($requiredTexture.id)/semantic" $Stage | Out-Null
            }
        }
    }
    $clustered = Get-PropertyValue $Status 'clusteredLighting'
    if ($null -eq $clustered) { Add-Issue -Code 'renderer.clustered-lighting-missing' -Stage $Stage -Path '/clusteredLighting' -Message 'Clustered local-light status is missing.' }
    else {
        Assert-Scalar (Get-PropertyValue $clustered 'enabled') $true '/clusteredLighting/enabled' $Stage | Out-Null
        Assert-AtLeast (Get-PropertyValue $clustered 'submittedLights') 2 '/clusteredLighting/submittedLights' $Stage | Out-Null
        Assert-Scalar (Get-PropertyValue $clustered 'localLightShadows') $true '/clusteredLighting/localLightShadows' $Stage | Out-Null
    }
    $shadow = Get-PropertyValue $Status 'shadow'
    if ($null -eq $shadow) { Add-Issue -Code 'renderer.directional-shadow-missing' -Stage $Stage -Path '/shadow' -Message 'Directional shadow status is missing.' }
    else {
        Assert-AtLeast (Get-PropertyValue $shadow 'cascadesAvailable') 1 '/shadow/cascadesAvailable' $Stage | Out-Null
        # A cached CSM reports zero newly submitted instances. Require either
        # real submission or a non-zero avoided set so cache reuse cannot make
        # an unconsumed authored caster look valid.
        $directionalShadowWork = [double](Get-PropertyValue $shadow 'instances') +
            [double](Get-PropertyValue $shadow 'avoidedInstancesEstimate')
        Assert-AtLeast $directionalShadowWork 1 '/shadow/instances+avoidedInstancesEstimate' $Stage | Out-Null
    }
    $localShadow = Get-PropertyValue $Status 'localShadow'
    if ($null -eq $localShadow) { Add-Issue -Code 'renderer.local-shadow-missing' -Stage $Stage -Path '/localShadow' -Message 'Local-light shadow status is missing.' }
    else {
        Assert-AtLeast (Get-PropertyValue $localShadow 'selectedLights') 1 '/localShadow/selectedLights' $Stage | Out-Null
        Assert-AtLeast (Get-PropertyValue $localShadow 'facesAvailable') 1 '/localShadow/facesAvailable' $Stage | Out-Null
        $localShadowWork = [double](Get-PropertyValue $localShadow 'instances') +
            [double](Get-PropertyValue $localShadow 'avoidedInstancesEstimate')
        Assert-AtLeast $localShadowWork 1 '/localShadow/instances+avoidedInstancesEstimate' $Stage | Out-Null
    }
    $submission = Get-PropertyValue $Status 'submission'
    if ($null -eq $submission) { Add-Issue -Code 'renderer.submission-missing' -Stage $Stage -Path '/submission' -Message 'Renderer submission evidence is missing.' }
    else { Assert-AtLeast (Get-PropertyValue $submission 'opaqueInstances') 1 '/submission/opaqueInstances' $Stage | Out-Null }
    Assert-AtLeast (Get-PropertyValue $Status 'visibleRenderables') 6 '/visibleRenderables' $Stage | Out-Null
    Assert-AtLeast (Get-PropertyValue $Status 'shadowCasters') 4 '/shadowCasters' $Stage | Out-Null
    # Cross source/Player comparison is a semantic projection, not a hash of
    # the whole execution status.  Source and packaged projects legitimately
    # have different registry identities and therefore different texture
    # counts, resource handles, uploads, paths and cache counters.  Keep only
    # the authored/profile and renderer capability invariants that the
    # per-capture contract above already proved.
    $profileForFingerprint = Get-PropertyValue (Get-PropertyValue $Status 'hybridPixel') 'profile'
    $spriteChannelsForFingerprint = @((Get-PropertyValue (Get-PropertyValue $Status 'sprites') 'materialChannels') | ForEach-Object { [string]$_ } | Sort-Object -Unique)
    $clusteredForFingerprint = Get-PropertyValue $Status 'clusteredLighting'
    $directionalShadowForFingerprint = Get-PropertyValue $Status 'shadow'
    $localShadowForFingerprint = Get-PropertyValue $Status 'localShadow'
    $submissionForFingerprint = Get-PropertyValue $Status 'submission'
    $hybridVfxForFingerprint = Get-PropertyValue (Get-PropertyValue $Status 'hybridPixel') 'vfx'
    $hybridPostForFingerprint = Get-PropertyValue (Get-PropertyValue $Status 'hybridPixel') 'postProcess'
    $fingerprintObject = [ordered]@{
        schemaVersion = Get-PropertyValue $Status 'schemaVersion'
        hybridProfile = [ordered]@{
            schema = Get-PropertyValue $profileForFingerprint 'schema'
            profileId = Get-PropertyValue $profileForFingerprint 'profileId'
            enabled = Get-PropertyValue $profileForFingerprint 'enabled'
            virtualWidth = Get-PropertyValue $profileForFingerprint 'virtualWidth'
            virtualHeight = Get-PropertyValue $profileForFingerprint 'virtualHeight'
            pixelsPerUnit = Get-PropertyValue $profileForFingerprint 'pixelsPerUnit'
            integerScaling = Get-PropertyValue $profileForFingerprint 'integerScaling'
            snapCamera = Get-PropertyValue $profileForFingerprint 'snapCamera'
            snapSprites = Get-PropertyValue $profileForFingerprint 'snapSprites'
            presentationFilter = Get-PropertyValue $profileForFingerprint 'presentationFilter'
        }
        hybridVfx = $hybridVfxForFingerprint
        hybridPost = $hybridPostForFingerprint
        vfxAbi = Get-PropertyValue (Get-PropertyValue $Status 'vfxGpu') 'abi'
        sprites = [ordered]@{
            pipelineCreated = Get-PropertyValue (Get-PropertyValue $Status 'sprites') 'pipelineCreated'
            instancesSubmitted = Get-PropertyValue (Get-PropertyValue $Status 'sprites') 'instancesSubmitted'
            litInstances = Get-PropertyValue (Get-PropertyValue $Status 'sprites') 'litInstances'
            unlitInstances = Get-PropertyValue (Get-PropertyValue $Status 'sprites') 'unlitInstances'
            authoredShadowCasters = Get-PropertyValue (Get-PropertyValue $Status 'sprites') 'authoredShadowCasters'
            missingMaterialTextures = Get-PropertyValue (Get-PropertyValue $Status 'sprites') 'missingMaterialTextures'
            materialChannels = @($spriteChannelsForFingerprint)
            lightingPath = Get-PropertyValue (Get-PropertyValue $Status 'sprites') 'lightingPath'
        }
        clusteredLighting = [ordered]@{
            enabled = Get-PropertyValue $clusteredForFingerprint 'enabled'
            assignment = Get-PropertyValue $clusteredForFingerprint 'assignment'
            consumption = Get-PropertyValue $clusteredForFingerprint 'consumption'
            grid = @((Get-PropertyValue $clusteredForFingerprint 'grid'))
            depthSlices = Get-PropertyValue $clusteredForFingerprint 'depthSlices'
            submittedLights = Get-PropertyValue $clusteredForFingerprint 'submittedLights'
            localLightShadows = Get-PropertyValue $clusteredForFingerprint 'localLightShadows'
            abi = Get-PropertyValue $clusteredForFingerprint 'abi'
        }
        directionalShadow = [ordered]@{
            technique = Get-PropertyValue $directionalShadowForFingerprint 'technique'
            cascadeCount = Get-PropertyValue $directionalShadowForFingerprint 'cascadeCount'
            resolutionPerCascade = Get-PropertyValue $directionalShadowForFingerprint 'resolutionPerCascade'
            format = Get-PropertyValue $directionalShadowForFingerprint 'format'
            cachePolicy = Get-PropertyValue $directionalShadowForFingerprint 'cachePolicy'
            invalidation = @((Get-PropertyValue $directionalShadowForFingerprint 'invalidation'))
        }
        localShadow = [ordered]@{
            enabled = Get-PropertyValue $localShadowForFingerprint 'enabled'
            technique = Get-PropertyValue $localShadowForFingerprint 'technique'
            quality = Get-PropertyValue $localShadowForFingerprint 'quality'
            resolutionPerLayer = Get-PropertyValue $localShadowForFingerprint 'resolutionPerLayer'
            maximumPointLights = Get-PropertyValue $localShadowForFingerprint 'maximumPointLights'
            maximumSpotLights = Get-PropertyValue $localShadowForFingerprint 'maximumSpotLights'
            layerCapacity = Get-PropertyValue $localShadowForFingerprint 'layerCapacity'
            selectedLights = Get-PropertyValue $localShadowForFingerprint 'selectedLights'
            pointLights = Get-PropertyValue $localShadowForFingerprint 'pointLights'
            spotLights = Get-PropertyValue $localShadowForFingerprint 'spotLights'
            facesAvailable = Get-PropertyValue $localShadowForFingerprint 'facesAvailable'
            selection = Get-PropertyValue $localShadowForFingerprint 'selection'
            cachePolicy = Get-PropertyValue $localShadowForFingerprint 'cachePolicy'
            invalidation = @((Get-PropertyValue $localShadowForFingerprint 'invalidation'))
            format = Get-PropertyValue $localShadowForFingerprint 'format'
        }
        submission = [ordered]@{
            opaqueInstances = Get-PropertyValue $submissionForFingerprint 'opaqueInstances'
        }
    }
    return [ordered]@{ pass = ($script:Issues.Count -eq $before); fingerprint = Get-TextSha256 (($fingerprintObject | ConvertTo-Json -Depth 30 -Compress)); schemaVersion = Get-PropertyValue $Status 'schemaVersion'; snapshot = $fingerprintObject }
}

function Get-BmpEvidence {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][int]$ExpectedWidth,
        [Parameter(Mandatory = $true)][int]$ExpectedHeight,
        [Parameter(Mandatory = $true)][string]$Stage
    )
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { Add-Issue -Code 'capture.image-missing' -Stage $Stage -Path (Get-RelativeArtifactPath $Path) -Message 'Hidden capture did not produce a BMP.'; return [ordered]@{ pass = $false; bytes = 0; sha256 = ''; width = 0; height = 0 } }
    try {
        $bytes = [IO.File]::ReadAllBytes($Path)
        if ($bytes.Length -lt 54 -or $bytes[0] -ne 0x42 -or $bytes[1] -ne 0x4D) { throw 'BMP header is missing.' }
        $width = [BitConverter]::ToInt32($bytes, 18); $height = [Math]::Abs([BitConverter]::ToInt32($bytes, 22))
        if ($width -ne $ExpectedWidth -or $height -ne $ExpectedHeight) { throw "Expected ${ExpectedWidth}x${ExpectedHeight}, got ${width}x${height}." }
        return [ordered]@{ pass = $true; bytes = $bytes.Length; sha256 = Get-Sha256 $Path; width = $width; height = $height }
    }
    catch { Add-Issue -Code 'capture.image-invalid' -Stage $Stage -Path (Get-RelativeArtifactPath $Path) -Message $_.Exception.Message; return [ordered]@{ pass = $false; bytes = (Get-Item -LiteralPath $Path).Length; sha256 = Get-Sha256 $Path; width = 0; height = 0 } }
}

function Invoke-HeadlessProbe {
    param(
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [Parameter(Mandatory = $true)][string]$Stage
    )
    $process = Invoke-BoundedProcess -Executable $Executable -Arguments $Arguments -WorkingDirectory $WorkingDirectory `
        -StdoutPath (Join-Path $script:OutputRootPath ($Stage + '.stdout.jsonl')) `
        -StderrPath (Join-Path $script:OutputRootPath ($Stage + '.stderr.log')) -Stage $Stage
    $events = Get-JsonLogEvents $process.stdoutText
    $errors = @($events | Where-Object { $_.level -eq 'error' })
    $stop = Get-LastEvent -Events $events -Name 'runtime.stop'
    if ($process.exitCode -ne 0) { Add-Issue -Code 'headless.failed' -Stage $Stage -Message "Headless probe exited with code $($process.exitCode)." }
    if ($errors.Count -gt 0) { Add-Issue -Code 'headless.error-event' -Stage $Stage -Message "Headless probe emitted $($errors.Count) error event(s)." }
    if ($null -eq $stop) { Add-Issue -Code 'headless.stop-missing' -Stage $Stage -Message 'Headless probe did not publish runtime.stop.' }
    return [ordered]@{ pass = ($script:Issues.Count -eq 0); process = Get-ProcessEvidence $process; eventCount = $events.Count; errorEventCount = $errors.Count; runtimeStop = if ($null -ne $stop) { $stop.payload } else { $null } }
}

function Invoke-HiddenCapture {
    param(
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string[]]$PrefixArguments,
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [Parameter(Mandatory = $true)][string]$Stage,
        [Parameter(Mandatory = $true)][string]$Backend
    )
    $before = $script:Issues.Count
    $imagePath = Join-Path $script:OutputRootPath ($Stage + '.bmp')
    $arguments = @($PrefixArguments + @('--format', 'json', '--gpu-backend', $Backend, '--capture-frame', $imagePath, '--frames', [string]$Frames, '--vfx-respawn-interval', '1', '--window-width', [string]$WindowWidth, '--window-height', [string]$WindowHeight))
    $process = Invoke-BoundedProcess -Executable $Executable -Arguments $arguments -WorkingDirectory $WorkingDirectory `
        -StdoutPath (Join-Path $script:OutputRootPath ($Stage + '.stdout.jsonl')) `
        -StderrPath (Join-Path $script:OutputRootPath ($Stage + '.stderr.log')) -Stage $Stage
    $events = Get-JsonLogEvents $process.stdoutText
    $finalEvent = Get-LastEvent -Events $events -Name 'render.scene.final'
    if ($null -eq $finalEvent) { Add-Issue -Code 'renderer.final-status-missing' -Stage $Stage -Message 'Hidden capture did not publish render.scene.final.' }
    $status = if ($null -ne $finalEvent) { $finalEvent.payload } else { $null }
    $statusEvidence = Test-RendererStatusContract -Status $status -Stage $Stage
    $world = Get-RenderWorldCandidate -Events $events -Status $status -Stage $Stage
    $worldEvidence = Test-RenderWorldContract -World $world -Stage $Stage
    $imageEvidence = Get-BmpEvidence -Path $imagePath -ExpectedWidth $WindowWidth -ExpectedHeight $WindowHeight -Stage $Stage
    if ($process.exitCode -ne 0) { Add-Issue -Code 'renderer.capture-failed' -Stage $Stage -Message "Hidden $Backend capture exited with code $($process.exitCode)." }
    $pass = ($script:Issues.Count -eq $before -and [bool]$statusEvidence.pass -and [bool]$worldEvidence.pass -and [bool]$imageEvidence.pass -and $process.exitCode -eq 0)
    return [ordered]@{
        pass = $pass; backend = $Backend; width = $WindowWidth; height = $WindowHeight; eventCount = $events.Count
        process = Get-ProcessEvidence $process; image = [ordered]@{ path = Get-RelativeArtifactPath $imagePath; bytes = $imageEvidence.bytes; sha256 = $imageEvidence.sha256; width = $imageEvidence.width; height = $imageEvidence.height; pass = $imageEvidence.pass }
        status = $statusEvidence; renderWorld = $worldEvidence
    }
}

function Invoke-HiddenPerformanceProbe {
    param(
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string]$ProfilePath,
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [Parameter(Mandatory = $true)][string]$Backend
    )
    $stage = 'package-performance-' + $Backend
    $evidencePath = Join-Path $script:OutputRootPath ($stage + '.json')
    $arguments = @(
        'player', '--profile', $ProfilePath, '--gpu-backend', $Backend,
        '--performance-evidence', $evidencePath, '--performance-hidden',
        '--performance-workload', 'noemancer.hybrid-pixel-production/0.1',
        '--performance-warmup-frames', '30', '--performance-sample-frames', '60',
        '--window-width', [string]$WindowWidth, '--window-height', [string]$WindowHeight,
        '--format', 'json'
    )
    $process = Invoke-BoundedProcess -Executable $Executable -Arguments $arguments -WorkingDirectory $WorkingDirectory `
        -StdoutPath (Join-Path $script:OutputRootPath ($stage + '.stdout.jsonl')) `
        -StderrPath (Join-Path $script:OutputRootPath ($stage + '.stderr.log')) -Stage $stage
    if ($process.exitCode -ne 0) {
        Add-Issue -Code 'performance.process-failed' -Stage $stage -Message "Hidden performance probe exited with code $($process.exitCode)."
        return [ordered]@{ pass = $false; backend = $Backend; process = Get-ProcessEvidence $process }
    }
    $evidence = Read-JsonFile -Path $evidencePath -Stage $stage -Label 'Performance evidence'
    if ($null -eq $evidence) { return [ordered]@{ pass = $false; backend = $Backend; process = Get-ProcessEvidence $process } }
    Assert-Scalar (Get-PropertyValue $evidence 'schemaVersion') 'noemancer.performance-evidence/0.1' '/schemaVersion' $stage | Out-Null
    $workload = Get-PropertyValue $evidence 'workload'
    Assert-Scalar (Get-PropertyValue $workload 'id') 'noemancer.hybrid-pixel-production/0.1' '/workload/id' $stage | Out-Null
    Assert-Scalar (Get-PropertyValue (Get-PropertyValue $evidence 'runtime') 'backend') $Backend '/runtime/backend' $stage | Out-Null
    $frame = Get-PropertyValue (Get-PropertyValue $evidence 'cpu') 'frameTime'
    $p95 = [double](Get-PropertyValue $frame 'p95')
    $sampleCount = [int](Get-PropertyValue $frame 'sampleCount')
    if ($sampleCount -ne 60) { Add-Issue -Code 'performance.sample-count-mismatch' -Stage $stage -Path '/cpu/frameTime/sampleCount' -Message "Expected 60 measured frames, got $sampleCount." }
    if (-not [double]::IsFinite($p95) -or $p95 -le 0.0 -or $p95 -gt 20.0) {
        Add-Issue -Code 'performance.frame-p95-budget' -Stage $stage -Path '/cpu/frameTime/p95' -Message "CPU frame p95 $p95 ms is outside the bounded (0, 20] ms production contract."
    }
    return [ordered]@{
        pass = ($process.exitCode -eq 0 -and $sampleCount -eq 60 -and [double]::IsFinite($p95) -and $p95 -gt 0.0 -and $p95 -le 20.0)
        backend = $Backend; frameP95Milliseconds = $p95; frameMeanMilliseconds = [double](Get-PropertyValue $frame 'mean')
        sampleCount = $sampleCount; evidence = Get-RelativeArtifactPath $evidencePath; sha256 = Get-Sha256 $evidencePath
        process = Get-ProcessEvidence $process
    }
}

function Get-JsonDocumentFromText {
    param([Parameter(Mandatory = $true)][string]$Text)
    try { return ConvertFrom-Json -InputObject $Text -Depth 100 } catch { }
    foreach ($line in ($Text -split "`r?`n" | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })) {
        try { return ConvertFrom-Json -InputObject $line -Depth 100 } catch { }
    }
    return $null
}

function Test-PackageClosure {
    param(
        [Parameter(Mandatory = $true)]$PackageResult,
        [Parameter(Mandatory = $true)][string]$PackageRoot,
        [Parameter(Mandatory = $true)][string]$CopiedProject
    )
    $before = $script:Issues.Count
    $envelope = $PackageResult.envelope
    if ($null -eq $envelope) { return [ordered]@{ pass = $false; plan = $null; receipt = $null; contentRegistry = $null; cookManifest = $null; packageEntries = @(); assetClosure = @() } }
    Assert-Scalar (Get-PropertyValue $envelope 'schema') 'noemancer.windows-package/0.1' '/schema' 'packageClosure' | Out-Null
    Assert-Scalar (Get-PropertyValue $envelope 'success') $true '/success' 'packageClosure' | Out-Null
    $plan = Get-PropertyValue $envelope 'plan'
    $receipt = Get-PropertyValue $envelope 'receipt'
    if ($null -eq $plan) { Add-Issue -Code 'package.plan-missing' -Stage 'packageClosure' -Path '/plan' -Message 'Package envelope plan is missing.' }
    if ($null -eq $receipt) { Add-Issue -Code 'package.receipt-missing' -Stage 'packageClosure' -Path '/receipt' -Message 'Package envelope receipt is missing.' }
    if ($null -ne $plan) {
        Assert-Scalar (Get-PropertyValue $plan 'schema') 'noemancer.package-plan/0.1' '/plan/schema' 'packageClosure' | Out-Null
        Assert-Scalar (Get-PropertyValue $plan 'valid') $true '/plan/valid' 'packageClosure' | Out-Null
        Assert-Scalar (Get-PropertyValue $plan 'targetProfile') $script:TargetProfile '/plan/targetProfile' 'packageClosure' | Out-Null
    }
    if ($null -ne $receipt) {
        Assert-Scalar (Get-PropertyValue $receipt 'schema') 'noemancer.package-receipt/0.1' '/receipt/schema' 'packageClosure' | Out-Null
        Assert-Scalar (Get-PropertyValue $receipt 'success') $true '/receipt/success' 'packageClosure' | Out-Null
        Assert-Scalar (Get-PropertyValue $receipt 'committed') $true '/receipt/committed' 'packageClosure' | Out-Null
        Assert-Scalar (Get-PropertyValue $receipt 'atomic') $true '/receipt/atomic' 'packageClosure' | Out-Null
    }
    $closure = @((Get-PropertyValue $plan 'assetClosure'))
    foreach ($id in $script:Fixture.expectedAssetIds) { if ($closure -notcontains $id) { Add-Issue -Code 'package.closure-missing-asset' -Stage 'packageClosure' -Path '/plan/assetClosure' -Message "Package asset closure omits '$id'." } }
    $registryPath = Join-Path $PackageRoot 'content\assets\registry.json'
    $contentRegistry = Read-JsonFile -Path $registryPath -Stage 'packageClosure' -Label 'Package content Asset Registry'
    if ($null -ne $contentRegistry) {
        Assert-Scalar (Get-PropertyValue $contentRegistry 'schema') 'noemancer.assets/0.1' '/content/assets/registry.json/schema' 'packageClosure' | Out-Null
        $contentIds = @((Get-PropertyValue $contentRegistry 'assets') | ForEach-Object { [string](Get-PropertyValue $_ 'id') })
        foreach ($id in $script:Fixture.expectedAssetIds) { if ($contentIds -notcontains $id) { Add-Issue -Code 'package.registry-missing-asset' -Stage 'packageClosure' -Path '/content/assets/registry.json/assets' -Message "Package content registry omits '$id'." } }
    }
    $manifestRoot = Join-Path $CopiedProject 'generated\cook-manifests'
    $cookManifestFile = $null; $cookManifest = $null
    if (Test-Path -LiteralPath $manifestRoot -PathType Container) {
        foreach ($candidate in @(Get-ChildItem -LiteralPath $manifestRoot -Filter '*.json' -File | Sort-Object Name)) {
            $parsed = Read-JsonFile -Path $candidate.FullName -Stage 'packageClosure' -Label "Cook manifest $($candidate.Name)"
            if ($null -ne $parsed -and [string](Get-PropertyValue $parsed 'targetProfile') -eq $script:TargetProfile) { $cookManifestFile = $candidate; $cookManifest = $parsed; break }
        }
    }
    if ($null -eq $cookManifest) { Add-Issue -Code 'package.cook-manifest-missing' -Stage 'packageClosure' -Path 'generated/cook-manifests' -Message 'No matching windows-x64-release Cook manifest was found in the temporary copy.' }
    $cookOutputs = @((Get-PropertyValue $cookManifest 'outputs'))
    if ($null -ne $cookManifest) {
        Assert-Scalar (Get-PropertyValue $cookManifest 'schema') 'noemancer.cook-manifest/0.1' '/cookManifest/schema' 'packageClosure' | Out-Null
        foreach ($id in $script:Fixture.expectedAssetIds) {
            if (@($cookOutputs | Where-Object { [string](Get-PropertyValue $_ 'assetId') -eq $id }).Count -eq 0) { Add-Issue -Code 'package.cook-output-missing-asset' -Stage 'packageClosure' -Path '/cookManifest/outputs' -Message "Cook manifest omits '$id'." }
        }
        foreach ($output in $cookOutputs) {
            $payloadUri = [string](Get-PropertyValue $output 'payloadUri')
            if ($payloadUri -notmatch '^generated://') { Add-Issue -Code 'package.cook-payload-uri-invalid' -Stage 'packageClosure' -Message "Cook output payload URI '$payloadUri' is not generated://."; continue }
            $relative = $payloadUri.Substring('generated://'.Length).Replace('/', '\')
            $payloadPath = Resolve-SafeRelativePath -Root (Join-Path $CopiedProject 'generated') -RelativePath $relative -Stage 'packageClosure' -Field '/cookManifest/outputs/payloadUri'
            if ($null -ne $payloadPath -and -not (Test-Path -LiteralPath $payloadPath -PathType Leaf)) { Add-Issue -Code 'package.cook-payload-missing' -Stage 'packageClosure' -Message "Cook payload '$payloadUri' is missing."; continue }
            if ($null -ne $payloadPath) { $declaredHash = ([string](Get-PropertyValue $output 'payloadHash')) -replace '^sha256:', ''; if ($declaredHash -and $declaredHash -ne (Get-Sha256 $payloadPath)) { Add-Issue -Code 'package.cook-payload-hash-mismatch' -Stage 'packageClosure' -Message "Cook payload hash mismatch for '$payloadUri'." } }
        }
    }
    $entries = @((Get-PropertyValue $plan 'entries'))
    foreach ($id in $script:Fixture.expectedAssetIds) {
        $entry = @($entries | Where-Object { [string](Get-PropertyValue $_ 'id') -eq $id -and [string](Get-PropertyValue $_ 'role') -eq 'cook-artifact' } | Select-Object -First 1)
        if ($entry.Count -eq 0) { Add-Issue -Code 'package.entry-missing-asset' -Stage 'packageClosure' -Path '/plan/entries' -Message "Package plan has no cook-artifact entry for '$id'."; continue }
        $staging = [string](Get-PropertyValue $entry[0] 'staging')
        $entryPath = Resolve-SafeRelativePath -Root $PackageRoot -RelativePath $staging -Stage 'packageClosure' -Field '/plan/entries/staging'
        if ($null -eq $entryPath -or -not (Test-Path -LiteralPath $entryPath -PathType Leaf)) { Add-Issue -Code 'package.entry-file-missing' -Stage 'packageClosure' -Message "Package entry '$id' has no staged file."; continue }
        $declaredHash = ([string](Get-PropertyValue $entry[0] 'contentHash')) -replace '^sha256:', ''
        if ($declaredHash -and $declaredHash -ne (Get-Sha256 $entryPath)) { Add-Issue -Code 'package.entry-hash-mismatch' -Stage 'packageClosure' -Message "Package staged artifact hash mismatch for '$id'." }
    }
    $packageProfilePath = Join-Path $PackageRoot 'config\game-profile.json'
    $gameProfile = Read-JsonFile -Path $packageProfilePath -Stage 'packageClosure' -Label 'Packaged Game Profile'
    if ($null -ne $gameProfile) {
        Assert-Scalar (Get-PropertyValue $gameProfile 'schema') 'noemancer.game-profile/0.4' '/config/game-profile.json/schema' 'packageClosure' | Out-Null
        Assert-Scalar (Get-PropertyValue $gameProfile 'targetProfile') $script:TargetProfile '/config/game-profile.json/targetProfile' 'packageClosure' | Out-Null
        $startupScene = [string](Get-PropertyValue $gameProfile 'startupScene')
        $startupPath = Resolve-SafeRelativePath -Root (Join-Path $PackageRoot 'content') -RelativePath $startupScene -Stage 'packageClosure' -Field '/config/game-profile.json/startupScene'
        if ($null -eq $startupPath -or -not (Test-Path -LiteralPath $startupPath -PathType Leaf)) { Add-Issue -Code 'package.startup-scene-missing' -Stage 'packageClosure' -Message 'Packaged startup scene is missing.' }
        $profileHybrid = Get-PropertyValue $gameProfile 'hybridPixelProfile'
        if ($null -eq $profileHybrid) { Add-Issue -Code 'package.hybrid-profile-missing' -Stage 'packageClosure' -Message 'Packaged Game Profile omitted Hybrid Pixel profile.' }
        else { Assert-Scalar (Get-PropertyValue $profileHybrid 'profileId') $script:ExpectedProfile.profileId '/config/game-profile.json/hybridPixelProfile/profileId' 'packageClosure' | Out-Null }
    }
    return [ordered]@{
        pass = ($script:Issues.Count -eq $before); envelope = [ordered]@{ schema = Get-PropertyValue $envelope 'schema'; success = Get-PropertyValue $envelope 'success'; code = Get-PropertyValue $envelope 'code' }
        plan = $plan; receipt = $receipt; assetClosure = @($closure); contentRegistry = if ($null -ne $contentRegistry) { [ordered]@{ schema = Get-PropertyValue $contentRegistry 'schema'; assetCount = @((Get-PropertyValue $contentRegistry 'assets')).Count; path = Get-RelativeArtifactPath $registryPath; sha256 = Get-Sha256 $registryPath } } else { $null }
        cookManifest = if ($null -ne $cookManifest) { [ordered]@{ schema = Get-PropertyValue $cookManifest 'schema'; path = Get-RelativeArtifactPath $cookManifestFile.FullName; outputCount = $cookOutputs.Count; contentHash = Get-PropertyValue $cookManifest 'contentHash' } } else { $null }
        packageEntries = @($entries | Where-Object { [string](Get-PropertyValue $_ 'role') -eq 'cook-artifact' } | ForEach-Object { [ordered]@{ id = Get-PropertyValue $_ 'id'; staging = Get-PropertyValue $_ 'staging'; contentHash = Get-PropertyValue $_ 'contentHash' } })
        gameProfile = if ($null -ne $gameProfile) { [ordered]@{ schema = Get-PropertyValue $gameProfile 'schema'; startupScene = Get-PropertyValue $gameProfile 'startupScene'; targetProfile = Get-PropertyValue $gameProfile 'targetProfile' } } else { $null }
    }
}

function Invoke-PackageBuild {
    param(
        [Parameter(Mandatory = $true)][string]$Runtime,
        [Parameter(Mandatory = $true)][string]$CopiedProject,
        [Parameter(Mandatory = $true)][string]$PackageRoot
    )
    if (Test-Path -LiteralPath $PackageRoot) { throw "Package output already exists: $PackageRoot" }
    $process = Invoke-BoundedProcess -Executable $Runtime -Arguments @('package', '--project', $CopiedProject, '--output', $PackageRoot, '--target-profile', $script:TargetProfile, '--format', 'json') -WorkingDirectory $CopiedProject `
        -StdoutPath (Join-Path $script:OutputRootPath 'package-build.stdout.json') -StderrPath (Join-Path $script:OutputRootPath 'package-build.stderr.log') -Stage 'packageBuild'
    $envelope = Get-JsonDocumentFromText -Text $process.stdoutText
    if ($process.exitCode -ne 0 -or $null -eq $envelope -or -not [bool](Get-PropertyValue $envelope 'success')) { Add-Issue -Code 'package.build-failed' -Stage 'packageBuild' -Message "Package CLI did not return success (exit=$($process.exitCode))." }
    return [ordered]@{ pass = ($process.exitCode -eq 0 -and $null -ne $envelope -and [bool](Get-PropertyValue $envelope 'success')); process = Get-ProcessEvidence $process; envelope = $envelope; packageRoot = $PackageRoot; profilePath = Join-Path $PackageRoot 'config\game-profile.json'; playerPath = $null }
}

function Resolve-PackagedPlayer {
    param([Parameter(Mandatory = $true)]$PackageResult)
    $profilePath = $PackageResult.profilePath
    $profile = Read-JsonFile -Path $profilePath -Stage 'packagePlayer' -Label 'Packaged Game Profile'
    if ($null -eq $profile) { return $PackageResult }
    $executable = [string](Get-PropertyValue $profile 'executable')
    if ([string]::IsNullOrWhiteSpace($executable) -or [IO.Path]::IsPathRooted($executable) -or $executable -match '(^|[\\/])\.\.(?:[\\/]|$)') { Add-Issue -Code 'package.player-path-invalid' -Stage 'packagePlayer' -Message 'Packaged executable path is not relative.'; return $PackageResult }
    $playerPath = Join-Path $PackageResult.packageRoot ('bin\' + $executable)
    if (-not (Test-Path -LiteralPath $playerPath -PathType Leaf)) { Add-Issue -Code 'package.player-missing' -Stage 'packagePlayer' -Message "Packaged Player executable is missing: $executable"; return $PackageResult }
    $PackageResult.playerPath = $playerPath
    $PackageResult.profile = [ordered]@{ schema = Get-PropertyValue $profile 'schema'; targetProfile = Get-PropertyValue $profile 'targetProfile'; executable = $executable; startupScene = Get-PropertyValue $profile 'startupScene' }
    return $PackageResult
}

function Compare-CaptureContracts {
    param(
        [Parameter(Mandatory = $true)]$Source,
        [Parameter(Mandatory = $true)]$Package
    )
    $before = $script:Issues.Count
    $comparisons = [System.Collections.Generic.List[object]]::new()
    foreach ($backend in $script:Backends) {
        $sourceCapture = @($Source | Where-Object { $_.backend -eq $backend -and $_.pass } | Select-Object -First 1)
        $packageCapture = @($Package | Where-Object { $_.backend -eq $backend -and $_.pass } | Select-Object -First 1)
        if ($sourceCapture.Count -eq 0 -or $packageCapture.Count -eq 0) { Add-Issue -Code 'capture.comparison-missing' -Stage 'comparison' -Message "No passing source/package capture exists for $backend."; continue }
        $worldEqual = $sourceCapture[0].renderWorld.fingerprint -eq $packageCapture[0].renderWorld.fingerprint
        $statusEqual = $sourceCapture[0].status.fingerprint -eq $packageCapture[0].status.fingerprint
        if (-not $worldEqual) { Add-Issue -Code 'capture.source-player-world-mismatch' -Stage 'comparison' -Message "Source and package Render World semantic fingerprints differ for $backend." }
        if (-not $statusEqual) { Add-Issue -Code 'capture.source-player-status-mismatch' -Stage 'comparison' -Message "Source and package Renderer Status semantic fingerprints differ for $backend." }
        [void]$comparisons.Add([ordered]@{ backend = $backend; renderWorldFingerprintEqual = $worldEqual; rendererStatusFingerprintEqual = $statusEqual; sourceWorld = $sourceCapture[0].renderWorld.fingerprint; packageWorld = $packageCapture[0].renderWorld.fingerprint; sourceStatus = $sourceCapture[0].status.fingerprint; packageStatus = $packageCapture[0].status.fingerprint })
    }
    if (@($Source | Where-Object pass).Count -eq $script:Backends.Count -and @($Package | Where-Object pass).Count -eq $script:Backends.Count) {
        $d3d12 = @($Source | Where-Object { $_.backend -eq 'direct3d12' -and $_.pass }) | Select-Object -First 1
        $vulkan = @($Source | Where-Object { $_.backend -eq 'vulkan' -and $_.pass }) | Select-Object -First 1
        if ($null -ne $d3d12 -and $null -ne $vulkan) {
            $worldBackendEqual = $d3d12.renderWorld.fingerprint -eq $vulkan.renderWorld.fingerprint
            if (-not $worldBackendEqual) { Add-Issue -Code 'capture.backend-world-mismatch' -Stage 'comparison' -Message 'D3D12 and Vulkan Render World semantic fingerprints differ.' }
            [void]$comparisons.Add([ordered]@{ backend = 'direct3d12-vulkan'; renderWorldFingerprintEqual = $worldBackendEqual; sourceWorld = $d3d12.renderWorld.fingerprint; packageWorld = $vulkan.renderWorld.fingerprint })
        }
    }
    return [ordered]@{ pass = ($script:Issues.Count -eq $before); comparisons = @($comparisons) }
}

function Write-Receipt {
    param([Parameter(Mandatory = $true)][bool]$Success)
    $receiptName = if ($script:ProductionContract) { 'hybrid-pixel-production-receipt.json' } else { 'hybrid-pixel-vfx-post-receipt.json' }
    $receiptPath = Join-Path $script:OutputRootPath $receiptName
    $receipt = [ordered]@{
        schemaVersion = if ($script:ProductionContract) { 'noemancer.hybrid-pixel-production-receipt/0.1' } else { 'noemancer.hybrid-pixel-vfx-post-receipt/0.1' }; capturedAt = [DateTimeOffset]::UtcNow.ToString('o'); success = $Success
        currentSlice = if ($script:ProductionContract) { 'hybrid-pixel.package-player-production-validation' } else { 'hybrid-pixel.pixel-aligned-vfx-and-controlled-post' }; sourceProject = $ProjectRoot; copiedProject = Get-RelativeArtifactPath $script:TemporaryProjectRoot; outputRoot = $script:OutputRootPath
        sourceProjectManifest = [ordered]@{ path = $ProjectRoot; sha256Before = $script:SourceManifestHashBefore; sha256After = $script:SourceManifestHashAfter; unchanged = (-not [string]::IsNullOrWhiteSpace($script:SourceManifestHashBefore) -and $script:SourceManifestHashBefore -eq $script:SourceManifestHashAfter) }
        sourceProjectTree = [ordered]@{ path = $ProjectRoot; sha256Before = $script:SourceProjectHashBefore; sha256After = $script:SourceProjectHashAfter; unchanged = (-not [string]::IsNullOrWhiteSpace($script:SourceProjectHashBefore) -and $script:SourceProjectHashBefore -eq $script:SourceProjectHashAfter) }
        runtime = [ordered]@{ path = $RuntimePath; configuration = $Config; targetProfile = $script:TargetProfile; sha256 = if (Test-Path -LiteralPath $RuntimePath -PathType Leaf) { Get-Sha256 $RuntimePath } else { '' } }
        fixture = $script:Fixture; nativeBoundary = $script:NativeBoundary; sourceHeadless = $script:SourceHeadless; package = $script:Package; packageHeadless = $script:PackageHeadless; packageClosure = $script:PackageClosure
        sourceCaptures = @($script:SourceCaptures); packageCaptures = @($script:PackageCaptures); comparisons = $script:Comparisons; productionEvidence = $script:ProductionEvidence; performanceEvidence = @($script:PerformanceEvidence); issueCount = $script:Issues.Count; issues = @($script:Issues)
        receipt = $receiptName
    }
    $json = $receipt | ConvertTo-Json -Depth 50
    Write-Utf8 -Path $receiptPath -Text ($json + "`n")
    [Console]::Out.WriteLine($json)
    return $receiptPath
}

$resolvedProject = $null
$sourceManifestPath = $null
$exitCode = 3
try {
    if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
        $stamp = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
        $prefix = if ($script:ProductionContract) { 'hybrid-pixel-production-' } else { 'hybrid-pixel-vfx-post-' }
        $OutputRoot = Join-Path $script:EngineRoot ('generated\acceptance\' + $prefix + $stamp)
    }
    $script:OutputRootPath = Get-FullPath $OutputRoot
    if (Test-Path -LiteralPath $script:OutputRootPath) { Add-Issue -Code 'output.exists' -Stage 'invocation' -Message "Evidence output already exists: $script:OutputRootPath" } else { New-Item -ItemType Directory -Path $script:OutputRootPath -Force | Out-Null }
    $script:DiagnosticsRoot = Join-Path $script:OutputRootPath 'diagnostics'
    New-Item -ItemType Directory -Path $script:DiagnosticsRoot -Force | Out-Null

    $resolvedProject = Get-FullPath $ProjectRoot
    $sourceManifestPath = Join-Path $resolvedProject 'noemancer.project.json'
    if (-not (Test-Path -LiteralPath $resolvedProject -PathType Container)) { Add-Issue -Code 'project.root-invalid' -Stage 'invocation' -Message "Project root is not a directory: $ProjectRoot" }
    if (-not (Test-Path -LiteralPath $sourceManifestPath -PathType Leaf)) { Add-Issue -Code 'project.manifest-missing' -Stage 'invocation' -Message 'Source Lumen Run manifest is missing.' }
    if ([string]::IsNullOrWhiteSpace($RuntimePath)) { $RuntimePath = Join-Path $script:EngineRoot ("build\windows-msvc-debug\src\runtime\$Config\noemancer.exe") }
    $RuntimePath = Get-FullPath $RuntimePath
    if ($Config -ne 'Release') { Add-Issue -Code 'runtime.release-required' -Stage 'invocation' -Message 'The acceptance requires the Release Runtime and windows-x64-release package profile.' }
    if (-not (Test-Path -LiteralPath $RuntimePath -PathType Leaf)) { Add-Issue -Code 'runtime.missing' -Stage 'invocation' -Message "Runtime executable is missing: $RuntimePath" }
    if (Test-Path -LiteralPath $sourceManifestPath -PathType Leaf) { $script:SourceManifestHashBefore = Get-Sha256 $sourceManifestPath }
    if (Test-Path -LiteralPath $resolvedProject -PathType Container) { $script:SourceProjectHashBefore = Get-ProjectTreeHash -ProjectPath $resolvedProject -Stage 'sourceBoundary' }

    $script:TemporaryProjectRoot = Join-Path $script:OutputRootPath 'lumen-run-project'
    if ($script:Issues.Count -eq 0) {
        Copy-ProjectSnapshot -Source $resolvedProject -Destination $script:TemporaryProjectRoot
        $script:Fixture = if ($script:ProductionContract) {
            New-HybridPixelProductionFixture -ProjectPath $script:TemporaryProjectRoot
        } else {
            Inject-AcceptanceFixture -ProjectPath $script:TemporaryProjectRoot
        }
        if ($null -ne $script:Fixture) { $script:ExpectedProfile = $script:Fixture.profile; $script:NativeBoundary = Test-ProjectNativeBoundary -ProjectPath $script:TemporaryProjectRoot }
    }
    if ($script:Issues.Count -eq 0) {
        $script:SourceHeadless = Invoke-HeadlessProbe -Executable $RuntimePath -Arguments @('run', '--headless', '--frames', [string]$Frames, '--format', 'json', '--project', $script:TemporaryProjectRoot) -WorkingDirectory $script:TemporaryProjectRoot -Stage 'source-headless'
    }
    $packageRoot = Join-Path $script:OutputRootPath 'package'
    if ($script:Issues.Count -eq 0) {
        $script:Package = Invoke-PackageBuild -Runtime $RuntimePath -CopiedProject $script:TemporaryProjectRoot -PackageRoot $packageRoot
        if ($null -ne $script:Package.envelope) { $script:PackageClosure = Test-PackageClosure -PackageResult $script:Package -PackageRoot $packageRoot -CopiedProject $script:TemporaryProjectRoot }
        $script:Package = Resolve-PackagedPlayer -PackageResult $script:Package
    }
    if ($script:Issues.Count -eq 0 -and $null -ne $script:Package -and $null -ne $script:Package.playerPath) {
        $script:PackageHeadless = Invoke-HeadlessProbe -Executable $script:Package.playerPath -Arguments @('player', '--profile', $script:Package.profilePath, '--headless', '--frames', [string]$Frames, '--format', 'json') -WorkingDirectory $packageRoot -Stage 'package-player-headless'
    }
    if ($script:Issues.Count -eq 0) {
        foreach ($backend in $script:Backends) {
            $capture = Invoke-HiddenCapture -Executable $RuntimePath -PrefixArguments @('run', '--project', $script:TemporaryProjectRoot) -WorkingDirectory $script:TemporaryProjectRoot -Stage ('source-' + $backend) -Backend $backend
            [void]$script:SourceCaptures.Add([pscustomobject]$capture)
        }
    }
    if ($script:Issues.Count -eq 0 -and $null -ne $script:Package -and $null -ne $script:Package.playerPath) {
        foreach ($backend in $script:Backends) {
            $capture = Invoke-HiddenCapture -Executable $script:Package.playerPath -PrefixArguments @('player', '--profile', $script:Package.profilePath) -WorkingDirectory $packageRoot -Stage ('package-' + $backend) -Backend $backend
            [void]$script:PackageCaptures.Add([pscustomobject]$capture)
        }
    }
    if ($script:Issues.Count -eq 0) { $script:Comparisons = Compare-CaptureContracts -Source @($script:SourceCaptures) -Package @($script:PackageCaptures) }
    if ($script:ProductionContract -and $script:Issues.Count -eq 0) {
        foreach ($backend in $script:Backends) {
            [void]$script:PerformanceEvidence.Add((Invoke-HiddenPerformanceProbe -Executable $script:Package.playerPath `
                -ProfilePath $script:Package.profilePath -WorkingDirectory $packageRoot -Backend $backend))
        }
    }
    if ($script:ProductionContract -and $script:Issues.Count -eq 0) {
        $script:ProductionEvidence = Test-HybridPixelProductionEvidence `
            -SourceHeadlessPath (Join-Path $script:OutputRootPath 'source-headless.stdout.jsonl') `
            -PackageHeadlessPath (Join-Path $script:OutputRootPath 'package-player-headless.stdout.jsonl') `
            -SourceCapturePaths @($script:Backends | ForEach-Object { Join-Path $script:OutputRootPath ('source-' + $_ + '.stdout.jsonl') }) `
            -PackageCapturePaths @($script:Backends | ForEach-Object { Join-Path $script:OutputRootPath ('package-' + $_ + '.stdout.jsonl') }) `
            -PackageRoot $packageRoot -CopiedProjectRoot $script:TemporaryProjectRoot -Fixture $script:Fixture
        foreach ($issue in @($script:ProductionEvidence.issues)) {
            Add-Issue -Code ([string]$issue.code) -Stage ([string]$issue.stage) -Path ([string]$issue.path) -Message ([string]$issue.message)
        }
    }
    $exitCode = if ($script:Issues.Count -eq 0) { 0 } else { 3 }
}
catch {
    Add-Issue -Code 'script.unexpected-error' -Stage 'script' -Message $_.Exception.ToString()
    $exitCode = 1
}
finally {
    if ($null -ne $sourceManifestPath -and (Test-Path -LiteralPath $sourceManifestPath -PathType Leaf)) {
        $script:SourceManifestHashAfter = Get-Sha256 $sourceManifestPath
        if (-not [string]::IsNullOrWhiteSpace($script:SourceManifestHashBefore) -and $script:SourceManifestHashAfter -ne $script:SourceManifestHashBefore) { Add-Issue -Code 'project.source-modified' -Stage 'sourceBoundary' -Message 'Source Lumen Run manifest changed during verification.'; $exitCode = 3 }
    }
    if ($null -ne $resolvedProject -and (Test-Path -LiteralPath $resolvedProject -PathType Container)) {
        $script:SourceProjectHashAfter = Get-ProjectTreeHash -ProjectPath $resolvedProject -Stage 'sourceBoundary'
        if (-not [string]::IsNullOrWhiteSpace($script:SourceProjectHashBefore) -and $script:SourceProjectHashAfter -ne $script:SourceProjectHashBefore) { Add-Issue -Code 'project.tree-modified' -Stage 'sourceBoundary' -Message 'Source Lumen Run tree changed during verification.'; $exitCode = 3 }
    }
    if ($null -ne $script:OutputRootPath -and (Test-Path -LiteralPath $script:OutputRootPath -PathType Container)) {
        try { [void](Write-Receipt -Success:($exitCode -eq 0 -and $script:Issues.Count -eq 0)) } catch { [Console]::Error.WriteLine("Could not write acceptance receipt: $($_.Exception.Message)"); $exitCode = 1 }
    }
}
exit $exitCode
