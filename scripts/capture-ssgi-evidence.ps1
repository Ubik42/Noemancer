[CmdletBinding()]
param(
    [ValidateSet('Release')]
    [string]$Config = 'Release',
    [ValidateSet('direct3d12', 'vulkan')]
    [string[]]$GpuBackends = @('direct3d12', 'vulkan'),
    [string]$ProjectPath = 'D:\3D\NoemancerProjects\NoemancerRenderLab',
    [string]$ContractPath,
    [string]$OutputRoot,
    [ValidateRange(8, 600)]
    [int]$CaptureFrames = 64,
    [ValidateRange(0, 10000)]
    [int]$PerformanceWarmupFrames = 32,
    [ValidateRange(60, 10000)]
    [int]$PerformanceSampleFrames = 60,
    [ValidateRange(30, 900)]
    [int]$TimeoutSeconds = 180
)

# SSGI acceptance is a strict hidden Release contract.  The Runtime command
# surface expected by this script is:
#   --ssgi-quality low|medium|high --disable-ssgi --disable-auto-exposure
# The script intentionally does not infer a successful SSGI pass from pixels;
# the Renderer Status, Render Graph, shared history and real GPU timestamp
# contracts must all be present before the receipt can pass.
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$script:Checks = New-Object 'System.Collections.Generic.List[object]'
$script:Issues = New-Object 'System.Collections.Generic.List[object]'
$script:ReceiptPath = $null

function Add-Issue {
    param(
        [Parameter(Mandatory = $true)][string]$Code,
        [Parameter(Mandatory = $true)][string]$Stage,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Message,
        [AllowNull()][object]$Observed,
        [AllowNull()][object]$Expected
    )
    [void]$script:Issues.Add([ordered]@{ code = $Code; stage = $Stage; message = $Message; observed = $Observed; expected = $Expected })
}

function Add-Check {
    param(
        [Parameter(Mandatory = $true)][string]$Code,
        [Parameter(Mandatory = $true)][bool]$Pass,
        [Parameter(Mandatory = $true)][string]$Stage,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Message,
        [AllowNull()][object]$Observed,
        [AllowNull()][object]$Expected
    )
    [void]$script:Checks.Add([ordered]@{ code = $Code; stage = $Stage; pass = $Pass; message = $Message; observed = $Observed; expected = $Expected })
    if (-not $Pass) { Add-Issue $Code $Stage $Message $Observed $Expected }
}

function Write-JsonDocument {
    param([Parameter(Mandatory = $true)][string]$Path, [Parameter(Mandatory = $true)]$Value)
    $parent = Split-Path -Parent $Path
    if (-not [string]::IsNullOrWhiteSpace($parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    $json = $Value | ConvertTo-Json -Depth 100
    $utf8NoBom = New-Object System.Text.UTF8Encoding -ArgumentList $false
    [IO.File]::WriteAllText($Path, $json + [Environment]::NewLine, $utf8NoBom)
}

function Get-FileSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    return ([string](Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash).ToLowerInvariant()
}

function Read-JsonFile {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    return Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json
}

function Get-ExactProperty {
    param([AllowNull()][object]$Object, [Parameter(Mandatory = $true)][string]$Name)
    if ($null -eq $Object) { return $null }
    if ($Object -is [System.Collections.IDictionary]) {
        if ($Object.Contains($Name)) { return $Object[$Name] }
        return $null
    }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}

function Get-JsonProperty {
    param([AllowNull()][object]$Object, [Parameter(Mandatory = $true)][string]$Path)
    $current = $Object
    foreach ($part in $Path.Split('.')) {
        $current = Get-ExactProperty $current $part
        if ($null -eq $current) { return $null }
    }
    return $current
}

function Get-FiniteDouble {
    param([AllowNull()][object]$Value)
    if ($null -eq $Value) { return $null }
    try {
        $number = [double]$Value
        if ([double]::IsNaN($number) -or [double]::IsInfinity($number)) { return $null }
        return $number
    } catch { return $null }
}

function Invoke-HiddenProcess {
    param(
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [Parameter(Mandatory = $true)][string]$StdoutPath,
        [Parameter(Mandatory = $true)][string]$StderrPath,
        [Parameter(Mandatory = $true)][int]$TimeoutSeconds
    )
    $startedAt = [DateTimeOffset]::UtcNow
    $process = Start-Process -FilePath $Executable -ArgumentList $Arguments -WorkingDirectory $WorkingDirectory `
        -WindowStyle Hidden -RedirectStandardOutput $StdoutPath -RedirectStandardError $StderrPath -PassThru
    $completed = $process.WaitForExit($TimeoutSeconds * 1000)
    if (-not $completed) {
        try { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue } catch { }
        return [ordered]@{ started = $true; completed = $false; timedOut = $true; exitCode = $null; processId = $process.Id; durationMilliseconds = ([DateTimeOffset]::UtcNow - $startedAt).TotalMilliseconds; stdout = $StdoutPath; stderr = $StderrPath }
    }
    return [ordered]@{ started = $true; completed = $true; timedOut = $false; exitCode = $process.ExitCode; processId = $process.Id; durationMilliseconds = ([DateTimeOffset]::UtcNow - $startedAt).TotalMilliseconds; stdout = $StdoutPath; stderr = $StderrPath }
}

function Get-Int32LE {
    param([byte[]]$Bytes, [int]$Offset)
    return [BitConverter]::ToInt32($Bytes, $Offset)
}

function Get-UInt16LE {
    param([byte[]]$Bytes, [int]$Offset)
    return [BitConverter]::ToUInt16($Bytes, $Offset)
}

function Convert-SrgbByteToLinear {
    param([int]$Value)
    $srgb = [double]$Value / 255.0
    if ($srgb -le 0.04045) { return $srgb / 12.92 }
    return [Math]::Pow(($srgb + 0.055) / 1.055, 2.4)
}

function Read-BmpHeader {
    param([Parameter(Mandatory = $true)][byte[]]$Bytes, [Parameter(Mandatory = $true)][int]$ExpectedWidth, [Parameter(Mandatory = $true)][int]$ExpectedHeight)
    if ($Bytes.Length -lt 54 -or [char]$Bytes[0] -ne 'B' -or [char]$Bytes[1] -ne 'M') { throw 'SSGI A/B capture must be a Windows BMP.' }
    $dibSize = Get-Int32LE $Bytes 14
    $width = Get-Int32LE $Bytes 18
    $heightSigned = Get-Int32LE $Bytes 22
    $height = [Math]::Abs($heightSigned)
    $planes = Get-UInt16LE $Bytes 26
    $bitsPerPixel = Get-UInt16LE $Bytes 28
    $compression = Get-Int32LE $Bytes 30
    $pixelOffset = Get-Int32LE $Bytes 10
    $standardBgraBitfields = $bitsPerPixel -eq 32 -and $compression -eq 3 -and $dibSize -ge 56 -and
        (Get-Int32LE $Bytes 54) -eq 0x00FF0000 -and
        (Get-Int32LE $Bytes 58) -eq 0x0000FF00 -and
        (Get-Int32LE $Bytes 62) -eq 0x000000FF
    if ($dibSize -lt 40 -or $width -ne $ExpectedWidth -or $height -ne $ExpectedHeight -or $planes -ne 1 -or
        $bitsPerPixel -notin @(24, 32) -or ($compression -ne 0 -and -not $standardBgraBitfields)) {
        throw "SSGI A/B BMP must be uncompressed 24/32-bit BGR or standard 32-bit BGRA bitfields ${ExpectedWidth}x${ExpectedHeight}; got ${width}x${height}, bpp=$bitsPerPixel, compression=$compression."
    }
    $bytesPerPixel = [int]($bitsPerPixel / 8)
    $rowStride = [int]([Math]::Floor(($width * $bitsPerPixel + 31) / 32.0) * 4)
    if ($pixelOffset -lt 54 -or $pixelOffset + $rowStride * $height -gt $Bytes.Length) { throw 'SSGI A/B BMP pixel payload is truncated.' }
    return [ordered]@{ width = $width; height = $height; topDown = $heightSigned -lt 0; pixelOffset = $pixelOffset; bytesPerPixel = $bytesPerPixel; rowStride = $rowStride }
}

function Get-ContractRoi {
    param([Parameter(Mandatory = $true)]$Contract, [Parameter(Mandatory = $true)][string]$Name)
    $roi = Get-JsonProperty $Contract "rois.$Name"
    if ($null -eq $roi) { throw "SSGI contract is missing rois.$Name." }
    foreach ($field in @('x0', 'x1', 'y0', 'y1')) {
        $value = Get-FiniteDouble (Get-ExactProperty $roi $field)
        if ($null -eq $value -or $value -lt 0 -or $value -gt 1) { throw "SSGI ROI '$Name.$field' must be finite normalized coordinates." }
    }
    if ([double]$roi.x0 -ge [double]$roi.x1 -or [double]$roi.y0 -ge [double]$roi.y1) { throw "SSGI ROI '$Name' has invalid bounds." }
    return [ordered]@{ x0 = [double]$roi.x0; x1 = [double]$roi.x1; y0 = [double]$roi.y0; y1 = [double]$roi.y1 }
}

function Get-SsgiAbLuma {
    param(
        [Parameter(Mandatory = $true)][string]$EnabledPath,
        [Parameter(Mandatory = $true)][string]$DisabledPath,
        [Parameter(Mandatory = $true)][int]$ExpectedWidth,
        [Parameter(Mandatory = $true)][int]$ExpectedHeight,
        [Parameter(Mandatory = $true)]$DiffuseGiRoi,
        [Parameter(Mandatory = $true)]$ControlRoi
    )
    $enabledBytes = [IO.File]::ReadAllBytes($EnabledPath)
    $disabledBytes = [IO.File]::ReadAllBytes($DisabledPath)
    $enabledHeader = Read-BmpHeader $enabledBytes $ExpectedWidth $ExpectedHeight
    $disabledHeader = Read-BmpHeader $disabledBytes $ExpectedWidth $ExpectedHeight
    $linearLut = [double[]](0..255 | ForEach-Object { Convert-SrgbByteToLinear $_ })
    $regions = [ordered]@{
        diffuseGi = [ordered]@{ roi = $DiffuseGiRoi; count = 0; enabledSum = 0.0; disabledSum = 0.0; absDeltaSum = 0.0; changedCount = 0; maxAbsDelta = 0.0 }
        control = [ordered]@{ roi = $ControlRoi; count = 0; enabledSum = 0.0; disabledSum = 0.0; absDeltaSum = 0.0; changedCount = 0; maxAbsDelta = 0.0 }
    }
    for ($y = 0; $y -lt $ExpectedHeight; $y++) {
        $enabledRow = if ($enabledHeader.topDown) { $y } else { $ExpectedHeight - 1 - $y }
        $disabledRow = if ($disabledHeader.topDown) { $y } else { $ExpectedHeight - 1 - $y }
        for ($x = 0; $x -lt $ExpectedWidth; $x++) {
            $enabledOffset = $enabledHeader.pixelOffset + $enabledRow * $enabledHeader.rowStride + $x * $enabledHeader.bytesPerPixel
            $disabledOffset = $disabledHeader.pixelOffset + $disabledRow * $disabledHeader.rowStride + $x * $disabledHeader.bytesPerPixel
            $enabledLuma = (0.2126 * $linearLut[[int]$enabledBytes[$enabledOffset + 2]]) + (0.7152 * $linearLut[[int]$enabledBytes[$enabledOffset + 1]]) + (0.0722 * $linearLut[[int]$enabledBytes[$enabledOffset]])
            $disabledLuma = (0.2126 * $linearLut[[int]$disabledBytes[$disabledOffset + 2]]) + (0.7152 * $linearLut[[int]$disabledBytes[$disabledOffset + 1]]) + (0.0722 * $linearLut[[int]$disabledBytes[$disabledOffset]])
            $absoluteDelta = [Math]::Abs($enabledLuma - $disabledLuma)
            foreach ($name in @('diffuseGi', 'control')) {
                $roi = $regions[$name]['roi']
                $inside = $x -ge [int]([double]$roi.x0 * $ExpectedWidth) -and $x -lt [int]([double]$roi.x1 * $ExpectedWidth) -and $y -ge [int]([double]$roi.y0 * $ExpectedHeight) -and $y -lt [int]([double]$roi.y1 * $ExpectedHeight)
                if ($inside) {
                    $region = $regions[$name]
                    $region['count']++
                    $region['enabledSum'] += $enabledLuma
                    $region['disabledSum'] += $disabledLuma
                    $region['absDeltaSum'] += $absoluteDelta
                    if ($absoluteDelta -gt 0.0005) { $region['changedCount']++ }
                    $region['maxAbsDelta'] = [Math]::Max($region['maxAbsDelta'], $absoluteDelta)
                }
            }
        }
    }
    $result = [ordered]@{ width = $ExpectedWidth; height = $ExpectedHeight; regions = [ordered]@{} }
    foreach ($name in @('diffuseGi', 'control')) {
        $region = $regions[$name]
        if ($region['count'] -le 0) { throw "SSGI ROI '$name' contains no pixels." }
        $result.regions[$name] = [ordered]@{
            roi = $region['roi']; pixelCount = $region['count']
            enabledMeanLinear = $region['enabledSum'] / $region['count']
            disabledMeanLinear = $region['disabledSum'] / $region['count']
            meanAbsoluteDeltaLinear = $region['absDeltaSum'] / $region['count']
            changedFraction = $region['changedCount'] / [double]$region['count']
            maxAbsoluteDeltaLinear = $region['maxAbsDelta']
        }
    }
    return $result
}

function Test-SsgiStatus {
    param([Parameter(Mandatory = $true)]$Payload, [Parameter(Mandatory = $true)][string]$Stage, [Parameter(Mandatory = $true)][string]$Backend, [Parameter(Mandatory = $true)][bool]$ExpectedEnabled, [Parameter(Mandatory = $true)][int]$ExpectedWidth, [Parameter(Mandatory = $true)][int]$ExpectedHeight)
    $renderer = Get-JsonProperty $Payload 'renderer'
    $device = Get-JsonProperty $renderer 'device'
    $graph = Get-JsonProperty $renderer 'graph'
    $ssgi = Get-JsonProperty $renderer 'screenSpaceGlobalIllumination'
    $settings = Get-JsonProperty $ssgi 'settings'
    $hiz = Get-JsonProperty $ssgi 'hiZ'
    $history = Get-JsonProperty $ssgi 'historyAuthority'
    $validity = Get-JsonProperty $ssgi 'materialValidity'
    $fallback = Get-JsonProperty $ssgi 'fallback'
    $autoExposure = Get-JsonProperty $renderer 'colorPipeline.autoExposure'
    Add-Check "$Stage.renderer-schema" ($renderer.schemaVersion -eq 'noemancer.renderer-status.v28') $Stage 'Renderer Status must use noemancer.renderer-status.v28.' $renderer.schemaVersion 'noemancer.renderer-status.v28'
    Add-Check "$Stage.backend" ($device.backend -eq $Backend) $Stage "Renderer backend must be $Backend." $device.backend $Backend
    Add-Check "$Stage.sidecar" ([bool]$Payload.pass -and [bool]$Payload.dimensionsMatch) $Stage 'Capture quality sidecar must pass before SSGI evidence is considered.' ([ordered]@{ pass = $Payload.pass; dimensionsMatch = $Payload.dimensionsMatch }) 'pass=true, dimensionsMatch=true'
    Add-Check "$Stage.resolution" ([int]$Payload.width -eq $ExpectedWidth -and [int]$Payload.height -eq $ExpectedHeight -and [int]$renderer.surface.width -eq $ExpectedWidth -and [int]$renderer.surface.height -eq $ExpectedHeight) $Stage 'SSGI capture and renderer status must both report 1920x1080.' ([ordered]@{ capture = @($Payload.width, $Payload.height); renderer = @($renderer.surface.width, $renderer.surface.height) }) @($ExpectedWidth, $ExpectedHeight)
    Add-Check "$Stage.auto-exposure" ($autoExposure.enabled -eq $false -and $autoExposure.mode -eq 'locked-unity') $Stage 'SSGI A/B must explicitly disable automatic exposure.' ([ordered]@{ enabled = $autoExposure.enabled; mode = $autoExposure.mode }) 'enabled=false, mode=locked-unity'
    Add-Check "$Stage.graph" ($graph.graphId -eq 'render.graph.forward.v16' -and $graph.schemaVersion -eq 'noemancer.render-graph.v11' -and [bool]$graph.valid -and @($graph.errors).Count -eq 0) $Stage 'SSGI must run in valid Render Graph v16 while retaining serializer schema v11.' ([ordered]@{ id = $graph.graphId; schema = $graph.schemaVersion; valid = $graph.valid; errors = @($graph.errors).Count }) 'id=v16, schema=v11, valid=true, errors=0'
    $passIds = @($graph.passes | ForEach-Object { [string]$_.id })
    $requiredPasses = @('render.pass.depth-pyramid-seed', 'render.pass.depth-pyramid-reduce', 'render.pass.ssgi-hierarchical-gather', 'render.pass.ssgi-spatial-resolve', 'render.pass.ssgi-temporal-resolve', 'render.pass.ssgi-composite')
    $missing = @($requiredPasses | Where-Object { $passIds -notcontains $_ })
    Add-Check "$Stage.graph-passes" ($missing.Count -eq 0) $Stage 'Render Graph must contain real SSGI passes and shared HiZ preparation.' $missing $requiredPasses
    Add-Check "$Stage.ssgi-schema" ($ssgi.schema -eq 'noemancer.screen-space-global-illumination/0.1') $Stage 'SSGI status must publish noemancer.screen-space-global-illumination/0.1.' $ssgi.schema 'noemancer.screen-space-global-illumination/0.1'
    Add-Check "$Stage.enabled" ([bool]$ssgi.enabled -eq $ExpectedEnabled) $Stage "SSGI enabled state must be $ExpectedEnabled for this stage." $ssgi.enabled $ExpectedEnabled
    $expectedQuality = if ($ExpectedEnabled) { 'high' } else { 'off' }
    Add-Check "$Stage.quality" ($settings.quality -eq $expectedQuality) $Stage "SSGI quality must report '$expectedQuality' for this stage." $settings.quality $expectedQuality
    $sampleCount = Get-FiniteDouble $settings.sampleCount; $directions = Get-FiniteDouble $settings.directions; $steps = Get-FiniteDouble $settings.maxSteps; $maxMip = Get-FiniteDouble $settings.maxMip; $radius = Get-FiniteDouble $settings.radius; $maxDistance = Get-FiniteDouble $settings.maxDistance; $thickness = Get-FiniteDouble $settings.thickness; $historyWeight = Get-FiniteDouble $settings.historyWeight
    $settingsPass = if ($ExpectedEnabled) { $null -ne $sampleCount -and $sampleCount -ge 1 -and $null -ne $directions -and $directions -ge 1 -and $null -ne $steps -and $steps -ge 1 -and $null -ne $maxMip -and $maxMip -ge 1 -and $null -ne $radius -and $radius -gt 0 -and $null -ne $maxDistance -and $maxDistance -gt 0 -and $null -ne $thickness -and $thickness -gt 0 -and $null -ne $historyWeight -and $historyWeight -ge 0 -and $historyWeight -le 1 } else { $settings.quality -eq 'off' -and $historyWeight -eq 0 }
    Add-Check "$Stage.settings" $settingsPass $Stage 'SSGI settings must expose bounded high-quality sampling/history parameters when enabled, and an explicit Off policy with temporal weight zero when disabled.' ([ordered]@{ sampleCount = $sampleCount; directions = $directions; maxSteps = $steps; maxMip = $maxMip; radius = $radius; maxDistance = $maxDistance; thickness = $thickness; historyWeight = $historyWeight }) 'enabled: positive sampling budget, historyWeight=[0,1]; disabled: quality=off and historyWeight=0'
    Add-Check "$Stage.hiz" ([bool]$hiz.reused -and $hiz.resourceId -eq 'render.resource.scene-depth-pyramid' -and [int]$hiz.mipCount -gt 1) $Stage 'SSGI must reuse the shared min/max HiZ resource.' ([ordered]@{ reused = $hiz.reused; resourceId = $hiz.resourceId; mipCount = $hiz.mipCount }) 'reused=true, resourceId=render.resource.scene-depth-pyramid, mipCount>1'
    Add-Check "$Stage.material-validity" ($validity.contract -eq 'normal.rgb+roughness.a+baseColor.rgb+metallic.a' -and -not [string]::IsNullOrWhiteSpace([string]$validity.roughnessSource) -and -not [string]::IsNullOrWhiteSpace([string]$validity.surfaceSource) -and $validity.invalidMaterialPolicy -eq 'retain-ibl-diffuse') $Stage 'SSGI must publish the normal/roughness/base-color/metallic material contract and IBL diffuse fallback.' ([ordered]@{ contract = $validity.contract; surfaceSource = $validity.surfaceSource; roughnessSource = $validity.roughnessSource; invalidMaterialPolicy = $validity.invalidMaterialPolicy }) 'normal.rgb+roughness.a+baseColor.rgb+metallic.a, sources present, retain-ibl-diffuse'
    Add-Check "$Stage.fallback" ($fallback.policy -eq 'retain-ibl-diffuse' -and -not [string]::IsNullOrWhiteSpace([string]$fallback.offscreen) -and $fallback.offscreen -eq 'retain-ibl-diffuse' -and $fallback.history -eq 'spatial-current-frame') $Stage 'SSGI must publish deterministic offscreen and history fallbacks.' ([ordered]@{ policy = $fallback.policy; offscreen = $fallback.offscreen; history = $fallback.history }) 'retain-ibl-diffuse, spatial-current-frame'
    $debugViews = @($ssgi.debugViews | ForEach-Object { [string]$_ }); $requiredDebugViews = @('final', 'confidence', 'visibility', 'bent-normal', 'miss')
    Add-Check "$Stage.debug-views" (@($requiredDebugViews | Where-Object { $debugViews -notcontains $_ }).Count -eq 0) $Stage 'SSGI must expose final, confidence, visibility, bent-normal and miss debug views.' $debugViews $requiredDebugViews
    $minimumRevision = if ($ExpectedEnabled) { 1 } else { 0 }
    Add-Check "$Stage.history" ($history.schema -eq 'noemancer.temporal-history/0.1' -and $history.consumer -eq 'ssgi' -and [int]$history.revision -ge $minimumRevision -and $null -ne $history.currentValid -and $null -ne $history.previousValid -and $null -ne $history.lastResetReasons) $Stage 'SSGI must report its independent shared History Authority slot, revision, validity and reset reasons.' ([ordered]@{ schema = $history.schema; consumer = $history.consumer; revision = $history.revision; currentValid = $history.currentValid; previousValid = $history.previousValid; lastResetReasons = $history.lastResetReasons }) "schema=temporal-history/0.1, consumer=ssgi, revision>=$minimumRevision, validity/reset fields present"
    Add-Check "$Stage.artifact" ($device.artifactStatus -eq 'manifest-and-artifact-verified' -and $device.shaderArtifact -in @('DXIL', 'SPIR-V')) $Stage 'SSGI shader artifacts must be verified for the selected backend.' ([ordered]@{ status = $device.artifactStatus; shaderArtifact = $device.shaderArtifact }) 'manifest-and-artifact-verified, DXIL or SPIR-V'
    return [ordered]@{ schemaVersion = $renderer.schemaVersion; graphId = $graph.graphId; passIds = $passIds; enabled = [bool]$ssgi.enabled; quality = $settings.quality; settings = $settings; hiZ = $hiz; history = $history; materialValidity = $validity; fallback = $fallback; debugViews = $debugViews; shaderArtifact = $device.shaderArtifact; shaderManifestHash = $device.artifactContract.manifestHash }
}

function Get-GpuSummary {
    param([Parameter(Mandatory = $true)]$Payload, [Parameter(Mandatory = $true)][string]$Stage, [Parameter(Mandatory = $true)][int]$MinimumSamples)
    Add-Check "$Stage.schema" ($Payload.schemaVersion -eq 'noemancer.performance-evidence/0.1') $Stage 'Performance evidence must use noemancer.performance-evidence/0.1.' $Payload.schemaVersion 'noemancer.performance-evidence/0.1'
    Add-Check "$Stage.available" ([bool]$Payload.gpu.available -and -not [string]::IsNullOrWhiteSpace([string]$Payload.gpu.source)) $Stage 'SSGI performance requires real GPU telemetry; CPU frame time cannot substitute.' ([ordered]@{ available = $Payload.gpu.available; source = $Payload.gpu.source }) 'gpu.available=true and source present'
    $timestamps = $Payload.gpu.passTimestamps
    Add-Check "$Stage.timestamp-contract" ($timestamps.schemaVersion -eq 'noemancer.gpu-pass-timestamps/0.1' -and [bool]$timestamps.supported -and $timestamps.queueScope -eq 'graphics' -and $timestamps.readback -eq 'fence-gated-frame-ring') $Stage 'GPU pass timestamps must use a supported fence-gated graphics frame ring.' ([ordered]@{ schema = $timestamps.schemaVersion; supported = $timestamps.supported; queueScope = $timestamps.queueScope; readback = $timestamps.readback }) 'supported=true, graphics, fence-gated-frame-ring'
    $summary = [ordered]@{}
    foreach ($passId in @('render.pass.depth-pyramid-reduce', 'render.pass.ssgi-hierarchical-gather', 'render.pass.ssgi-spatial-resolve', 'render.pass.ssgi-temporal-resolve', 'render.pass.ssgi-composite')) {
        $distribution = Get-ExactProperty $timestamps.passDistributions $passId
        $sampleCount = Get-FiniteDouble (Get-ExactProperty $distribution 'sampleCount'); $p95 = Get-FiniteDouble (Get-ExactProperty $distribution 'p95'); $unit = [string](Get-ExactProperty $distribution 'unit')
        $pass = $null -ne $distribution -and $null -ne $sampleCount -and $sampleCount -ge $MinimumSamples -and $null -ne $p95 -and $p95 -ge 0 -and $unit -eq 'milliseconds'
        Add-Check "$Stage.gpu-$($passId.Replace('.', '-'))" $pass $Stage "GPU timestamp distribution for $passId must have at least $MinimumSamples samples and a finite p95." ([ordered]@{ sampleCount = $sampleCount; p95 = $p95; unit = $unit }) "sampleCount>=$MinimumSamples, p95>=0 milliseconds"
        $summary[$passId] = [ordered]@{ sampleCount = $sampleCount; p95Milliseconds = $p95; unit = $unit }
    }
    return [ordered]@{ schemaVersion = $timestamps.schemaVersion; source = $Payload.gpu.source; availableFrameCount = $timestamps.availableFrameCount; distributions = $summary }
}

function Test-SsgiAb {
    param([Parameter(Mandatory = $true)]$Ab, [Parameter(Mandatory = $true)]$Thresholds)
    $diffuseGi = $Ab.regions.diffuseGi; $control = $Ab.regions.control
    $giMin = [double]$Thresholds.diffuseGiMeanAbsoluteDeltaMin; $giChangedMin = [double]$Thresholds.diffuseGiChangedFractionMin; $controlMax = [double]$Thresholds.controlMeanAbsoluteDeltaMax
    $giPass = [double]$diffuseGi.meanAbsoluteDeltaLinear -ge $giMin -and [double]$diffuseGi.changedFraction -ge $giChangedMin
    $controlPass = [double]$control.meanAbsoluteDeltaLinear -le $controlMax
    Add-Check 'ab.diffuse-gi-change' $giPass 'ab' 'SSGI enabled/disabled must produce a direction-independent absolute linear-luma delta in the diffuse GI ROI.' ([ordered]@{ meanAbsoluteDeltaLinear = $diffuseGi.meanAbsoluteDeltaLinear; changedFraction = $diffuseGi.changedFraction }) ([ordered]@{ meanAbsoluteDeltaLinearMin = $giMin; changedFractionMin = $giChangedMin })
    Add-Check 'ab.control-stable' $controlPass 'ab' 'SSGI enabled/disabled must not materially change the control ROI.' ([ordered]@{ meanAbsoluteDeltaLinear = $control.meanAbsoluteDeltaLinear; maxAbsoluteDeltaLinear = $control.maxAbsoluteDeltaLinear }) ([ordered]@{ meanAbsoluteDeltaLinearMax = $controlMax })
    return [ordered]@{ diffuseGi = $diffuseGi; control = $control; thresholds = [ordered]@{ diffuseGiMeanAbsoluteDeltaMin = $giMin; diffuseGiChangedFractionMin = $giChangedMin; controlMeanAbsoluteDeltaMax = $controlMax }; pass = $giPass -and $controlPass }
}

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$runtime = Join-Path $repositoryRoot "build/windows-msvc-debug/src/runtime/$Config/noemancer.exe"
$engineScript = Join-Path $repositoryRoot 'scripts/engine.ps1'
$expectedWidth = 1920; $expectedHeight = 1080
$expectedArtifacts = @{ direct3d12 = 'DXIL'; vulkan = 'SPIR-V' }

try {
    $ProjectPath = [IO.Path]::GetFullPath($ProjectPath)
    if ([string]::IsNullOrWhiteSpace($ContractPath)) { $ContractPath = Join-Path $ProjectPath 'ssgi-evidence.contract.json' }
    $ContractPath = [IO.Path]::GetFullPath($ContractPath)
    if ([string]::IsNullOrWhiteSpace($OutputRoot)) { $OutputRoot = Join-Path $repositoryRoot "generated/acceptance/ssgi-v16-$(Get-Date -Format yyyyMMdd-HHmmss)" }
    $OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
    if (Test-Path -LiteralPath $OutputRoot) {
        if (Get-ChildItem -LiteralPath $OutputRoot -Force | Select-Object -First 1) { throw "SSGI evidence output must be empty because receipts are immutable: $OutputRoot" }
    } else { New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null }
    $script:ReceiptPath = Join-Path $OutputRoot 'ssgi-evidence.json'

    $contract = Read-JsonFile $ContractPath
    Add-Check 'static.contract' ($null -ne $contract -and $contract.schemaVersion -eq 'noemancer.ssgi-evidence-contract/0.1') 'static' 'SSGI evidence contract must be present and versioned.' (Get-JsonProperty $contract 'schemaVersion') 'noemancer.ssgi-evidence-contract/0.1'
    if ($null -eq $contract) { throw "SSGI evidence contract is missing: $ContractPath" }
    Add-Check 'static.contract-id' ((Get-JsonProperty $contract 'contractId') -eq 'commercial-raster.ssgi-production-path') 'static' 'SSGI contract must identify the production-path acceptance surface.' (Get-JsonProperty $contract 'contractId') 'commercial-raster.ssgi-production-path'
    Add-Check 'static.quality' ((Get-JsonProperty $contract 'quality') -eq 'high') 'static' 'SSGI acceptance contract must pin the high quality profile.' (Get-JsonProperty $contract 'quality') 'high'
    $contractWidth = [int](Get-JsonProperty $contract 'surface.width'); $contractHeight = [int](Get-JsonProperty $contract 'surface.height')
    Add-Check 'static.resolution' ($contractWidth -eq $expectedWidth -and $contractHeight -eq $expectedHeight) 'static' 'SSGI contract must pin 1920x1080.' @($contractWidth, $contractHeight) @($expectedWidth, $expectedHeight)
    $diffuseGiRoi = Get-ContractRoi $contract 'diffuseGi'; $controlRoi = Get-ContractRoi $contract 'control'; $thresholds = Get-JsonProperty $contract 'thresholds'
    foreach ($field in @('diffuseGiMeanAbsoluteDeltaMin', 'diffuseGiChangedFractionMin', 'controlMeanAbsoluteDeltaMax')) { if ($null -eq (Get-FiniteDouble (Get-ExactProperty $thresholds $field))) { throw "SSGI threshold is missing or non-finite: thresholds.$field" } }
    Add-Check 'static.ab-intent' ((Get-JsonProperty $contract 'abIntent.delta') -eq 'enabled-minus-disabled-absolute-linear-luma') 'static' 'SSGI A/B contract must define direction-independent absolute linear-luma semantics.' (Get-JsonProperty $contract 'abIntent.delta') 'enabled-minus-disabled-absolute-linear-luma'
    Add-Check 'static.auto-exposure' ((Get-JsonProperty $contract 'abIntent.autoExposure') -eq 'disabled') 'static' 'SSGI contract must require automatic exposure to be disabled.' (Get-JsonProperty $contract 'abIntent.autoExposure') 'disabled'
    Add-Check 'static.diffuse-gi-region' ((Get-JsonProperty $contract 'abIntent.diffuseGiRegion') -eq 'material-grid-and-adjacent-diffuse-indirect-light') 'static' 'SSGI contract must name the diffuse-GI measurement region.' (Get-JsonProperty $contract 'abIntent.diffuseGiRegion') 'material-grid-and-adjacent-diffuse-indirect-light'
    Add-Check 'static.control-region' ((Get-JsonProperty $contract 'abIntent.controlRegion') -eq 'upper-left-atmosphere-background') 'static' 'SSGI contract must name the stable control region.' (Get-JsonProperty $contract 'abIntent.controlRegion') 'upper-left-atmosphere-background'
    Add-Check 'static.gpu-pass-timestamps' ((Get-JsonProperty $contract 'gpuTelemetry.passTimestampsRequired') -eq $true) 'static' 'SSGI contract must require GPU pass timestamps.' (Get-JsonProperty $contract 'gpuTelemetry.passTimestampsRequired') $true
    Add-Check 'static.cpu-cannot-substitute' ((Get-JsonProperty $contract 'gpuTelemetry.cpuTimeMayNotSubstitute') -eq $true) 'static' 'SSGI contract must reject CPU timing as a GPU substitute.' (Get-JsonProperty $contract 'gpuTelemetry.cpuTimeMayNotSubstitute') $true
    Add-Check 'static.project' (Test-Path -LiteralPath $ProjectPath -PathType Container) 'static' 'SSGI project directory must exist.' $ProjectPath 'directory exists'
    Add-Check 'static.engine-script' (Test-Path -LiteralPath $engineScript -PathType Leaf) 'static' 'scripts/engine.ps1 must exist for reproducible builds.' $engineScript 'file exists'
    Add-Check 'static.release' ($Config -eq 'Release') 'static' 'SSGI acceptance is Release-only.' $Config 'Release'

    $buildRun = $null
    if (-not (Test-Path -LiteralPath $runtime -PathType Leaf)) {
        $pwsh = (Get-Command pwsh -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
        $buildRun = Invoke-HiddenProcess $pwsh @('-NoLogo', '-NoProfile', '-File', $engineScript, 'build', '-Config', $Config, '-Target', 'noemancer') $repositoryRoot (Join-Path $OutputRoot 'build.stdout.log') (Join-Path $OutputRoot 'build.stderr.log') $TimeoutSeconds
        Add-Check 'build.process' ($buildRun.completed -and $buildRun.exitCode -eq 0) 'build' 'Hidden fallback Noemancer Release build must complete successfully.' $buildRun 'completed=true, exitCode=0'
    }
    Add-Check 'build.runtime' (Test-Path -LiteralPath $runtime -PathType Leaf) 'build' 'Selected Noemancer Release runtime must exist before SSGI capture.' $runtime 'file exists'
    if (-not (Test-Path -LiteralPath $runtime -PathType Leaf)) { throw "Runtime is missing: $runtime" }

    $runs = New-Object 'System.Collections.Generic.List[object]'
    foreach ($backend in @($GpuBackends | Select-Object -Unique)) {
        $backendRoot = Join-Path $OutputRoot $backend; New-Item -ItemType Directory -Path $backendRoot -Force | Out-Null
        $enabledImage = Join-Path $backendRoot 'ssgi-enabled-1920x1080.bmp'; $disabledImage = Join-Path $backendRoot 'ssgi-disabled-1920x1080.bmp'
        $enabledSidecar = "$enabledImage.quality.json"; $disabledSidecar = "$disabledImage.quality.json"; $performancePath = Join-Path $backendRoot 'ssgi-enabled.performance.json'
        $enabledOut = Join-Path $backendRoot 'enabled.stdout.jsonl'; $enabledErr = Join-Path $backendRoot 'enabled.stderr.log'; $disabledOut = Join-Path $backendRoot 'disabled.stdout.jsonl'; $disabledErr = Join-Path $backendRoot 'disabled.stderr.log'; $performanceOut = Join-Path $backendRoot 'performance.stdout.jsonl'; $performanceErr = Join-Path $backendRoot 'performance.stderr.log'
        $common = @('run', '--format', 'json', '--project', $ProjectPath, '--frames', [string]$CaptureFrames, '--window-width', [string]$expectedWidth, '--window-height', [string]$expectedHeight, '--exposure', '1.0', '--render-scale', '1.0', '--gpu-backend', $backend, '--disable-auto-exposure', '--ssgi-quality', 'high')
        $enabledArgs = @($common) + @('--capture-frame', $enabledImage); $disabledArgs = @($common) + @('--disable-ssgi', '--capture-frame', $disabledImage)
        $performanceArgs = @('run', '--format', 'json', '--project', $ProjectPath, '--window-width', [string]$expectedWidth, '--window-height', [string]$expectedHeight, '--exposure', '1.0', '--render-scale', '1.0', '--gpu-backend', $backend, '--disable-auto-exposure', '--ssgi-quality', 'high', '--performance-evidence', $performancePath, '--performance-hidden', '--performance-workload', 'screen-space-global-illumination-v16', '--performance-warmup-frames', [string]$PerformanceWarmupFrames, '--performance-sample-frames', [string]$PerformanceSampleFrames)
        $enabledProcess = Invoke-HiddenProcess $runtime $enabledArgs $ProjectPath $enabledOut $enabledErr $TimeoutSeconds; $disabledProcess = Invoke-HiddenProcess $runtime $disabledArgs $ProjectPath $disabledOut $disabledErr $TimeoutSeconds; $performanceProcess = Invoke-HiddenProcess $runtime $performanceArgs $ProjectPath $performanceOut $performanceErr $TimeoutSeconds
        Add-Check "$backend.enabled-process" ($enabledProcess.completed -and $enabledProcess.exitCode -eq 0) "$backend/capture" 'Hidden SSGI-enabled capture must exit successfully.' $enabledProcess 'completed=true, exitCode=0'
        Add-Check "$backend.disabled-process" ($disabledProcess.completed -and $disabledProcess.exitCode -eq 0) "$backend/capture" 'Hidden SSGI-disabled capture must exit successfully.' $disabledProcess 'completed=true, exitCode=0'
        Add-Check "$backend.performance-process" ($performanceProcess.completed -and $performanceProcess.exitCode -eq 0) "$backend/performance" 'Hidden SSGI GPU performance run must exit successfully.' $performanceProcess 'completed=true, exitCode=0'
        foreach ($path in @($enabledImage, $disabledImage, $enabledSidecar, $disabledSidecar, $performancePath)) { Add-Check "$backend.artifact-$([IO.Path]::GetFileName($path))" (Test-Path -LiteralPath $path -PathType Leaf) "$backend/artifacts" "Required SSGI evidence artifact must exist: $([IO.Path]::GetFileName($path))." $path 'file exists' }
        $enabledStatus = $null; $disabledStatus = $null; $abSummary = $null; $gpuSummary = $null
        if ((Test-Path -LiteralPath $enabledSidecar -PathType Leaf) -and (Test-Path -LiteralPath $disabledSidecar -PathType Leaf)) {
            try { $enabledStatus = Test-SsgiStatus (Read-JsonFile $enabledSidecar) "$backend/enabled" $backend $true $expectedWidth $expectedHeight; $disabledStatus = Test-SsgiStatus (Read-JsonFile $disabledSidecar) "$backend/disabled" $backend $false $expectedWidth $expectedHeight; $abSummary = Test-SsgiAb (Get-SsgiAbLuma $enabledImage $disabledImage $expectedWidth $expectedHeight $diffuseGiRoi $controlRoi) $thresholds } catch { Add-Issue "$backend.capture-validation" "$backend/capture" $_.Exception.Message ([ordered]@{ enabled = $enabledSidecar; disabled = $disabledSidecar }) 'parseable v28 SSGI sidecars and 1920x1080 BMP A/B' }
        }
        if (Test-Path -LiteralPath $performancePath -PathType Leaf) { try { $gpuSummary = Get-GpuSummary (Read-JsonFile $performancePath) "$backend/performance" $PerformanceSampleFrames } catch { Add-Issue "$backend.performance-validation" "$backend/performance" $_.Exception.Message $performancePath 'parseable GPU pass timestamp evidence' } }
        $artifactRecords = @($enabledImage, $disabledImage, $enabledSidecar, $disabledSidecar, $performancePath) | ForEach-Object { [ordered]@{ path = [IO.Path]::GetFileName($_); exists = Test-Path -LiteralPath $_ -PathType Leaf; sha256 = Get-FileSha256 $_ } }
        [void]$runs.Add([ordered]@{ backend = $backend; shaderArtifact = $expectedArtifacts[$backend]; commands = @([ordered]@{ purpose = 'ssgi-enabled-hidden-capture'; executable = $runtime; arguments = $enabledArgs; process = $enabledProcess }, [ordered]@{ purpose = 'ssgi-disabled-hidden-capture'; executable = $runtime; arguments = $disabledArgs; process = $disabledProcess }, [ordered]@{ purpose = 'ssgi-enabled-hidden-gpu-performance'; executable = $runtime; arguments = $performanceArgs; process = $performanceProcess }); artifacts = $artifactRecords; enabled = $enabledStatus; disabled = $disabledStatus; ab = $abSummary; gpu = $gpuSummary })
    }

    $manifest = [ordered]@{
        schemaVersion = 'noemancer.ssgi-capture-evidence/0.1'; capturedAt = [DateTimeOffset]::UtcNow.ToString('o'); pass = ($script:Issues.Count -eq 0)
        configuration = [ordered]@{ config = $Config; project = $ProjectPath; contract = $ContractPath; requestedResolution = [ordered]@{ width = $expectedWidth; height = $expectedHeight }; captureFrames = $CaptureFrames; performanceWarmupFrames = $PerformanceWarmupFrames; performanceSampleFrames = $PerformanceSampleFrames; timeoutSeconds = $TimeoutSeconds; hiddenProcess = $true; computerUse = $false; autoExposure = 'disabled'; gpuBackends = @($GpuBackends | Select-Object -Unique) }
        contract = [ordered]@{ schemaVersion = $contract.schemaVersion; contractId = $contract.contractId; quality = $contract.quality; sha256 = Get-FileSha256 $ContractPath; surface = $contract.surface; rois = $contract.rois; thresholds = $thresholds; abIntent = $contract.abIntent; gpuTelemetry = $contract.gpuTelemetry }
        runtime = [ordered]@{ path = $runtime.Replace('\', '/'); sha256 = Get-FileSha256 $runtime }; build = $buildRun; runs = $runs.ToArray(); checks = $script:Checks.ToArray(); issues = $script:Issues.ToArray()
        policy = 'SSGI acceptance requires real v28/v16 status, a bounded SSGI pass chain, shared HiZ/history/material-validity/fallback/debug contracts, fixed-exposure A/B, direction-independent diffuse-GI change, stable control ROI, and real GPU pass timestamps. CPU timing cannot satisfy the GPU contract.'
    }
    Write-JsonDocument $script:ReceiptPath $manifest; Write-Output ($manifest | ConvertTo-Json -Depth 100 -Compress); if (-not [bool]$manifest.pass) { exit 5 }
} catch {
    if ($null -ne $script:ReceiptPath) { try { Write-JsonDocument $script:ReceiptPath ([ordered]@{ schemaVersion = 'noemancer.ssgi-capture-evidence/0.1'; capturedAt = [DateTimeOffset]::UtcNow.ToString('o'); pass = $false; error = $_.Exception.Message; checks = $script:Checks.ToArray(); issues = $script:Issues.ToArray() }) } catch { } }
    Write-Error "SSGI evidence failed: $($_.Exception.Message)"; exit 1
}
