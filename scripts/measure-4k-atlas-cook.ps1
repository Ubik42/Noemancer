[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'All')]
    [string]$Configuration = 'Debug',
    [ValidateRange(1, 1800)]
    [int]$TimeoutSeconds = 120,
    [string]$OutputRoot = ''
)

$ErrorActionPreference = 'Stop'
$repositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$engineScript = Join-Path $repositoryRoot 'scripts\engine.ps1'
$target = 'noemancer_ktx2_cook_adapter_tests'
$testArgument = '--pressure-4k'
$width = 4096
$height = 4096
$workerStrategies = @(
    [ordered]@{
        name = 'workers-1'
        argument = '1'
        requested = 1
        label = '1'
    },
    [ordered]@{
        name = 'workers-auto-8'
        argument = 'auto'
        requested = 0
        label = 'auto/8'
    }
)

if (-not (Test-Path -LiteralPath $engineScript -PathType Leaf)) {
    throw "The repository engine helper is missing: $engineScript"
}

if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $repositoryRoot ('generated\acceptance\atlas-cook-performance-' +
        (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
$OutputRoot = [System.IO.Path]::GetFullPath($OutputRoot)
if (Test-Path -LiteralPath $OutputRoot) {
    throw "Performance evidence output already exists: $OutputRoot"
}
New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null

function Get-OptionalFileHash([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-DescendantProcessIds([int]$RootProcessId) {
    $all = @()
    try {
        $all = @(Get-CimInstance Win32_Process |
            Select-Object ProcessId, ParentProcessId, Name, ExecutablePath)
    } catch {
        return @($RootProcessId)
    }

    $ids = [System.Collections.Generic.HashSet[int]]::new()
    $null = $ids.Add($RootProcessId)
    $changed = $true
    while ($changed) {
        $changed = $false
        foreach ($process in $all) {
            $processId = [int]$process.ProcessId
            $parentId = [int]$process.ParentProcessId
            if ($ids.Contains($parentId) -and $ids.Add($processId)) {
                $changed = $true
            }
        }
    }
    return @($ids)
}

function Stop-OwnedProcessTree([System.Diagnostics.Process]$Process) {
    $rootId = $Process.Id
    $ownedIds = [System.Collections.Generic.HashSet[int]]::new()
    foreach ($processId in (Get-DescendantProcessIds $rootId)) {
        $null = $ownedIds.Add([int]$processId)
    }

    # A child can appear between the first process snapshot and termination.
    # Refresh once before killing and once after killing so cleanup is explicit.
    Start-Sleep -Milliseconds 100
    foreach ($processId in (Get-DescendantProcessIds $rootId)) {
        $null = $ownedIds.Add([int]$processId)
    }
    foreach ($processId in @($ownedIds | Sort-Object -Descending)) {
        try {
            $ownedProcess = Get-Process -Id $processId -ErrorAction Stop
            if (-not $ownedProcess.HasExited) {
                Stop-Process -Id $processId -Force -ErrorAction SilentlyContinue
            }
        } catch {
            # The process may have exited during the cleanup race.
        }
    }

    Start-Sleep -Milliseconds 200
    $residual = @()
    foreach ($processId in $ownedIds) {
        try {
            $ownedProcess = Get-Process -Id $processId -ErrorAction Stop
            if (-not $ownedProcess.HasExited) { $residual += $processId }
        } catch {
            # No process with this owned PID remains.
        }
    }
    return [ordered]@{
        attempted = $true
        ownedProcessIds = @($ownedIds | Sort-Object)
        residualProcessIds = @($residual | Sort-Object)
        clean = ($residual.Count -eq 0)
    }
}

function Get-MachineEvidence {
    $processor = $null
    $computer = $null
    try {
        $processor = Get-CimInstance Win32_Processor |
            Select-Object -First 1 Name, NumberOfCores, NumberOfLogicalProcessors, MaxClockSpeed
    } catch { }
    try {
        $computer = Get-CimInstance Win32_ComputerSystem |
            Select-Object Manufacturer, Model, TotalPhysicalMemory
    } catch { }
    return [ordered]@{
        computerName = [Environment]::MachineName
        operatingSystem = [Environment]::OSVersion.VersionString
        powershell = $PSVersionTable.PSVersion.ToString()
        processor = $processor
        computerSystem = $computer
    }
}

function Get-PropertyValue($Object, [string[]]$Names) {
    if ($null -eq $Object) { return $null }
    foreach ($name in $Names) {
        $property = $Object.PSObject.Properties[$name]
        if ($null -ne $property) { return $property.Value }
    }
    return $null
}

function Get-StructuredProbe([string]$Stdout) {
    if ([string]::IsNullOrWhiteSpace($Stdout)) { return $null }
    $lines = @($Stdout -split "`r?`n" | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    for ($index = $lines.Count - 1; $index -ge 0; --$index) {
        $line = $lines[$index].Trim()
        if (-not $line.StartsWith('{')) { continue }
        try {
            $candidate = $line | ConvertFrom-Json
            if ($null -ne $candidate) { return $candidate }
        } catch {
            # The probe may have printed a diagnostic line before its JSON.
        }
    }
    return $null
}

function Get-StageTimings($Block) {
    if ($null -eq $Block) { return @() }
    $timing = Get-PropertyValue $Block @('timing', 'timings')
    $stages = if ($null -ne $timing) {
        Get-PropertyValue $timing @('stages', 'stageTimings', 'stage_timings')
    } else {
        Get-PropertyValue $Block @('stages', 'stageTimings', 'stage_timings')
    }
    if ($null -eq $stages) { return @() }
    $normalized = @()
    foreach ($stage in @($stages)) {
        $name = Get-PropertyValue $stage @('name', 'stage')
        $microseconds = Get-PropertyValue $stage @('microseconds', 'microSeconds', 'durationMicroseconds')
        $milliseconds = Get-PropertyValue $stage @('milliseconds', 'durationMilliseconds')
        if ($null -eq $microseconds -and $null -ne $milliseconds) {
            $microseconds = [double]$milliseconds * 1000.0
        }
        $normalized += [ordered]@{
            name = $name
            microseconds = if ($null -ne $microseconds) { [double]$microseconds } else { $null }
            milliseconds = if ($null -ne $microseconds) { [double]$microseconds / 1000.0 } else {
                if ($null -ne $milliseconds) { [double]$milliseconds } else { $null }
            }
        }
    }
    return $normalized
}

function Get-PhaseSummary($Block, $FallbackRoot, [string]$Role) {
    $source = if ($null -ne $Block) { $Block } else { $FallbackRoot }
    $timing = if ($null -ne $source) { Get-PropertyValue $source @('timing', 'timings') } else { $null }
    $totalMicroseconds = if ($null -ne $timing) {
        Get-PropertyValue $timing @('totalMicroseconds', 'total_microseconds', 'microseconds')
    } else { $null }
    if ($null -eq $totalMicroseconds -and $null -ne $source) {
        $totalMicroseconds = Get-PropertyValue $source @('totalMicroseconds', 'total_microseconds')
    }
    $milliseconds = if ($null -ne $source) {
        Get-PropertyValue $source @('milliseconds', 'durationMilliseconds', "${Role}Milliseconds")
    } else { $null }
    if ($null -eq $totalMicroseconds -and $null -ne $milliseconds) {
        $totalMicroseconds = [double]$milliseconds * 1000.0
    }
    $payload = if ($null -ne $source) { Get-PropertyValue $source @('payload') } else { $null }
    $fingerprint = if ($null -ne $source) {
        Get-PropertyValue $source @('payloadFingerprint', 'payload_fingerprint', "${Role}PayloadFingerprint")
    } else { $null }
    if ($null -eq $fingerprint -and $null -ne $payload) {
        $fingerprint = Get-PropertyValue $payload @('fingerprint', 'payloadFingerprint')
    }
    $stages = @(Get-StageTimings $source)
    return [ordered]@{
        valid = if ($null -ne $source) { Get-PropertyValue $source @('valid', 'success') } else { $null }
        cacheHit = if ($null -ne $source) { Get-PropertyValue $source @('cacheHit', 'cache_hit') } else { $null }
        totalMicroseconds = if ($null -ne $totalMicroseconds) { [double]$totalMicroseconds } else { $null }
        milliseconds = if ($null -ne $totalMicroseconds) { [double]$totalMicroseconds / 1000.0 } else { $null }
        payloadFingerprint = $fingerprint
        payloadBytes = if ($null -ne $payload) { Get-PropertyValue $payload @('bytes', 'payloadBytes') } else {
            if ($null -ne $source) { Get-PropertyValue $source @('payloadBytes', "${Role}PayloadBytes") } else { $null }
        }
        stages = $stages
    }
}

function Get-ProbeSummary($Probe) {
    if ($null -eq $Probe) { return $null }
    $first = Get-PropertyValue $Probe @('firstCook', 'first', 'firstResult', 'firstProduct')
    $repeat = Get-PropertyValue $Probe @('repeatCook', 'repeat', 'repeatResult', 'repeatProduct')
    $rootTiming = Get-PropertyValue $Probe @('timing', 'timings')
    if ($null -eq $first -and $null -ne $rootTiming) {
        $first = Get-PropertyValue $rootTiming @('first', 'firstCook', 'firstResult')
    }
    if ($null -eq $repeat -and $null -ne $rootTiming) {
        $repeat = Get-PropertyValue $rootTiming @('repeat', 'repeatCook', 'repeatResult')
    }
    $firstSummary = Get-PhaseSummary $first $Probe 'first'
    $repeatSummary = Get-PhaseSummary $repeat $Probe 'repeat'
    $sharedFingerprint = Get-PropertyValue $Probe @('payloadFingerprint', 'payload_fingerprint')
    if ([string]::IsNullOrWhiteSpace([string]$firstSummary.payloadFingerprint) -and
        -not [string]::IsNullOrWhiteSpace([string]$sharedFingerprint)) {
        $firstSummary.payloadFingerprint = $sharedFingerprint
    }
    if ([string]::IsNullOrWhiteSpace([string]$repeatSummary.payloadFingerprint) -and
        -not [string]::IsNullOrWhiteSpace([string]$sharedFingerprint)) {
        # The C++ probe compares first.payload and repeat.payload before it
        # publishes this shared fingerprint. Preserve that contract explicitly
        # instead of implying a second independently emitted hash.
        $repeatSummary.payloadFingerprint = $sharedFingerprint
        $repeatSummary.fingerprintSource = 'probe-payload-identity-check'
    }
    $sharedPayloadBytes = Get-PropertyValue $Probe @('payloadBytes', 'payload_bytes')
    if ($null -eq $firstSummary.payloadBytes -and $null -ne $sharedPayloadBytes) {
        $firstSummary.payloadBytes = $sharedPayloadBytes
    }
    if ($null -eq $repeatSummary.payloadBytes -and $null -ne $sharedPayloadBytes) {
        $repeatSummary.payloadBytes = $sharedPayloadBytes
    }
    $probeError = Get-PropertyValue $Probe @('error', 'errorCode', 'error_code')
    $probeCode = Get-PropertyValue $Probe @('code', 'status')
    if ([string]::IsNullOrWhiteSpace([string]$probeError) -and
        -not [string]::IsNullOrWhiteSpace([string]$probeCode) -and
        [string]$probeCode -notin @('ok', 'success', 'pass')) {
        $probeError = $probeCode
    }
    return [ordered]@{
        schema = Get-PropertyValue $Probe @('schema', 'schemaVersion')
        requestedWorkerCount = Get-PropertyValue $Probe @('requestedWorkerCount', 'requestedWorkers', 'workersRequested')
        workerCount = Get-PropertyValue $Probe @('workerCount', 'resolvedWorkerCount', 'workers')
        error = $probeError
        first = $firstSummary
        repeat = $repeatSummary
        raw = $Probe
    }
}

function Invoke-CookMeasurement([string]$Config, $Strategy, [string]$OutputDirectory) {
    $configRoot = Join-Path $repositoryRoot "build\windows-msvc-debug\tests\$Config"
    $executable = Join-Path $configRoot "$target.exe"
    $fileStem = "$($Config.ToLowerInvariant())-$($Strategy.name)"
    $stdoutPath = Join-Path $OutputDirectory "$fileStem.stdout.log"
    $stderrPath = Join-Path $OutputDirectory "$fileStem.stderr.log"
    $buildLogPath = Join-Path $OutputDirectory "$($Config.ToLowerInvariant()).build.log"
    $arguments = @($testArgument, [string]$Strategy.argument)
    $commandText = "`"$executable`" $testArgument $($Strategy.argument)"
    $build = [ordered]@{ attempted = $false; exitCode = $null; log = $null }

    if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
        if ($Config -eq 'Release') {
            return [ordered]@{
                configuration = $Config
                workerStrategy = [ordered]@{ name = $Strategy.name; argument = $Strategy.argument; requested = $Strategy.requested; label = $Strategy.label }
                status = 'unavailable'
                reason = 'Release executable is not present; the script does not build a full Release tree implicitly.'
                executable = $executable
                executablePresent = $false
                command = $commandText
                exitCode = $null
                timeoutSeconds = $TimeoutSeconds
                timedOut = $false
                atlas = [ordered]@{ width = $width; height = $height; input = 'RGBA8'; mipmaps = $true; compression = 'basis-lz' }
                phases = [ordered]@{}
                fingerprintComparison = [ordered]@{ first = $null; repeat = $null; available = $false; firstRepeatEqual = $false }
                probe = $null
                hashes = [ordered]@{}
                cleanup = [ordered]@{ attempted = $false; clean = $true; ownedProcessIds = @(); residualProcessIds = @() }
                build = $build
            }
        }

        $build.attempted = $true
        $build.log = Split-Path -Leaf $buildLogPath
        try {
            & $engineScript check -Config $Config -Target $target *> $buildLogPath
            $build.exitCode = $LASTEXITCODE
        } catch {
            $build.exitCode = 1
            $_ | Out-File -LiteralPath $buildLogPath -Encoding utf8
        }
        if (($build.exitCode -ne 0) -or -not (Test-Path -LiteralPath $executable -PathType Leaf)) {
            return [ordered]@{
                configuration = $Config
                workerStrategy = [ordered]@{ name = $Strategy.name; argument = $Strategy.argument; requested = $Strategy.requested; label = $Strategy.label }
                status = 'fail'
                reason = 'The existing engine.ps1 check could not provide the KTX2 pressure executable.'
                executable = $executable
                executablePresent = (Test-Path -LiteralPath $executable -PathType Leaf)
                command = $commandText
                exitCode = $build.exitCode
                timeoutSeconds = $TimeoutSeconds
                timedOut = $false
                atlas = [ordered]@{ width = $width; height = $height; input = 'RGBA8'; mipmaps = $true; compression = 'basis-lz' }
                phases = [ordered]@{}
                fingerprintComparison = [ordered]@{ first = $null; repeat = $null; available = $false; firstRepeatEqual = $false }
                probe = $null
                hashes = [ordered]@{ buildLogSha256 = Get-OptionalFileHash $buildLogPath }
                cleanup = [ordered]@{ attempted = $false; clean = $true; ownedProcessIds = @(); residualProcessIds = @() }
                build = $build
            }
        }
    }

    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    $process = $null
    $timedOut = $false
    $cleanup = [ordered]@{ attempted = $false; clean = $true; ownedProcessIds = @(); residualProcessIds = @() }
    $exitCode = $null
    $startError = $null
    try {
        $process = Start-Process -FilePath $executable -ArgumentList $arguments `
            -WorkingDirectory $repositoryRoot -WindowStyle Hidden -RedirectStandardOutput $stdoutPath `
            -RedirectStandardError $stderrPath -PassThru
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            $timedOut = $true
            $cleanup = Stop-OwnedProcessTree $process
            try { $process.Refresh(); $exitCode = $process.ExitCode } catch { }
        } else {
            $process.Refresh()
            $exitCode = $process.ExitCode
        }
    } catch {
        $startError = $_.Exception.Message
    } finally {
        $stopwatch.Stop()
        if ($null -ne $process -and -not $process.HasExited) {
            $cleanup = Stop-OwnedProcessTree $process
        } elseif ($null -ne $process -and $cleanup.ownedProcessIds.Count -eq 0) {
            # A normal exit still gets an explicit owned-PID check.
            $cleanup = [ordered]@{
                attempted = $true
                ownedProcessIds = @($process.Id)
                residualProcessIds = @()
                clean = $true
            }
        }
        if ($null -ne $process) { $process.Dispose() }
    }

    $stdout = if (Test-Path -LiteralPath $stdoutPath) {
        [string](Get-Content -LiteralPath $stdoutPath -Raw -Encoding UTF8)
    } else { [string]::Empty }
    $stderr = if (Test-Path -LiteralPath $stderrPath) {
        [string](Get-Content -LiteralPath $stderrPath -Raw -Encoding UTF8)
    } else { [string]::Empty }
    if ($null -eq $stdout) { $stdout = [string]::Empty }
    if ($null -eq $stderr) { $stderr = [string]::Empty }
    $probe = Get-StructuredProbe $stdout
    $probeSummary = Get-ProbeSummary $probe
    $firstMilliseconds = $null
    $repeatMilliseconds = $null
    $payloadBytes = $null
    $match = [regex]::Match($stdout, 'first_ms=(?<first>[0-9]+)\s+repeat_ms=(?<repeat>[0-9]+)\s+payload_bytes=(?<payload>[0-9]+)')
    if ($match.Success) {
        $firstMilliseconds = [int64]$match.Groups['first'].Value
        $repeatMilliseconds = [int64]$match.Groups['repeat'].Value
        $payloadBytes = [int64]$match.Groups['payload'].Value
    }

    $structuredFirst = if ($null -ne $probeSummary) { $probeSummary.first } else { $null }
    $structuredRepeat = if ($null -ne $probeSummary) { $probeSummary.repeat } else { $null }
    $probeError = if ($null -ne $probeSummary) { $probeSummary.error } else { $null }
    $hasStructuredPhases = $null -ne $probeSummary -and
        $null -ne $structuredFirst -and $null -ne $structuredRepeat -and
        ($null -ne $structuredFirst.totalMicroseconds -or $null -ne $structuredFirst.milliseconds) -and
        ($null -ne $structuredRepeat.totalMicroseconds -or $null -ne $structuredRepeat.milliseconds)
    if ($hasStructuredPhases) {
        $firstMilliseconds = $structuredFirst.milliseconds
        $repeatMilliseconds = $structuredRepeat.milliseconds
        if ($null -ne $structuredFirst.payloadBytes) { $payloadBytes = $structuredFirst.payloadBytes }
    }
    $firstFingerprint = if ($null -ne $structuredFirst) { $structuredFirst.payloadFingerprint } else { $null }
    $repeatFingerprint = if ($null -ne $structuredRepeat) { $structuredRepeat.payloadFingerprint } else { $null }
    $fingerprintAvailable = -not [string]::IsNullOrWhiteSpace([string]$firstFingerprint) -and
        -not [string]::IsNullOrWhiteSpace([string]$repeatFingerprint)
    $fingerprintsEqual = $fingerprintAvailable -and ($firstFingerprint -eq $repeatFingerprint)

    $status = if ($timedOut) { 'timeout' }
        elseif ($null -ne $startError) { 'fail' }
        elseif ($stdout -match 'KTX unavailable; 4K pressure skipped') { 'unavailable' }
        elseif ($stdout -match '"code":"asset\.ktx2-empty"' -and $stdout -match '"valid":false') { 'unavailable' }
        elseif (-not [string]::IsNullOrWhiteSpace([string]$probeError)) { 'unavailable' }
        elseif ($exitCode -eq 0 -and $hasStructuredPhases) { 'pass' }
        elseif ($exitCode -eq 0 -and -not $hasStructuredPhases -and
            $stdout -match 'ktx2_cook_adapter_tests: ok') { 'unavailable' }
        else { 'fail' }
    $reason = if ($timedOut) { "Cook process exceeded the $TimeoutSeconds second budget and was terminated." }
        elseif ($null -ne $startError) { $startError }
        elseif ($status -eq 'unavailable' -and $stdout -match '"code":"asset\.ktx2-empty"') {
            'The selected executable does not expose the --pressure-4k test entry point (the existing binary appears stale).'
        }
        elseif ($status -eq 'unavailable' -and $stdout -match 'ktx2_cook_adapter_tests: ok') {
            'The selected executable does not expose the structured --pressure-4k worker probe (the existing binary appears stale).'
        }
        elseif ($status -eq 'unavailable' -and -not [string]::IsNullOrWhiteSpace([string]$probeError)) {
            "The pressure probe reported an unavailable Cook dependency or input: $probeError."
        }
        elseif ($status -eq 'unavailable') { 'The existing test reported that the KTX dependency is unavailable.' }
        elseif ($status -eq 'fail') { 'The pressure test did not return a parseable structured first/repeat Cook result.' }
        else { 'The existing KTX2 pressure test completed successfully.' }

    return [ordered]@{
        configuration = $Config
        workerStrategy = [ordered]@{ name = $Strategy.name; argument = $Strategy.argument; requested = $Strategy.requested; label = $Strategy.label }
        status = $status
        reason = $reason
        executable = $executable
        executablePresent = $true
        command = $commandText
        exitCode = $exitCode
        timeoutSeconds = $TimeoutSeconds
        timedOut = $timedOut
        wallClockMilliseconds = $stopwatch.ElapsedMilliseconds
        atlas = [ordered]@{ width = $width; height = $height; input = 'RGBA8'; mipmaps = $true; compression = 'basis-lz' }
        phases = [ordered]@{
            firstCook = if ($null -ne $structuredFirst -and $hasStructuredPhases) { $structuredFirst } elseif ($null -ne $firstMilliseconds) {
                [ordered]@{ milliseconds = $firstMilliseconds; source = 'legacy test stdout'; stages = @() }
            } else { $null }
            repeatCook = if ($null -ne $structuredRepeat -and $hasStructuredPhases) { $structuredRepeat } elseif ($null -ne $repeatMilliseconds) {
                [ordered]@{ milliseconds = $repeatMilliseconds; source = 'legacy test stdout'; stages = @() }
            } else { $null }
            payloadBytes = $payloadBytes
        }
        fingerprintComparison = [ordered]@{
            first = $firstFingerprint
            repeat = $repeatFingerprint
            available = $fingerprintAvailable
            firstRepeatEqual = $fingerprintsEqual
        }
        probe = if ($null -ne $probeSummary) {
            [ordered]@{
                schema = $probeSummary.schema
                requestedWorkerCount = $probeSummary.requestedWorkerCount
                workerCount = $probeSummary.workerCount
                structured = $hasStructuredPhases
                raw = $probeSummary.raw
            }
        } else { $null }
        hashes = [ordered]@{
            executableSha256 = Get-OptionalFileHash $executable
            stdoutSha256 = Get-OptionalFileHash $stdoutPath
            stderrSha256 = Get-OptionalFileHash $stderrPath
            buildLogSha256 = Get-OptionalFileHash $buildLogPath
        }
        artifacts = [ordered]@{
            stdout = Split-Path -Leaf $stdoutPath
            stderr = Split-Path -Leaf $stderrPath
            buildLog = if (Test-Path -LiteralPath $buildLogPath) { Split-Path -Leaf $buildLogPath } else { $null }
        }
        cleanup = $cleanup
        build = $build
    }
}

$configurations = if ($Configuration -eq 'All') { @('Debug', 'Release') } else { @($Configuration) }
$runs = @()
foreach ($config in $configurations) {
    foreach ($strategy in $workerStrategies) {
        $runs += Invoke-CookMeasurement $config $strategy $OutputRoot
    }
}

$comparisons = @()
foreach ($config in $configurations) {
    $configRuns = @($runs | Where-Object { $_.configuration -eq $config })
    $single = $configRuns | Where-Object { $_.workerStrategy.name -eq 'workers-1' } | Select-Object -First 1
    $automatic = $configRuns | Where-Object { $_.workerStrategy.name -eq 'workers-auto-8' } | Select-Object -First 1
    $singleFirst = if ($null -ne $single) { $single.fingerprintComparison.first } else { $null }
    $automaticFirst = if ($null -ne $automatic) { $automatic.fingerprintComparison.first } else { $null }
    $singleRepeat = if ($null -ne $single) { $single.fingerprintComparison.repeat } else { $null }
    $automaticRepeat = if ($null -ne $automatic) { $automatic.fingerprintComparison.repeat } else { $null }
    $firstComparable = -not [string]::IsNullOrWhiteSpace([string]$singleFirst) -and
        -not [string]::IsNullOrWhiteSpace([string]$automaticFirst)
    $repeatComparable = -not [string]::IsNullOrWhiteSpace([string]$singleRepeat) -and
        -not [string]::IsNullOrWhiteSpace([string]$automaticRepeat)
    $comparisons += [ordered]@{
        configuration = $config
        workerStrategies = @('workers-1', 'workers-auto-8')
        payloadFingerprint = [ordered]@{
            workers1First = $singleFirst
            automaticFirst = $automaticFirst
            workers1Repeat = $singleRepeat
            automaticRepeat = $automaticRepeat
            firstComparable = $firstComparable
            repeatComparable = $repeatComparable
            firstEqual = $firstComparable -and ($singleFirst -eq $automaticFirst)
            repeatEqual = $repeatComparable -and ($singleRepeat -eq $automaticRepeat)
        }
        stageTiming = [ordered]@{
            workers1 = if ($null -ne $single) { $single.phases } else { $null }
            automatic = if ($null -ne $automatic) { $automatic.phases } else { $null }
            comparisonAvailable = ($null -ne $single -and $null -ne $automatic -and
                $single.probe.structured -and $automatic.probe.structured)
        }
    }
}

$revision = ''
$dirty = $false
try {
    $revision = (& git -C $repositoryRoot rev-parse HEAD 2>$null).Trim()
    $dirty = -not [string]::IsNullOrWhiteSpace((& git -C $repositoryRoot status --porcelain 2>$null))
} catch { }

$evidence = [ordered]@{
    schemaVersion = 'noemancer.atlas-cook-performance/0.1'
    generatedAtUtc = (Get-Date).ToUniversalTime().ToString('o')
    workload = [ordered]@{
        name = 'ktx2-cook-adapter-pressure'
        testTarget = $target
        testArgument = $testArgument
        workerArguments = @('1', 'auto')
        workerStrategies = @($workerStrategies | ForEach-Object {
            [ordered]@{ name = $_.name; argument = $_.argument; requested = $_.requested; label = $_.label }
        })
        fixedAtlas = [ordered]@{ width = $width; height = $height; input = 'RGBA8'; mipmaps = $true; compression = 'basis-lz' }
    }
    requestedConfiguration = $Configuration
    timeoutSeconds = $TimeoutSeconds
    machine = Get-MachineEvidence
    repository = [ordered]@{
        revision = $revision
        dirty = $dirty
        generationCommand = 'scripts/measure-4k-atlas-cook.ps1'
        engineHelper = 'scripts/engine.ps1'
    }
    runs = @($runs)
    comparisons = @($comparisons)
    summary = [ordered]@{
        pass = (@($runs | Where-Object status -eq 'pass')).Count
        unavailable = (@($runs | Where-Object status -eq 'unavailable')).Count
        timeout = (@($runs | Where-Object status -eq 'timeout')).Count
        fail = (@($runs | Where-Object status -eq 'fail')).Count
        allOwnedProcessesClean = (@($runs | Where-Object { -not $_.cleanup.clean })).Count -eq 0
    }
}

$evidencePath = Join-Path $OutputRoot 'atlas-cook-performance.json'
$evidence | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $evidencePath -Encoding utf8NoBOM

$failed = @($runs | Where-Object { $_.status -in @('fail', 'timeout') })
Write-Output $evidencePath
if ($failed.Count -gt 0) {
    if (@($runs | Where-Object status -eq 'timeout').Count -gt 0) { exit 124 }
    exit 1
}
exit 0
