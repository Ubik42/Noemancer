[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Release',
    [ValidateSet('direct3d12', 'vulkan')]
    [string[]]$GpuBackends = @('direct3d12', 'vulkan'),
    [string]$Runtime = '',
    [string]$OutputRoot = '',
    [ValidateRange(8, 600)]
    [int]$CaptureFrames = 64,
    [ValidateRange(0, 10000)]
    [int]$PerformanceWarmupFrames = 32,
    [ValidateRange(64, 10000)]
    [int]$PerformanceSampleFrames = 64,
    [ValidateRange(60, 10000)]
    [int]$MinimumGpuPassSamples = 60,
    [ValidateRange(30, 900)]
    [int]$TimeoutSeconds = 180,
    [ValidateRange(0.1, 1000.0)]
    [double]$CpuFrameP95BudgetMilliseconds = 16.67,
    [ValidateRange(0.1, 1000.0)]
    [double]$GpuShadowDepthP95BudgetMilliseconds = 4.0,
    [ValidateSet('low', 'medium', 'high')]
    [string]$ShadowQuality = 'high',
    [ValidateRange(32, 4096)]
    [int]$StressInstances = 1024
)

# This script is a decision receipt, not a benchmark wrapper.  It deliberately
# keeps capture and performance in separate hidden runtime invocations because
# --performance-evidence owns the frame budget.  Shadow counters are read from
# Renderer Status; GPU duration comes only from the runtime's fence-gated GPU
# pass timestamp distributions.  Values named *Estimate in the runtime remain
# estimates in this receipt and are never promoted to actual telemetry.
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$EvidenceSchema = 'noemancer.shadow-scalability-decision-evidence/0.1'
$PerformanceSchema = 'noemancer.performance-evidence/0.1'
$GpuTimestampSchema = 'noemancer.gpu-pass-timestamps/0.1'
$RendererSchema = 'noemancer.renderer-status.v30'
$GraphId = 'render.graph.forward.v17'
$GraphSchema = 'noemancer.render-graph.v11'
$QualitySchema = 'noemancer.render-quality.v1'
$Width = 1920
$Height = 1080
$CsmCascadeCount = 4
$CsmResolution = 2048
$LocalLayerCapacity = 8
$LocalResolutionByQuality = @{ low = 512; medium = 768; high = 1024 }
$ExpectedBytesPerD32Texel = 4
$RequiredPasses = @('render.pass.shadow-depth', 'render.pass.opaque-lit')
$ExpectedArtifacts = @{ direct3d12 = 'DXIL'; vulkan = 'SPIR-V' }

$script:Checks = New-Object 'System.Collections.Generic.List[object]'
$script:Issues = New-Object 'System.Collections.Generic.List[object]'
$script:ReceiptPath = $null
$script:NativeWindowReady = $false
$script:NativeWindowError = $null

function Add-Issue {
    param(
        [Parameter(Mandatory = $true)][string]$Code,
        [Parameter(Mandatory = $true)][string]$Stage,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Message,
        [AllowNull()][object]$Observed,
        [AllowNull()][object]$Expected
    )
    [void]$script:Issues.Add([ordered]@{
        code = $Code; stage = $Stage; message = $Message; observed = $Observed; expected = $Expected
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
        code = $Code; stage = $Stage; pass = $Pass; message = $Message; observed = $Observed; expected = $Expected
    })
    if (-not $Pass) {
        Add-Issue -Code $Code -Stage $Stage -Message $Message -Observed $Observed -Expected $Expected
    }
}

function Write-JsonDocument {
    param([Parameter(Mandatory = $true)][string]$Path, [Parameter(Mandatory = $true)]$Value)
    $parent = Split-Path -Parent $Path
    if (-not [string]::IsNullOrWhiteSpace($parent)) { [System.IO.Directory]::CreateDirectory($parent) | Out-Null }
    $json = $Value | ConvertTo-Json -Depth 100
    $utf8NoBom = New-Object System.Text.UTF8Encoding -ArgumentList $false
    [System.IO.File]::WriteAllText($Path, $json + [Environment]::NewLine, $utf8NoBom)
}

function Get-FileSha256 {
    param([AllowNull()][string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path) -or -not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    return ([string](Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash).ToLowerInvariant()
}

function Get-FileBytes {
    param([AllowNull()][string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path) -or -not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    return [int64](Get-Item -LiteralPath $Path).Length
}

function Get-RelativePath {
    param([Parameter(Mandatory = $true)][string]$Root, [Parameter(Mandatory = $true)][string]$Path)
    $rootFull = ([IO.Path]::GetFullPath($Root)).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    $pathFull = [IO.Path]::GetFullPath($Path)
    if ($pathFull.StartsWith($rootFull, [StringComparison]::OrdinalIgnoreCase)) {
        return $pathFull.Substring($rootFull.Length).Replace('\', '/')
    }
    return $pathFull.Replace('\', '/')
}

function Get-ArtifactRecord {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Kind
    )
    return [ordered]@{
        path = Get-RelativePath -Root $Root -Path $Path
        kind = $Kind
        exists = (Test-Path -LiteralPath $Path -PathType Leaf)
        bytes = Get-FileBytes -Path $Path
        sha256 = Get-FileSha256 -Path $Path
    }
}

function Read-JsonFile {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    return Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json -Depth 100
}

function Get-Property {
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

function Get-PathProperty {
    param([AllowNull()][object]$Object, [Parameter(Mandatory = $true)][string]$Path)
    $current = $Object
    foreach ($part in $Path.Split('.')) {
        $current = Get-Property -Object $current -Name $part
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

function Convert-ToNonNegativeInt64 {
    param([AllowNull()][object]$Value)
    if ($null -eq $Value) { return $null }
    try {
        $number = [int64]$Value
        if ($number -lt 0) { return $null }
        return $number
    } catch { return $null }
}

function ConvertTo-CommandLine {
    param([Parameter(Mandatory = $true)][string[]]$Tokens)
    return (($Tokens | ForEach-Object { '"' + $_.Replace('"', '\"') + '"' }) -join ' ')
}

function Ensure-NativeWindowHelper {
    if ($script:NativeWindowReady) { return $true }
    if ($env:OS -ne 'Windows_NT') {
        $script:NativeWindowError = 'The shadow scalability receipt requires the Windows hidden-window contract.'
        return $false
    }
    try {
        $typeName = [System.Management.Automation.PSTypeName]'NoemancerShadowEvidence.NativeWindow'
        if ($null -eq $typeName.Type) {
            Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace NoemancerShadowEvidence {
    public static class NativeWindow {
        [DllImport("user32.dll")]
        public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
    }
}
'@ -ErrorAction Stop
        }
        $script:NativeWindowReady = $true
        return $true
    } catch {
        $script:NativeWindowError = $_.Exception.Message
        return $false
    }
}

function Invoke-HiddenProcess {
    param(
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [Parameter(Mandatory = $true)][string]$StdoutPath,
        [Parameter(Mandatory = $true)][string]$StderrPath,
        [Parameter(Mandatory = $true)][int]$TimeoutSeconds,
        [switch]$RuntimeWindowStartsHidden
    )
    $parent = Split-Path -Parent $StdoutPath
    [System.IO.Directory]::CreateDirectory($parent) | Out-Null
    $startedAt = [DateTimeOffset]::UtcNow
    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = $Executable
    $startInfo.Arguments = ConvertTo-CommandLine -Tokens $Arguments
    $startInfo.WorkingDirectory = $WorkingDirectory
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $startInfo
    try {
        if (-not $process.Start()) { throw "Could not start '$Executable'." }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
        $hidden = $false
        while (-not $process.HasExited -and [DateTime]::UtcNow -lt $deadline) {
            if ($script:NativeWindowReady) {
                try {
                    $process.Refresh()
                    $handle = $process.MainWindowHandle
                    if ($handle -ne [IntPtr]::Zero) {
                        [void][NoemancerShadowEvidence.NativeWindow]::ShowWindow($handle, 0)
                        $hidden = $true
                    }
                } catch { }
            }
            Start-Sleep -Milliseconds 50
        }
        $timedOut = -not $process.HasExited
        if ($timedOut) {
            try { $process.Kill() } catch { }
            $process.WaitForExit(5000) | Out-Null
        }
        $stdout = $stdoutTask.Result
        $stderr = $stderrTask.Result
        [System.IO.File]::WriteAllText($StdoutPath, $stdout, (New-Object System.Text.UTF8Encoding -ArgumentList $false))
        [System.IO.File]::WriteAllText($StderrPath, $stderr, (New-Object System.Text.UTF8Encoding -ArgumentList $false))
        return [ordered]@{
            started = $true
            completed = -not $timedOut
            timedOut = $timedOut
            exitCode = if ($timedOut) { $null } else { [int]$process.ExitCode }
            processId = [int]$process.Id
            hiddenWindow = ($hidden -or [bool]$RuntimeWindowStartsHidden)
            hiddenWindowEvidence = if ($hidden) { 'user32.ShowWindow(SW_HIDE)' } elseif ($RuntimeWindowStartsHidden) { 'runtime-contract:SDL_WINDOW_HIDDEN' } else { $null }
            durationMilliseconds = ([DateTimeOffset]::UtcNow - $startedAt).TotalMilliseconds
            stdout = $StdoutPath
            stderr = $StderrPath
        }
    } finally {
        $process.Dispose()
    }
}

function Get-ProcessOutputText {
    param([Parameter(Mandatory = $true)]$ProcessReceipt)
    $parts = New-Object 'System.Collections.Generic.List[string]'
    foreach ($path in @($ProcessReceipt.stdout, $ProcessReceipt.stderr)) {
        if (-not [string]::IsNullOrWhiteSpace([string]$path) -and (Test-Path -LiteralPath $path -PathType Leaf)) {
            [void]$parts.Add((Get-Content -LiteralPath $path -Raw -Encoding UTF8))
        }
    }
    return ($parts -join "`n")
}

function Add-CliContractIssueIfRejected {
    param(
        [Parameter(Mandatory = $true)][string]$Backend,
        [Parameter(Mandatory = $true)][string]$Purpose,
        [Parameter(Mandatory = $true)]$ProcessReceipt
    )
    if ($ProcessReceipt.completed -and $ProcessReceipt.exitCode -eq 0) { return }
    $output = Get-ProcessOutputText -ProcessReceipt $ProcessReceipt
    # A runtime crash or normal render failure remains a backend failure.
    # Promote only explicit option/flag rejection to the preflight class so a
    # missing CLI parameter has a stable, actionable exit code (2).
    $optionRejected = $output -match '(?i)(unknown|unrecognized|unsupported|invalid)\s+(argument|option|flag)|unknown\s+option|no\s+such\s+option|expected[^\r\n]*--(shadow-scalability-stress-instances|capture-frame|performance-evidence|performance-hidden|shadow-quality|gpu-backend|window-width|window-height)'
    if ($optionRejected) {
        Add-Issue -Code "preflight.$Backend.$Purpose-cli-contract" -Stage 'preflight' -Message "Runtime rejected one or more required shadow evidence CLI options for $Purpose." -Observed ([ordered]@{ exitCode = $ProcessReceipt.exitCode; output = $output.Substring(0, [Math]::Min(2048, $output.Length)) }) -Expected 'reference-scene, shadow-quality, fixed window size, capture/performance evidence options accepted'
    }
}

function Get-CaptureStatus {
    param([Parameter(Mandatory = $true)][string]$SidecarPath)
    $payload = Read-JsonFile -Path $SidecarPath
    if ($null -eq $payload) { throw "Render quality sidecar is missing or invalid: $SidecarPath" }
    $renderer = Get-Property -Object $payload -Name 'renderer'
    if ($null -eq $renderer) { throw "Render quality sidecar has no renderer status: $SidecarPath" }
    return [ordered]@{ payload = $payload; renderer = $renderer }
}

function Get-GraphPassIds {
    param([AllowNull()][object]$Graph)
    $ids = New-Object 'System.Collections.Generic.List[string]'
    foreach ($pass in @((Get-Property -Object $Graph -Name 'passes'))) {
        $id = Get-Property -Object $pass -Name 'id'
        if ($null -eq $id -and $pass -is [string]) { $id = $pass }
        if (-not [string]::IsNullOrWhiteSpace([string]$id)) { [void]$ids.Add([string]$id) }
    }
    return @($ids)
}

function Test-ShadowStatus {
    param(
        [Parameter(Mandatory = $true)]$Status,
        [Parameter(Mandatory = $true)][string]$Backend,
        [Parameter(Mandatory = $true)][string]$Stage
    )
    $renderer = Get-Property -Object $Status -Name 'renderer'
    $device = Get-Property -Object $renderer -Name 'device'
    $surface = Get-Property -Object $renderer -Name 'surface'
    $graph = Get-Property -Object $renderer -Name 'graph'
    $shadow = Get-Property -Object $renderer -Name 'shadow'
    $local = Get-Property -Object $renderer -Name 'localShadow'
    $decision = Get-Property -Object $renderer -Name 'shadowScalability'
    $cpuRecords = Get-Property -Object $renderer -Name 'cpuRecordMicroseconds'

    Add-Check "$Stage.renderer-schema" ((Get-Property $renderer 'schemaVersion') -eq $RendererSchema) $Stage 'Renderer Status must publish the current v30 contract.' (Get-Property $renderer 'schemaVersion') $RendererSchema
    Add-Check "$Stage.backend" ((Get-Property $device 'backend') -eq $Backend) $Stage "Renderer backend must be $Backend." (Get-Property $device 'backend') $Backend
    Add-Check "$Stage.artifact" ((Get-Property $device 'artifactStatus') -eq 'manifest-and-artifact-verified' -and (Get-Property $device 'shaderArtifact') -eq $ExpectedArtifacts[$Backend]) $Stage 'The selected backend must use a verified native shader artifact.' ([ordered]@{ status = Get-Property $device 'artifactStatus'; shaderArtifact = Get-Property $device 'shaderArtifact' }) ([ordered]@{ status = 'manifest-and-artifact-verified'; shaderArtifact = $ExpectedArtifacts[$Backend] })
    $surfacePass = [int](Get-Property $surface 'width') -eq $Width -and [int](Get-Property $surface 'height') -eq $Height -and [int](Get-Property $surface 'renderWidth') -eq $Width -and [int](Get-Property $surface 'renderHeight') -eq $Height
    Add-Check "$Stage.fixed-resolution" $surfacePass $Stage 'Renderer Status must remain at the fixed 1920x1080 surface and render extent.' ([ordered]@{ width = Get-Property $surface 'width'; height = Get-Property $surface 'height'; renderWidth = Get-Property $surface 'renderWidth'; renderHeight = Get-Property $surface 'renderHeight' }) @($Width, $Height)
    $graphPassIds = Get-GraphPassIds -Graph $graph
    $missingPasses = @($RequiredPasses | Where-Object { $graphPassIds -notcontains $_ })
    $graphPass = (Get-Property $graph 'graphId') -eq $GraphId -and (Get-Property $graph 'schemaVersion') -eq $GraphSchema -and [bool](Get-Property $graph 'valid') -and @((Get-Property $graph 'errors')).Count -eq 0
    Add-Check "$Stage.graph" $graphPass $Stage 'Shadow evidence must execute in the valid v17 Render Graph.' ([ordered]@{ graphId = Get-Property $graph 'graphId'; schemaVersion = Get-Property $graph 'schemaVersion'; valid = Get-Property $graph 'valid'; errors = @((Get-Property $graph 'errors')).Count }) ([ordered]@{ graphId = $GraphId; schemaVersion = $GraphSchema; valid = $true; errors = 0 })
    Add-Check "$Stage.graph-passes" ($missingPasses.Count -eq 0) $Stage 'The Render Graph must contain the real shadow-depth and opaque-lit passes.' $missingPasses $RequiredPasses
    $decisionRecommendation = [string](Get-Property $decision 'recommendation')
    $decisionPass = (Get-Property $decision 'schema') -eq 'noemancer.shadow-scalability-policy/0.1' -and
        @('keep-atlas', 'extend-atlas', 'prototype-virtual-pages', 'insufficient-evidence') -contains $decisionRecommendation -and
        -not [string]::IsNullOrWhiteSpace([string](Get-Property $decision 'fingerprint'))
    Add-Check "$Stage.policy" $decisionPass $Stage 'Renderer Status must project the Engine-owned bounded shadow scalability decision instead of a script-owned duplicate.' ([ordered]@{ schema = Get-Property $decision 'schema'; recommendation = $decisionRecommendation; valid = Get-Property $decision 'valid'; evidenceComplete = Get-Property $decision 'evidenceComplete'; fingerprint = Get-Property $decision 'fingerprint' }) 'stable schema, bounded recommendation, fingerprint'

    $csmCascadeCount = Convert-ToNonNegativeInt64 (Get-Property $shadow 'cascadeCount')
    $csmAvailable = Convert-ToNonNegativeInt64 (Get-Property $shadow 'cascadesAvailable')
    $csmRendered = Convert-ToNonNegativeInt64 (Get-Property $shadow 'cascadesRendered')
    $csmCached = Convert-ToNonNegativeInt64 (Get-Property $shadow 'cascadesCached')
    $csmResolution = Convert-ToNonNegativeInt64 (Get-Property $shadow 'resolutionPerCascade')
    $csmTextureBytes = Convert-ToNonNegativeInt64 (Get-Property $shadow 'textureBytes')
    $csmHits = Convert-ToNonNegativeInt64 (Get-Property $shadow 'cacheHitsTotal')
    $csmMisses = Convert-ToNonNegativeInt64 (Get-Property $shadow 'cacheMissesTotal')
    $csmDrawsSaved = Convert-ToNonNegativeInt64 (Get-Property $shadow 'drawCallsSaved')
    $csmDrawsPerCascade = @((Get-Property $shadow 'drawCallsPerCascade'))
    $csmSavedPerCascade = @((Get-Property $shadow 'drawCallsSavedPerCascade'))
    $csmCulledPerCascade = @((Get-Property $shadow 'culledPerCascade'))
    $csmExpectedBytes = [int64]$CsmCascadeCount * $CsmResolution * $CsmResolution * $ExpectedBytesPerD32Texel
    $csmShapePass = $null -ne $shadow -and $csmCascadeCount -eq $CsmCascadeCount -and $csmAvailable -eq $CsmCascadeCount -and $csmRendered + $csmCached -eq $csmAvailable -and $csmResolution -gt 0
    Add-Check "$Stage.csm-layers" $csmShapePass $Stage 'Directional CSM must publish all configured layers and split rendered versus cached layers.' ([ordered]@{ cascadeCount = $csmCascadeCount; available = $csmAvailable; rendered = $csmRendered; cached = $csmCached; resolution = $csmResolution }) ([ordered]@{ cascadeCount = $CsmCascadeCount; available = $CsmCascadeCount; renderedPlusCached = $CsmCascadeCount })
    $csmCountersPass = $null -ne $csmHits -and $null -ne $csmMisses -and $csmDrawsSaved -ge 0 -and $csmDrawsPerCascade.Count -eq $CsmCascadeCount -and $csmSavedPerCascade.Count -eq $CsmCascadeCount -and $csmCulledPerCascade.Count -eq $CsmCascadeCount
    Add-Check "$Stage.csm-counters" $csmCountersPass $Stage 'CSM cache hit/miss and avoided-submission counters must be present and non-negative.' ([ordered]@{ cacheHitsTotal = $csmHits; cacheMissesTotal = $csmMisses; drawCallsSaved = $csmDrawsSaved; drawCallsPerCascade = $csmDrawsPerCascade.Count; drawCallsSavedPerCascade = $csmSavedPerCascade.Count; culledPerCascade = $csmCulledPerCascade.Count }) 'all counters present; four-element per-cascade arrays'
    $csmWorkingSetPass = $null -ne $csmTextureBytes -and $csmTextureBytes -gt 0 -and (Get-Property $shadow 'format') -eq 'D32_FLOAT' -and $csmExpectedBytes -eq $csmTextureBytes
    Add-Check "$Stage.csm-working-set" $csmWorkingSetPass $Stage 'CSM working set must be reported as actual D32 texture bytes; the derived value is only a consistency estimate.' ([ordered]@{ actualTextureBytes = $csmTextureBytes; derivedExpectedTextureBytes = $csmExpectedBytes; bytesPerTexel = $ExpectedBytesPerD32Texel; format = Get-Property $shadow 'format' }) ([ordered]@{ actualTextureBytes = $csmExpectedBytes; format = 'D32_FLOAT'; derivedMatchesActual = $true })

    $localEnabled = Get-Property $local 'enabled'
    $localQuality = [string](Get-Property $local 'quality')
    $expectedLocalResolution = [int64]$LocalResolutionByQuality[$ShadowQuality]
    $localCapacity = Convert-ToNonNegativeInt64 (Get-Property $local 'layerCapacity')
    $localAvailable = Convert-ToNonNegativeInt64 (Get-Property $local 'facesAvailable')
    $localRendered = Convert-ToNonNegativeInt64 (Get-Property $local 'facesRendered')
    $localCached = Convert-ToNonNegativeInt64 (Get-Property $local 'facesCached')
    $localResolution = Convert-ToNonNegativeInt64 (Get-Property $local 'resolutionPerLayer')
    $localTextureBytes = Convert-ToNonNegativeInt64 (Get-Property $local 'textureBytes')
    $localSelected = Convert-ToNonNegativeInt64 (Get-Property $local 'selectedLights')
    $localRequested = Convert-ToNonNegativeInt64 (Get-Property $local 'requestedLights')
    $localDropped = Convert-ToNonNegativeInt64 (Get-Property $local 'droppedLights')
    $localHits = Convert-ToNonNegativeInt64 (Get-Property $local 'cacheHitsTotal')
    $localMisses = Convert-ToNonNegativeInt64 (Get-Property $local 'cacheMissesTotal')
    $localDrawsSaved = Convert-ToNonNegativeInt64 (Get-Property $local 'drawCallsSaved')
    $localExpectedBytes = [int64]$LocalLayerCapacity * $localResolution * $localResolution * $ExpectedBytesPerD32Texel
    $localShapePass = $null -ne $local -and [bool]$localEnabled -and $localQuality -eq $ShadowQuality -and $localCapacity -eq $LocalLayerCapacity -and $localResolution -eq $expectedLocalResolution -and $localAvailable -eq ($localRendered + $localCached) -and $localAvailable -gt 0 -and $localSelected -gt 0 -and $localSelected -le $localRequested -and $localDropped -ge 0
    Add-Check "$Stage.local-layers" $localShapePass $Stage 'Local shadow must publish bounded point/spot layers and rendered versus cached faces.' ([ordered]@{ enabled = $localEnabled; quality = $localQuality; resolutionPerLayer = $localResolution; layerCapacity = $localCapacity; facesAvailable = $localAvailable; facesRendered = $localRendered; facesCached = $localCached; requestedLights = $localRequested; selectedLights = $localSelected; droppedLights = $localDropped }) ([ordered]@{ enabled = $true; quality = $ShadowQuality; resolutionPerLayer = $expectedLocalResolution; layerCapacity = $LocalLayerCapacity; facesAvailable = 'facesRendered+facesCached>0' })
    $localCountersPass = $null -ne $localHits -and $null -ne $localMisses -and $null -ne $localDrawsSaved -and $localDrawsSaved -ge 0
    Add-Check "$Stage.local-counters" $localCountersPass $Stage 'Local shadow cache hits/misses and avoided-submission counters must be actual non-negative telemetry.' ([ordered]@{ cacheHitsTotal = $localHits; cacheMissesTotal = $localMisses; drawCallsSaved = $localDrawsSaved }) 'all counters present and non-negative'
    $localWorkingSetPass = $null -ne $localTextureBytes -and $localTextureBytes -gt 0 -and (Get-Property $local 'format') -eq 'D32_FLOAT' -and $localExpectedBytes -eq $localTextureBytes
    Add-Check "$Stage.local-working-set" $localWorkingSetPass $Stage 'Local shadow working set must be reported as actual D32 texture bytes; derived bytes are not telemetry.' ([ordered]@{ actualTextureBytes = $localTextureBytes; derivedExpectedTextureBytes = $localExpectedBytes; bytesPerTexel = $ExpectedBytesPerD32Texel; format = Get-Property $local 'format' }) ([ordered]@{ actualTextureBytes = $localExpectedBytes; format = 'D32_FLOAT'; derivedMatchesActual = $true })

    $cpuShadow = Convert-ToFiniteDouble (Get-Property $cpuRecords 'render.pass.shadow-depth')
    return [ordered]@{
        schemaVersion = Get-Property $renderer 'schemaVersion'
        graph = [ordered]@{ graphId = Get-Property $graph 'graphId'; schemaVersion = Get-Property $graph 'schemaVersion'; passIds = $graphPassIds }
        device = [ordered]@{ backend = Get-Property $device 'backend'; shaderArtifact = Get-Property $device 'shaderArtifact'; artifactStatus = Get-Property $device 'artifactStatus' }
        csm = [ordered]@{
            layers = [ordered]@{ configured = $CsmCascadeCount; available = $csmAvailable; rendered = $csmRendered; cached = $csmCached; resolution = $csmResolution }
            cache = [ordered]@{ hitsTotal = $csmHits; missesTotal = $csmMisses }
            avoidedSubmissions = [ordered]@{ actualDrawCallsSaved = $csmDrawsSaved; actualDrawCallsSavedPerCascade = $csmSavedPerCascade; estimates = [ordered]@{ avoidedInstances = Get-Property $shadow 'avoidedInstancesEstimate'; avoidedDraws = Get-Property $shadow 'avoidedDrawsEstimate' } }
            workingSet = [ordered]@{ actualTextureBytes = $csmTextureBytes; derivedExpectedTextureBytes = $csmExpectedBytes; derivedIsEstimate = $true; format = Get-Property $shadow 'format' }
        }
        local = [ordered]@{
            layers = [ordered]@{ configured = $LocalLayerCapacity; available = $localAvailable; rendered = $localRendered; cached = $localCached; resolution = $localResolution }
            lights = [ordered]@{ requested = $localRequested; selected = $localSelected; dropped = $localDropped; point = Get-Property $local 'pointLights'; spot = Get-Property $local 'spotLights' }
            cache = [ordered]@{ hitsTotal = $localHits; missesTotal = $localMisses }
            avoidedSubmissions = [ordered]@{ actualDrawCallsSaved = $localDrawsSaved; estimates = [ordered]@{ avoidedInstances = Get-Property $local 'avoidedInstancesEstimate'; avoidedDraws = Get-Property $local 'avoidedDrawsEstimate' } }
            workingSet = [ordered]@{ actualTextureBytes = $localTextureBytes; derivedExpectedTextureBytes = $localExpectedBytes; derivedIsEstimate = $true; format = Get-Property $local 'format' }
        }
        cpu = [ordered]@{ shadowDepthRecordMicroseconds = $cpuShadow; source = 'renderer.cpuRecordMicroseconds.render.pass.shadow-depth'; isGpuTiming = $false }
        decision = $decision
    }
}

function Get-GpuTimestampEvidence {
    param(
        [Parameter(Mandatory = $true)]$Performance,
        [Parameter(Mandatory = $true)][string]$Backend,
        [Parameter(Mandatory = $true)][string]$Stage,
        [Parameter(Mandatory = $true)][int]$MinimumSamples
    )
    $schema = Get-Property $Performance 'schemaVersion'
    $runtimeBackend = Get-PathProperty $Performance 'runtime.backend'
    $workloadWidth = Get-PathProperty $Performance 'workload.resolution.width'
    $workloadHeight = Get-PathProperty $Performance 'workload.resolution.height'
    $gpu = Get-Property $Performance 'gpu'
    $timestamps = Get-Property $gpu 'passTimestamps'
    Add-Check "$Stage.schema" ($schema -eq $PerformanceSchema) $Stage 'Performance evidence must use the stable performance schema.' $schema $PerformanceSchema
    Add-Check "$Stage.backend" ($runtimeBackend -eq $Backend) $Stage 'Performance telemetry backend must match the selected backend.' $runtimeBackend $Backend
    Add-Check "$Stage.resolution" ([int]$workloadWidth -eq $Width -and [int]$workloadHeight -eq $Height) $Stage 'Performance workload must remain at 1920x1080.' ([ordered]@{ width = $workloadWidth; height = $workloadHeight }) @($Width, $Height)
    $available = Convert-ToNonNegativeInt64 (Get-Property $timestamps 'availableFrameCount')
    $gpuAvailable = [bool](Get-Property $gpu 'available')
    Add-Check "$Stage.gpu-available" ($gpuAvailable -and $null -ne $timestamps -and $available -gt 0) $Stage 'Shadow performance requires actual GPU timestamp telemetry; CPU time cannot substitute.' ([ordered]@{ available = $gpuAvailable; availableFrameCount = $available; source = Get-Property $gpu 'source' }) 'available=true, availableFrameCount>0, source present'
    $timestampContract = $null -ne $timestamps -and (Get-Property $timestamps 'schemaVersion') -eq $GpuTimestampSchema -and [bool](Get-Property $timestamps 'supported') -and (Get-Property $timestamps 'queueScope') -eq 'graphics' -and (Get-Property $timestamps 'readback') -eq 'fence-gated-frame-ring'
    Add-Check "$Stage.gpu-timestamp-contract" $timestampContract $Stage 'GPU timestamps must be supported graphics-queue values read from the fence-gated frame ring.' ([ordered]@{ schemaVersion = Get-Property $timestamps 'schemaVersion'; supported = Get-Property $timestamps 'supported'; queueScope = Get-Property $timestamps 'queueScope'; readback = Get-Property $timestamps 'readback' }) ([ordered]@{ schemaVersion = $GpuTimestampSchema; supported = $true; queueScope = 'graphics'; readback = 'fence-gated-frame-ring' })
    $distributions = Get-Property $timestamps 'passDistributions'
    $passSummary = [ordered]@{}
    $requiredShadowPass = $null
    if ($null -ne $distributions) {
        foreach ($property in @($distributions.PSObject.Properties | Sort-Object Name)) {
            $passId = [string]$property.Name
            $distribution = $property.Value
            $sampleCount = Convert-ToNonNegativeInt64 (Get-Property $distribution 'sampleCount')
            $p95 = Convert-ToFiniteDouble (Get-Property $distribution 'p95')
            $mean = Convert-ToFiniteDouble (Get-Property $distribution 'mean')
            $minimum = Convert-ToFiniteDouble (Get-Property $distribution 'min')
            $maximum = Convert-ToFiniteDouble (Get-Property $distribution 'max')
            $unit = [string](Get-Property $distribution 'unit')
            $valid = $null -ne $sampleCount -and $sampleCount -gt 0 -and $null -ne $p95 -and $p95 -ge 0 -and $null -ne $mean -and $null -ne $minimum -and $null -ne $maximum -and $unit -eq 'milliseconds'
            Add-Check "$Stage.pass-$($passId.Replace('.', '-'))" $valid $Stage "GPU timestamp distribution for $passId must contain finite millisecond samples." ([ordered]@{ sampleCount = $sampleCount; p95 = $p95; mean = $mean; min = $minimum; max = $maximum; unit = $unit }) 'sampleCount>0 and finite millisecond statistics'
            $passSummary[$passId] = [ordered]@{ sampleCount = $sampleCount; p95Milliseconds = $p95; meanMilliseconds = $mean; minMilliseconds = $minimum; maxMilliseconds = $maximum; unit = $unit; source = 'gpu.passTimestamps.passDistributions'; isGpuTiming = $true }
            if ($passId -eq 'render.pass.shadow-depth') { $requiredShadowPass = $passSummary[$passId] }
        }
    }
    $requiredPassValid = $null -ne $requiredShadowPass -and [int64]$requiredShadowPass.sampleCount -ge $MinimumSamples
    Add-Check "$Stage.shadow-depth" $requiredPassValid $Stage 'The real render.pass.shadow-depth GPU distribution must meet the requested sample count.' ([ordered]@{ sampleCount = if ($null -eq $requiredShadowPass) { $null } else { $requiredShadowPass.sampleCount }; p95Milliseconds = if ($null -eq $requiredShadowPass) { $null } else { $requiredShadowPass.p95Milliseconds } }) ([ordered]@{ sampleCount = ">=$MinimumSamples"; unit = 'milliseconds'; source = 'GPU timestamps' })
    $cpuFrameP95 = Convert-ToFiniteDouble (Get-PathProperty $Performance 'cpu.frameTime.p95')
    Add-Check "$Stage.cpu-frame-observation" ($null -ne $cpuFrameP95 -and $cpuFrameP95 -ge 0) $Stage 'CPU frame p95 is recorded as a diagnostic observation and cannot satisfy GPU timing.' ([ordered]@{ p95Milliseconds = $cpuFrameP95; source = 'cpu.frameTime.p95'; isGpuTiming = $false }) 'finite CPU frame p95'
    return [ordered]@{
        schemaVersion = $schema
        source = Get-Property $gpu 'source'
        available = $gpuAvailable
        availableFrameCount = $available
        timestampContract = [ordered]@{ schemaVersion = Get-Property $timestamps 'schemaVersion'; supported = Get-Property $timestamps 'supported'; queueScope = Get-Property $timestamps 'queueScope'; readback = Get-Property $timestamps 'readback'; resolve = Get-Property $timestamps 'resolve'; queriesPerPass = Get-Property $timestamps 'queriesPerPass' }
        passDistributions = $passSummary
        shadowDepth = $requiredShadowPass
        cpuFrame = [ordered]@{ p95Milliseconds = $cpuFrameP95; source = 'cpu.frameTime.p95'; isGpuTiming = $false }
    }
}

function Get-ExitCode {
    foreach ($issue in $script:Issues.ToArray()) {
        if ([string]$issue.code -like 'preflight.*' -or [string]$issue.stage -like 'preflight*') { return 2 }
    }
    return 5
}

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$runtimePath = if ([string]::IsNullOrWhiteSpace($Runtime)) { Join-Path $repositoryRoot "build/windows-msvc-debug/src/runtime/$Config/noemancer.exe" } else { [IO.Path]::GetFullPath($Runtime) }

try {
    if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
        $OutputRoot = Join-Path $repositoryRoot "generated/acceptance/shadow-scalability-vsm-$(Get-Date -Format yyyyMMdd-HHmmss)"
    }
    $OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
    if (Test-Path -LiteralPath $OutputRoot) {
        if (Get-ChildItem -LiteralPath $OutputRoot -Force | Select-Object -First 1) { throw "Shadow evidence output must be empty because receipts are immutable: $OutputRoot" }
    } else { [IO.Directory]::CreateDirectory($OutputRoot) | Out-Null }
    $script:ReceiptPath = Join-Path $OutputRoot 'shadow-scalability-decision-evidence.json'

    $nativeWindowContract = Ensure-NativeWindowHelper
    Add-Check 'preflight.hidden-window' $nativeWindowContract 'preflight' 'Windows hidden-window control must be available before launching GPU workloads.' ([ordered]@{ available = $nativeWindowContract; error = $script:NativeWindowError }) 'user32.ShowWindow or runtime SDL_WINDOW_HIDDEN'
    $uniqueBackends = @($GpuBackends | Select-Object -Unique)
    $dualBackendPass = $uniqueBackends -contains 'direct3d12' -and $uniqueBackends -contains 'vulkan'
    Add-Check 'preflight.dual-backend' $dualBackendPass 'preflight' 'The decision receipt must cover both D3D12 and Vulkan.' $uniqueBackends @('direct3d12', 'vulkan')
    Add-Check 'preflight.fixed-resolution' ($Width -eq 1920 -and $Height -eq 1080) 'preflight' 'Decision capture resolution is fixed and cannot be reduced to hide a budget miss.' @($Width, $Height) @(1920, 1080)
    Add-Check 'preflight.runtime' (Test-Path -LiteralPath $runtimePath -PathType Leaf) 'preflight' 'Selected Runtime executable must exist; this script never builds or patches Runtime.' $runtimePath 'file exists'
    Add-Check 'preflight.stress-instances' ($StressInstances -ge 32 -and $StressInstances -le 4096) 'preflight' 'The deterministic shadow pressure fixture must remain within its bounded caster range.' $StressInstances '32..4096'
    Add-Check 'preflight.gpu-sample-window' ($MinimumGpuPassSamples -le $PerformanceSampleFrames) 'preflight' 'The required completed GPU samples must fit inside the measured frame window.' ([ordered]@{ required = $MinimumGpuPassSamples; measuredFrames = $PerformanceSampleFrames }) 'required<=measuredFrames'
    if (-not (Test-Path -LiteralPath $runtimePath -PathType Leaf)) { throw "Runtime is missing: $runtimePath" }
    if (@($script:Issues | Where-Object { $_.stage -eq 'preflight' }).Count -gt 0) {
        throw 'Preflight contract failed; no GPU workload was launched.'
    }

    $runs = New-Object 'System.Collections.Generic.List[object]'
    foreach ($backend in $uniqueBackends) {
        $backendRoot = Join-Path $OutputRoot $backend
        [IO.Directory]::CreateDirectory($backendRoot) | Out-Null
        $imagePath = Join-Path $backendRoot 'shadow-scalability-1920x1080.bmp'
        $sidecarPath = "$imagePath.quality.json"
        $performancePath = Join-Path $backendRoot 'shadow-scalability.performance-evidence.json'
        $captureStdout = Join-Path $backendRoot 'capture.stdout.jsonl'
        $captureStderr = Join-Path $backendRoot 'capture.stderr.log'
        $performanceStdout = Join-Path $backendRoot 'performance.stdout.jsonl'
        $performanceStderr = Join-Path $backendRoot 'performance.stderr.log'

        $commonArguments = @('run', '--format', 'json', '--shadow-scalability-stress-instances', [string]$StressInstances, '--window-width', [string]$Width, '--window-height', [string]$Height, '--exposure', '1.0', '--render-scale', '1.0', '--disable-auto-exposure', '--shadow-quality', $ShadowQuality, '--gpu-backend', $backend)
        $captureArguments = @($commonArguments) + @('--frames', [string]$CaptureFrames, '--capture-frame', $imagePath)
        $performanceArguments = @($commonArguments) + @('--performance-evidence', $performancePath, '--performance-hidden', '--performance-workload', 'noemancer.shadow-scalability-vsm-decision/0.1', '--performance-warmup-frames', [string]$PerformanceWarmupFrames, '--performance-sample-frames', [string]$PerformanceSampleFrames)

        $captureProcess = Invoke-HiddenProcess -Executable $runtimePath -Arguments $captureArguments -WorkingDirectory $repositoryRoot -StdoutPath $captureStdout -StderrPath $captureStderr -TimeoutSeconds $TimeoutSeconds -RuntimeWindowStartsHidden
        $performanceProcess = Invoke-HiddenProcess -Executable $runtimePath -Arguments $performanceArguments -WorkingDirectory $repositoryRoot -StdoutPath $performanceStdout -StderrPath $performanceStderr -TimeoutSeconds $TimeoutSeconds -RuntimeWindowStartsHidden
        Add-CliContractIssueIfRejected -Backend $backend -Purpose 'capture' -ProcessReceipt $captureProcess
        Add-CliContractIssueIfRejected -Backend $backend -Purpose 'performance' -ProcessReceipt $performanceProcess
        Add-Check "$backend.capture-process" ($captureProcess.completed -and $captureProcess.exitCode -eq 0 -and $captureProcess.hiddenWindow) "backend:$backend/capture" 'Hidden shadow capture must complete successfully without a visible window.' $captureProcess 'completed=true, exitCode=0, hiddenWindow=true'
        Add-Check "$backend.performance-process" ($performanceProcess.completed -and $performanceProcess.exitCode -eq 0 -and $performanceProcess.hiddenWindow) "backend:$backend/performance" 'Hidden GPU performance probe must complete successfully without a visible window.' $performanceProcess 'completed=true, exitCode=0, hiddenWindow=true'
        Add-Check "$backend.capture-artifact" ((Test-Path -LiteralPath $imagePath -PathType Leaf) -and (Test-Path -LiteralPath $sidecarPath -PathType Leaf)) "backend:$backend/capture" 'Capture image and render-quality sidecar must both be published.' ([ordered]@{ image = Test-Path -LiteralPath $imagePath -PathType Leaf; sidecar = Test-Path -LiteralPath $sidecarPath -PathType Leaf }) 'image and sidecar exist'
        Add-Check "$backend.performance-artifact" (Test-Path -LiteralPath $performancePath -PathType Leaf) "backend:$backend/performance" 'GPU performance evidence JSON must be published.' (Test-Path -LiteralPath $performancePath -PathType Leaf) $true

        $captureStatusSummary = $null
        if (Test-Path -LiteralPath $sidecarPath -PathType Leaf) {
            try {
                $captureStatus = Get-CaptureStatus -SidecarPath $sidecarPath
                $payload = $captureStatus.payload
                Add-Check "$backend.capture-quality" ((Get-Property $payload 'schemaVersion') -eq $QualitySchema -and [bool](Get-Property $payload 'pass') -and [bool](Get-Property $payload 'dimensionsMatch') -and [int](Get-Property $payload 'width') -eq $Width -and [int](Get-Property $payload 'height') -eq $Height) "backend:$backend/capture" 'Capture sidecar must pass the fixed 1920x1080 render-quality contract.' ([ordered]@{ schemaVersion = Get-Property $payload 'schemaVersion'; pass = Get-Property $payload 'pass'; dimensionsMatch = Get-Property $payload 'dimensionsMatch'; width = Get-Property $payload 'width'; height = Get-Property $payload 'height' }) ([ordered]@{ schemaVersion = $QualitySchema; pass = $true; dimensionsMatch = $true; width = $Width; height = $Height })
                $captureStatusSummary = Test-ShadowStatus -Status $captureStatus -Backend $backend -Stage "backend:$backend/capture-status"
            } catch {
                Add-Issue -Code "$backend.capture-status-parse" -Stage "backend:$backend/capture" -Message $_.Exception.Message -Observed $sidecarPath -Expected 'parseable render-quality sidecar with v30 renderer status'
            }
        } else {
            Add-Issue -Code "$backend.capture-status-missing" -Stage "backend:$backend/capture" -Message 'Missing status sidecar is a contract failure, not a successful empty capture.' -Observed $sidecarPath -Expected 'file exists'
        }

        $gpuSummary = $null
        $performanceStatusSummary = $null
        if (Test-Path -LiteralPath $performancePath -PathType Leaf) {
            try {
                $performance = Read-JsonFile -Path $performancePath
                if ($null -eq $performance) { throw "Performance evidence is not valid JSON: $performancePath" }
                $gpuSummary = Get-GpuTimestampEvidence -Performance $performance -Backend $backend -Stage "backend:$backend/performance" -MinimumSamples $MinimumGpuPassSamples
                $performanceStatusSummary = Test-ShadowStatus -Status $performance -Backend $backend -Stage "backend:$backend/performance-status"
            } catch {
                Add-Issue -Code "$backend.performance-parse" -Stage "backend:$backend/performance" -Message $_.Exception.Message -Observed $performancePath -Expected 'parseable performance evidence with actual GPU pass timestamps'
            }
        } else {
            Add-Issue -Code "$backend.performance-missing" -Stage "backend:$backend/performance" -Message 'Missing performance evidence is a hard telemetry failure.' -Observed $performancePath -Expected 'file exists'
        }

        $artifactRecords = @(
            Get-ArtifactRecord -Root $OutputRoot -Path $imagePath -Kind 'image/bmp'
            Get-ArtifactRecord -Root $OutputRoot -Path $sidecarPath -Kind 'json/render-quality'
            Get-ArtifactRecord -Root $OutputRoot -Path $performancePath -Kind 'json/performance'
            Get-ArtifactRecord -Root $OutputRoot -Path $captureStdout -Kind 'log/stdout-jsonl'
            Get-ArtifactRecord -Root $OutputRoot -Path $captureStderr -Kind 'log/stderr'
            Get-ArtifactRecord -Root $OutputRoot -Path $performanceStdout -Kind 'log/stdout-jsonl'
            Get-ArtifactRecord -Root $OutputRoot -Path $performanceStderr -Kind 'log/stderr'
        )
        $cpuFrameP95 = if ($null -eq $gpuSummary) { $null } else { Get-PathProperty $gpuSummary 'cpuFrame.p95Milliseconds' }
        $gpuShadowP95 = if ($null -eq $gpuSummary -or $null -eq $gpuSummary.shadowDepth) { $null } else { Get-Property $gpuSummary.shadowDepth 'p95Milliseconds' }
        $cpuBudgetPass = $null -ne $cpuFrameP95 -and [double]$cpuFrameP95 -le $CpuFrameP95BudgetMilliseconds
        $gpuBudgetPass = $null -ne $gpuShadowP95 -and [double]$gpuShadowP95 -le $GpuShadowDepthP95BudgetMilliseconds
        Add-Check "$backend.cpu-frame-budget" $cpuBudgetPass "backend:$backend/performance" 'CPU frame budget is recorded as a diagnostic budget observation; the workload is never reduced when it fails.' ([ordered]@{ p95Milliseconds = $cpuFrameP95; budgetMilliseconds = $CpuFrameP95BudgetMilliseconds; isGpuTiming = $false }) ([ordered]@{ withinBudget = $true; budgetMilliseconds = $CpuFrameP95BudgetMilliseconds })
        Add-Check "$backend.gpu-shadow-budget" $gpuBudgetPass "backend:$backend/performance" 'GPU shadow-depth p95 must meet the declared budget; CPU time is not a substitute.' ([ordered]@{ p95Milliseconds = $gpuShadowP95; budgetMilliseconds = $GpuShadowDepthP95BudgetMilliseconds; isGpuTiming = $true }) ([ordered]@{ withinBudget = $true; budgetMilliseconds = $GpuShadowDepthP95BudgetMilliseconds })

        $runs.Add([ordered]@{
            backend = $backend
            shaderArtifact = $ExpectedArtifacts[$backend]
            fixture = [ordered]@{ type = 'runtime-shadow-scalability-stress'; id = 'noemancer.shadow-scalability-stress/0.1'; casterCount = $StressInstances; deterministic = $true; expected = 'directional CSM plus deliberately over-capacity point/spot shadow requests' }
            commands = @(
                [ordered]@{ purpose = 'hidden-shadow-capture'; executable = $runtimePath; arguments = $captureArguments; process = $captureProcess }
                [ordered]@{ purpose = 'hidden-shadow-gpu-performance'; executable = $runtimePath; arguments = $performanceArguments; process = $performanceProcess }
            )
            artifacts = $artifactRecords
            status = $performanceStatusSummary
            captureStatus = $captureStatusSummary
            performance = $gpuSummary
            budgets = [ordered]@{
                cpuFrameP95Milliseconds = $cpuFrameP95; cpuFrameBudgetMilliseconds = $CpuFrameP95BudgetMilliseconds; cpuWithinBudget = $cpuBudgetPass; cpuIsGpuTiming = $false
                gpuShadowDepthP95Milliseconds = $gpuShadowP95; gpuShadowDepthBudgetMilliseconds = $GpuShadowDepthP95BudgetMilliseconds; gpuWithinBudget = $gpuBudgetPass; gpuIsGpuTiming = $true
                decision = if ($null -eq $gpuSummary) { 'telemetry-unavailable' } elseif ($cpuBudgetPass -and $gpuBudgetPass) { 'within-budget' } else { 'budget-failed' }
            }
        })
    }

    $manifest = [ordered]@{
        schemaVersion = $EvidenceSchema
        capturedAt = [DateTimeOffset]::UtcNow.ToString('o')
        pass = ($script:Issues.Count -eq 0)
        configuration = [ordered]@{
            config = $Config; runtime = $runtimePath; requestedResolution = [ordered]@{ width = $Width; height = $Height }
            workload = 'noemancer.shadow-scalability-stress/0.1'; stressInstances = $StressInstances; shadowQuality = $ShadowQuality; gpuBackends = $uniqueBackends
            captureFrames = $CaptureFrames; performanceWarmupFrames = $PerformanceWarmupFrames; performanceSampleFrames = $PerformanceSampleFrames; minimumGpuPassSamples = $MinimumGpuPassSamples; timeoutSeconds = $TimeoutSeconds
            cpuFrameP95BudgetMilliseconds = $CpuFrameP95BudgetMilliseconds; gpuShadowDepthP95BudgetMilliseconds = $GpuShadowDepthP95BudgetMilliseconds
            hiddenProcess = $true; hiddenWindow = $true; computerUse = $false; captureAndPerformanceSeparated = $true
        }
        contract = [ordered]@{
            rendererSchema = $RendererSchema; graphId = $GraphId; graphSchema = $GraphSchema; qualitySchema = $QualitySchema
            requiredGraphPasses = $RequiredPasses; csmCascadeCount = $CsmCascadeCount; csmResolution = $CsmResolution; localLayerCapacity = $LocalLayerCapacity
            gpuTimestampSchema = $GpuTimestampSchema; gpuTimestampSource = 'gpu.passTimestamps.passDistributions'; cpuTimingCannotSubstitute = $true
            actualTelemetryFields = @('shadow.cascadesAvailable', 'shadow.cascadesRendered', 'shadow.cascadesCached', 'shadow.cacheHitsTotal', 'shadow.cacheMissesTotal', 'shadow.drawCallsSaved', 'shadow.textureBytes', 'localShadow.facesAvailable', 'localShadow.facesRendered', 'localShadow.facesCached', 'localShadow.cacheHitsTotal', 'localShadow.cacheMissesTotal', 'localShadow.drawCallsSaved', 'localShadow.textureBytes')
            estimatedFields = @('shadow.avoidedInstancesEstimate', 'shadow.avoidedDrawsEstimate', 'localShadow.avoidedInstancesEstimate', 'localShadow.avoidedDrawsEstimate', 'derivedExpectedTextureBytes')
        }
        runtime = [ordered]@{ path = $runtimePath.Replace('\', '/'); sha256 = Get-FileSha256 -Path $runtimePath; repositoryRoot = $repositoryRoot.Replace('\', '/') }
        runs = @($runs.ToArray())
        checks = @($script:Checks.ToArray())
        issues = @($script:Issues.ToArray())
        policy = 'This decision receipt preserves the requested fixed workload. Actual shadow status counters, actual shadow texture bytes, and real fence-gated GPU pass distributions are separate from named estimates and CPU diagnostics. Performance budget misses are recorded as failures; no capture is downscaled or shortened to manufacture a pass. Missing runtime CLI/status fields are explicit preflight or validation failures.'
    }
    Write-JsonDocument -Path $script:ReceiptPath -Value $manifest
    Write-Output ($manifest | ConvertTo-Json -Depth 100 -Compress)
    if (-not [bool]$manifest.pass) { exit (Get-ExitCode) }
} catch {
    if ($null -ne $script:ReceiptPath) {
        try {
            $failure = [ordered]@{ schemaVersion = $EvidenceSchema; capturedAt = [DateTimeOffset]::UtcNow.ToString('o'); pass = $false; error = $_.Exception.Message; checks = @($script:Checks.ToArray()); issues = @($script:Issues.ToArray()) }
            Write-JsonDocument -Path $script:ReceiptPath -Value $failure
        } catch { }
    }
    Write-Error "Shadow scalability decision evidence failed: $($_.Exception.Message)"
    exit (Get-ExitCode)
}
