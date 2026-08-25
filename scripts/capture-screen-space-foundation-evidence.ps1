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

# Hidden, fixed-resolution evidence for the v14 screen-space foundation.  The
# script deliberately validates the Runtime's own structured status rather
# than inferring HiZ/history readiness from a screenshot.
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

function Test-Vector {
    param(
        [AllowNull()][object]$Actual,
        [Parameter(Mandatory = $true)][int[]]$Expected
    )
    if ($null -eq $Actual) { return $false }
    $values = @($Actual)
    if ($values.Count -ne $Expected.Count) { return $false }
    for ($index = 0; $index -lt $Expected.Count; $index++) {
        try {
            if ([int]$values[$index] -ne $Expected[$index]) { return $false }
        } catch {
            return $false
        }
    }
    return $true
}

function Get-MipCount {
    param(
        [Parameter(Mandatory = $true)][int]$Width,
        [Parameter(Mandatory = $true)][int]$Height
    )
    $largest = [Math]::Max($Width, $Height)
    $count = 1
    while ($largest -gt 1) {
        $largest = [int][Math]::Ceiling([double]$largest / 2.0)
        $count++
    }
    return $count
}

function Invoke-HiddenProcess {
    param(
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$StdoutPath,
        [Parameter(Mandatory = $true)][string]$StderrPath,
        [Parameter(Mandatory = $true)][int]$TimeoutSeconds
    )
    $startedAt = [DateTimeOffset]::UtcNow
    $process = Start-Process -FilePath $Executable -ArgumentList $Arguments -WindowStyle Hidden `
        -RedirectStandardOutput $StdoutPath -RedirectStandardError $StderrPath -PassThru
    $completed = $process.WaitForExit($TimeoutSeconds * 1000)
    if (-not $completed) {
        try { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue } catch { }
        return [ordered]@{
            started = $true
            completed = $false
            timedOut = $true
            exitCode = $null
            processId = $process.Id
            durationMilliseconds = ([DateTimeOffset]::UtcNow - $startedAt).TotalMilliseconds
            stdout = $StdoutPath
            stderr = $StderrPath
        }
    }
    return [ordered]@{
        started = $true
        completed = $true
        timedOut = $false
        exitCode = $process.ExitCode
        processId = $process.Id
        durationMilliseconds = ([DateTimeOffset]::UtcNow - $startedAt).TotalMilliseconds
        stdout = $StdoutPath
        stderr = $StderrPath
    }
}

function Test-ScreenSpaceStatus {
    param(
        [Parameter(Mandatory = $true)]$Payload,
        [Parameter(Mandatory = $true)][string]$Stage,
        [Parameter(Mandatory = $true)][string]$Backend,
        [Parameter(Mandatory = $true)][string]$ExpectedArtifact,
        [Parameter(Mandatory = $true)][int]$ExpectedWidth,
        [Parameter(Mandatory = $true)][int]$ExpectedHeight,
        [Parameter(Mandatory = $true)][bool]$RequireRootDimensions
    )
    $renderer = Get-JsonProperty -Object $Payload -Path 'renderer'
    $device = Get-JsonProperty -Object $renderer -Path 'device'
    $artifact = Get-JsonProperty -Object $device -Path 'artifactContract'
    $graph = Get-JsonProperty -Object $renderer -Path 'graph'
    $foundation = Get-JsonProperty -Object $renderer -Path 'screenSpaceFoundation'
    $depth = Get-JsonProperty -Object $foundation -Path 'depthPyramid'
    $history = Get-JsonProperty -Object $foundation -Path 'historyAuthority'

    Add-Check -Code "$Stage.renderer-schema" -Stage $Stage `
        -Pass ((Get-JsonProperty -Object $renderer -Path 'schemaVersion') -eq 'noemancer.renderer-status.v27') `
        -Message 'Renderer status must use noemancer.renderer-status.v27.' `
        -Observed (Get-JsonProperty -Object $renderer -Path 'schemaVersion') -Expected 'noemancer.renderer-status.v27'
    Add-Check -Code "$Stage.backend" -Stage $Stage `
        -Pass ((Get-JsonProperty -Object $device -Path 'backend') -eq $Backend) `
        -Message "Renderer backend must be $Backend." `
        -Observed (Get-JsonProperty -Object $device -Path 'backend') -Expected $Backend
    Add-Check -Code "$Stage.shader-artifact" -Stage $Stage `
        -Pass ((Get-JsonProperty -Object $device -Path 'shaderArtifact') -eq $ExpectedArtifact) `
        -Message "Shader artifact must be $ExpectedArtifact for $Backend." `
        -Observed (Get-JsonProperty -Object $device -Path 'shaderArtifact') -Expected $ExpectedArtifact
    Add-Check -Code "$Stage.shader-manifest" -Stage $Stage `
        -Pass ((Get-JsonProperty -Object $artifact -Path 'code') -eq 'ok' -and
            (Get-JsonProperty -Object $artifact -Path 'schema') -eq 'noemancer.shader-artifact-manifest/0.1' -and
            [string](Get-JsonProperty -Object $artifact -Path 'manifestHash') -match '^sha256:[0-9a-fA-F]{64}$') `
        -Message 'Shader artifact manifest must be verified with a SHA-256 identity.' `
        -Observed $artifact -Expected 'code=ok, schema=noemancer.shader-artifact-manifest/0.1, manifestHash=sha256:64hex'

    # v14 is the graph identity; the serializer schema remains v11 for
    # compatibility with the existing Render Graph JSON contract.
    Add-Check -Code "$Stage.graph-schema" -Stage $Stage `
        -Pass ((Get-JsonProperty -Object $graph -Path 'schemaVersion') -eq 'noemancer.render-graph.v11') `
        -Message 'Render Graph serializer schema must remain noemancer.render-graph.v11.' `
        -Observed (Get-JsonProperty -Object $graph -Path 'schemaVersion') -Expected 'noemancer.render-graph.v11'
    Add-Check -Code "$Stage.graph-v14" -Stage $Stage `
        -Pass ((Get-JsonProperty -Object $graph -Path 'graphId') -eq 'render.graph.forward.v15') `
        -Message 'Render Graph identity must be render.graph.forward.v15.' `
        -Observed (Get-JsonProperty -Object $graph -Path 'graphId') -Expected 'render.graph.forward.v15'
    Add-Check -Code "$Stage.graph-valid" -Stage $Stage `
        -Pass ([bool](Get-JsonProperty -Object $graph -Path 'valid')) `
        -Message 'Render Graph must report valid=true.' `
        -Observed (Get-JsonProperty -Object $graph -Path 'valid') -Expected $true
    $passes = @(Get-JsonProperty -Object $graph -Path 'passes')
    $passIds = @($passes | ForEach-Object { [string](Get-JsonProperty -Object $_ -Path 'id') })
    $requiredPasses = @('render.pass.depth-pyramid-seed', 'render.pass.depth-pyramid-reduce', 'render.pass.temporal-resolve')
    $missingPasses = @($requiredPasses | Where-Object { $passIds -notcontains $_ })
    Add-Check -Code "$Stage.graph-screen-space-passes" -Stage $Stage `
        -Pass ($missingPasses.Count -eq 0) `
        -Message 'v14 graph must contain depth-pyramid seed/reduce and shared temporal resolve passes.' `
        -Observed $passIds -Expected $requiredPasses

    Add-Check -Code "$Stage.foundation-schema" -Stage $Stage `
        -Pass ((Get-JsonProperty -Object $foundation -Path 'schema') -eq 'noemancer.screen-space-foundation/0.1') `
        -Message 'Screen-space foundation must use its published schema.' `
        -Observed (Get-JsonProperty -Object $foundation -Path 'schema') -Expected 'noemancer.screen-space-foundation/0.1'
    $expectedMipCount = Get-MipCount -Width $ExpectedWidth -Height $ExpectedHeight
    $baseExtent = Get-JsonProperty -Object $depth -Path 'baseExtent'
    $mipCount = Convert-ToFiniteDouble (Get-JsonProperty -Object $depth -Path 'mipCount')
    $seedDispatches = Convert-ToFiniteDouble (Get-JsonProperty -Object $depth -Path 'seedDispatches')
    $reduceDispatches = Convert-ToFiniteDouble (Get-JsonProperty -Object $depth -Path 'reduceDispatches')
    Add-Check -Code "$Stage.depth-ready" -Stage $Stage `
        -Pass ([bool](Get-JsonProperty -Object $depth -Path 'ready')) `
        -Message 'Depth pyramid must report ready=true after a real frame.' `
        -Observed (Get-JsonProperty -Object $depth -Path 'ready') -Expected $true
    Add-Check -Code "$Stage.depth-format" -Stage $Stage `
        -Pass ((Get-JsonProperty -Object $depth -Path 'format') -eq 'RG32_FLOAT' -and
            (Get-JsonProperty -Object $depth -Path 'encoding') -eq 'linear-view-depth-min-max') `
        -Message 'Depth pyramid must use RG32_FLOAT linear view-depth min/max encoding.' `
        -Observed ([ordered]@{ format = Get-JsonProperty -Object $depth -Path 'format'; encoding = Get-JsonProperty -Object $depth -Path 'encoding' }) `
        -Expected 'format=RG32_FLOAT, encoding=linear-view-depth-min-max'
    Add-Check -Code "$Stage.depth-extent" -Stage $Stage `
        -Pass (Test-Vector -Actual $baseExtent -Expected @($ExpectedWidth, $ExpectedHeight)) `
        -Message 'Depth pyramid base extent must match the fixed capture resolution.' `
        -Observed $baseExtent -Expected @($ExpectedWidth, $ExpectedHeight)
    Add-Check -Code "$Stage.depth-mips" -Stage $Stage `
        -Pass ($null -ne $mipCount -and $mipCount -eq $expectedMipCount) `
        -Message "Depth pyramid mip count must be the complete ceil-half chain ($expectedMipCount levels)." `
        -Observed $mipCount -Expected $expectedMipCount
    Add-Check -Code "$Stage.depth-dispatches" -Stage $Stage `
        -Pass ($null -ne $seedDispatches -and $seedDispatches -gt 0 -and $null -ne $reduceDispatches -and $reduceDispatches -gt 0) `
        -Message 'Depth pyramid seed and reduce dispatch counters must both be non-zero.' `
        -Observed ([ordered]@{ seedDispatches = $seedDispatches; reduceDispatches = $reduceDispatches }) `
        -Expected 'seedDispatches>0, reduceDispatches>0'

    Add-Check -Code "$Stage.history-schema" -Stage $Stage `
        -Pass ((Get-JsonProperty -Object $history -Path 'schema') -eq 'noemancer.temporal-history/0.1') `
        -Message 'Temporal history authority must use noemancer.temporal-history/0.1.' `
        -Observed (Get-JsonProperty -Object $history -Path 'schema') -Expected 'noemancer.temporal-history/0.1'
    $consumers = @(Get-JsonProperty -Object $history -Path 'consumers')
    $consumerNames = @($consumers | ForEach-Object { [string](Get-JsonProperty -Object $_ -Path 'consumer') } | Sort-Object)
    $expectedConsumers = @('rtgi', 'ssgi', 'ssr', 'taa')
    Add-Check -Code "$Stage.history-consumers" -Stage $Stage `
        -Pass (($consumerNames -join '|') -eq ($expectedConsumers -join '|')) `
        -Message 'Temporal history authority must publish independent TAA/SSR/SSGI/RTGI consumer state.' `
        -Observed $consumerNames -Expected $expectedConsumers
    foreach ($consumer in $consumers) {
        $consumerName = [string](Get-JsonProperty -Object $consumer -Path 'consumer')
        $revision = Convert-ToFiniteDouble (Get-JsonProperty -Object $consumer -Path 'revision')
        $currentValid = Get-JsonProperty -Object $consumer -Path 'currentValid'
        $previousValid = Get-JsonProperty -Object $consumer -Path 'previousValid'
        $resetReasonValue = Get-JsonProperty -Object $consumer -Path 'lastResetReasons'
        $resetReasons = if ($null -eq $resetReasonValue) { @() } else { @($resetReasonValue) }
        $lifecycle = [string](Get-JsonProperty -Object $consumer -Path 'lifecycle')
        $minimumRevision = if ($consumerName -eq 'taa') { 1 } else { 0 }
        $statePass = $null -ne $revision -and $revision -ge $minimumRevision -and
            $null -ne $currentValid -and $null -ne $previousValid -and
            $null -ne (Get-JsonProperty -Object $consumer -Path 'current') -and
            $null -ne (Get-JsonProperty -Object $consumer -Path 'previous') -and
            $resetReasons.Count -ge 0 -and $lifecycle -eq 'idle'
        Add-Check -Code "$Stage.history-state-$consumerName" -Stage $Stage `
            -Pass $statePass `
            -Message "Temporal history consumer $consumerName must publish a committed idle state with current/previous validity." `
            -Observed ([ordered]@{ revision = $revision; currentValid = $currentValid; previousValid = $previousValid; lifecycle = $lifecycle; resetReasons = $resetReasons }) `
            -Expected "revision>=$minimumRevision, currentValid/previousValid present, lifecycle=idle"
    }

    $statusWidth = Convert-ToFiniteDouble (Get-JsonProperty -Object $renderer -Path 'surface.width')
    $statusHeight = Convert-ToFiniteDouble (Get-JsonProperty -Object $renderer -Path 'surface.height')
    $payloadWidth = if ($RequireRootDimensions) { Convert-ToFiniteDouble (Get-JsonProperty -Object $Payload -Path 'width') } else { $null }
    $payloadHeight = if ($RequireRootDimensions) { Convert-ToFiniteDouble (Get-JsonProperty -Object $Payload -Path 'height') } else { $null }
    $workloadWidth = Convert-ToFiniteDouble (Get-JsonProperty -Object $Payload -Path 'workload.resolution.width')
    $workloadHeight = Convert-ToFiniteDouble (Get-JsonProperty -Object $Payload -Path 'workload.resolution.height')
    $actualWidth = if ($null -ne $payloadWidth) { $payloadWidth } elseif ($null -ne $workloadWidth) { $workloadWidth } else { $statusWidth }
    $actualHeight = if ($null -ne $payloadHeight) { $payloadHeight } elseif ($null -ne $workloadHeight) { $workloadHeight } else { $statusHeight }
    Add-Check -Code "$Stage.fixed-resolution" -Stage $Stage `
        -Pass ($actualWidth -eq $ExpectedWidth -and $actualHeight -eq $ExpectedHeight -and
            $statusWidth -eq $ExpectedWidth -and $statusHeight -eq $ExpectedHeight) `
        -Message 'Capture and renderer status must both report the fixed 1920x1080 surface.' `
        -Observed ([ordered]@{ payloadWidth = $actualWidth; payloadHeight = $actualHeight; statusWidth = $statusWidth; statusHeight = $statusHeight }) `
        -Expected ([ordered]@{ width = $ExpectedWidth; height = $ExpectedHeight })

    return [ordered]@{
        rendererSchema = Get-JsonProperty -Object $renderer -Path 'schemaVersion'
        graphId = Get-JsonProperty -Object $graph -Path 'graphId'
        graphSchema = Get-JsonProperty -Object $graph -Path 'schemaVersion'
        foundationSchema = Get-JsonProperty -Object $foundation -Path 'schema'
        depth = [ordered]@{ ready = Get-JsonProperty -Object $depth -Path 'ready'; format = Get-JsonProperty -Object $depth -Path 'format'; baseExtent = $baseExtent; mipCount = $mipCount; seedDispatches = $seedDispatches; reduceDispatches = $reduceDispatches }
        history = [ordered]@{ schema = Get-JsonProperty -Object $history -Path 'schema'; consumers = $consumerNames }
        fixedResolution = [ordered]@{ width = $actualWidth; height = $actualHeight }
        shaderArtifact = Get-JsonProperty -Object $device -Path 'shaderArtifact'
        manifestHash = Get-JsonProperty -Object $artifact -Path 'manifestHash'
    }
}

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$runtime = Join-Path $repositoryRoot "build/windows-msvc-debug/src/runtime/$Config/noemancer.exe"
$engineScript = Join-Path $repositoryRoot 'scripts/engine.ps1'
$expectedArtifact = if ($GpuBackend -eq 'direct3d12') { 'DXIL' } else { 'SPIR-V' }
$expectedWidth = 1920
$expectedHeight = 1080

try {
    if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
        $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
        $OutputRoot = Join-Path $repositoryRoot "generated/acceptance/screen-space-foundation-$GpuBackend-$stamp"
    }
    $OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
    if (Test-Path -LiteralPath $OutputRoot) {
        if (Get-ChildItem -LiteralPath $OutputRoot -Force | Select-Object -First 1) {
            throw "Screen-space evidence output must be empty because receipts are immutable: $OutputRoot"
        }
    } else {
        New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null
    }
    $script:EvidenceRoot = $OutputRoot
    $script:ReceiptPath = Join-Path $OutputRoot 'screen-space-foundation-evidence.json'
    $ProjectPath = [IO.Path]::GetFullPath($ProjectPath)
    $projectManifest = if (Test-Path -LiteralPath $ProjectPath -PathType Container) {
        Join-Path $ProjectPath 'noemancer.project.json'
    } else { $ProjectPath }
    Add-Check -Code 'static.project' -Stage 'static' `
        -Pass (Test-Path -LiteralPath $projectManifest -PathType Leaf) `
        -Message 'The fixed-resolution screen-space project manifest must exist.' `
        -Observed $projectManifest -Expected 'file exists'
    Add-Check -Code 'static.engine-script' -Stage 'static' `
        -Pass (Test-Path -LiteralPath $engineScript -PathType Leaf) `
        -Message 'scripts/engine.ps1 is required for a reproducible fallback build.' `
        -Observed $engineScript -Expected 'file exists'

    $buildRun = $null
    if (-not (Test-Path -LiteralPath $runtime -PathType Leaf)) {
        try {
            $pwsh = (Get-Command pwsh -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
            $buildRun = Invoke-HiddenProcess -Executable $pwsh `
                -Arguments @('-NoLogo', '-NoProfile', '-File', $engineScript, 'build', '-Config', $Config, '-Target', 'noemancer') `
                -StdoutPath (Join-Path $OutputRoot 'build.stdout.log') `
                -StderrPath (Join-Path $OutputRoot 'build.stderr.log') `
                -TimeoutSeconds $TimeoutSeconds
            Add-Check -Code 'build.process' -Stage 'build' `
                -Pass ([bool]$buildRun.completed -and [int]$buildRun.exitCode -eq 0) `
                -Message 'Hidden fallback engine build must complete successfully.' `
                -Observed $buildRun -Expected 'completed=true, exitCode=0'
        } catch {
            Add-Issue -Code 'build.launch-failed' -Stage 'build' -Message $_.Exception.Message -Observed $engineScript -Expected 'hidden build process'
        }
    }
    Add-Check -Code 'build.runtime' -Stage 'build' `
        -Pass (Test-Path -LiteralPath $runtime -PathType Leaf) `
        -Message 'The selected Noemancer runtime must exist before capture.' `
        -Observed $runtime -Expected 'file exists'

    $image = Join-Path $OutputRoot 'screen-space-foundation-1920x1080.bmp'
    $sidecar = "$image.quality.json"
    $captureStdout = Join-Path $OutputRoot 'runtime.capture.stdout.jsonl'
    $captureStderr = Join-Path $OutputRoot 'runtime.capture.stderr.log'
    $performance = Join-Path $OutputRoot 'performance.json'
    $performanceStdout = Join-Path $OutputRoot 'runtime.performance.stdout.jsonl'
    $performanceStderr = Join-Path $OutputRoot 'runtime.performance.stderr.log'
    $captureArguments = @(
        'run', '--format', 'json', '--project', $ProjectPath,
        '--capture-frame', $image, '--frames', [string]$CaptureFrames,
        '--window-width', [string]$expectedWidth, '--window-height', [string]$expectedHeight,
        '--exposure', '1.0', '--render-scale', '1.0', '--gpu-backend', $GpuBackend
    )
    $performanceArguments = @(
        'run', '--format', 'json', '--project', $ProjectPath,
        '--performance-evidence', $performance, '--performance-hidden',
        '--performance-workload', 'screen-space-foundation-v14',
        '--performance-warmup-frames', [string]$PerformanceWarmupFrames,
        '--performance-sample-frames', [string]$PerformanceSampleFrames,
        '--window-width', [string]$expectedWidth, '--window-height', [string]$expectedHeight,
        '--exposure', '1.0', '--render-scale', '1.0', '--gpu-backend', $GpuBackend
    )

    $captureRun = $null
    $captureStatus = $null
    $performanceRun = $null
    $performanceStatus = $null
    $performanceInfo = $null

    try {
        $captureRun = Invoke-HiddenProcess -Executable $runtime -Arguments $captureArguments `
            -StdoutPath $captureStdout -StderrPath $captureStderr -TimeoutSeconds $TimeoutSeconds
        Add-Check -Code 'capture.process' -Stage 'capture' `
            -Pass ([bool]$captureRun.completed -and [int]$captureRun.exitCode -eq 0) `
            -Message 'Hidden fixed-resolution capture must exit successfully.' `
            -Observed $captureRun -Expected 'completed=true, exitCode=0'
        Add-Check -Code 'capture.image' -Stage 'capture' `
            -Pass (Test-Path -LiteralPath $image -PathType Leaf) `
            -Message 'Runtime must produce the fixed-resolution BMP capture.' `
            -Observed $image -Expected 'file exists'
        if (Test-Path -LiteralPath $sidecar -PathType Leaf) {
            try {
                $captureDocument = Read-JsonFile -Path $sidecar
                $captureStatus = Test-ScreenSpaceStatus -Payload $captureDocument -Stage 'capture' `
                    -Backend $GpuBackend -ExpectedArtifact $expectedArtifact -ExpectedWidth $expectedWidth `
                    -ExpectedHeight $expectedHeight -RequireRootDimensions $true
            } catch {
                Add-Issue -Code 'capture.sidecar-invalid' -Stage 'capture' -Message $_.Exception.Message `
                    -Observed $sidecar -Expected 'parseable renderer-status v26 sidecar'
            }
        } else {
            Add-Issue -Code 'capture.sidecar-missing' -Stage 'capture' `
                -Message 'Runtime did not produce a capture quality sidecar.' `
                -Observed $sidecar -Expected 'file exists'
        }
    } catch {
        Add-Issue -Code 'capture.launch-failed' -Stage 'capture' -Message $_.Exception.Message `
            -Observed $runtime -Expected 'hidden runtime process'
    }

    if (-not $SkipPerformance) {
        try {
            $performanceRun = Invoke-HiddenProcess -Executable $runtime -Arguments $performanceArguments `
                -StdoutPath $performanceStdout -StderrPath $performanceStderr -TimeoutSeconds $TimeoutSeconds
            Add-Check -Code 'performance.process' -Stage 'performance' `
                -Pass ([bool]$performanceRun.completed -and [int]$performanceRun.exitCode -eq 0) `
                -Message 'Hidden fixed-resolution GPU performance run must exit successfully.' `
                -Observed $performanceRun -Expected 'completed=true, exitCode=0'
            if (Test-Path -LiteralPath $performance -PathType Leaf) {
                try {
                    $performanceDocument = Read-JsonFile -Path $performance
                    $performanceStatus = Test-ScreenSpaceStatus -Payload $performanceDocument -Stage 'performance' `
                        -Backend $GpuBackend -ExpectedArtifact $expectedArtifact -ExpectedWidth $expectedWidth `
                        -ExpectedHeight $expectedHeight -RequireRootDimensions $false
                    $performanceInfo = [ordered]@{
                        schemaVersion = Get-JsonProperty -Object $performanceDocument -Path 'schemaVersion'
                        workload = Get-JsonProperty -Object $performanceDocument -Path 'workload.id'
                        resolution = Get-JsonProperty -Object $performanceDocument -Path 'workload.resolution'
                        gpuPassTimestamp = Get-JsonProperty -Object $performanceDocument -Path 'gpu.passTimestamps'
                    }
                    Add-Check -Code 'performance.workload' -Stage 'performance' `
                        -Pass ((Get-JsonProperty -Object $performanceDocument -Path 'workload.id') -eq 'screen-space-foundation-v14') `
                        -Message 'Performance evidence must identify the screen-space-foundation-v14 workload.' `
                        -Observed (Get-JsonProperty -Object $performanceDocument -Path 'workload.id') -Expected 'screen-space-foundation-v14'
                } catch {
                    Add-Issue -Code 'performance.invalid' -Stage 'performance' -Message $_.Exception.Message `
                        -Observed $performance -Expected 'parseable performance evidence'
                }
            } else {
                Add-Issue -Code 'performance.evidence-missing' -Stage 'performance' `
                    -Message 'Runtime did not produce performance JSON.' -Observed $performance -Expected 'file exists'
            }
        } catch {
            Add-Issue -Code 'performance.launch-failed' -Stage 'performance' -Message $_.Exception.Message `
                -Observed $runtime -Expected 'hidden performance process'
        }
    } else {
        $performanceInfo = [ordered]@{ skipped = $true; reason = 'SkipPerformance switch was supplied.' }
    }

    $artifactPaths = @($image, $sidecar, $captureStdout, $captureStderr, $performance, $performanceStdout, $performanceStderr,
        (Join-Path $OutputRoot 'build.stdout.log'), (Join-Path $OutputRoot 'build.stderr.log'))
    $artifacts = [ordered]@{}
    foreach ($path in $artifactPaths) {
        $name = [IO.Path]::GetFileName($path)
        $artifacts[$name] = [ordered]@{
            path = $name
            exists = (Test-Path -LiteralPath $path -PathType Leaf)
            sha256 = Get-FileSha256 -Path $path
        }
    }
    $manifest = [ordered]@{
        schemaVersion = 'noemancer.screen-space-foundation-capture-evidence/0.1'
        capturedAt = [DateTimeOffset]::UtcNow.ToString('o')
        pass = ($script:Issues.Count -eq 0)
        configuration = [ordered]@{
            config = $Config
            gpuBackend = $GpuBackend
            shaderArtifact = $expectedArtifact
            project = $ProjectPath
            projectManifestSha256 = Get-FileSha256 -Path $projectManifest
            requestedResolution = [ordered]@{ width = $expectedWidth; height = $expectedHeight }
            captureFrames = $CaptureFrames
            performanceWarmupFrames = $PerformanceWarmupFrames
            performanceSampleFrames = $PerformanceSampleFrames
            timeoutSeconds = $TimeoutSeconds
            hiddenProcess = $true
            computerUse = $false
        }
        commands = @(
            [ordered]@{ purpose = 'fixed-resolution-hidden-capture'; executable = $runtime; arguments = $captureArguments }
            if (-not $SkipPerformance) { [ordered]@{ purpose = 'fixed-resolution-hidden-performance'; executable = $runtime; arguments = $performanceArguments } }
        )
        artifacts = $artifacts
        build = $buildRun
        capture = [ordered]@{ process = $captureRun; renderer = $captureStatus }
        performance = [ordered]@{ process = $performanceRun; status = $performanceStatus; evidence = $performanceInfo }
        checks = @($script:Checks)
        issues = @($script:Issues)
        policy = 'No visible window is opened; a failed status, missing artifact, timeout or unavailable GPU evidence remains an explicit failing receipt.'
    }
    Write-JsonDocument -Path $script:ReceiptPath -Value $manifest
    Write-Output ($manifest | ConvertTo-Json -Depth 100 -Compress)
    if (-not [bool]$manifest.pass) { exit 5 }
} catch {
    if ($null -ne $script:ReceiptPath) {
        $failure = [ordered]@{
            schemaVersion = 'noemancer.screen-space-foundation-capture-evidence/0.1'
            capturedAt = [DateTimeOffset]::UtcNow.ToString('o')
            pass = $false
            error = $_.Exception.Message
            checks = @($script:Checks)
            issues = @($script:Issues)
        }
        try { Write-JsonDocument -Path $script:ReceiptPath -Value $failure } catch { }
    }
    Write-Error "Screen-space foundation evidence failed: $($_.Exception.Message)"
    exit 1
}
