[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
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

# Production SSR acceptance is intentionally a strict, hidden, dual-backend
# contract.  Runtime integration is expected to publish the fields consumed
# below; a missing field is a failed receipt, never an inferred pass.  The
# command line contract is default-on/--disable-ssr and --ssr-quality high. Keeping
# this contract in a standalone script prevents a visual A/B from being
# mistaken for proof that a real graph pass or GPU timestamp exists.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:Checks = [System.Collections.Generic.List[object]]::new()
$script:Issues = [System.Collections.Generic.List[object]]::new()
$script:EvidenceRoot = $null
$script:ReceiptPath = $null

function Add-Issue {
    param(
        [Parameter(Mandatory = $true)][string]$Code,
        [Parameter(Mandatory = $true)][string]$Stage,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Message,
        [AllowNull()][object]$Observed,
        [AllowNull()][object]$Expected
    )
    [void]$script:Issues.Add([ordered]@{
        code = $Code
        stage = $Stage
        message = $Message
        observed = $Observed
        expected = $Expected
    })
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
    [void]$script:Checks.Add([ordered]@{
        code = $Code
        stage = $Stage
        pass = $Pass
        message = $Message
        observed = $Observed
        expected = $Expected
    })
    if (-not $Pass) {
        Add-Issue -Code $Code -Stage $Stage -Message $Message -Observed $Observed -Expected $Expected
    }
}

function Write-JsonDocument {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)]$Value
    )
    $parent = Split-Path -Parent $Path
    if (-not [string]::IsNullOrWhiteSpace($parent)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    $json = $Value | ConvertTo-Json -Depth 100
    [IO.File]::WriteAllText($Path, $json + "`n", [Text.UTF8Encoding]::new($false))
}

function Get-FileSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Read-JsonFile {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    return Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json -Depth 100
}

function Get-ExactProperty {
    param(
        [AllowNull()][object]$Object,
        [Parameter(Mandatory = $true)][string]$Name
    )
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
    param(
        [AllowNull()][object]$Object,
        [Parameter(Mandatory = $true)][string]$Path
    )
    $current = $Object
    foreach ($part in $Path.Split('.')) {
        $current = Get-ExactProperty -Object $current -Name $part
        if ($null -eq $current) { return $null }
    }
    return $current
}

function Convert-ToFiniteDouble {
    param([AllowNull()][object]$Value)
    if ($null -eq $Value) { return $null }
    try {
        $number = [double]$Value
        if ([double]::IsNaN($number) -or [double]::IsInfinity($number)) { return $null }
        return $number
    } catch {
        return $null
    }
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
        return [ordered]@{
            started = $true; completed = $false; timedOut = $true; exitCode = $null; processId = $process.Id
            durationMilliseconds = ([DateTimeOffset]::UtcNow - $startedAt).TotalMilliseconds
            stdout = $StdoutPath; stderr = $StderrPath
        }
    }
    return [ordered]@{
        started = $true; completed = $true; timedOut = $false; exitCode = $process.ExitCode; processId = $process.Id
        durationMilliseconds = ([DateTimeOffset]::UtcNow - $startedAt).TotalMilliseconds
        stdout = $StdoutPath; stderr = $StderrPath
    }
}

function Get-Int32LittleEndian {
    param([byte[]]$Bytes, [int]$Offset)
    return [BitConverter]::ToInt32($Bytes, $Offset)
}

function Get-UInt16LittleEndian {
    param([byte[]]$Bytes, [int]$Offset)
    return [BitConverter]::ToUInt16($Bytes, $Offset)
}

function Convert-SrgbByteToLinear {
    param([int]$Value)
    $srgb = [double]$Value / 255.0
    if ($srgb -le 0.04045) { return $srgb / 12.92 }
    return [Math]::Pow(($srgb + 0.055) / 1.055, 2.4)
}

function Get-BmpLumaAB {
    param(
        [Parameter(Mandatory = $true)][string]$EnabledPath,
        [Parameter(Mandatory = $true)][string]$DisabledPath,
        [Parameter(Mandatory = $true)][int]$ExpectedWidth,
        [Parameter(Mandatory = $true)][int]$ExpectedHeight,
        [Parameter(Mandatory = $true)]$ReflectionRoi,
        [Parameter(Mandatory = $true)]$ControlRoi
    )
    $enabledBytes = [IO.File]::ReadAllBytes($EnabledPath)
    $disabledBytes = [IO.File]::ReadAllBytes($DisabledPath)
    $regions = [ordered]@{
        reflection = [ordered]@{ roi = $ReflectionRoi; count = 0; enabledSum = 0.0; disabledSum = 0.0; absDeltaSum = 0.0; changedCount = 0; positiveCount = 0; negativeCount = 0; maxAbsDelta = 0.0 }
        control = [ordered]@{ roi = $ControlRoi; count = 0; enabledSum = 0.0; disabledSum = 0.0; absDeltaSum = 0.0; changedCount = 0; positiveCount = 0; negativeCount = 0; maxAbsDelta = 0.0 }
    }

    $headers = @()
    foreach ($bytes in @($enabledBytes, $disabledBytes)) {
        if ($bytes.Length -lt 54 -or [char]$bytes[0] -ne 'B' -or [char]$bytes[1] -ne 'M') { throw 'SSR A/B images must be Windows BMP files.' }
        $dibSize = Get-Int32LittleEndian $bytes 14
        if ($dibSize -lt 40 -or $bytes.Length -lt (14 + $dibSize)) { throw 'SSR A/B BMP DIB header is truncated or unsupported.' }
        $width = Get-Int32LittleEndian $bytes 18
        $heightSigned = Get-Int32LittleEndian $bytes 22
        $height = [Math]::Abs($heightSigned)
        $planes = Get-UInt16LittleEndian $bytes 26
        $bitsPerPixel = Get-UInt16LittleEndian $bytes 28
        $compression = Get-Int32LittleEndian $bytes 30
        $pixelOffset = Get-Int32LittleEndian $bytes 10
        $standardBgraBitfields = $bitsPerPixel -eq 32 -and $compression -eq 3 -and $dibSize -ge 56 -and
            (Get-Int32LittleEndian $bytes 54) -eq 0x00FF0000 -and
            (Get-Int32LittleEndian $bytes 58) -eq 0x0000FF00 -and
            (Get-Int32LittleEndian $bytes 62) -eq 0x000000FF
        if ($width -le 0 -or $height -le 0 -or $planes -ne 1 -or $bitsPerPixel -notin @(24, 32) -or
            ($compression -ne 0 -and -not $standardBgraBitfields)) {
            throw "SSR A/B BMP must be uncompressed 24/32-bit BGR or standard 32-bit BGRA bitfields: ${width}x${height}, bpp=$bitsPerPixel, compression=$compression."
        }
        if ($width -ne $ExpectedWidth -or $height -ne $ExpectedHeight) {
            throw "SSR A/B BMP must be ${ExpectedWidth}x${ExpectedHeight}, got ${width}x${height}."
        }
        $bytesPerPixel = [int]($bitsPerPixel / 8)
        $rowStride = [int]([Math]::Floor(($width * $bitsPerPixel + 31) / 32.0) * 4)
        $requiredBytes = $pixelOffset + $rowStride * $height
        if ($pixelOffset -lt 54 -or $requiredBytes -gt $bytes.Length) { throw 'SSR A/B BMP pixel payload is truncated.' }
        $headers += [ordered]@{ width = $width; height = $height; topDown = $heightSigned -lt 0; pixelOffset = $pixelOffset; bytesPerPixel = $bytesPerPixel; rowStride = $rowStride }
    }
    # The verifier may inspect two 1920x1080 captures.  Use a 256-entry LUT
    # instead of invoking a transfer-function routine for every B/G/R byte.
    $linearLut = [double[]]::new(256)
    for ($index = 0; $index -lt 256; $index++) { $linearLut[$index] = Convert-SrgbByteToLinear $index }

    for ($y = 0; $y -lt $ExpectedHeight; $y++) {
        $enabledRow = if ($headers[0].topDown) { $y } else { $ExpectedHeight - 1 - $y }
        $disabledRow = if ($headers[1].topDown) { $y } else { $ExpectedHeight - 1 - $y }
        for ($x = 0; $x -lt $ExpectedWidth; $x++) {
            $enabledOffset = $headers[0].pixelOffset + $enabledRow * $headers[0].rowStride + $x * $headers[0].bytesPerPixel
            $disabledOffset = $headers[1].pixelOffset + $disabledRow * $headers[1].rowStride + $x * $headers[1].bytesPerPixel
            $enabledLuma = (0.2126 * $linearLut[[int]$enabledBytes[$enabledOffset + 2]]) +
                (0.7152 * $linearLut[[int]$enabledBytes[$enabledOffset + 1]]) +
                (0.0722 * $linearLut[[int]$enabledBytes[$enabledOffset]])
            $disabledLuma = (0.2126 * $linearLut[[int]$disabledBytes[$disabledOffset + 2]]) +
                (0.7152 * $linearLut[[int]$disabledBytes[$disabledOffset + 1]]) +
                (0.0722 * $linearLut[[int]$disabledBytes[$disabledOffset]])
            $delta = $enabledLuma - $disabledLuma
            foreach ($name in @('reflection', 'control')) {
                $roi = $regions[$name]['roi']
                $inside = $x -ge [int]([double]$roi.x0 * $ExpectedWidth) -and
                    $x -lt [int]([double]$roi.x1 * $ExpectedWidth) -and
                    $y -ge [int]([double]$roi.y0 * $ExpectedHeight) -and
                    $y -lt [int]([double]$roi.y1 * $ExpectedHeight)
                if ($inside) {
                    $region = $regions[$name]
                    $region['count']++
                    $region['enabledSum'] += $enabledLuma
                    $region['disabledSum'] += $disabledLuma
                    $region['absDeltaSum'] += [Math]::Abs($delta)
                    if ([Math]::Abs($delta) -ge 0.001) { $region['changedCount']++ }
                    if ($delta -gt 0) { $region['positiveCount']++ }
                    if ($delta -lt 0) { $region['negativeCount']++ }
                    $region['maxAbsDelta'] = [Math]::Max($region['maxAbsDelta'], [Math]::Abs($delta))
                }
            }
        }
    }
    $result = [ordered]@{ width = $ExpectedWidth; height = $ExpectedHeight; regions = [ordered]@{} }
    foreach ($name in @('reflection', 'control')) {
        $region = $regions[$name]
        if ($region['count'] -le 0) { throw "SSR A/B ROI '$name' contains no pixels." }
        $result.regions[$name] = [ordered]@{
            roi = $region.roi
            pixelCount = $region['count']
            enabledMeanLinear = $region['enabledSum'] / $region['count']
            disabledMeanLinear = $region['disabledSum'] / $region['count']
            meanDeltaLinear = ($region['enabledSum'] - $region['disabledSum']) / $region['count']
            meanAbsoluteDeltaLinear = $region['absDeltaSum'] / $region['count']
            changedFraction = $region['changedCount'] / [double]$region['count']
            positiveDeltaFraction = $region['positiveCount'] / [double]$region['count']
            negativeDeltaFraction = $region['negativeCount'] / [double]$region['count']
            maxAbsoluteDeltaLinear = $region['maxAbsDelta']
        }
    }
    return $result
}

function Get-ContractRoi {
    param([Parameter(Mandatory = $true)]$Contract, [Parameter(Mandatory = $true)][string]$Name)
    $roi = Get-JsonProperty -Object $Contract -Path "rois.$Name"
    if ($null -eq $roi) { throw "SSR contract is missing rois.$Name." }
    foreach ($field in @('x0', 'x1', 'y0', 'y1')) {
        $value = Convert-ToFiniteDouble (Get-ExactProperty -Object $roi -Name $field)
        if ($null -eq $value -or $value -lt 0 -or $value -gt 1) { throw "SSR ROI '$Name.$field' must be a finite normalized coordinate." }
    }
    if ([double]$roi.x0 -ge [double]$roi.x1 -or [double]$roi.y0 -ge [double]$roi.y1) { throw "SSR ROI '$Name' has invalid bounds." }
    return [ordered]@{ x0 = [double]$roi.x0; x1 = [double]$roi.x1; y0 = [double]$roi.y0; y1 = [double]$roi.y1 }
}

function Test-SsrRendererStatus {
    param(
        [Parameter(Mandatory = $true)]$Payload,
        [Parameter(Mandatory = $true)][string]$Stage,
        [Parameter(Mandatory = $true)][string]$Backend,
        [Parameter(Mandatory = $true)][bool]$ExpectedEnabled,
        [Parameter(Mandatory = $true)][int]$ExpectedWidth,
        [Parameter(Mandatory = $true)][int]$ExpectedHeight
    )
    $renderer = Get-JsonProperty $Payload 'renderer'
    $device = Get-JsonProperty $renderer 'device'
    $graph = Get-JsonProperty $renderer 'graph'
    $ssr = Get-JsonProperty $renderer 'screenSpaceReflections'
    $hiz = Get-JsonProperty $ssr 'hiZ'
    $history = Get-JsonProperty $ssr 'historyAuthority'
    $validity = Get-JsonProperty $ssr 'materialValidity'
    $settings = Get-JsonProperty $ssr 'settings'
    $fallback = Get-JsonProperty $ssr 'fallback'

    Add-Check "$Stage.renderer-schema" ($renderer.schemaVersion -eq 'noemancer.renderer-status.v27') $Stage 'Renderer Status must use noemancer.renderer-status.v27.' $renderer.schemaVersion 'noemancer.renderer-status.v27'
    Add-Check "$Stage.backend" ($device.backend -eq $Backend) $Stage "Renderer backend must be $Backend." $device.backend $Backend
    Add-Check "$Stage.capture-sidecar" ([bool]$Payload.pass -and [bool]$Payload.dimensionsMatch) $Stage 'Capture quality sidecar must pass before SSR evidence is considered.' ([ordered]@{ pass = $Payload.pass; dimensionsMatch = $Payload.dimensionsMatch }) 'pass=true, dimensionsMatch=true'
    Add-Check "$Stage.fixed-resolution" ([int]$Payload.width -eq $ExpectedWidth -and [int]$Payload.height -eq $ExpectedHeight -and [int]$renderer.surface.width -eq $ExpectedWidth -and [int]$renderer.surface.height -eq $ExpectedHeight) $Stage 'SSR capture and renderer status must both report 1920x1080.' ([ordered]@{ capture = @($Payload.width, $Payload.height); renderer = @($renderer.surface.width, $renderer.surface.height) }) @($ExpectedWidth, $ExpectedHeight)
    Add-Check "$Stage.graph-id" ($graph.graphId -eq 'render.graph.forward.v15' -and $graph.schemaVersion -eq 'noemancer.render-graph.v11' -and [bool]$graph.valid -and @($graph.errors).Count -eq 0) $Stage 'SSR must run in valid Render Graph v15 while retaining serializer schema v11.' ([ordered]@{ id = $graph.graphId; schema = $graph.schemaVersion; valid = $graph.valid; errors = @($graph.errors).Count }) 'id=v15, schema=v11, valid=true, errors=0'
    $passIds = @($graph.passes | ForEach-Object { [string]$_.id })
    $requiredPasses = @('render.pass.depth-pyramid-seed', 'render.pass.depth-pyramid-reduce', 'render.pass.ssr-hierarchical-trace', 'render.pass.ssr-temporal-resolve', 'render.pass.ssr-composite')
    $missingPasses = @($requiredPasses | Where-Object { $passIds -notcontains $_ })
    Add-Check "$Stage.graph-passes" ($missingPasses.Count -eq 0) $Stage 'Render Graph must contain real HiZ reuse and SSR ray-march/resolve/temporal passes.' $missingPasses $requiredPasses
    Add-Check "$Stage.ssr-schema" ($ssr.schema -eq 'noemancer.screen-space-reflections/0.1') $Stage 'SSR status must publish noemancer.screen-space-reflections/0.1.' $ssr.schema 'noemancer.screen-space-reflections/0.1'
    Add-Check "$Stage.enabled" ([bool]$ssr.enabled -eq $ExpectedEnabled) $Stage "SSR enabled state must be $ExpectedEnabled for this stage." $ssr.enabled $ExpectedEnabled
    $expectedQuality = if ($ExpectedEnabled) { 'high' } else { 'off' }
    Add-Check "$Stage.quality" (-not [string]::IsNullOrWhiteSpace([string]$settings.quality) -and [string]$settings.quality -eq $expectedQuality) $Stage "SSR must publish the $expectedQuality quality profile for this stage." $settings.quality $expectedQuality
    Add-Check "$Stage.hiz-reuse" ([bool]$hiz.reused -and [string]$hiz.resourceId -eq 'render.resource.scene-depth-pyramid' -and [int]$hiz.mipCount -gt 1) $Stage 'SSR must reuse the shared min/max HiZ resource and report its mip chain.' ([ordered]@{ reused = $hiz.reused; resourceId = $hiz.resourceId; mipCount = $hiz.mipCount }) 'reused=true, resourceId=render.resource.scene-depth-pyramid, mipCount>1'
    $roughness = Convert-ToFiniteDouble $settings.roughnessCutoff
    $thickness = Convert-ToFiniteDouble $settings.thickness
    $maxRayDistance = Convert-ToFiniteDouble $settings.maxRayDistance
    $maxSteps = Convert-ToFiniteDouble $settings.maxSteps
    Add-Check "$Stage.settings" ($null -ne $roughness -and $roughness -ge 0 -and $roughness -le 1 -and $null -ne $thickness -and $thickness -gt 0 -and $null -ne $maxRayDistance -and $maxRayDistance -gt 0 -and $null -ne $maxSteps -and $maxSteps -ge 1) $Stage 'SSR settings must expose bounded roughness cutoff, positive thickness/distance and max steps.' ([ordered]@{ roughnessCutoff = $roughness; thickness = $thickness; maxRayDistance = $maxRayDistance; maxSteps = $maxSteps }) 'roughnessCutoff=[0,1], thickness>0, maxRayDistance>0, maxSteps>=1'
    Add-Check "$Stage.material-validity" (-not [string]::IsNullOrWhiteSpace([string]$validity.roughnessSource) -and -not [string]::IsNullOrWhiteSpace([string]$validity.normalSource) -and -not [string]::IsNullOrWhiteSpace([string]$validity.invalidMaterialPolicy)) $Stage 'SSR must publish explicit roughness/normal sources and invalid-material fallback policy.' ([ordered]@{ roughnessSource = $validity.roughnessSource; normalSource = $validity.normalSource; invalidMaterialPolicy = $validity.invalidMaterialPolicy }) 'all material-validity fields present'
    Add-Check "$Stage.fallback" (-not [string]::IsNullOrWhiteSpace([string]$fallback.policy) -and -not [string]::IsNullOrWhiteSpace([string]$fallback.debugView)) $Stage 'SSR must publish a deterministic miss/invalid-material fallback and debug view.' ([ordered]@{ policy = $fallback.policy; debugView = $fallback.debugView }) 'policy and debugView present'
    $requiredDebugViews = @('final', 'confidence', 'hit-distance', 'roughness', 'miss')
    $debugViews = @($ssr.debugViews | ForEach-Object { [string]$_ })
    Add-Check "$Stage.debug-views" (@($requiredDebugViews | Where-Object { $debugViews -notcontains $_ }).Count -eq 0) $Stage 'SSR must expose final, mask, hit-distance, confidence and reprojection debug views.' $debugViews $requiredDebugViews
    Add-Check "$Stage.history" ($history.schema -eq 'noemancer.temporal-history/0.1' -and $history.consumer -eq 'ssr' -and [int]$history.revision -ge 1 -and $null -ne $history.currentValid -and $null -ne $history.previousValid -and $null -ne $history.lastResetReasons) $Stage 'SSR must report its independent shared History Authority slot, revision, validity and reset reasons.' ([ordered]@{ schema = $history.schema; consumer = $history.consumer; revision = $history.revision; currentValid = $history.currentValid; previousValid = $history.previousValid; lastResetReasons = $history.lastResetReasons }) 'schema=temporal-history/0.1, consumer=ssr, revision>=1, validity/reset fields present'
    Add-Check "$Stage.artifact" ($device.artifactStatus -eq 'manifest-and-artifact-verified' -and $device.shaderArtifact -in @('DXIL', 'SPIR-V')) $Stage 'SSR shader artifacts must be verified for the selected backend.' ([ordered]@{ status = $device.artifactStatus; shaderArtifact = $device.shaderArtifact }) 'manifest-and-artifact-verified, DXIL or SPIR-V'
    return [ordered]@{ schemaVersion = $renderer.schemaVersion; graphId = $graph.graphId; passIds = $passIds; enabled = [bool]$ssr.enabled; quality = $settings.quality; hiZ = $hiz; history = $history; materialValidity = $validity; fallback = $fallback; debugViews = $debugViews; shaderArtifact = $device.shaderArtifact; shaderManifestHash = $device.artifactContract.manifestHash }
}

function Get-GpuPassTimestampSummary {
    param(
        [Parameter(Mandatory = $true)]$Payload,
        [Parameter(Mandatory = $true)][string]$Stage,
        [Parameter(Mandatory = $true)][int]$MinimumSamples
    )
    Add-Check "$Stage.performance-schema" ($Payload.schemaVersion -eq 'noemancer.performance-evidence/0.1') $Stage 'Performance evidence must use noemancer.performance-evidence/0.1.' $Payload.schemaVersion 'noemancer.performance-evidence/0.1'
    Add-Check "$Stage.gpu-available" ([bool]$Payload.gpu.available -and [string]$Payload.gpu.source -ne '') $Stage 'SSR performance requires real GPU telemetry; CPU frame time cannot substitute.' ([ordered]@{ available = $Payload.gpu.available; source = $Payload.gpu.source }) 'gpu.available=true and source present'
    $timestamps = $Payload.gpu.passTimestamps
    Add-Check "$Stage.gpu-timestamp-schema" ($timestamps.schemaVersion -eq 'noemancer.gpu-pass-timestamps/0.1' -and [bool]$timestamps.supported -and $timestamps.queueScope -eq 'graphics' -and $timestamps.readback -eq 'fence-gated-frame-ring') $Stage 'GPU pass timestamps must be supported and read back from a fence-gated frame ring.' ([ordered]@{ schema = $timestamps.schemaVersion; supported = $timestamps.supported; queueScope = $timestamps.queueScope; readback = $timestamps.readback }) 'supported=true, graphics, fence-gated-frame-ring'
    $distributions = $timestamps.passDistributions
    $summary = [ordered]@{}
    foreach ($passId in @('render.pass.depth-pyramid-reduce', 'render.pass.ssr-hierarchical-trace', 'render.pass.ssr-temporal-resolve', 'render.pass.ssr-composite')) {
        $distribution = Get-ExactProperty -Object $distributions -Name $passId
        $sampleCount = Convert-ToFiniteDouble (Get-ExactProperty -Object $distribution -Name 'sampleCount')
        $p95 = Convert-ToFiniteDouble (Get-ExactProperty -Object $distribution -Name 'p95')
        $unit = [string](Get-ExactProperty -Object $distribution -Name 'unit')
        $pass = $null -ne $distribution -and $null -ne $sampleCount -and $sampleCount -ge $MinimumSamples -and $null -ne $p95 -and $p95 -ge 0 -and $unit -eq 'milliseconds'
        Add-Check "$Stage.gpu-pass-$($passId.Replace('.', '-'))" $pass $Stage "GPU timestamp distribution for $passId must have at least $MinimumSamples samples and a finite p95." ([ordered]@{ sampleCount = $sampleCount; p95 = $p95; unit = $unit }) "sampleCount>=$MinimumSamples, p95>=0 milliseconds"
        $summary[$passId] = [ordered]@{ sampleCount = $sampleCount; p95Milliseconds = $p95; unit = $unit }
    }
    return [ordered]@{ schemaVersion = $timestamps.schemaVersion; source = $Payload.gpu.source; availableFrameCount = $timestamps.availableFrameCount; distributions = $summary }
}

function Test-SsrAbResult {
    param(
        [Parameter(Mandatory = $true)]$Ab,
        [Parameter(Mandatory = $true)]$Thresholds
    )
    $reflection = $Ab.regions.reflection
    $control = $Ab.regions.control
    $reflectionAbsoluteMin = [double]$Thresholds.reflectionMeanAbsoluteDeltaMin
    $reflectionChangedMin = [double]$Thresholds.reflectionChangedFractionMin
    $controlMax = [double]$Thresholds.controlMeanAbsoluteDeltaMax
    $reflectionPass = [double]$reflection.meanAbsoluteDeltaLinear -ge $reflectionAbsoluteMin -and [double]$reflection.changedFraction -ge $reflectionChangedMin
    $controlPass = [double]$control.meanAbsoluteDeltaLinear -le $controlMax
    Add-Check 'ab.reflection-material' $reflectionPass 'ab' 'SSR enabled versus disabled must produce a material, direction-independent luminance change in the reflection ROI.' ([ordered]@{ meanAbsoluteDeltaLinear = $reflection.meanAbsoluteDeltaLinear; changedFraction = $reflection.changedFraction }) ([ordered]@{ meanAbsoluteDeltaLinearMin = $reflectionAbsoluteMin; changedFractionMin = $reflectionChangedMin })
    Add-Check 'ab.control-stable' $controlPass 'ab' 'SSR enabled-minus-disabled must not materially change the control ROI.' ([ordered]@{ meanAbsoluteDeltaLinear = $control.meanAbsoluteDeltaLinear; maxAbsoluteDeltaLinear = $control.maxAbsoluteDeltaLinear }) ([ordered]@{ meanAbsoluteDeltaLinearMax = $controlMax })
    return [ordered]@{ reflection = $reflection; control = $control; thresholds = [ordered]@{ reflectionMeanAbsoluteDeltaMin = $reflectionAbsoluteMin; reflectionChangedFractionMin = $reflectionChangedMin; controlMeanAbsoluteDeltaMax = $controlMax }; pass = $reflectionPass -and $controlPass }
}

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$runtime = Join-Path $repositoryRoot "build/windows-msvc-debug/src/runtime/$Config/noemancer.exe"
$engineScript = Join-Path $repositoryRoot 'scripts/engine.ps1'
$expectedWidth = 1920
$expectedHeight = 1080
$expectedArtifacts = @{ direct3d12 = 'DXIL'; vulkan = 'SPIR-V' }

try {
    $ProjectPath = [IO.Path]::GetFullPath($ProjectPath)
    if ([string]::IsNullOrWhiteSpace($ContractPath)) { $ContractPath = Join-Path $ProjectPath 'ssr-evidence.contract.json' }
    $ContractPath = [IO.Path]::GetFullPath($ContractPath)
    if ([string]::IsNullOrWhiteSpace($OutputRoot)) { $OutputRoot = Join-Path $repositoryRoot "generated/acceptance/ssr-v15-$(Get-Date -Format yyyyMMdd-HHmmss)" }
    $OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
    if (Test-Path -LiteralPath $OutputRoot) {
        if (Get-ChildItem -LiteralPath $OutputRoot -Force | Select-Object -First 1) { throw "SSR evidence output must be empty because receipts are immutable: $OutputRoot" }
    } else { New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null }
    $script:EvidenceRoot = $OutputRoot
    $script:ReceiptPath = Join-Path $OutputRoot 'ssr-evidence.json'

    $contract = Read-JsonFile $ContractPath
    Add-Check 'static.contract' ($null -ne $contract -and $contract.schemaVersion -eq 'noemancer.ssr-evidence-contract/0.1') 'static' 'SSR evidence contract must be present and versioned.' (Get-JsonProperty $contract 'schemaVersion') 'noemancer.ssr-evidence-contract/0.1'
    if ($null -eq $contract) { throw "SSR evidence contract is missing: $ContractPath" }
    $contractWidth = [int](Get-JsonProperty $contract 'surface.width')
    $contractHeight = [int](Get-JsonProperty $contract 'surface.height')
    Add-Check 'static.resolution' ($contractWidth -eq $expectedWidth -and $contractHeight -eq $expectedHeight) 'static' 'SSR contract must pin 1920x1080.' @($contractWidth, $contractHeight) @($expectedWidth, $expectedHeight)
    $reflectionRoi = Get-ContractRoi $contract 'reflection'
    $controlRoi = Get-ContractRoi $contract 'control'
    $thresholds = Get-JsonProperty $contract 'thresholds'
    foreach ($field in @('reflectionMeanAbsoluteDeltaMin', 'reflectionChangedFractionMin', 'controlMeanAbsoluteDeltaMax')) {
        if ($null -eq (Convert-ToFiniteDouble (Get-ExactProperty $thresholds $field))) { throw "SSR contract threshold is missing or non-finite: thresholds.$field" }
    }
    Add-Check 'static.ab-intent' ((Get-JsonProperty $contract 'abIntent.delta') -eq 'absolute-enabled-minus-disabled-linear-luma') 'static' 'SSR A/B contract must define direction-independent absolute linear-luma semantics.' (Get-JsonProperty $contract 'abIntent.delta') 'absolute-enabled-minus-disabled-linear-luma'
    Add-Check 'static.project' (Test-Path -LiteralPath $ProjectPath -PathType Container) 'static' 'SSR project directory must exist.' $ProjectPath 'directory exists'
    Add-Check 'static.engine-script' (Test-Path -LiteralPath $engineScript -PathType Leaf) 'static' 'scripts/engine.ps1 must exist for reproducible builds.' $engineScript 'file exists'

    $buildRun = $null
    if (-not (Test-Path -LiteralPath $runtime -PathType Leaf)) {
        $pwsh = (Get-Command pwsh -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
        $buildRun = Invoke-HiddenProcess $pwsh @('-NoLogo', '-NoProfile', '-File', $engineScript, 'build', '-Config', $Config, '-Target', 'noemancer') $repositoryRoot (Join-Path $OutputRoot 'build.stdout.log') (Join-Path $OutputRoot 'build.stderr.log') $TimeoutSeconds
        Add-Check 'build.process' ($buildRun.completed -and $buildRun.exitCode -eq 0) 'build' 'Hidden fallback Noemancer build must complete successfully.' $buildRun 'completed=true, exitCode=0'
    }
    Add-Check 'build.runtime' (Test-Path -LiteralPath $runtime -PathType Leaf) 'build' 'Selected Noemancer runtime must exist before SSR capture.' $runtime 'file exists'
    if (-not (Test-Path -LiteralPath $runtime -PathType Leaf)) { throw "Runtime is missing: $runtime" }

    $runs = [System.Collections.Generic.List[object]]::new()
    foreach ($backend in @($GpuBackends | Select-Object -Unique)) {
        $backendRoot = Join-Path $OutputRoot $backend
        New-Item -ItemType Directory -Path $backendRoot -Force | Out-Null
        $enabledImage = Join-Path $backendRoot 'ssr-enabled-1920x1080.bmp'
        $disabledImage = Join-Path $backendRoot 'ssr-disabled-1920x1080.bmp'
        $enabledSidecar = "$enabledImage.quality.json"
        $disabledSidecar = "$disabledImage.quality.json"
        $performancePath = Join-Path $backendRoot 'ssr-enabled.performance.json'
        $enabledOut = Join-Path $backendRoot 'enabled.stdout.jsonl'; $enabledErr = Join-Path $backendRoot 'enabled.stderr.log'
        $disabledOut = Join-Path $backendRoot 'disabled.stdout.jsonl'; $disabledErr = Join-Path $backendRoot 'disabled.stderr.log'
        $performanceOut = Join-Path $backendRoot 'performance.stdout.jsonl'; $performanceErr = Join-Path $backendRoot 'performance.stderr.log'
        $common = @('run', '--format', 'json', '--project', $ProjectPath, '--window-width', [string]$expectedWidth, '--window-height', [string]$expectedHeight, '--exposure', '1.0', '--render-scale', '1.0', '--disable-auto-exposure', '--gpu-backend', $backend, '--ssr-quality', 'high')
        $enabledArgs = @($common) + @('--frames', [string]$CaptureFrames, '--capture-frame', $enabledImage)
        $disabledArgs = @($common) + @('--frames', [string]$CaptureFrames, '--disable-ssr', '--capture-frame', $disabledImage)
        $performanceArgs = @($common) + @('--performance-evidence', $performancePath, '--performance-hidden', '--performance-workload', 'screen-space-reflections-v15', '--performance-warmup-frames', [string]$PerformanceWarmupFrames, '--performance-sample-frames', [string]$PerformanceSampleFrames)
        $enabledProcess = Invoke-HiddenProcess $runtime $enabledArgs $ProjectPath $enabledOut $enabledErr $TimeoutSeconds
        $disabledProcess = Invoke-HiddenProcess $runtime $disabledArgs $ProjectPath $disabledOut $disabledErr $TimeoutSeconds
        $performanceProcess = Invoke-HiddenProcess $runtime $performanceArgs $ProjectPath $performanceOut $performanceErr $TimeoutSeconds
        Add-Check "$backend.enabled-process" ($enabledProcess.completed -and $enabledProcess.exitCode -eq 0) "$backend/capture" 'Hidden SSR-enabled capture must exit successfully.' $enabledProcess 'completed=true, exitCode=0'
        Add-Check "$backend.disabled-process" ($disabledProcess.completed -and $disabledProcess.exitCode -eq 0) "$backend/capture" 'Hidden SSR-disabled capture must exit successfully.' $disabledProcess 'completed=true, exitCode=0'
        Add-Check "$backend.performance-process" ($performanceProcess.completed -and $performanceProcess.exitCode -eq 0) "$backend/performance" 'Hidden SSR GPU performance run must exit successfully.' $performanceProcess 'completed=true, exitCode=0'
        foreach ($path in @($enabledImage, $disabledImage, $enabledSidecar, $disabledSidecar, $performancePath)) { Add-Check "$backend.artifact-$([IO.Path]::GetFileName($path))" (Test-Path -LiteralPath $path -PathType Leaf) "$backend/artifacts" "Required SSR evidence artifact must exist: $([IO.Path]::GetFileName($path))." $path 'file exists' }

        $enabledStatus = $null; $disabledStatus = $null; $gpuSummary = $null; $abSummary = $null
        if ((Test-Path -LiteralPath $enabledSidecar -PathType Leaf) -and (Test-Path -LiteralPath $disabledSidecar -PathType Leaf)) {
            try {
                $enabledStatus = Test-SsrRendererStatus (Read-JsonFile $enabledSidecar) "$backend/enabled" $backend $true $expectedWidth $expectedHeight
                $disabledStatus = Test-SsrRendererStatus (Read-JsonFile $disabledSidecar) "$backend/disabled" $backend $false $expectedWidth $expectedHeight
                $ab = Get-BmpLumaAB $enabledImage $disabledImage $expectedWidth $expectedHeight $reflectionRoi $controlRoi
                $abSummary = Test-SsrAbResult $ab $thresholds
            } catch { Add-Issue "$backend.capture-validation" "$backend/capture" $_.Exception.Message ([ordered]@{ enabled = $enabledSidecar; disabled = $disabledSidecar }) 'parseable v27 SSR sidecars and 1920x1080 BMP A/B'
            }
        }
        if (Test-Path -LiteralPath $performancePath -PathType Leaf) {
            try { $gpuSummary = Get-GpuPassTimestampSummary (Read-JsonFile $performancePath) "$backend/performance" $PerformanceSampleFrames } catch { Add-Issue "$backend.performance-validation" "$backend/performance" $_.Exception.Message $performancePath 'parseable GPU pass timestamp evidence' }
        }
        $artifactRecords = @($enabledImage, $disabledImage, $enabledSidecar, $disabledSidecar, $performancePath) |
            ForEach-Object { [ordered]@{ path = [IO.Path]::GetRelativePath($backendRoot, $_).Replace('\', '/'); exists = Test-Path -LiteralPath $_ -PathType Leaf; sha256 = Get-FileSha256 $_ } }
        $runs.Add([ordered]@{
            backend = $backend
            shaderArtifact = $expectedArtifacts[$backend]
            commands = @(
                [ordered]@{ purpose = 'ssr-enabled-hidden-capture'; executable = $runtime; arguments = $enabledArgs; process = $enabledProcess }
                [ordered]@{ purpose = 'ssr-disabled-hidden-capture'; executable = $runtime; arguments = $disabledArgs; process = $disabledProcess }
                [ordered]@{ purpose = 'ssr-enabled-hidden-gpu-performance'; executable = $runtime; arguments = $performanceArgs; process = $performanceProcess }
            )
            artifacts = $artifactRecords
            enabled = $enabledStatus
            disabled = $disabledStatus
            ab = $abSummary
            gpu = $gpuSummary
        })
    }

    $manifest = [ordered]@{
        schemaVersion = 'noemancer.ssr-capture-evidence/0.1'
        capturedAt = [DateTimeOffset]::UtcNow.ToString('o')
        pass = ($script:Issues.Count -eq 0)
        configuration = [ordered]@{ config = $Config; project = $ProjectPath; contract = $ContractPath; requestedResolution = [ordered]@{ width = $expectedWidth; height = $expectedHeight }; captureFrames = $CaptureFrames; performanceWarmupFrames = $PerformanceWarmupFrames; performanceSampleFrames = $PerformanceSampleFrames; timeoutSeconds = $TimeoutSeconds; hiddenProcess = $true; computerUse = $false; gpuBackends = @($GpuBackends | Select-Object -Unique) }
        contract = [ordered]@{ schemaVersion = $contract.schemaVersion; sha256 = Get-FileSha256 $ContractPath; reflectionRoi = $reflectionRoi; controlRoi = $controlRoi; thresholds = $thresholds; abIntent = $contract.abIntent }
        runtime = [ordered]@{ path = $runtime.Replace('\', '/'); sha256 = Get-FileSha256 $runtime }
        build = $buildRun
        runs = @($runs)
        checks = @($script:Checks)
        issues = @($script:Issues)
        policy = 'SSR acceptance requires real v27/v15 status, real HiZ/history/material-validity/fallback/debug contracts, material direction-independent reflection ROI A/B, stable control ROI, and GPU pass timestamps. CPU frame time is diagnostic only and cannot satisfy GPU timing.'
    }
    Write-JsonDocument $script:ReceiptPath $manifest
    Write-Output ($manifest | ConvertTo-Json -Depth 100 -Compress)
    if (-not [bool]$manifest.pass) { exit 5 }
} catch {
    if ($null -ne $script:ReceiptPath) {
        try { Write-JsonDocument $script:ReceiptPath ([ordered]@{ schemaVersion = 'noemancer.ssr-capture-evidence/0.1'; capturedAt = [DateTimeOffset]::UtcNow.ToString('o'); pass = $false; error = $_.Exception.Message; checks = @($script:Checks); issues = @($script:Issues) }) } catch { }
    }
    Write-Error "SSR evidence failed: $($_.Exception.Message)"
    exit 1
}
