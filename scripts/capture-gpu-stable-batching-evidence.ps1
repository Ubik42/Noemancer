[CmdletBinding()]
param(
    [string]$Runtime = (Join-Path $PSScriptRoot '..\build\windows-msvc-debug\src\runtime\Release\noemancer.exe'),
    [string]$OutputRoot = '',
    [ValidateRange(32,4096)][int]$Instances = 1024,
    [ValidateRange(3,600)][int]$Frames = 64,
    [ValidateRange(640,7680)][int]$Width = 1920,
    [ValidateRange(360,4320)][int]$Height = 1080,
    [ValidateSet('direct3d12','vulkan')][string[]]$Backends = @('direct3d12','vulkan'),
    [string]$StableBatchingPath = 'submission.gpuDriven.stableBatching',
    [string]$ExpectedStableBatchingSchemaVersion = 'noemancer.gpu-stable-batching/0.1',
    [bool]$RequireInitialDirtyUpload = $true,
    [bool]$RequireFinalStableZeroUpload = $true,
    [switch]$GpuDebug
)

$ErrorActionPreference = 'Stop'

# Keep the status contract in one place.  If the renderer schema moves, this
# table and StableBatchingPath are the only script-level names that need to be
# adjusted; the evidence format below remains stable.
$ExpectedRendererSchemaVersion = 'noemancer.renderer-status.v27'
$StableBatchingFields = [ordered]@{
    SchemaVersion = 'schemaVersion'
    Enabled = 'enabled'
    TopologyReused = 'topologyReused'
    TopologyChanged = 'topologyChanged'
    DirtyRanges = 'dirtyRanges'
    UploadBytes = 'uploadBytes'
    UploadBytesTotal = 'uploadBytesTotal'
}
$EvidenceSchemaVersion = 'noemancer.gpu-stable-batching-evidence/0.1'
$WorkloadSchemaVersion = 'noemancer.gpu-stable-batching-workload/0.1'

function Get-RequiredProperty {
    param(
        [Parameter(Mandatory=$true)][object]$Object,
        [Parameter(Mandatory=$true)][string]$Name,
        [Parameter(Mandatory=$true)][string]$Context
    )
    if ($null -eq $Object) {
        throw "$Context is missing; expected field '$Name'."
    }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) {
        throw "$Context is missing required field '$Name'."
    }
    return $property.Value
}

function Get-RequiredPath {
    param(
        [Parameter(Mandatory=$true)][object]$Object,
        [Parameter(Mandatory=$true)][string]$Path,
        [Parameter(Mandatory=$true)][string]$Context
    )
    $current = $Object
    foreach ($segment in ($Path -split '\.')) {
        $current = Get-RequiredProperty -Object $current -Name $segment -Context $Context
        $Context = "$Context.$segment"
    }
    return $current
}

function Get-RequiredBoolean {
    param(
        [Parameter(Mandatory=$true)][object]$Object,
        [Parameter(Mandatory=$true)][string]$Name,
        [Parameter(Mandatory=$true)][string]$Context
    )
    $value = Get-RequiredProperty -Object $Object -Name $Name -Context $Context
    if ($value -isnot [bool]) {
        throw "$Context.$Name must be a JSON boolean; received '$value'."
    }
    return [bool]$value
}

function Get-RequiredNonNegativeInteger {
    param(
        [Parameter(Mandatory=$true)][object]$Object,
        [Parameter(Mandatory=$true)][string]$Name,
        [Parameter(Mandatory=$true)][string]$Context
    )
    $value = Get-RequiredProperty -Object $Object -Name $Name -Context $Context
    if ($null -eq $value -or $value -is [bool]) {
        throw "$Context.$Name must be a non-negative JSON integer; received '$value'."
    }
    try { $converted = [int64]$value } catch {
        throw "$Context.$Name must be a non-negative JSON integer; received '$value'."
    }
    if ($converted -lt 0) {
        throw "$Context.$Name must be non-negative; received '$converted'."
    }
    return $converted
}

function Get-Sha256 {
    param([Parameter(Mandatory=$true)][string]$Path)
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Evidence artifact is missing: $Path"
    }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function ConvertTo-CommandLine {
    param([Parameter(Mandatory=$true)][string[]]$Tokens)
    return (($Tokens | ForEach-Object { '"' + $_.Replace('"','\"') + '"' }) -join ' ')
}

function ConvertTo-RendererStatus {
    param(
        [Parameter(Mandatory=$true)][object]$Event,
        [Parameter(Mandatory=$true)][string]$Context
    )
    $message = Get-RequiredProperty -Object $Event -Name 'message' -Context $Context
    if ($message -is [string]) {
        try { return ($message | ConvertFrom-Json) } catch {
            throw "$Context.message is not valid renderer-status JSON: $($_.Exception.Message)"
        }
    }
    return $message
}

function Get-StableBatchingSnapshot {
    param(
        [Parameter(Mandatory=$true)][object]$Renderer,
        [Parameter(Mandatory=$true)][string]$Backend,
        [Parameter(Mandatory=$true)][string]$Phase
    )
    $rendererSchema = Get-RequiredProperty -Object $Renderer -Name 'schemaVersion' -Context "Renderer status ($Backend/$Phase)"
    if ($rendererSchema -ne $ExpectedRendererSchemaVersion) {
        throw "Unexpected renderer status schema '$rendererSchema' for $Backend/$Phase; expected '$ExpectedRendererSchemaVersion'."
    }
    $stable = Get-RequiredPath -Object $Renderer -Path $StableBatchingPath -Context "Renderer status ($Backend/$Phase)"
    $stableSchema = Get-RequiredProperty -Object $stable -Name $StableBatchingFields.SchemaVersion -Context "Stable batching ($Backend/$Phase)"
    if ($stableSchema -ne $ExpectedStableBatchingSchemaVersion) {
        throw "Unexpected stable batching schema '$stableSchema' for $Backend/$Phase; expected '$ExpectedStableBatchingSchemaVersion'."
    }
    $enabled = Get-RequiredBoolean -Object $stable -Name $StableBatchingFields.Enabled -Context "Stable batching ($Backend/$Phase)"
    if (!$enabled) { throw "Stable batching is disabled for $Backend/$Phase." }
    $topologyReused = Get-RequiredBoolean -Object $stable -Name $StableBatchingFields.TopologyReused -Context "Stable batching ($Backend/$Phase)"
    $topologyChanged = Get-RequiredBoolean -Object $stable -Name $StableBatchingFields.TopologyChanged -Context "Stable batching ($Backend/$Phase)"
    $dirtyRanges = Get-RequiredNonNegativeInteger -Object $stable -Name $StableBatchingFields.DirtyRanges -Context "Stable batching ($Backend/$Phase)"
    $uploadBytes = Get-RequiredNonNegativeInteger -Object $stable -Name $StableBatchingFields.UploadBytes -Context "Stable batching ($Backend/$Phase)"
    $uploadBytesTotal = Get-RequiredNonNegativeInteger -Object $stable -Name $StableBatchingFields.UploadBytesTotal -Context "Stable batching ($Backend/$Phase)"
    if ($dirtyRanges -eq 0 -and $uploadBytes -ne 0) {
        throw "Stable batching reports uploadBytes=$uploadBytes with dirtyRanges=0 for $Backend/$Phase."
    }
    if ($dirtyRanges -gt 0 -and $uploadBytes -eq 0) {
        throw "Stable batching reports dirtyRanges=$dirtyRanges with uploadBytes=0 for $Backend/$Phase."
    }
    [ordered]@{
        phase = $Phase
        schemaVersion = $stableSchema
        enabled = $enabled
        topologyReused = $topologyReused
        topologyChanged = $topologyChanged
        dirtyRanges = $dirtyRanges
        uploadBytes = $uploadBytes
        uploadBytesTotal = $uploadBytesTotal
    }
}

function Get-DeviceEvidence {
    param(
        [Parameter(Mandatory=$true)][object]$Renderer,
        [Parameter(Mandatory=$true)][string]$Backend
    )
    $device = Get-RequiredProperty -Object $Renderer -Name 'device' -Context "Renderer status ($Backend)"
    $deviceBackend = Get-RequiredProperty -Object $device -Name 'backend' -Context "Renderer device ($Backend)"
    if ($deviceBackend -ne $Backend) {
        throw "Renderer device backend '$deviceBackend' does not match requested backend '$Backend'."
    }
    [ordered]@{
        backend = $deviceBackend
        adapter = Get-RequiredProperty -Object $device -Name 'adapter' -Context "Renderer device ($Backend)"
        driverName = Get-RequiredProperty -Object $device -Name 'driverName' -Context "Renderer device ($Backend)"
        driverVersion = Get-RequiredProperty -Object $device -Name 'driverVersion' -Context "Renderer device ($Backend)"
        driverInfo = Get-RequiredProperty -Object $device -Name 'driverInfo' -Context "Renderer device ($Backend)"
        validationEnabled = Get-RequiredBoolean -Object $device -Name 'validationEnabled' -Context "Renderer device ($Backend)"
        shaderArtifact = Get-RequiredProperty -Object $device -Name 'shaderArtifact' -Context "Renderer device ($Backend)"
        artifactStatus = Get-RequiredProperty -Object $device -Name 'artifactStatus' -Context "Renderer device ($Backend)"
    }
}

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$runtimePath = (Resolve-Path -LiteralPath $Runtime).Path
$runtimeConfiguration = if ($runtimePath -match '[\\/]Release[\\/]') { 'Release' } else { 'unknown' }
if ($runtimeConfiguration -ne 'Release') {
    throw "Stable batching evidence requires a Release runtime; resolved '$runtimePath'."
}
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $repositoryRoot ('generated\acceptance\gpu-stable-batching-' + (Get-Date -Format yyyyMMdd-HHmmss))
}
$outputPath = [System.IO.Path]::GetFullPath($OutputRoot)
if (Test-Path -LiteralPath $outputPath) {
    throw "Evidence output already exists: $outputPath"
}
New-Item -ItemType Directory -Force -Path $outputPath | Out-Null

$sourceRevision = (& git -C $repositoryRoot rev-parse HEAD 2>$null)
$sourceDirty = [bool](& git -C $repositoryRoot status --short 2>$null)
$results = @()

foreach ($backend in $Backends) {
    $image = Join-Path $outputPath "$backend.bmp"
    $stdout = Join-Path $outputPath "$backend.stdout.jsonl"
    $stderr = Join-Path $outputPath "$backend.stderr.log"
    $arguments = @(
        'run', '--format', 'json', '--gpu-backend', $backend,
        '--render-stress-instances', "$Instances",
        '--capture-frame', $image, '--frames', "$Frames",
        '--window-width', "$Width", '--window-height', "$Height"
    )
    if ($GpuDebug) { $arguments += '--gpu-debug' }
    $commandLine = ConvertTo-CommandLine -Tokens (@($runtimePath) + $arguments)

    & $runtimePath @arguments 1> $stdout 2> $stderr
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        throw "Stable batching hidden run failed for $backend with exit code $exitCode. See $stderr"
    }
    if (!(Test-Path -LiteralPath $image -PathType Leaf)) {
        throw "Stable batching hidden run did not produce the requested scene capture for ${backend}: $image"
    }

    $events = @(Get-Content -LiteralPath $stdout | ForEach-Object {
        if ([string]::IsNullOrWhiteSpace($_)) { return }
        try { $_ | ConvertFrom-Json } catch { $null }
    } | Where-Object { $_ -and ($_.event -eq 'render.scene' -or $_.event -eq 'render.scene.final') })
    if ($events.Count -eq 0) {
        throw "Renderer Status events are missing for $backend; expected render.scene/render.scene.final in $stdout."
    }
    $initialEvent = $events | Where-Object event -eq 'render.scene' | Select-Object -First 1
    $finalEvent = $events | Where-Object event -eq 'render.scene.final' | Select-Object -Last 1
    if ($null -eq $initialEvent) { $initialEvent = $events | Select-Object -First 1 }
    if ($null -eq $finalEvent) { $finalEvent = $events | Select-Object -Last 1 }
    $initialRenderer = ConvertTo-RendererStatus -Event $initialEvent -Context "Initial renderer status ($backend)"
    $finalRenderer = ConvertTo-RendererStatus -Event $finalEvent -Context "Final renderer status ($backend)"
    $initialStable = Get-StableBatchingSnapshot -Renderer $initialRenderer -Backend $backend -Phase 'initial'
    $finalStable = Get-StableBatchingSnapshot -Renderer $finalRenderer -Backend $backend -Phase 'final'

    if ($RequireInitialDirtyUpload -and ($initialStable.dirtyRanges -le 0 -or $initialStable.uploadBytes -le 0)) {
        throw "Initial stable batching publication did not report dirty ranges and upload bytes for ${backend}: $($initialStable | ConvertTo-Json -Compress)"
    }
    if (!$finalStable.topologyReused -or $finalStable.topologyChanged) {
        throw "Stable batching topology was not reused on the final frame for ${backend}: $($finalStable | ConvertTo-Json -Compress)"
    }
    if ($RequireFinalStableZeroUpload -and ($finalStable.dirtyRanges -ne 0 -or $finalStable.uploadBytes -ne 0)) {
        throw "Stable batching final frame still reports dirty/upload work for ${backend}: $($finalStable | ConvertTo-Json -Compress)"
    }
    if ($finalStable.uploadBytesTotal -lt $initialStable.uploadBytes) {
        throw "Stable batching uploadBytesTotal regressed below the initial publication for $backend."
    }

    $results += [ordered]@{
        backend = $backend
        commandLine = $commandLine
        arguments = $arguments
        exitCode = [int]$exitCode
        frames = $Frames
        instances = $Instances
        device = Get-DeviceEvidence -Renderer $finalRenderer -Backend $backend
        initial = $initialStable
        final = $finalStable
        runtimeLog = [System.IO.Path]::GetFileName($stdout)
        runtimeLogSha256 = Get-Sha256 -Path $stdout
        stderrLog = [System.IO.Path]::GetFileName($stderr)
        stderrLogSha256 = Get-Sha256 -Path $stderr
        capture = if (Test-Path -LiteralPath $image -PathType Leaf) {
            [ordered]@{ path = [System.IO.Path]::GetFileName($image); sha256 = Get-Sha256 -Path $image }
        } else { $null }
    }
}

$manifest = [ordered]@{
    schemaVersion = $EvidenceSchemaVersion
    capturedAt = [DateTimeOffset]::UtcNow.ToString('o')
    sourceRevision = [string]$sourceRevision
    sourceDirty = $sourceDirty
    runtime = [ordered]@{
        path = $runtimePath
        configuration = $runtimeConfiguration
        sha256 = Get-Sha256 -Path $runtimePath
    }
    workload = [ordered]@{
        schemaVersion = $WorkloadSchemaVersion
        instances = $Instances
        frames = $Frames
        width = $Width
        height = $Height
        stableBatchingPath = $StableBatchingPath
        expectedStableBatchingSchemaVersion = $ExpectedStableBatchingSchemaVersion
        requireInitialDirtyUpload = $RequireInitialDirtyUpload
        requireFinalStableZeroUpload = $RequireFinalStableZeroUpload
    }
    commands = @($results | ForEach-Object {
        [ordered]@{ backend = $_.backend; commandLine = $_.commandLine; arguments = $_.arguments }
    })
    devices = @($results | ForEach-Object { $_.device })
    results = @($results)
    pass = $true
}
$manifestPath = Join-Path $outputPath 'gpu-stable-batching-evidence.json'
$manifest | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $manifestPath -Encoding utf8
[pscustomobject]@{
    Success = $true
    Evidence = $manifestPath
    Backends = ($Backends -join ',')
    Runtime = $runtimePath
}
