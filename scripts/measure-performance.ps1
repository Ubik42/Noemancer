[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$PackageRoot,
    [string]$OutputRoot = '',
    [ValidateRange(0,10000)][int]$WarmupFrames = 180,
    [ValidateRange(60,10000)][int]$SampleFrames = 600,
    [ValidateRange(640,7680)][int]$Width = 1920,
    [ValidateRange(360,4320)][int]$Height = 1080,
    [ValidateRange(0,4096)][int]$RenderStressInstances = 0,
    [string]$Workload = '',
    [switch]$DisableGpuDriven,
    [switch]$RequireGpuTelemetry
)

$ErrorActionPreference = 'Stop'
$package = (Resolve-Path -LiteralPath $PackageRoot).Path
$profile = Join-Path $package 'config\game-profile.json'
if (!(Test-Path -LiteralPath $profile)) { throw "Game Profile is missing: $profile" }
$profileDocument = Get-Content -LiteralPath $profile -Raw | ConvertFrom-Json
if([string]::IsNullOrWhiteSpace($Workload)) {
    $Workload=if($RenderStressInstances -gt 0){'noemancer.high-raster/0.1'}else{'lumen-run.vertical-slice/0.1'}
}
$player = Join-Path $package ('bin\' + $profileDocument.executable)
if (!(Test-Path -LiteralPath $player)) { throw "Packaged Player is missing: $player" }

$presentMon = (& (Join-Path $PSScriptRoot 'bootstrap-presentmon.ps1')).Path
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $PSScriptRoot ('..\generated\acceptance\performance-' + (Get-Date -Format yyyyMMdd-HHmmss))
}
if (Test-Path -LiteralPath $OutputRoot) { throw "Performance evidence output already exists: $OutputRoot" }
New-Item -ItemType Directory -Path $OutputRoot | Out-Null
$output = (Resolve-Path -LiteralPath $OutputRoot).Path
$internalJson = Join-Path $output 'engine-internal.json'
$presentCsv = Join-Path $output 'presentmon.csv'
$playerLog = Join-Path $output 'player.stdout.jsonl'
$playerError = Join-Path $output 'player.stderr.jsonl'
$presentLog = Join-Path $output 'presentmon.stdout.log'
$presentError = Join-Path $output 'presentmon.stderr.log'

$suffix = [Guid]::NewGuid().ToString('N')
$readyName = "Local\Noemancer.Performance.Ready.$suffix"
$releaseName = "Local\Noemancer.Performance.Release.$suffix"
$ready = [Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,$readyName)
$release = [Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,$releaseName)
try {
    $arguments = @('player','--profile',$profile,'--performance-evidence',$internalJson,
        '--performance-workload',$Workload,'--performance-warmup-frames',$WarmupFrames,
        '--performance-sample-frames',$SampleFrames,'--window-width',$Width,'--window-height',$Height,
        '--debug-ready',$readyName,'--debug-wait',$releaseName,'--format','json')
    if($RenderStressInstances -gt 0){$arguments+=@('--render-stress-instances',$RenderStressInstances)}
    if($DisableGpuDriven){$arguments+='--disable-gpu-driven'}
    $playerProcess = Start-Process -FilePath $player -ArgumentList $arguments -WorkingDirectory $package `
        -RedirectStandardOutput $playerLog -RedirectStandardError $playerError -PassThru
    if (!$ready.WaitOne([TimeSpan]::FromSeconds(60))) {
        if (!$playerProcess.HasExited) { $playerProcess.Kill($true) }
        throw 'Player did not reach the synchronized performance start gate.'
    }
    # Budget at a conservative 30 FPS so the trace ends cleanly and flushes its
    # CSV even if process-exit tracking is unavailable on a host ETW version.
    $captureSeconds = [Math]::Ceiling(($WarmupFrames + $SampleFrames) / 30.0) + 5
    $presentArguments = @('--process_name',$profileDocument.executable,'--output_file',$presentCsv,'--v2_metrics',
        '--timed',$captureSeconds,'--terminate_after_timed','--no_console_stats','--session_name',"Noemancer-$suffix")
    $presentProcess = Start-Process -FilePath $presentMon -ArgumentList $presentArguments `
        -RedirectStandardOutput $presentLog -RedirectStandardError $presentError -PassThru
    # PresentMon buffers redirected console output, so its text cannot serve as
    # a live readiness event. The Player remains blocked at its native named
    # event while ETW receives a bounded startup interval.
    Start-Sleep -Seconds 3
    if ($presentProcess.HasExited) { throw 'PresentMon exited before the workload was released.' }
    $release.Set() | Out-Null
    $playerProcess.WaitForExit()
    if (!$presentProcess.WaitForExit(($captureSeconds + 10) * 1000)) {
        $presentProcess.Kill($true); throw 'PresentMon timed trace session could not be stopped cleanly.'
    }
    if ($playerProcess.ExitCode -ne 0) { throw "Player performance workload failed with exit code $($playerProcess.ExitCode)." }
    if (!(Test-Path -LiteralPath $internalJson)) { throw 'Player did not publish internal performance evidence.' }
} finally {
    $ready.Dispose(); $release.Dispose()
}

function Get-Distribution([double[]]$Values) {
    $sorted = @($Values | Sort-Object)
    if ($sorted.Count -eq 0) { return $null }
    function Percentile([double]$fraction) {
        $position = $fraction * ($sorted.Count - 1)
        $lower = [Math]::Floor($position); $upper = [Math]::Ceiling($position)
        $weight = $position - $lower
        return $sorted[$lower] * (1.0 - $weight) + $sorted[$upper] * $weight
    }
    $measure = $sorted | Measure-Object -Average -Minimum -Maximum
    [ordered]@{ unit='milliseconds'; sampleCount=$sorted.Count; min=$measure.Minimum; mean=$measure.Average;
        p50=(Percentile 0.50); p95=(Percentile 0.95); p99=(Percentile 0.99); max=$measure.Maximum }
}

$rows = if(Test-Path -LiteralPath $presentCsv){ @(Import-Csv -LiteralPath $presentCsv) }else{ @() }
$measuredRows = @($rows | Select-Object -Skip $WarmupFrames -First $SampleFrames)
if ($RequireGpuTelemetry -and $measuredRows.Count -lt [Math]::Min(60,$SampleFrames)) {
    throw "PresentMon captured only $($measuredRows.Count) measured rows after warmup; evidence is incomplete."
}
$headers = if($rows.Count -gt 0){ @($rows[0].PSObject.Properties.Name) }else{ @() }
$metrics = [ordered]@{}
foreach ($column in @('FrameTime','CPUBusy','CPUWait','GPUTime','GPUBusy','GPUWait','DisplayLatency')) {
    if ($headers -contains $column) {
        $values = @($measuredRows | ForEach-Object { $value=0.0; if([double]::TryParse($_.$column,
            [Globalization.NumberStyles]::Float,[Globalization.CultureInfo]::InvariantCulture,[ref]$value)){ $value } })
        if ($values.Count -gt 0) { $metrics[$column] = Get-Distribution $values }
    }
}

$evidence = Get-Content -LiteralPath $internalJson -Raw | ConvertFrom-Json -AsHashtable
$evidence.schemaVersion = 'noemancer.performance-evidence/0.2'
$gpuAvailable=$metrics.Contains('GPUTime') -or $metrics.Contains('GPUBusy')
$internalGpu = $evidence.gpu
$passTimestampAvailable = $null -ne $internalGpu -and [bool]$internalGpu.available
$evidence.gpu = [ordered]@{ available=($gpuAvailable -or $passTimestampAvailable);
    sources=@('Noemancer SDL_GPU native timestamp adapter','PresentMon/2.4.1');
    passTimestamps=if($null -ne $internalGpu){$internalGpu.passTimestamps}else{$null};
    presentationTelemetry=[ordered]@{ available=$gpuAvailable; source='PresentMon/2.4.1'; capturedRows=$rows.Count;
        measuredRows=$measuredRows.Count; metrics=$metrics; diagnostic=if($gpuAvailable){'ok'}else{
            'PresentMon session completed but this host exposed no target Present rows; presentation GPU milliseconds were not inferred or fabricated.'} } }
$video = @(Get-CimInstance Win32_VideoController | Select-Object Name,DriverVersion,AdapterRAM)
$processor = Get-CimInstance Win32_Processor | Select-Object -First 1 Name,NumberOfCores,NumberOfLogicalProcessors
$evidence.machine = [ordered]@{ operatingSystem=[Environment]::OSVersion.VersionString; processor=$processor; videoControllers=$video }
$evidence.artifacts = [ordered]@{ playerSha256=(Get-FileHash -LiteralPath $player -Algorithm SHA256).Hash.ToLowerInvariant();
    presentMonCsv=if(Test-Path -LiteralPath $presentCsv){'presentmon.csv'}else{$null};
    presentMonStdout='presentmon.stdout.log'; presentMonStderr='presentmon.stderr.log';
    engineInternal='engine-internal.json'; playerLog='player.stdout.jsonl' }
$finalPath = Join-Path $output 'performance-evidence.json'
$evidence | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $finalPath -Encoding utf8NoBOM
[pscustomobject]@{ Success=$true; Evidence=$finalPath; CapturedRows=$rows.Count; MeasuredRows=$measuredRows.Count;
    CpuFrameP95Ms=$evidence.cpu.frameTime.p95; GpuTimeP95Ms=$metrics.GPUTime.p95; GpuBusyP95Ms=$metrics.GPUBusy.p95 }
