[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Release',
    [ValidateSet('direct3d12', 'vulkan')]
    [string]$GpuBackend = 'direct3d12',
    [string]$ProjectPath = 'D:\3D\NoemancerProjects\NoemancerRenderLab',
    [string]$OutputRoot,
    [ValidateRange(8, 600)]
    [int]$CaptureFrames = 64,
    [ValidateRange(0, 10000)]
    [int]$PerformanceWarmupFrames = 32,
    [ValidateRange(60, 10000)]
    [int]$PerformanceSampleFrames = 60,
    [ValidateRange(30, 900)]
    [int]$TimeoutSeconds = 180,
    [switch]$SkipPerformance
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:Checks = [System.Collections.Generic.List[object]]::new()
$script:Issues = [System.Collections.Generic.List[object]]::new()

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
    if (-not $Pass) { Add-Issue -Code $Code -Stage $Stage -Message $Message -Observed $Observed -Expected $Expected }
}

function Write-JsonDocument {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)]$Value
    )
    $parent = Split-Path -Parent $Path
    if (-not [string]::IsNullOrWhiteSpace($parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
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
    } catch { return $null }
}

function Test-Vector {
    param(
        [AllowNull()][object]$Actual,
        [Parameter(Mandatory = $true)][int[]]$Expected
    )
    $values = @($Actual)
    if ($values.Count -ne $Expected.Count) { return $false }
    for ($index = 0; $index -lt $Expected.Count; $index++) {
        if ([int]$values[$index] -ne $Expected[$index]) { return $false }
    }
    return $true
}

function Invoke-HiddenRuntime {
    param(
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$StdoutPath,
        [Parameter(Mandatory = $true)][string]$StderrPath,
        [Parameter(Mandatory = $true)][int]$TimeoutSeconds
    )
    $process = Start-Process -FilePath $Executable -ArgumentList $Arguments -WindowStyle Hidden `
        -RedirectStandardOutput $StdoutPath -RedirectStandardError $StderrPath -PassThru
    $completed = $process.WaitForExit($TimeoutSeconds * 1000)
    if (-not $completed) {
        try { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue } catch { }
        return [ordered]@{ started = $true; completed = $false; timedOut = $true; exitCode = $null; processId = $process.Id }
    }
    return [ordered]@{ started = $true; completed = $true; timedOut = $false; exitCode = $process.ExitCode; processId = $process.Id }
}

function Test-AtmosphereStatus {
    param(
        [Parameter(Mandatory = $true)]$Payload,
        [Parameter(Mandatory = $true)][string]$Stage,
        [Parameter(Mandatory = $true)][string]$ExpectedArtifact,
        [switch]$RequireDimensions,
        [int]$ExpectedWidth = 1920,
        [int]$ExpectedHeight = 1080,
        [int]$MaxLutRegenerations = 4,
        [int]$MaxCameraVolumeRegenerations = 4
    )
    $renderer = Get-JsonProperty -Object $Payload -Path 'renderer'
    $graph = Get-JsonProperty -Object $renderer -Path 'graph'
    $sky = Get-JsonProperty -Object $renderer -Path 'skyAtmosphere'
    $device = Get-JsonProperty -Object $renderer -Path 'device'
    $artifact = Get-JsonProperty -Object $device -Path 'artifactContract'
    $stageName = $Stage

    Add-Check -Code "$Stage.renderer-schema" -Stage $stageName -Pass ((Get-JsonProperty $renderer 'schemaVersion') -eq 'noemancer.renderer-status.v25') `
        -Message 'Renderer status schema must be noemancer.renderer-status.v25.' -Observed (Get-JsonProperty $renderer 'schemaVersion') -Expected 'noemancer.renderer-status.v25'
    Add-Check -Code "$Stage.renderer-backend" -Stage $stageName -Pass ((Get-JsonProperty $device 'backend') -eq $GpuBackend) `
        -Message "Renderer backend must be $GpuBackend." -Observed (Get-JsonProperty $device 'backend') -Expected $GpuBackend
    Add-Check -Code "$Stage.shader-artifact" -Stage $stageName -Pass ((Get-JsonProperty $device 'shaderArtifact') -eq $ExpectedArtifact) `
        -Message "Shader artifact must be $ExpectedArtifact for $GpuBackend." -Observed (Get-JsonProperty $device 'shaderArtifact') -Expected $ExpectedArtifact
    Add-Check -Code "$Stage.shader-manifest" -Stage $stageName -Pass ((Get-JsonProperty $artifact 'code') -eq 'ok' -and (Get-JsonProperty $artifact 'schema') -eq 'noemancer.shader-artifact-manifest/0.1' -and [string](Get-JsonProperty $artifact 'manifestHash') -match '^sha256:[0-9a-fA-F]{64}$') `
        -Message 'Shader artifact manifest must be verified with a stable SHA-256 identity.' -Observed $artifact -Expected 'code=ok, schema=noemancer.shader-artifact-manifest/0.1, manifestHash=sha256:64hex'
    Add-Check -Code "$Stage.graph-schema" -Stage $stageName -Pass ((Get-JsonProperty $graph 'schemaVersion') -eq 'noemancer.render-graph.v11') `
        -Message 'Render Graph schema must be noemancer.render-graph.v11.' -Observed (Get-JsonProperty $graph 'schemaVersion') -Expected 'noemancer.render-graph.v11'
    Add-Check -Code "$Stage.graph-id" -Stage $stageName -Pass ((Get-JsonProperty $graph 'graphId') -eq 'render.graph.forward.v13') `
        -Message 'Render Graph must be render.graph.forward.v13.' -Observed (Get-JsonProperty $graph 'graphId') -Expected 'render.graph.forward.v13'
    Add-Check -Code "$Stage.graph-valid" -Stage $stageName -Pass ([bool](Get-JsonProperty $graph 'valid')) `
        -Message 'Render Graph must report valid=true.' -Observed (Get-JsonProperty $graph 'valid') -Expected $true

    $passes = @(Get-JsonProperty -Object $graph -Path 'passes')
    $passIds = @($passes | ForEach-Object { [string](Get-JsonProperty -Object $_ -Path 'id') })
    $aerial = $passes | Where-Object { [string](Get-JsonProperty -Object $_ -Path 'id') -eq 'render.pass.aerial-perspective' } | Select-Object -First 1
    Add-Check -Code "$Stage.graph-aerial-pass" -Stage $stageName -Pass ($null -ne $aerial -and (Get-JsonProperty $aerial 'pipelineId') -eq 'render.pipeline.aerial-perspective' -and $passIds -contains 'render.pass.sky-atmosphere') `
        -Message 'Render Graph must contain sky-atmosphere and an aerial-perspective pass.' -Observed $passIds -Expected @('render.pass.sky-atmosphere', 'render.pass.aerial-perspective')

    Add-Check -Code "$Stage.sky-path" -Stage $stageName -Pass ((Get-JsonProperty $sky 'path') -eq 'transmittance/multi-scattering/sky-view/camera-volume') `
        -Message 'Sky path must include all four LUT stages and camera volume.' -Observed (Get-JsonProperty $sky 'path') -Expected 'transmittance/multi-scattering/sky-view/camera-volume'
    Add-Check -Code "$Stage.sky-ready" -Stage $stageName -Pass ([bool](Get-JsonProperty $sky 'pipelineCreated') -and [bool](Get-JsonProperty $sky 'lutPipelinesCreated') -and [bool](Get-JsonProperty $sky 'lutPathReady') -and [bool](Get-JsonProperty $sky 'aerialPerspectiveReady') -and [bool](Get-JsonProperty $sky.contract 'enabled')) `
        -Message 'Sky LUT pipelines, LUT path, camera-volume aerial path and authored enable state must be ready.' -Observed ([ordered]@{ pipelineCreated = Get-JsonProperty $sky 'pipelineCreated'; lutPipelinesCreated = Get-JsonProperty $sky 'lutPipelinesCreated'; lutPathReady = Get-JsonProperty $sky 'lutPathReady'; aerialPerspectiveReady = Get-JsonProperty $sky 'aerialPerspectiveReady'; enabled = Get-JsonProperty $sky.contract 'enabled' }) -Expected 'all true'
    Add-Check -Code "$Stage.sky-schema" -Stage $stageName -Pass ((Get-JsonProperty $sky.contract 'schema') -eq 'noemancer.sky-atmosphere/0.1') `
        -Message 'Sky settings must expose the published atmosphere schema.' -Observed (Get-JsonProperty $sky.contract 'schema') -Expected 'noemancer.sky-atmosphere/0.1'
    Add-Check -Code "$Stage.sky-lut-size" -Stage $stageName -Pass (Test-Vector -Actual (Get-JsonProperty $sky 'lutSize') -Expected @(192, 108)) `
        -Message 'Sky-view LUT must use the high-quality 192x108 budget.' -Observed (Get-JsonProperty $sky 'lutSize') -Expected @(192, 108)
    Add-Check -Code "$Stage.sky-camera-volume-size" -Stage $stageName -Pass (Test-Vector -Actual (Get-JsonProperty $sky 'cameraVolumeSize') -Expected @(32, 32, 32)) `
        -Message 'Camera volume must use the bounded 32x32x32 contract.' -Observed (Get-JsonProperty $sky 'cameraVolumeSize') -Expected @(32, 32, 32)
    $lutRegens = Convert-ToFiniteDouble (Get-JsonProperty $sky 'lutRegenerations')
    $volumeRegens = Convert-ToFiniteDouble (Get-JsonProperty $sky 'cameraVolumeRegenerations')
    Add-Check -Code "$Stage.sky-lut-regenerations" -Stage $stageName -Pass ($null -ne $lutRegens -and $lutRegens -ge 1 -and $lutRegens -le $MaxLutRegenerations) `
        -Message "Static LUT regeneration count must be in [1,$MaxLutRegenerations]." -Observed $lutRegens -Expected "1..$MaxLutRegenerations"
    Add-Check -Code "$Stage.sky-camera-volume-regenerations" -Stage $stageName -Pass ($null -ne $volumeRegens -and $volumeRegens -ge 1 -and $volumeRegens -le $MaxCameraVolumeRegenerations) `
        -Message "Camera-volume regeneration count must be in [1,$MaxCameraVolumeRegenerations]." -Observed $volumeRegens -Expected "1..$MaxCameraVolumeRegenerations"

    if ($RequireDimensions) {
        $width = Convert-ToFiniteDouble (Get-JsonProperty $Payload 'width')
        $height = Convert-ToFiniteDouble (Get-JsonProperty $Payload 'height')
        Add-Check -Code "$Stage.fixed-resolution" -Stage $stageName -Pass ($width -eq $ExpectedWidth -and $height -eq $ExpectedHeight) `
            -Message "Fixed capture must be ${ExpectedWidth}x${ExpectedHeight}." -Observed ([ordered]@{ width = $width; height = $height }) -Expected ([ordered]@{ width = $ExpectedWidth; height = $ExpectedHeight })
    }
    return [ordered]@{
        rendererSchema = Get-JsonProperty $renderer 'schemaVersion'
        graphId = Get-JsonProperty $graph 'graphId'
        graphSchema = Get-JsonProperty $graph 'schemaVersion'
        skyPath = Get-JsonProperty $sky 'path'
        lutRegenerations = $lutRegens
        cameraVolumeRegenerations = $volumeRegens
        lutSize = Get-JsonProperty $sky 'lutSize'
        cameraVolumeSize = Get-JsonProperty $sky 'cameraVolumeSize'
        shaderArtifact = Get-JsonProperty $device 'shaderArtifact'
        manifestHash = Get-JsonProperty $artifact 'manifestHash'
    }
}

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$runtime = Join-Path $repositoryRoot "build/windows-msvc-debug/src/runtime/$Config/noemancer.exe"
$analyzer = Join-Path $PSScriptRoot 'analyze-sky-atmosphere-quality.ps1'
$pwshCommand = Get-Command pwsh -CommandType Application -ErrorAction Stop | Select-Object -First 1
$pwsh = [string]$pwshCommand.Source
if ([string]::IsNullOrWhiteSpace($pwsh)) { $pwsh = [string]$pwshCommand.Path }
$expectedArtifact = if ($GpuBackend -eq 'direct3d12') { 'DXIL' } else { 'SPIR-V' }
$expectedWidth = 1920
$expectedHeight = 1080

if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $OutputRoot = Join-Path $repositoryRoot "generated/acceptance/sky-atmosphere-$GpuBackend-$stamp"
}
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null
if (Get-ChildItem -LiteralPath $OutputRoot -Force | Select-Object -First 1) {
    throw "Sky atmosphere evidence output must be empty because receipts are immutable: $OutputRoot"
}
$ProjectPath = [IO.Path]::GetFullPath($ProjectPath)
if (-not (Test-Path -LiteralPath $runtime -PathType Leaf)) { throw "Noemancer $Config runtime is missing: $runtime" }
if (-not (Test-Path -LiteralPath $analyzer -PathType Leaf)) { throw "Sky atmosphere analyzer is missing: $analyzer" }
if (-not (Test-Path -LiteralPath $ProjectPath -PathType Container) -and -not (Test-Path -LiteralPath $ProjectPath -PathType Leaf)) { throw "Sky atmosphere project is missing: $ProjectPath" }
$manifest = if (Test-Path -LiteralPath $ProjectPath -PathType Container) { Join-Path $ProjectPath 'noemancer.project.json' } else { $ProjectPath }
if (-not (Test-Path -LiteralPath $manifest -PathType Leaf)) { throw "Sky atmosphere project manifest is missing: $manifest" }

$image = Join-Path $OutputRoot 'sky-atmosphere-1920x1080.bmp'
$sidecar = "$image.quality.json"
$qualityEvidence = Join-Path $OutputRoot 'sky-atmosphere-quality.json'
$captureStdout = Join-Path $OutputRoot 'runtime.capture.stdout.jsonl'
$captureStderr = Join-Path $OutputRoot 'runtime.capture.stderr.log'
$performance = Join-Path $OutputRoot 'performance.json'
$performanceStdout = Join-Path $OutputRoot 'runtime.performance.stdout.jsonl'
$performanceStderr = Join-Path $OutputRoot 'runtime.performance.stderr.log'
$receiptPath = Join-Path $OutputRoot 'sky-atmosphere-evidence.json'
$captureArguments = @(
    'run', '--format', 'json', '--project', $ProjectPath,
    '--capture-frame', $image, '--frames', [string]$CaptureFrames,
    '--window-width', [string]$expectedWidth, '--window-height', [string]$expectedHeight,
    '--exposure', '1.0', '--render-scale', '1.0', '--gpu-backend', $GpuBackend
)
$performanceArguments = @(
    'run', '--format', 'json', '--project', $ProjectPath,
    '--performance-evidence', $performance, '--performance-hidden',
    '--performance-workload', 'sky-aerial-v13',
    '--performance-warmup-frames', [string]$PerformanceWarmupFrames,
    '--performance-sample-frames', [string]$PerformanceSampleFrames,
    '--window-width', [string]$expectedWidth, '--window-height', [string]$expectedHeight,
    '--exposure', '1.0', '--render-scale', '1.0', '--gpu-backend', $GpuBackend
)

$captureRun = $null
$performanceRun = $null
$captureStatus = $null
$performanceStatus = $null
$quality = $null
$performanceInfo = $null
$sourceHash = Get-FileSha256 -Path $manifest

try {
    $captureRun = Invoke-HiddenRuntime -Executable $runtime -Arguments $captureArguments -StdoutPath $captureStdout -StderrPath $captureStderr -TimeoutSeconds $TimeoutSeconds
    Add-Check -Code 'capture.process' -Stage 'capture' -Pass ([bool]$captureRun.completed -and [int]$captureRun.exitCode -eq 0) `
        -Message 'Hidden fixed-resolution capture must exit successfully.' -Observed $captureRun -Expected 'completed=true, exitCode=0'
    if (Test-Path -LiteralPath $sidecar -PathType Leaf) {
        try {
            $captureSidecar = Read-JsonFile -Path $sidecar
            Add-Check -Code 'capture.sidecar-schema' -Stage 'capture' -Pass ((Get-JsonProperty $captureSidecar 'schemaVersion') -eq 'noemancer.render-quality.v1') `
                -Message 'Capture quality sidecar must use noemancer.render-quality.v1.' -Observed (Get-JsonProperty $captureSidecar 'schemaVersion') -Expected 'noemancer.render-quality.v1'
            Add-Check -Code 'capture.sidecar-pass' -Stage 'capture' -Pass ([bool](Get-JsonProperty $captureSidecar 'pass') -and [bool](Get-JsonProperty $captureSidecar 'dimensionsMatch')) `
                -Message 'Runtime capture sidecar must pass its own dimension/quality contract.' -Observed ([ordered]@{ pass = Get-JsonProperty $captureSidecar 'pass'; dimensionsMatch = Get-JsonProperty $captureSidecar 'dimensionsMatch' }) -Expected 'pass=true, dimensionsMatch=true'
            $captureStatus = Test-AtmosphereStatus -Payload $captureSidecar -Stage 'capture' -ExpectedArtifact $expectedArtifact -RequireDimensions -ExpectedWidth $expectedWidth -ExpectedHeight $expectedHeight
        } catch {
            Add-Issue -Code 'capture.sidecar-invalid' -Stage 'capture' -Message $_.Exception.Message -Observed $sidecar -Expected 'parseable renderer status'
        }
    } else {
        Add-Issue -Code 'capture.sidecar-missing' -Stage 'capture' -Message 'Runtime did not produce the capture quality sidecar.' -Observed $sidecar -Expected 'file exists'
    }
    if (-not (Test-Path -LiteralPath $image -PathType Leaf)) {
        Add-Issue -Code 'capture.image-missing' -Stage 'capture' -Message 'Runtime did not produce the fixed-resolution BMP.' -Observed $image -Expected 'file exists'
    } elseif (Test-Path -LiteralPath $analyzer -PathType Leaf) {
        try {
            & $pwsh -NoLogo -NoProfile -File $analyzer `
                -ImagePath $image -OutputPath $qualityEvidence -ExpectedWidth $expectedWidth -ExpectedHeight $expectedHeight | Out-Null
            $quality = Read-JsonFile -Path $qualityEvidence
            Add-Check -Code 'capture.image-quality' -Stage 'quality' -Pass ([bool](Get-JsonProperty $quality 'pass')) `
                -Message 'Sky image must pass non-black, gradient, colour-separation and horizon-continuity checks.' -Observed $qualityEvidence -Expected 'pass=true'
        } catch {
            Add-Issue -Code 'capture.image-quality-failed' -Stage 'quality' -Message $_.Exception.Message -Observed $qualityEvidence -Expected 'analyzer exitCode=0'
        }
    }
} catch {
    Add-Issue -Code 'capture.process-launch-failed' -Stage 'capture' -Message $_.Exception.Message -Observed $runtime -Expected 'hidden process launch'
}

if (-not $SkipPerformance) {
    try {
        $performanceRun = Invoke-HiddenRuntime -Executable $runtime -Arguments $performanceArguments -StdoutPath $performanceStdout -StderrPath $performanceStderr -TimeoutSeconds $TimeoutSeconds
        Add-Check -Code 'performance.process' -Stage 'performance' -Pass ([bool]$performanceRun.completed -and [int]$performanceRun.exitCode -eq 0) `
            -Message 'Hidden GPU performance run must exit successfully.' -Observed $performanceRun -Expected 'completed=true, exitCode=0'
        if (-not (Test-Path -LiteralPath $performance -PathType Leaf)) {
            Add-Issue -Code 'performance.evidence-missing' -Stage 'performance' -Message 'Runtime did not produce performance JSON.' -Observed $performance -Expected 'file exists'
        } else {
            try {
                $performanceDocument = Read-JsonFile -Path $performance
                Add-Check -Code 'performance.schema' -Stage 'performance' -Pass ((Get-JsonProperty $performanceDocument 'schemaVersion') -eq 'noemancer.performance-evidence/0.1') `
                    -Message 'Performance evidence must use noemancer.performance-evidence/0.1.' -Observed (Get-JsonProperty $performanceDocument 'schemaVersion') -Expected 'noemancer.performance-evidence/0.1'
                Add-Check -Code 'performance.workload' -Stage 'performance' -Pass ((Get-JsonProperty $performanceDocument 'workload.id') -eq 'sky-aerial-v13') `
                    -Message 'Performance evidence must identify the sky-aerial-v13 workload.' -Observed (Get-JsonProperty $performanceDocument 'workload.id') -Expected 'sky-aerial-v13'
                $timestamp = Get-JsonProperty $performanceDocument 'gpu.passTimestamps'
                Add-Check -Code 'performance.gpu-timestamps' -Stage 'performance' -Pass ([bool](Get-JsonProperty $timestamp 'availableFrameCount') -and [bool](Get-JsonProperty $timestamp 'supported') -and (Get-JsonProperty $timestamp 'reason') -eq 'ok' -and [int](Get-JsonProperty $timestamp 'availableFrameCount') -ge $PerformanceSampleFrames) `
                    -Message 'GPU pass timestamps must be supported and available for every sampled frame.' -Observed ([ordered]@{ availableFrameCount = Get-JsonProperty $timestamp 'availableFrameCount'; supported = Get-JsonProperty $timestamp 'supported'; reason = Get-JsonProperty $timestamp 'reason' }) -Expected "availableFrameCount>=$PerformanceSampleFrames, supported=true, reason=ok"
                $performanceStatus = Test-AtmosphereStatus -Payload $performanceDocument -Stage 'performance' -ExpectedArtifact $expectedArtifact
                $measuredWidth = [int](Get-JsonProperty $performanceDocument 'workload.resolution.width')
                $measuredHeight = [int](Get-JsonProperty $performanceDocument 'workload.resolution.height')
                $resolutionMatches = $measuredWidth -eq $expectedWidth -and $measuredHeight -eq $expectedHeight
                # This is intentionally a hard check. Current Runtime performance mode
                # reports the Editor Scene viewport rather than the requested window;
                # do not silently call that a fixed 1080p performance result.
                Add-Check -Code 'performance.fixed-resolution' -Stage 'performance' -Pass $resolutionMatches `
                    -Message "GPU performance evidence must report the requested ${expectedWidth}x${expectedHeight} resolution; Runtime-reported viewport dimensions are not substituted." `
                    -Observed ([ordered]@{ width = $measuredWidth; height = $measuredHeight }) -Expected ([ordered]@{ width = $expectedWidth; height = $expectedHeight })
                $distributions = Get-JsonProperty $performanceDocument 'gpu.passTimestamps.passDistributions'
                foreach ($passId in @('render.pass.sky-atmosphere', 'render.pass.aerial-perspective')) {
                    $distribution = Get-ExactProperty -Object $distributions -Name $passId
                    $p95 = Convert-ToFiniteDouble (Get-JsonProperty $distribution 'p95')
                    Add-Check -Code ("performance." + $passId.Replace('.', '-')) -Stage 'performance' -Pass ($null -ne $distribution -and $p95 -ge 0.0 -and (Get-JsonProperty $distribution 'unit') -eq 'milliseconds' -and [int](Get-JsonProperty $distribution 'sampleCount') -ge $PerformanceSampleFrames) `
                        -Message "$passId must have finite GPU milliseconds and a complete sampled distribution." -Observed $distribution -Expected "unit=milliseconds, sampleCount>=$PerformanceSampleFrames"
                }
                $performanceInfo = [ordered]@{
                    schemaVersion = Get-JsonProperty $performanceDocument 'schemaVersion'
                    workload = Get-JsonProperty $performanceDocument 'workload.id'
                    requestedResolution = [ordered]@{ width = $expectedWidth; height = $expectedHeight }
                    measuredResolution = [ordered]@{ width = $measuredWidth; height = $measuredHeight }
                    fixedResolutionMatch = $resolutionMatches
                    gpuPassTimestampFrames = Get-JsonProperty $timestamp 'availableFrameCount'
                    skyAtmosphereP95Milliseconds = Convert-ToFiniteDouble (Get-JsonProperty (Get-ExactProperty $distributions 'render.pass.sky-atmosphere') 'p95')
                    aerialPerspectiveP95Milliseconds = Convert-ToFiniteDouble (Get-JsonProperty (Get-ExactProperty $distributions 'render.pass.aerial-perspective') 'p95')
                    note = if ($resolutionMatches) { 'Performance evidence reports the fixed requested resolution.' } else { 'Runtime performance mode reported its Editor Scene viewport; this receipt rejects it as a fixed-resolution performance claim.' }
                }
            } catch {
                Add-Issue -Code 'performance.invalid' -Stage 'performance' -Message $_.Exception.Message -Observed $performance -Expected 'parseable GPU performance evidence'
            }
        }
    } catch {
        Add-Issue -Code 'performance.process-launch-failed' -Stage 'performance' -Message $_.Exception.Message -Observed $runtime -Expected 'hidden performance process launch'
    }
} else {
    $performanceInfo = [ordered]@{ skipped = $true; reason = 'SkipPerformance switch was supplied.' }
}

$artifacts = [ordered]@{}
foreach ($path in @($image, $sidecar, $qualityEvidence, $performance, $captureStdout, $captureStderr, $performanceStdout, $performanceStderr)) {
    $name = [IO.Path]::GetFileName($path)
    $artifacts[$name] = [ordered]@{ path = $name; exists = (Test-Path -LiteralPath $path -PathType Leaf); sha256 = Get-FileSha256 -Path $path }
}
$receipt = [ordered]@{
    schemaVersion = 'noemancer.sky-atmosphere-capture-evidence/0.1'
    capturedAt = [DateTimeOffset]::UtcNow.ToString('o')
    configuration = [ordered]@{ config = $Config; gpuBackend = $GpuBackend; shaderArtifact = $expectedArtifact; project = $ProjectPath; projectManifestSha256 = $sourceHash; requestedResolution = [ordered]@{ width = $expectedWidth; height = $expectedHeight }; captureFrames = $CaptureFrames; performanceWarmupFrames = $PerformanceWarmupFrames; performanceSampleFrames = $PerformanceSampleFrames; timeoutSeconds = $TimeoutSeconds; hiddenProcess = $true }
    commands = @(
        [ordered]@{ purpose = 'fixed-resolution-hidden-capture'; executable = $runtime; arguments = $captureArguments }
        if (-not $SkipPerformance) { [ordered]@{ purpose = 'hidden-gpu-performance'; executable = $runtime; arguments = $performanceArguments } }
        [ordered]@{ purpose = 'sky-image-quality-analysis'; executable = 'pwsh'; arguments = @('-NoLogo', '-NoProfile', '-File', $analyzer, '-ImagePath', $image, '-OutputPath', $qualityEvidence, '-ExpectedWidth', $expectedWidth, '-ExpectedHeight', $expectedHeight) }
    )
    artifacts = $artifacts
    capture = [ordered]@{ process = $captureRun; renderer = $captureStatus }
    quality = $quality
    performance = $performanceInfo
    checks = @($script:Checks)
    issues = @($script:Issues)
    pass = ($script:Issues.Count -eq 0)
    limitation = 'The visual capture is fixed at 1920x1080. The current Runtime performance path may report the Editor Scene viewport (for example 1172x629); that mismatch is intentionally a failing check, never silently relabeled as 1080p.'
}
Write-JsonDocument -Path $receiptPath -Value $receipt
Write-Output ($receipt | ConvertTo-Json -Depth 100 -Compress)
if (-not [bool]$receipt.pass) { exit 5 }
