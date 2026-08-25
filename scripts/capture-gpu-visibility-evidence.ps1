param(
    [string]$Runtime = (Join-Path $PSScriptRoot '..\build\windows-msvc-debug\src\runtime\Release\noemancer.exe'),
    [Parameter(Mandatory=$true)][string]$OutputRoot,
    [ValidateRange(32,4096)][int]$Instances = 1024,
    [ValidateRange(25,50)][int]$OffscreenPercent = 37,
    [ValidateRange(3,600)][int]$Frames = 64,
    [ValidateSet('direct3d12','vulkan')][string[]]$Backends = @('direct3d12','vulkan')
)

$ErrorActionPreference = 'Stop'
$ExpectedRendererSchemaVersion = 'noemancer.renderer-status.v27'
$ExpectedReadbackAbi = 'noemancer.gpu-visibility-readback/0.2'
$ExpectedQualitySchemaVersion = 'noemancer.render-quality.v1'
$Width = 1920
$Height = 1080

function Get-RequiredProperty {
    param(
        [Parameter(Mandatory=$true)][object]$Object,
        [Parameter(Mandatory=$true)][string]$Name,
        [Parameter(Mandatory=$true)][string]$Context
    )
    if($null -eq $Object) { throw "$Context is missing; expected property '$Name'." }
    $property = $Object.PSObject.Properties[$Name]
    if($null -eq $property) { throw "$Context is missing required property '$Name'." }
    return $property.Value
}

function Get-RequiredBoolean {
    param(
        [Parameter(Mandatory=$true)][object]$Object,
        [Parameter(Mandatory=$true)][string]$Name,
        [Parameter(Mandatory=$true)][string]$Context
    )
    $value = Get-RequiredProperty -Object $Object -Name $Name -Context $Context
    if($value -isnot [bool]) { throw "$Context.$Name must be a JSON boolean." }
    return [bool]$value
}

function Get-RequiredInteger {
    param(
        [Parameter(Mandatory=$true)][object]$Object,
        [Parameter(Mandatory=$true)][string]$Name,
        [Parameter(Mandatory=$true)][string]$Context
    )
    $value = Get-RequiredProperty -Object $Object -Name $Name -Context $Context
    try { return [int64]$value } catch { throw "$Context.$Name must be an integer." }
}

function Get-Sha256 {
    param([Parameter(Mandatory=$true)][string]$Path)
    if(!(Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Evidence artifact is missing: $Path" }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function ConvertTo-CommandLine {
    param([Parameter(Mandatory=$true)][string[]]$Tokens)
    return (($Tokens | ForEach-Object {
        '"' + $_.Replace('"','\"') + '"'
    }) -join ' ')
}

function Get-DeviceEvidence {
    param(
        [Parameter(Mandatory=$true)][object]$Renderer,
        [Parameter(Mandatory=$true)][string]$Backend
    )
    $device = Get-RequiredProperty -Object $Renderer -Name 'device' -Context 'Renderer status'
    $deviceBackend = Get-RequiredProperty -Object $device -Name 'backend' -Context 'Renderer device'
    if($deviceBackend -ne $Backend) {
        throw "Renderer device backend '$deviceBackend' does not match requested backend '$Backend'."
    }
    return [ordered]@{
        backend = $deviceBackend
        adapter = Get-RequiredProperty -Object $device -Name 'adapter' -Context 'Renderer device'
        driverName = Get-RequiredProperty -Object $device -Name 'driverName' -Context 'Renderer device'
        driverVersion = Get-RequiredProperty -Object $device -Name 'driverVersion' -Context 'Renderer device'
        driverInfo = Get-RequiredProperty -Object $device -Name 'driverInfo' -Context 'Renderer device'
        validationEnabled = Get-RequiredBoolean -Object $device -Name 'validationEnabled' -Context 'Renderer device'
        availableBackends = Get-RequiredProperty -Object $device -Name 'availableBackends' -Context 'Renderer device'
        shaderArtifact = Get-RequiredProperty -Object $device -Name 'shaderArtifact' -Context 'Renderer device'
        artifactStatus = Get-RequiredProperty -Object $device -Name 'artifactStatus' -Context 'Renderer device'
    }
}

$runtimePath = (Resolve-Path -LiteralPath $Runtime).Path
$runtimeConfiguration = if($runtimePath -match '[\\/]Release[\\/]') {'Release'} elseif($runtimePath -match '[\\/]Debug[\\/]') {'Debug'} else {'unknown'}
if($runtimeConfiguration -ne 'Release') { throw "GPU visibility acceptance evidence requires a Release runtime; resolved '$runtimePath'." }
$outputPath = [System.IO.Path]::GetFullPath($OutputRoot)
[System.IO.Directory]::CreateDirectory($outputPath) | Out-Null

$expectedCulled = [int64][Math]::Floor(([double]$Instances * [double]$OffscreenPercent) / 100.0)
$expectedCandidates = [int64]$Instances + 1L
$expectedVisible = $expectedCandidates - $expectedCulled
$workingDirectory = (Get-Location).Path
$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$sourceRevision = (& git -C $repositoryRoot rev-parse HEAD 2>$null)
$sourceDirty = [bool](& git -C $repositoryRoot status --short 2>$null)
$results = @()

foreach($backend in $Backends) {
    $stdout = Join-Path $outputPath "$backend.stdout.jsonl"
    $stderr = Join-Path $outputPath "$backend.stderr.log"
    $image = Join-Path $outputPath "$backend.bmp"
    $qualityPath = "$image.quality.json"
    $arguments = @(
        'run','--format','json','--gpu-backend',$backend,
        '--render-stress-instances',"$Instances",
        '--render-stress-offscreen-percent',"$OffscreenPercent",
        '--gpu-visibility-readback','--capture-frame',$image,'--frames',"$Frames",
        '--window-width',"$Width",'--window-height',"$Height"
    )
    $commandTokens = @($runtimePath) + $arguments
    $commandLine = ConvertTo-CommandLine -Tokens $commandTokens

    & $runtimePath @arguments 1> $stdout 2> $stderr
    $exitCode = $LASTEXITCODE
    $stdoutSha256 = Get-Sha256 -Path $stdout
    $stderrSha256 = Get-Sha256 -Path $stderr
    if($exitCode -ne 0) {
        throw "GPU visibility probe failed for $backend with exit code $exitCode. See $stderr"
    }

    $event = Get-Content -LiteralPath $stdout | ForEach-Object {
        if([string]::IsNullOrWhiteSpace($_)) { return }
        try { $_ | ConvertFrom-Json } catch { $null }
    } | Where-Object { $_.event -eq 'render.scene.final' } | Select-Object -Last 1
    if(!$event) { throw "Final renderer status is missing for $backend." }
    $rendererMessage = Get-RequiredProperty -Object $event -Name 'message' -Context 'Final renderer event'
    $renderer = if($rendererMessage -is [string]) { $rendererMessage | ConvertFrom-Json } else { $rendererMessage }

    $rendererSchemaVersion = Get-RequiredProperty -Object $renderer -Name 'schemaVersion' -Context 'Renderer status'
    if($rendererSchemaVersion -ne $ExpectedRendererSchemaVersion) {
        throw "Unexpected renderer status schema '$rendererSchemaVersion' for $backend; expected '$ExpectedRendererSchemaVersion'."
    }
    $submission = Get-RequiredProperty -Object $renderer -Name 'submission' -Context 'Renderer status'
    $gpuDriven = Get-RequiredProperty -Object $submission -Name 'gpuDriven' -Context 'Renderer submission'
    $gpuDrivenAbi = Get-RequiredProperty -Object $gpuDriven -Name 'abi' -Context 'GPU-driven submission'
    if(!(Get-RequiredBoolean -Object $gpuDriven -Name 'enabled' -Context 'GPU-driven submission') -or
       !(Get-RequiredBoolean -Object $gpuDriven -Name 'available' -Context 'GPU-driven submission')) {
        throw "GPU-driven path is not enabled and available for $backend."
    }
    $lastRuntimeFallback = Get-RequiredProperty -Object $gpuDriven -Name 'lastRuntimeFallback' -Context 'GPU-driven submission'
    if($null -ne $lastRuntimeFallback) { throw "GPU-driven runtime fallback was reported for ${backend}: $lastRuntimeFallback" }

    $readback = Get-RequiredProperty -Object $gpuDriven -Name 'readback' -Context 'GPU-driven submission'
    $readbackAbi = Get-RequiredProperty -Object $readback -Name 'abi' -Context 'GPU visibility readback'
    if($readbackAbi -ne $ExpectedReadbackAbi) {
        throw "Unexpected GPU visibility readback ABI '$readbackAbi' for $backend; expected '$ExpectedReadbackAbi'."
    }
    if((Get-RequiredProperty -Object $readback -Name 'state' -Context 'GPU visibility readback') -ne 'complete') {
        throw "GPU visibility readback did not complete for ${backend}: $($readback | ConvertTo-Json -Compress)"
    }
    if(!(Get-RequiredBoolean -Object $readback -Name 'exactSetMatch' -Context 'GPU visibility readback') -or
       !(Get-RequiredBoolean -Object $readback -Name 'countMatch' -Context 'GPU visibility readback')) {
        throw "GPU/CPU visibility set or count disagreement for ${backend}: $($readback | ConvertTo-Json -Compress)"
    }
    if(Get-RequiredBoolean -Object $readback -Name 'includedInPerformanceSample' -Context 'GPU visibility readback') {
        throw "GPU visibility readback must be excluded from formal performance samples for $backend."
    }

    $invalidBatchCounts = Get-RequiredInteger -Object $readback -Name 'invalidBatchCounts' -Context 'GPU visibility readback'
    $mismatchedBatchCounts = Get-RequiredInteger -Object $readback -Name 'mismatchedBatchCounts' -Context 'GPU visibility readback'
    $outOfRangeIndices = Get-RequiredInteger -Object $readback -Name 'outOfRangeIndices' -Context 'GPU visibility readback'
    $wrongBatchIndices = Get-RequiredInteger -Object $readback -Name 'wrongBatchIndices' -Context 'GPU visibility readback'
    $duplicateIndices = Get-RequiredInteger -Object $readback -Name 'duplicateIndices' -Context 'GPU visibility readback'
    if($invalidBatchCounts -ne 0 -or $mismatchedBatchCounts -ne 0 -or $outOfRangeIndices -ne 0 -or
       $wrongBatchIndices -ne 0 -or $duplicateIndices -ne 0) {
        throw "GPU visibility readback integrity counters are non-zero for ${backend}: $($readback | ConvertTo-Json -Compress)"
    }

    $overallMatch = Get-RequiredBoolean -Object $readback -Name 'match' -Context 'GPU visibility readback'
    if(!$overallMatch) { throw "GPU visibility readback overall match is false for $backend." }
    if((Get-RequiredProperty -Object $readback -Name 'synchronization' -Context 'GPU visibility readback') -ne
       'same-command-buffer-command-and-index-copy/fenced-one-shot-submit') {
        throw "GPU visibility readback synchronization contract is unexpected for $backend."
    }
    if($null -ne (Get-RequiredProperty -Object $readback -Name 'error' -Context 'GPU visibility readback')) {
        throw "GPU visibility readback reported an error for $backend."
    }
    $cpuSetHash = Get-RequiredProperty -Object $readback -Name 'cpuSetHash' -Context 'GPU visibility readback'
    $gpuSetHash = Get-RequiredProperty -Object $readback -Name 'gpuSetHash' -Context 'GPU visibility readback'
    if([string]::IsNullOrWhiteSpace([string]$cpuSetHash) -or [string]::IsNullOrWhiteSpace([string]$gpuSetHash) -or
       $cpuSetHash -ne $gpuSetHash) {
        throw "GPU/CPU visible-index set hashes disagree for $backend."
    }

    $observedCandidates = Get-RequiredInteger -Object $gpuDriven -Name 'candidates' -Context 'GPU-driven submission'
    $readbackCandidates = Get-RequiredInteger -Object $readback -Name 'candidates' -Context 'GPU visibility readback'
    $cpuReferenceVisible = Get-RequiredInteger -Object $readback -Name 'cpuReferenceVisible' -Context 'GPU visibility readback'
    $gpuVisible = Get-RequiredInteger -Object $readback -Name 'gpuVisible' -Context 'GPU visibility readback'
    $gpuDrivenBatches = Get-RequiredInteger -Object $gpuDriven -Name 'batches' -Context 'GPU-driven submission'
    $readbackBatches = Get-RequiredInteger -Object $readback -Name 'batches' -Context 'GPU visibility readback'
    $gpuDrivenCpuReference = Get-RequiredInteger -Object $gpuDriven -Name 'cpuReferenceVisible' -Context 'GPU-driven submission'
    $culled = $observedCandidates - $gpuVisible
    if($observedCandidates -ne $expectedCandidates -or $readbackCandidates -ne $expectedCandidates -or
       $cpuReferenceVisible -ne $expectedVisible -or $gpuVisible -ne $expectedVisible -or
       $culled -ne $expectedCulled -or $readbackBatches -ne $gpuDrivenBatches -or
       $gpuDrivenCpuReference -ne $cpuReferenceVisible) {
        throw "Unexpected visibility counts for ${backend}: observed candidates=$observedCandidates, readback candidates=$readbackCandidates, CPU visible=$cpuReferenceVisible, GPU visible=$gpuVisible, culled=$culled; expected candidates=$expectedCandidates, visible=$expectedVisible, culled=$expectedCulled."
    }
    $culledFraction = $culled / [double]$observedCandidates
    if($culledFraction -lt 0.25 -or $culledFraction -gt 0.50) {
        throw "Observed culled fraction $culledFraction is outside [0.25, 0.50] for $backend."
    }

    if(!(Test-Path -LiteralPath $image -PathType Leaf) -or !(Test-Path -LiteralPath $qualityPath -PathType Leaf)) {
        throw "Hidden capture or quality sidecar is missing for $backend."
    }
    $quality = Get-Content -LiteralPath $qualityPath -Raw | ConvertFrom-Json
    $qualitySchemaVersion = Get-RequiredProperty -Object $quality -Name 'schemaVersion' -Context 'Capture quality sidecar'
    if($qualitySchemaVersion -ne $ExpectedQualitySchemaVersion) {
        throw "Unexpected capture quality schema '$qualitySchemaVersion' for $backend."
    }
    if(!(Get-RequiredBoolean -Object $quality -Name 'pass' -Context 'Capture quality sidecar') -or
       !(Get-RequiredBoolean -Object $quality -Name 'dimensionsMatch' -Context 'Capture quality sidecar') -or
       (Get-RequiredInteger -Object $quality -Name 'width' -Context 'Capture quality sidecar') -ne $Width -or
       (Get-RequiredInteger -Object $quality -Name 'height' -Context 'Capture quality sidecar') -ne $Height) {
        throw "Hidden capture quality contract failed for $backend."
    }

    $deviceEvidence = Get-DeviceEvidence -Renderer $renderer -Backend $backend
    $indexSets = Get-RequiredProperty -Object $readback -Name 'indexSets' -Context 'GPU visibility readback'
    if((Get-RequiredProperty -Object $indexSets -Name 'ordering' -Context 'GPU visibility index sets') -ne 'batch-then-ascending-u32' -or
       (Get-RequiredProperty -Object $indexSets -Name 'hashAlgorithm' -Context 'GPU visibility index sets') -ne
        'fnv1a64/le-u64-batch-and-count/le-u32-index') { throw "GPU visibility index-set normalization is unexpected for $backend." }
    $cpuSets = Get-RequiredProperty -Object $indexSets -Name 'cpu' -Context 'GPU visibility index sets'
    $gpuSets = Get-RequiredProperty -Object $indexSets -Name 'gpu' -Context 'GPU visibility index sets'
    if(($cpuSets | ConvertTo-Json -Depth 8 -Compress) -ne ($gpuSets | ConvertTo-Json -Depth 8 -Compress)) {
        throw "GPU visibility index artifacts disagree for $backend."
    }
    $indexSetsPath = Join-Path $outputPath "$backend.visible-index-sets.json"
    $indexSets | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $indexSetsPath -Encoding utf8
    $shaderExtension = if($backend -eq 'direct3d12') {'dxil'} else {'spv'}
    $shaderRoot = [System.IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $runtimePath) '..\shaders'))
    $shaderArtifacts = @('gpu_visibility.comp','scene_gpu_driven.vert','scene_lit.frag') | ForEach-Object {
        $shaderPath = Join-Path $shaderRoot "$_.${shaderExtension}"
        [ordered]@{ path = $shaderPath; sha256 = Get-Sha256 -Path $shaderPath }
    }
    $results += [ordered]@{
        backend = $backend
        commandLine = $commandLine
        arguments = $arguments
        exitCode = [int]$exitCode
        candidates = $observedCandidates
        expectedCandidates = $expectedCandidates
        batches = $gpuDrivenBatches
        cpuReferenceVisible = $cpuReferenceVisible
        gpuVisible = $gpuVisible
        expectedVisible = $expectedVisible
        culled = $culled
        expectedCulled = $expectedCulled
        culledFraction = $culledFraction
        countMatch = [bool](Get-RequiredBoolean -Object $readback -Name 'countMatch' -Context 'GPU visibility readback')
        exactSetMatch = [bool](Get-RequiredBoolean -Object $readback -Name 'exactSetMatch' -Context 'GPU visibility readback')
        match = $overallMatch
        invalidBatchCounts = $invalidBatchCounts
        mismatchedBatchCounts = $mismatchedBatchCounts
        outOfRangeIndices = $outOfRangeIndices
        wrongBatchIndices = $wrongBatchIndices
        duplicateIndices = $duplicateIndices
        cpuSetHash = $cpuSetHash
        gpuSetHash = $gpuSetHash
        transferBytes = Get-RequiredInteger -Object $readback -Name 'transferBytes' -Context 'GPU visibility readback'
        synchronization = Get-RequiredProperty -Object $readback -Name 'synchronization' -Context 'GPU visibility readback'
        includedInPerformanceSample = [bool](Get-RequiredBoolean -Object $readback -Name 'includedInPerformanceSample' -Context 'GPU visibility readback')
        rendererSchemaVersion = $rendererSchemaVersion
        rendererAbi = $gpuDrivenAbi
        readbackAbi = $readbackAbi
        device = $deviceEvidence
        shaderArtifacts = $shaderArtifacts
        indexSets = [System.IO.Path]::GetFileName($indexSetsPath)
        indexSetsSha256 = Get-Sha256 -Path $indexSetsPath
        image = [System.IO.Path]::GetFileName($image)
        imageSha256 = Get-Sha256 -Path $image
        quality = [System.IO.Path]::GetFileName($qualityPath)
        qualitySha256 = Get-Sha256 -Path $qualityPath
        stdout = [System.IO.Path]::GetFileName($stdout)
        stdoutSha256 = $stdoutSha256
        stderr = [System.IO.Path]::GetFileName($stderr)
        stderrSha256 = $stderrSha256
        runtimeLog = [System.IO.Path]::GetFileName($stdout)
    }
}

$manifest = [ordered]@{
    schemaVersion = 'noemancer.gpu-frustum-culling-evidence/0.2'
    capturedAt = [DateTime]::UtcNow.ToString('o')
    workingDirectory = $workingDirectory
    sourceRevision = [string]$sourceRevision
    sourceDirty = $sourceDirty
    workload = [ordered]@{
        schemaVersion = 'noemancer.gpu-visibility-stress/0.1'
        instances = $Instances
        authoredOffscreenPercent = $OffscreenPercent
        frames = $Frames
        width = $Width
        height = $Height
        expected = [ordered]@{
            candidates = $expectedCandidates
            visible = $expectedVisible
            culled = $expectedCulled
        }
    }
    runtime = [ordered]@{
        path = $runtimePath
        sha256 = Get-Sha256 -Path $runtimePath
        configuration = $runtimeConfiguration
    }
    commands = @($results | ForEach-Object {
        [ordered]@{
            backend = $_.backend
            commandLine = $_.commandLine
            arguments = $_.arguments
        }
    })
    devices = @($results | ForEach-Object { $_.device })
    separation = [ordered]@{
        mode = 'one-shot-fenced-readback'
        includedInPerformanceSample = $false
        formalPerformanceSample = $false
        description = 'Readback is submitted and fenced as a one-shot probe; formal performance sampling is a separate invocation.'
    }
    results = @($results)
}
$manifestPath = Join-Path $outputPath 'gpu-frustum-culling-evidence.json'
$temporaryPath = "$manifestPath.tmp"
$manifest | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $temporaryPath -Encoding utf8
Move-Item -LiteralPath $temporaryPath -Destination $manifestPath -Force
Write-Output $manifestPath
