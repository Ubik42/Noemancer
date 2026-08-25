[CmdletBinding()]
param(
    [string]$Runtime = '',
    [Parameter(Mandatory = $true)][string]$OutputRoot,
    [ValidateRange(32, 4096)][int]$Instances = 1024,
    [ValidateRange(3, 600)][int]$Frames = 64,
    [ValidateRange(30, 900)][int]$TimeoutSeconds = 180,
    [ValidateRange(0, 10000)][int]$PerformanceWarmupFrames = 32,
    [ValidateRange(60, 10000)][int]$PerformanceSampleFrames = 60,
    [ValidateSet('direct3d12', 'vulkan')][string[]]$Backends = @('direct3d12', 'vulkan')
)

# This is a deliberately independent GPU-occlusion receipt.  It does not
# modify or rely on the older frustum-visibility receipt.  Correctness and
# performance are separate runtime invocations because --performance-evidence
# owns the frame budget and cannot be combined with capture/readback.
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($Runtime)) {
    $Runtime = Join-Path $PSScriptRoot '..\build\windows-msvc-debug\src\runtime\Release\noemancer.exe'
}

$ExpectedRendererSchema = 'noemancer.renderer-status.v29'
$ExpectedGraphId = 'render.graph.forward.v17'
$ExpectedGraphSchema = 'noemancer.render-graph.v11'
$ExpectedReadbackAbi = 'noemancer.gpu-visibility-readback/0.3'
$ExpectedQualitySchema = 'noemancer.render-quality.v1'
$ExpectedSynchronization = 'same-command-buffer-command-index-and-optional-statistics-copy/fenced-one-shot-submit'
$StressArgument = '--gpu-occlusion-stress'
$StressSchema = 'noemancer.gpu-occlusion-stress-scene/0.1'
$EvidenceSchema = 'noemancer.gpu-occlusion-evidence/0.1'
$PerformanceSchema = 'noemancer.performance-evidence/0.1'
$Width = 1920
$Height = 1080
$FrameBudgetMilliseconds = 1000.0 / 60.0

$script:Checks = New-Object 'System.Collections.Generic.List[object]'
$script:Issues = New-Object 'System.Collections.Generic.List[object]'
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
    if (-not $Pass) { Add-Issue -Code $Code -Stage $Stage -Message $Message -Observed $Observed -Expected $Expected }
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
    param([Parameter(Mandatory = $true)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    return ([string](Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash).ToLowerInvariant()
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

function Get-RequiredProperty {
    param([Parameter(Mandatory = $true)][object]$Object, [Parameter(Mandatory = $true)][string]$Name, [Parameter(Mandatory = $true)][string]$Context)
    $value = Get-Property -Object $Object -Name $Name
    if ($null -eq $value) { throw "$Context is missing required property '$Name'." }
    return $value
}

function Get-RequiredBoolean {
    param([Parameter(Mandatory = $true)][object]$Object, [Parameter(Mandatory = $true)][string]$Name, [Parameter(Mandatory = $true)][string]$Context)
    $value = Get-RequiredProperty -Object $Object -Name $Name -Context $Context
    if ($value -isnot [bool]) { throw "$Context.$Name must be a JSON boolean." }
    return [bool]$value
}

function Get-RequiredInteger {
    param([Parameter(Mandatory = $true)][object]$Object, [Parameter(Mandatory = $true)][string]$Name, [Parameter(Mandatory = $true)][string]$Context)
    $value = Get-RequiredProperty -Object $Object -Name $Name -Context $Context
    try { return [int64]$value } catch { throw "$Context.$Name must be an integer." }
}

function Get-RequiredFiniteDouble {
    param([Parameter(Mandatory = $true)][object]$Object, [Parameter(Mandatory = $true)][string]$Name, [Parameter(Mandatory = $true)][string]$Context)
    $value = Get-RequiredProperty -Object $Object -Name $Name -Context $Context
    try {
        $number = [double]$value
        if ([double]::IsNaN($number) -or [double]::IsInfinity($number)) { throw 'non-finite' }
        return $number
    } catch { throw "$Context.$Name must be a finite number." }
}

function ConvertTo-CommandLine {
    param([Parameter(Mandatory = $true)][string[]]$Tokens)
    return (($Tokens | ForEach-Object { '"' + $_.Replace('"', '\"') + '"' }) -join ' ')
}

function Ensure-NativeWindowHelper {
    if ($script:NativeWindowReady) { return $true }
    if ($env:OS -ne 'Windows_NT') {
        $script:NativeWindowError = 'The occlusion receipt requires the Windows hidden-window contract.'
        return $false
    }
    try {
        $typeName = [System.Management.Automation.PSTypeName]'NoemancerOcclusionEvidence.NativeWindow'
        if ($null -eq $typeName.Type) {
            Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace NoemancerOcclusionEvidence {
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
                        [void][NoemancerOcclusionEvidence.NativeWindow]::ShowWindow($handle, 0)
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
            started = $true; completed = -not $timedOut; timedOut = $timedOut
            exitCode = if ($timedOut) { $null } else { [int]$process.ExitCode }
            processId = [int]$process.Id; hiddenWindow = ($hidden -or $RuntimeWindowStartsHidden)
            hiddenWindowEvidence = if ($hidden) { 'user32.ShowWindow(SW_HIDE)' } elseif ($RuntimeWindowStartsHidden) { 'runtime-contract:SDL_WINDOW_HIDDEN' } else { $null }
            durationMilliseconds = ([DateTimeOffset]::UtcNow - $startedAt).TotalMilliseconds
            stdout = $StdoutPath; stderr = $StderrPath
        }
    } finally {
        $process.Dispose()
    }
}

function Get-FinalRendererStatus {
    param([Parameter(Mandatory = $true)][string]$StdoutPath)
    if (-not (Test-Path -LiteralPath $StdoutPath -PathType Leaf)) { throw "Runtime stdout is missing: $StdoutPath" }
    $finalEvent = $null
    foreach ($line in Get-Content -LiteralPath $StdoutPath -Encoding UTF8) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        try {
            $candidate = $line | ConvertFrom-Json
            if ((Get-Property $candidate 'event') -eq 'render.scene.final') { $finalEvent = $candidate }
        } catch { }
    }
    if ($null -eq $finalEvent) { throw 'Final render.scene.final event is missing.' }
    $message = Get-RequiredProperty -Object $finalEvent -Name 'message' -Context 'Final renderer event'
    if ($message -is [string]) {
        try { return ($message | ConvertFrom-Json) } catch { throw 'Final renderer event message is not valid JSON.' }
    }
    return $message
}

function Get-ControlVisibility {
    param([Parameter(Mandatory = $true)][object]$Renderer, [Parameter(Mandatory = $true)][object]$Occlusion, [Parameter(Mandatory = $true)][object]$Readback)
    $drawIds = @((Get-Property $Readback 'actualDrawIds'))
    if ($drawIds.Count -gt 0) {
        $requiredPrefixes = @()
        foreach ($index in 0..3) {
            $requiredPrefixes += "entity.gpu-occlusion-stress.side-control-$index"
            $requiredPrefixes += "entity.gpu-occlusion-stress.front-control-$index"
        }
        $missing = @($requiredPrefixes | Where-Object {
            $prefix = $_
            -not ($drawIds | Where-Object { ([string]$_).StartsWith($prefix, [System.StringComparison]::Ordinal) } | Select-Object -First 1)
        })
        return [ordered]@{ source = 'submission.gpuDriven.readback.actualDrawIds'; value = ($missing.Count -eq 0); missing = $missing }
    }
    foreach ($candidate in @(
        [ordered]@{ source = 'submission.occlusion.controlVisible'; value = (Get-Property $Occlusion 'controlVisible') },
        [ordered]@{ source = 'submission.occlusion.control.visible'; value = (Get-PathProperty $Occlusion 'control.visible') },
        [ordered]@{ source = 'submission.readback.controlVisible'; value = (Get-Property $Readback 'controlVisible') },
        [ordered]@{ source = 'renderer.controlVisible'; value = (Get-Property $Renderer 'controlVisible') }
    )) {
        if ($null -ne $candidate.value -and $candidate.value -is [bool]) { return $candidate }
    }
    return [ordered]@{ source = $null; value = $null }
}

function Get-ShaderArtifacts {
    param([Parameter(Mandatory = $true)][string]$RuntimePath, [Parameter(Mandatory = $true)][string]$Backend)
    $extension = if ($Backend -eq 'direct3d12') { 'dxil' } else { 'spv' }
    $shaderRoot = [System.IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $RuntimePath) '..\shaders'))
    $names = @('gpu_occlusion.comp', 'gpu_visibility.comp', 'scene_gpu_driven.vert', 'scene_lit.frag')
    $artifacts = @()
    foreach ($name in $names) {
        $path = Join-Path $shaderRoot "$name.$extension"
        $artifacts += [ordered]@{ path = $path; exists = (Test-Path -LiteralPath $path -PathType Leaf); sha256 = Get-FileSha256 -Path $path }
    }
    return $artifacts
}

function Add-BackendCheck {
    param([Parameter(Mandatory = $true)][string]$Backend, [Parameter(Mandatory = $true)][string]$Code, [Parameter(Mandatory = $true)][bool]$Pass, [Parameter(Mandatory = $true)][string]$Message, [AllowNull()][object]$Observed, [AllowNull()][object]$Expected)
    Add-Check -Code "$Backend.$Code" -Pass $Pass -Stage "backend:$Backend" -Message $Message -Observed $Observed -Expected $Expected
}

function Invoke-PerformanceProbe {
    param(
        [Parameter(Mandatory = $true)][string]$Backend,
        [Parameter(Mandatory = $true)][string]$RuntimePath,
        [Parameter(Mandatory = $true)][string]$OutputPath,
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [Parameter(Mandatory = $true)][int]$Timeout
    )
    $stdout = Join-Path $OutputPath "$Backend.performance.stdout.jsonl"
    $stderr = Join-Path $OutputPath "$Backend.performance.stderr.log"
    $evidence = Join-Path $OutputPath "$Backend.performance-evidence.json"
    $arguments = @(
        'run', '--format', 'json', '--gpu-backend', $Backend, $StressArgument,
        '--gpu-occlusion-stress-instances', "$Instances", '--enable-gpu-occlusion',
        '--performance-evidence', $evidence, '--performance-hidden',
        '--performance-workload', 'noemancer.gpu-occlusion/0.1',
        '--performance-warmup-frames', "$PerformanceWarmupFrames",
        '--performance-sample-frames', "$PerformanceSampleFrames",
        '--window-width', "$Width", '--window-height', "$Height"
    )
    $run = Invoke-HiddenProcess -Executable $RuntimePath -Arguments $arguments -WorkingDirectory $WorkingDirectory -StdoutPath $stdout -StderrPath $stderr -TimeoutSeconds $Timeout -RuntimeWindowStartsHidden
    $result = [ordered]@{
        backend = $Backend; commandLine = ConvertTo-CommandLine -Tokens (@($RuntimePath) + $arguments); arguments = $arguments
        process = $run; stdout = [System.IO.Path]::GetFileName($stdout); stderr = [System.IO.Path]::GetFileName($stderr)
        stdoutSha256 = Get-FileSha256 -Path $stdout; stderrSha256 = Get-FileSha256 -Path $stderr
        evidence = [System.IO.Path]::GetFileName($evidence); evidenceSha256 = Get-FileSha256 -Path $evidence
        pass = $false; checks = [ordered]@{}
    }
    Add-BackendCheck $Backend 'performance-process' ($run.completed -and $run.exitCode -eq 0) 'Hidden performance probe must complete successfully.' ([ordered]@{ completed = $run.completed; exitCode = $run.exitCode; timedOut = $run.timedOut }) 'completed=true, exitCode=0'
    Add-BackendCheck $Backend 'performance-hidden' ([bool]$run.hiddenWindow) 'Performance probe must remain hidden.' $run.hiddenWindow $true
    $document = $null
    if (Test-Path -LiteralPath $evidence -PathType Leaf) {
        try { $document = Get-Content -LiteralPath $evidence -Raw -Encoding UTF8 | ConvertFrom-Json } catch { Add-BackendCheck $Backend 'performance-json' $false 'Performance evidence must be valid JSON.' $_.Exception.Message 'valid JSON' }
    }
    Add-BackendCheck $Backend 'performance-evidence' ($null -ne $document) 'Performance evidence file must be published.' ([bool](Test-Path -LiteralPath $evidence -PathType Leaf)) $true
    if ($null -ne $document) {
        $schema = Get-Property $document 'schemaVersion'
        $workload = Get-Property $document 'workload'
        $runtime = Get-Property $document 'runtime'
        $gpu = Get-Property $document 'gpu'
        $passTimestamps = Get-Property $gpu 'passTimestamps'
        $availableFrames = if ($null -ne $passTimestamps) { Get-Property $passTimestamps 'availableFrameCount' } else { $null }
        $cpuP95 = Get-PathProperty $document 'cpu.frameTime.p95'
        Add-BackendCheck $Backend 'performance-schema' ($schema -eq $PerformanceSchema) 'Performance evidence schema must be stable.' $schema $PerformanceSchema
        Add-BackendCheck $Backend 'performance-resolution' ((Get-PathProperty $workload 'resolution.width') -eq $Width -and (Get-PathProperty $workload 'resolution.height') -eq $Height) 'Performance evidence must be 1920x1080.' ([ordered]@{ width = (Get-PathProperty $workload 'resolution.width'); height = (Get-PathProperty $workload 'resolution.height') }) ([ordered]@{ width = $Width; height = $Height })
        Add-BackendCheck $Backend 'performance-backend' ((Get-Property $runtime 'backend') -eq $Backend) 'Performance evidence backend must match the requested backend.' (Get-Property $runtime 'backend') $Backend
        Add-BackendCheck $Backend 'gpu-timestamps' ((Get-Property $gpu 'available') -eq $true -and [int64]$availableFrames -gt 0) 'Performance evidence must contain completed GPU pass timestamps.' ([ordered]@{ available = (Get-Property $gpu 'available'); availableFrameCount = $availableFrames }) 'available=true, availableFrameCount>0'
        $finiteCpu = $false
        try { $finiteCpu = ([double]$cpuP95 -gt 0 -and -not [double]::IsInfinity([double]$cpuP95) -and -not [double]::IsNaN([double]$cpuP95)) } catch { $finiteCpu = $false }
        Add-BackendCheck $Backend 'frame-budget' $finiteCpu 'Performance evidence must expose a finite sampled frame-time budget observation.' ([ordered]@{ p95Milliseconds = $cpuP95; budgetMilliseconds = $FrameBudgetMilliseconds }) 'p95Milliseconds is finite and > 0'
        $result.performanceEvidence = [ordered]@{
            schemaVersion = $schema; backend = (Get-Property $runtime 'backend'); gpuAvailable = (Get-Property $gpu 'available')
            gpuTimestampAvailableFrames = $availableFrames; frameTimeP95Milliseconds = $cpuP95
            frameBudgetMilliseconds = $FrameBudgetMilliseconds
            within60HzBudget = $finiteCpu -and ([double]$cpuP95 -le $FrameBudgetMilliseconds)
        }
    } else {
        $result.performanceEvidence = $null
    }
    $result.pass = @($script:Issues | Where-Object { $_.stage -eq "backend:$Backend" -and $_.code -like "$Backend.performance-*" -and -not $_.message.Contains('informational') }).Count -eq 0
    return $result
}

$outputPath = [System.IO.Path]::GetFullPath($OutputRoot)
[System.IO.Directory]::CreateDirectory($outputPath) | Out-Null
$workingDirectory = (Get-Location).Path
$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$sourceRevision = $null
$sourceDirty = $false
try { $sourceRevision = [string](& git -C $repositoryRoot rev-parse HEAD 2>$null); $sourceDirty = [bool](& git -C $repositoryRoot status --short 2>$null) } catch { }

$manifest = [ordered]@{
    schemaVersion = $EvidenceSchema
    capturedAt = [DateTime]::UtcNow.ToString('o')
    pass = $false
    checks = @()
    issues = @()
    workingDirectory = $workingDirectory
    repositoryRoot = $repositoryRoot
    sourceRevision = $sourceRevision
    sourceDirty = $sourceDirty
    contract = [ordered]@{
        stressArgument = $StressArgument; stressSchemaVersion = $StressSchema; deterministic = $true
        rendererSchemaVersion = $ExpectedRendererSchema; graphId = $ExpectedGraphId; graphSchemaVersion = $ExpectedGraphSchema
        readbackAbi = $ExpectedReadbackAbi; synchronization = $ExpectedSynchronization; hiddenWindow = $true
    }
    workload = [ordered]@{
        instances = $Instances; frames = $Frames
        width = $Width; height = $Height; backends = @($Backends)
        expected = [ordered]@{ hizTested = '>0'; hizCulled = '>0'; controlVisible = $true; integrityCounters = 0 }
    }
    runtime = $null
    preflight = $null
    results = @()
}

$fatalError = $null
try {
    $runtimePath = [System.IO.Path]::GetFullPath((Resolve-Path -LiteralPath $Runtime).Path)
    $runtimeConfiguration = if ($runtimePath -match '[\\/]Release[\\/]') { 'Release' } elseif ($runtimePath -match '[\\/]Debug[\\/]') { 'Debug' } else { 'unknown' }
    $manifest.runtime = [ordered]@{ path = $runtimePath; configuration = $runtimeConfiguration; sha256 = Get-FileSha256 -Path $runtimePath }
    Add-Check 'runtime.release' ($runtimeConfiguration -eq 'Release') 'preflight' 'GPU occlusion evidence requires a Release runtime.' $runtimeConfiguration 'Release'
    Add-Check 'runtime.sha256' ($null -ne $manifest.runtime.sha256) 'preflight' 'Release runtime checksum must be recorded.' $manifest.runtime.sha256 'SHA-256'
    Add-Check 'capture.resolution' ($Width -eq 1920 -and $Height -eq 1080) 'preflight' 'Capture resolution is fixed at 1920x1080.' ([ordered]@{ width = $Width; height = $Height }) '1920x1080'
    Add-Check 'capture.hidden-window-helper' (Ensure-NativeWindowHelper) 'preflight' 'Windows hidden-window helper must be available.' $script:NativeWindowError 'user32.ShowWindow'

    $preflightStdout = Join-Path $outputPath 'preflight.stdout.txt'
    $preflightStderr = Join-Path $outputPath 'preflight.stderr.txt'
    $preflightArguments = @('run', '--help')
    $preflight = Invoke-HiddenProcess -Executable $runtimePath -Arguments $preflightArguments -WorkingDirectory $repositoryRoot -StdoutPath $preflightStdout -StderrPath $preflightStderr -TimeoutSeconds 30
    $helpText = ((Get-Content -LiteralPath $preflightStdout -Raw -ErrorAction SilentlyContinue) + (Get-Content -LiteralPath $preflightStderr -Raw -ErrorAction SilentlyContinue))
    $stressAvailable = $helpText.Contains($StressArgument)
    $manifest.preflight = [ordered]@{
        commandLine = ConvertTo-CommandLine -Tokens (@($runtimePath) + $preflightArguments); process = $preflight
        stdout = [System.IO.Path]::GetFileName($preflightStdout); stderr = [System.IO.Path]::GetFileName($preflightStderr)
        stdoutSha256 = Get-FileSha256 -Path $preflightStdout; stderrSha256 = Get-FileSha256 -Path $preflightStderr
        stressArgument = $StressArgument; stressSchemaVersion = $StressSchema; stressAvailable = $stressAvailable
    }
    Add-Check 'preflight.help' ($preflight.completed -and $preflight.exitCode -eq 0) 'preflight' 'Runtime help preflight must complete successfully.' ([ordered]@{ completed = $preflight.completed; exitCode = $preflight.exitCode }) 'completed=true, exitCode=0'
    Add-Check 'preflight.deterministic-occlusion-stress' $stressAvailable 'preflight' 'Runtime must advertise the deterministic GPU occlusion stress workload before any evidence is collected.' $stressAvailable "help contains $StressArgument"

    if ($stressAvailable) {
        foreach ($backend in $Backends) {
            $stdout = Join-Path $outputPath "$backend.stdout.jsonl"
            $stderr = Join-Path $outputPath "$backend.stderr.log"
            $image = Join-Path $outputPath "$backend.bmp"
            $qualityPath = "$image.quality.json"
            $arguments = @(
                'run', '--format', 'json', '--gpu-backend', $backend, $StressArgument,
                '--gpu-occlusion-stress-instances', "$Instances",
                '--enable-gpu-occlusion', '--gpu-visibility-readback', '--capture-frame', $image,
                '--frames', "$Frames", '--window-width', "$Width", '--window-height', "$Height"
            )
            $result = [ordered]@{
                backend = $backend; pass = $false; commandLine = ConvertTo-CommandLine -Tokens (@($runtimePath) + $arguments); arguments = $arguments
                process = $null; rendererSchemaVersion = $null; graph = $null; occlusion = $null; readback = $null
                controlVisible = $null; controlVisibleSource = $null; capture = $null; performance = $null
                shaderArtifacts = $null; stdout = [System.IO.Path]::GetFileName($stdout); stderr = [System.IO.Path]::GetFileName($stderr)
            }
            $backendIssueStart = $script:Issues.Count
            try {
                $run = Invoke-HiddenProcess -Executable $runtimePath -Arguments $arguments -WorkingDirectory $repositoryRoot -StdoutPath $stdout -StderrPath $stderr -TimeoutSeconds $TimeoutSeconds -RuntimeWindowStartsHidden
                $result.process = $run
                Add-BackendCheck $backend 'process' ($run.completed -and $run.exitCode -eq 0) 'Occlusion capture/readback process must complete successfully.' ([ordered]@{ completed = $run.completed; exitCode = $run.exitCode; timedOut = $run.timedOut }) 'completed=true, exitCode=0'
                Add-BackendCheck $backend 'hidden-window' ([bool]$run.hiddenWindow) 'Occlusion capture/readback window must remain hidden.' $run.hiddenWindow $true
                $renderer = Get-FinalRendererStatus -StdoutPath $stdout
                $result.rendererSchemaVersion = Get-Property $renderer 'schemaVersion'
                $device = Get-Property $renderer 'device'
                $graph = Get-Property $renderer 'graph'
                $submission = Get-RequiredProperty -Object $renderer -Name 'submission' -Context 'Renderer status'
                $gpuDriven = Get-RequiredProperty -Object $submission -Name 'gpuDriven' -Context 'Renderer submission'
                $occlusion = Get-RequiredProperty -Object $gpuDriven -Name 'occlusion' -Context 'GPU-driven submission'
                $readback = Get-RequiredProperty -Object $gpuDriven -Name 'readback' -Context 'GPU-driven submission'
                $result.graph = $graph; $result.occlusion = $occlusion; $result.readback = $readback
                $result.shaderArtifacts = Get-ShaderArtifacts -RuntimePath $runtimePath -Backend $backend
                Add-BackendCheck $backend 'renderer-schema' ($result.rendererSchemaVersion -eq $ExpectedRendererSchema) 'Renderer status must use v29.' $result.rendererSchemaVersion $ExpectedRendererSchema
                Add-BackendCheck $backend 'backend' ((Get-Property $device 'backend') -eq $backend) 'Renderer device backend must match the requested backend.' (Get-Property $device 'backend') $backend
                Add-BackendCheck $backend 'surface' ((Get-PathProperty $renderer 'surface.width') -eq $Width -and (Get-PathProperty $renderer 'surface.height') -eq $Height) 'Renderer surface must be 1920x1080.' ([ordered]@{ width = (Get-PathProperty $renderer 'surface.width'); height = (Get-PathProperty $renderer 'surface.height') }) ([ordered]@{ width = $Width; height = $Height })
                Add-BackendCheck $backend 'graph' ((Get-Property $graph 'graphId') -eq $ExpectedGraphId -and (Get-Property $graph 'schemaVersion') -eq $ExpectedGraphSchema -and (Get-Property $graph 'valid') -eq $true -and @((Get-Property $graph 'errors')).Count -eq 0) 'Render Graph must be valid v17 with no errors.' ([ordered]@{ graphId = (Get-Property $graph 'graphId'); schemaVersion = (Get-Property $graph 'schemaVersion'); valid = (Get-Property $graph 'valid'); errors = @((Get-Property $graph 'errors')).Count }) ([ordered]@{ graphId = $ExpectedGraphId; schemaVersion = $ExpectedGraphSchema; valid = $true; errors = 0 })
                $passIds = @((Get-Property $graph 'passes') | ForEach-Object { [string](Get-Property $_ 'id') })
                Add-BackendCheck $backend 'graph-gpu-visibility-pass' ($passIds -contains 'render.pass.gpu-visibility') 'Render Graph must expose the GPU visibility pass.' $passIds 'render.pass.gpu-visibility'
                Add-BackendCheck $backend 'occlusion-requested' ((Get-Property $occlusion 'requested') -eq $true) 'Occlusion must be explicitly requested.' (Get-Property $occlusion 'requested') $true
                Add-BackendCheck $backend 'occlusion-available' ((Get-Property $occlusion 'available') -eq $true) 'Occlusion resources must be available.' (Get-Property $occlusion 'available') $true
                Add-BackendCheck $backend 'occlusion-used' ((Get-Property $occlusion 'usedThisFrame') -eq $true) 'Occlusion must be used for the captured frame.' (Get-Property $occlusion 'usedThisFrame') $true
                $statistics = Get-RequiredProperty -Object $occlusion -Name 'statistics' -Context 'GPU occlusion evidence'
                $candidates = Get-RequiredInteger -Object $statistics -Name 'candidates' -Context 'GPU occlusion statistics'
                $frustumCulled = Get-RequiredInteger -Object $statistics -Name 'frustumCulled' -Context 'GPU occlusion statistics'
                $hizTested = Get-RequiredInteger -Object $statistics -Name 'hizTested' -Context 'GPU occlusion statistics'
                $hizCulled = Get-RequiredInteger -Object $statistics -Name 'hizCulled' -Context 'GPU occlusion statistics'
                $acceptedVisible = Get-RequiredInteger -Object $statistics -Name 'acceptedVisible' -Context 'GPU occlusion statistics'
                $readbackCandidates = Get-RequiredInteger -Object $readback -Name 'candidates' -Context 'GPU visibility readback'
                $readbackVisible = Get-RequiredInteger -Object $readback -Name 'gpuVisible' -Context 'GPU visibility readback'
                Add-BackendCheck $backend 'statistics-frame-local' ($candidates -eq $readbackCandidates -and $acceptedVisible -eq $readbackVisible) 'Occlusion statistics must describe exactly the readback frame rather than an accumulated backing buffer.' ([ordered]@{ candidates = $candidates; readbackCandidates = $readbackCandidates; acceptedVisible = $acceptedVisible; readbackVisible = $readbackVisible }) 'candidates=readback.candidates, acceptedVisible=readback.gpuVisible'
                Add-BackendCheck $backend 'statistics-closure' (($candidates - $frustumCulled) -eq ($hizCulled + $acceptedVisible)) 'The deterministic valid-input workload must close every frustum-visible candidate as either HiZ-culled or accepted.' ([ordered]@{ frustumVisible = ($candidates - $frustumCulled); hizCulled = $hizCulled; acceptedVisible = $acceptedVisible }) 'frustumVisible=hizCulled+acceptedVisible'
                Add-BackendCheck $backend 'hiz-tested' ($hizTested -gt 0) 'HiZ must test at least one candidate.' $hizTested '>0'
                Add-BackendCheck $backend 'hiz-culled' ($hizCulled -gt 0) 'HiZ must cull at least one candidate.' $hizCulled '>0'
                $control = Get-ControlVisibility -Renderer $renderer -Occlusion $occlusion -Readback $readback
                $result.controlVisible = $control.value; $result.controlVisibleSource = $control.source
                Add-BackendCheck $backend 'control-visible' ($control.value -is [bool] -and $control.value) 'Deterministic control object must remain visible.' ([ordered]@{ source = $control.source; visible = $control.value }) 'visible=true'
                $readbackAbi = Get-Property $readback 'abi'
                Add-BackendCheck $backend 'readback-abi' ($readbackAbi -eq $ExpectedReadbackAbi) 'GPU visibility readback ABI must be 0.3.' $readbackAbi $ExpectedReadbackAbi
                Add-BackendCheck $backend 'readback-state' ((Get-Property $readback 'state') -eq 'complete') 'GPU visibility readback must complete.' (Get-Property $readback 'state') 'complete'
                Add-BackendCheck $backend 'readback-occlusion-active' ((Get-Property $readback 'occlusionActive') -eq $true) 'Readback must identify the occlusion-active frame.' (Get-Property $readback 'occlusionActive') $true
                Add-BackendCheck $backend 'conservative-subset' ((Get-Property $readback 'conservativeSubsetMatch') -eq $true -and (Get-Property $readback 'countMatch') -eq $true -and (Get-Property $readback 'match') -eq $true) 'Occlusion-visible indices must be a valid conservative subset with matching counts.' ([ordered]@{ conservativeSubsetMatch = (Get-Property $readback 'conservativeSubsetMatch'); countMatch = (Get-Property $readback 'countMatch'); match = (Get-Property $readback 'match') }) 'conservativeSubsetMatch=true, countMatch=true, match=true'
                Add-BackendCheck $backend 'readback-synchronization' ((Get-Property $readback 'synchronization') -eq $ExpectedSynchronization) 'Readback synchronization must use the v0.3 command-index/statistics contract.' (Get-Property $readback 'synchronization') $ExpectedSynchronization
                $integrityNames = @('invalidBatchCounts', 'mismatchedBatchCounts', 'outOfRangeIndices', 'wrongBatchIndices', 'duplicateIndices', 'unexpectedVisibleIndices')
                $integrity = [ordered]@{}
                foreach ($name in $integrityNames) { $integrity[$name] = Get-RequiredInteger -Object $readback -Name $name -Context 'GPU visibility readback' }
                Add-BackendCheck $backend 'readback-integrity' (@($integrity.Values | Where-Object { $_ -ne 0 }).Count -eq 0) 'Readback integrity counters must all be zero.' $integrity 'all counters = 0'
                Add-BackendCheck $backend 'readback-error' ($null -eq (Get-Property $readback 'error')) 'Readback error must be null.' (Get-Property $readback 'error') $null
                if (-not (Test-Path -LiteralPath $qualityPath -PathType Leaf)) { throw "Capture quality sidecar is missing: $qualityPath" }
                $quality = Get-Content -LiteralPath $qualityPath -Raw -Encoding UTF8 | ConvertFrom-Json
                $qualityPass = (Get-Property $quality 'schemaVersion') -eq $ExpectedQualitySchema -and (Get-Property $quality 'pass') -eq $true -and (Get-Property $quality 'dimensionsMatch') -eq $true -and (Get-Property $quality 'width') -eq $Width -and (Get-Property $quality 'height') -eq $Height
                Add-BackendCheck $backend 'capture-quality' $qualityPass 'Capture quality sidecar must pass at 1920x1080.' ([ordered]@{ schemaVersion = (Get-Property $quality 'schemaVersion'); pass = (Get-Property $quality 'pass'); dimensionsMatch = (Get-Property $quality 'dimensionsMatch'); width = (Get-Property $quality 'width'); height = (Get-Property $quality 'height') }) ([ordered]@{ schemaVersion = $ExpectedQualitySchema; pass = $true; dimensionsMatch = $true; width = $Width; height = $Height })
                $captureArtifacts = [ordered]@{ image = [System.IO.Path]::GetFileName($image); imageSha256 = Get-FileSha256 -Path $image; quality = [System.IO.Path]::GetFileName($qualityPath); qualitySha256 = Get-FileSha256 -Path $qualityPath; stdoutSha256 = Get-FileSha256 -Path $stdout; stderrSha256 = Get-FileSha256 -Path $stderr }
                $captureHashPass = ($null -ne $captureArtifacts.imageSha256 -and $null -ne $captureArtifacts.qualitySha256 -and $null -ne $captureArtifacts.stdoutSha256 -and $null -ne $captureArtifacts.stderrSha256)
                Add-BackendCheck $backend 'capture-checksum' $captureHashPass 'Capture, quality, stdout and stderr checksums must be recorded.' $captureArtifacts 'non-null SHA-256 values'
                $result.capture = $captureArtifacts
                $framePipeline = Get-Property $renderer 'framePipeline'
                $result.timestamps = [ordered]@{ status = (Get-Property $framePipeline 'gpuTimestamp'); supported = (Get-Property $framePipeline 'gpuTimestampQueries'); reason = (Get-Property $framePipeline 'gpuTimestampReason') }
                Add-BackendCheck $backend 'capture-gpu-timestamp-status' ((Get-Property $framePipeline 'gpuTimestampQueries') -eq $true) 'Renderer capture status must advertise GPU timestamp query support.' (Get-Property $framePipeline 'gpuTimestampQueries') $true
                $result.pass = @($script:Issues | Where-Object { $_.stage -eq "backend:$backend" -and -not $_.pass }).Count -eq 0
            } catch {
                Add-BackendCheck $backend 'execution' $false 'Occlusion evidence collection failed without manufacturing a pass.' $_.Exception.Message 'all required evidence fields present'
            }
            $result.stdoutSha256 = Get-FileSha256 -Path $stdout
            $result.stderrSha256 = Get-FileSha256 -Path $stderr
            $backendIssues = @($script:Issues | Select-Object -Skip $backendIssueStart)
            $result.issues = $backendIssues
            $result.pass = ($backendIssues.Count -eq 0)
            $result.performance = $null
            if ($result.pass) {
                try { $result.performance = Invoke-PerformanceProbe -Backend $backend -RuntimePath $runtimePath -OutputPath $outputPath -WorkingDirectory $repositoryRoot -Timeout $TimeoutSeconds } catch { Add-BackendCheck $backend 'performance-execution' $false 'Independent GPU timestamp/performance probe failed.' $_.Exception.Message 'performance evidence published'; $result.performance = $null; $result.pass = $false }
            }
            $result.issues = @($script:Issues | Select-Object -Skip $backendIssueStart)
            $result.pass = ($result.issues.Count -eq 0)
            $manifest.results += $result
        }
    }
} catch {
    $fatalError = $_.Exception.Message
    Add-Issue -Code 'fatal' -Stage 'script' -Message $fatalError -Observed $null -Expected 'evidence manifest'
}

$manifest.checks = @($script:Checks | ForEach-Object { $_ })
$manifest.issues = @($script:Issues | ForEach-Object { $_ })
$manifest.pass = ($script:Issues.Count -eq 0 -and @($manifest.results).Count -eq @($Backends).Count -and @($manifest.results | Where-Object { -not $_.pass }).Count -eq 0)
$manifest.capturedAt = [DateTime]::UtcNow.ToString('o')
$manifest.runtime = if ($null -eq $manifest.runtime) { [ordered]@{ path = $Runtime; configuration = 'unknown'; sha256 = $null } } else { $manifest.runtime }
$manifest.fatalError = $fatalError
$manifest.checksum = [ordered]@{ manifestSourceRevision = $sourceRevision; preflightStdoutSha256 = Get-FileSha256 -Path (Join-Path $outputPath 'preflight.stdout.txt'); preflightStderrSha256 = Get-FileSha256 -Path (Join-Path $outputPath 'preflight.stderr.txt') }
$manifestPath = Join-Path $outputPath 'gpu-occlusion-evidence.json'
Write-JsonDocument -Path $manifestPath -Value $manifest
Write-Output ("GPU occlusion evidence: " + $manifestPath)
Write-Output ("pass=" + $manifest.pass)
if (-not $manifest.pass) { exit 1 }
exit 0
