[CmdletBinding()]
param(
    [string]$Project = 'D:\\3D\\NoemancerHd2dSlice',
    [string]$RuntimePath = (Join-Path $PSScriptRoot '..\\build\\windows-msvc-debug\\src\\runtime\\Debug\\noemancer.exe'),
    [string]$OutputPath = (Join-Path $PSScriptRoot '..\\generated\\acceptance\\managed-compile-cache-evidence.json'),
    [ValidateRange(10, 600)][int]$TimeoutSeconds = 180
)

$ErrorActionPreference = 'Stop'
$script:StageRoot = $null
$script:CacheRoot = $null
$script:Issues = [System.Collections.Generic.List[object]]::new()

function Add-Issue {
    param([Parameter(Mandatory = $true)][string]$Code, [Parameter(Mandatory = $true)][string]$Message)
    [void]$script:Issues.Add([ordered]@{ code = $Code; message = $Message })
}

function Convert-ToSafeObject {
    param([AllowNull()][object]$Value)
    if ($null -eq $Value -or $Value -is [string] -or $Value.GetType().IsPrimitive -or $Value -is [decimal]) { return $Value }
    if ($Value -is [System.Collections.IDictionary]) {
        $object = [ordered]@{}
        foreach ($key in $Value.Keys) { $object[[string]$key] = Convert-ToSafeObject $Value[$key] }
        return $object
    }
    if ($Value -is [System.Collections.IEnumerable]) {
        $array = [System.Collections.Generic.List[object]]::new()
        foreach ($item in $Value) { [void]$array.Add((Convert-ToSafeObject $item)) }
        return @($array)
    }
    $object = [ordered]@{}
    foreach ($property in @($Value.PSObject.Properties)) { $object[$property.Name] = Convert-ToSafeObject $property.Value }
    return $object
}

function Write-Json {
    param([Parameter(Mandatory = $true)][string]$Path, [Parameter(Mandatory = $true)]$Value)
    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    [IO.File]::WriteAllText($Path, ((Convert-ToSafeObject $Value) | ConvertTo-Json -Depth 100) + "`n", [Text.UTF8Encoding]::new($false))
}

function Get-PropertyValue {
    param([AllowNull()][object]$Object, [Parameter(Mandatory = $true)][string]$Name)
    if ($null -eq $Object) { return $null }
    if ($Object -is [System.Collections.IDictionary]) {
        if ($Object.Contains($Name)) { return $Object[$Name] }
        return $null
    }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -ne $property) { return $property.Value }
    return $null
}

function Get-JsonEvents {
    param([Parameter(Mandatory = $true)][string]$Text)
    $events = [System.Collections.Generic.List[object]]::new()
    foreach ($line in ($Text -split "`r?`n")) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        try { $envelope = $line | ConvertFrom-Json -Depth 100 } catch { continue }
        $message = [string](Get-PropertyValue $envelope 'message')
        $payload = $null
        if ($message) { try { $payload = $message | ConvertFrom-Json -Depth 100 } catch { } }
        [void]$events.Add([ordered]@{ event = [string](Get-PropertyValue $envelope 'event'); payload = $payload })
    }
    return @($events)
}

function Invoke-HiddenRuntime {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$Label
    )
    $stdoutPath = Join-Path $script:StageRoot ($Label + '.stdout.jsonl')
    $stderrPath = Join-Path $script:StageRoot ($Label + '.stderr.log')
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = [IO.Path]::GetFullPath($RuntimePath)
    $info.WorkingDirectory = $Root
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.RedirectStandardInput = $true
    $info.Environment['NOEMANCER_MANAGED_COMPILE_CACHE'] = $script:CacheRoot
    foreach ($argument in @('run', '--headless', '--frames', '1', '--format', 'json', '--editor-project-settings', '--project', $Root)) {
        [void]$info.ArgumentList.Add([string]$argument)
    }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $info
    $watch = [Diagnostics.Stopwatch]::StartNew()
    try {
        if (-not $process.Start()) { throw "Could not start Runtime for '$Label'." }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        $timedOut = -not $process.WaitForExit($TimeoutSeconds * 1000)
        if ($timedOut) {
            try { $process.Kill($true) } catch { }
            $process.WaitForExit()
        }
        $stdout = $stdoutTask.GetAwaiter().GetResult()
        $stderr = $stderrTask.GetAwaiter().GetResult()
        [IO.File]::WriteAllText($stdoutPath, $stdout, [Text.UTF8Encoding]::new($false))
        [IO.File]::WriteAllText($stderrPath, $stderr, [Text.UTF8Encoding]::new($false))
        $events = Get-JsonEvents $stdout
        $production = @($events | Where-Object { $_.event -eq 'runtime.production_state' } | Select-Object -Last 1)
        $scripting = if ($production.Count -gt 0) { Get-PropertyValue $production[0].payload 'scripting' } else { $null }
        $compile = Get-PropertyValue $scripting 'lastCompile'
        if ($timedOut -or $process.ExitCode -ne 0 -or $null -eq $compile) {
            Add-Issue -Code 'runtime.compile-observation-missing' -Message "$Label did not produce a successful structured compile observation (exit=$($process.ExitCode), timeout=$timedOut)."
        }
        return [ordered]@{
            label = $Label; pass = (-not $timedOut -and $process.ExitCode -eq 0 -and $null -ne $compile)
            process = [ordered]@{ exitCode = if ($timedOut) { 124 } else { $process.ExitCode }; timedOut = $timedOut; durationMs = [math]::Round($watch.Elapsed.TotalMilliseconds, 2) }
            compile = $compile; stdout = $stdoutPath; stderr = $stderrPath
        }
    } finally { $process.Dispose() }
}

function Copy-ProjectSnapshot {
    param([Parameter(Mandatory = $true)][string]$Source, [Parameter(Mandatory = $true)][string]$Destination)
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    foreach ($entry in (Get-ChildItem -LiteralPath $Source -Force | Where-Object { $_.Name -notin @('.git', 'bin', 'obj', 'generated', '.vs') })) {
        Copy-Item -LiteralPath $entry.FullName -Destination (Join-Path $Destination $entry.Name) -Recurse -Force
    }
}

function Get-SourceHash {
    param([Parameter(Mandatory = $true)][string]$Root)
    $rows = foreach ($file in (Get-ChildItem -LiteralPath $Root -Recurse -File -Force | Where-Object { $_.FullName -notmatch '\\(bin|obj|generated)(\\|$)' } | Sort-Object FullName)) {
        "$([IO.Path]::GetRelativePath($Root, $file.FullName).Replace('\', '/'))|$((Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash)"
    }
    $digest = [Security.Cryptography.SHA256]::HashData([Text.UTF8Encoding]::new($false).GetBytes(($rows -join "`n")))
    return ([BitConverter]::ToString($digest).Replace('-', '').ToLowerInvariant())
}

$status = 'failed'
$runs = [ordered]@{}
$sourceBefore = $null
$sourceAfter = $null
try {
    $resolvedProject = [IO.Path]::GetFullPath((Resolve-Path -LiteralPath $Project -ErrorAction Stop).Path)
    $resolvedRuntime = [IO.Path]::GetFullPath((Resolve-Path -LiteralPath $RuntimePath -ErrorAction Stop).Path)
    $script:StageRoot = Join-Path ([IO.Path]::GetTempPath()) ('noemancer-managed-cache-evidence-' + [Guid]::NewGuid().ToString('N'))
    $script:CacheRoot = Join-Path $script:StageRoot 'cache'
    $sourceBefore = Get-SourceHash $resolvedProject
    $firstRoot = Join-Path $script:StageRoot 'first'
    $secondRoot = Join-Path $script:StageRoot 'second'
    $changedRoot = Join-Path $script:StageRoot 'changed'
    Copy-ProjectSnapshot $resolvedProject $firstRoot
    Copy-ProjectSnapshot $resolvedProject $secondRoot
    Copy-ProjectSnapshot $resolvedProject $changedRoot
    $mutation = Get-ChildItem -LiteralPath (Join-Path $changedRoot 'scripts') -Recurse -File -Filter '*.cs' | Select-Object -First 1
    if ($null -eq $mutation) { throw 'No project-owned C# file was found for invalidation probe.' }
    [IO.File]::AppendAllText($mutation.FullName, "`r`n// managed compile cache invalidation probe`r`n", [Text.UTF8Encoding]::new($false))
    $runs.first = Invoke-HiddenRuntime -Root $firstRoot -Label 'first'
    $runs.second = Invoke-HiddenRuntime -Root $secondRoot -Label 'second'
    $runs.changed = Invoke-HiddenRuntime -Root $changedRoot -Label 'changed'
    $firstCompile = $runs.first.compile
    $secondCompile = $runs.second.compile
    $changedCompile = $runs.changed.compile
    if (-not $runs.first.pass -or [bool](Get-PropertyValue $firstCompile 'cacheHit')) { Add-Issue -Code 'first-run-not-cold' -Message 'The first isolated process did not perform a cold compile.' }
    if (-not $runs.second.pass -or (Get-PropertyValue $secondCompile 'cacheHit') -ne $true -or [string](Get-PropertyValue $secondCompile 'cacheScope') -ne 'disk') { Add-Issue -Code 'second-run-not-disk-hit' -Message 'The second isolated process did not hit the content-addressed disk cache.' }
    if (-not $runs.changed.pass -or [bool](Get-PropertyValue $changedCompile 'cacheHit') -or [string](Get-PropertyValue $changedCompile 'fingerprint') -eq [string](Get-PropertyValue $firstCompile 'fingerprint')) { Add-Issue -Code 'changed-source-not-invalidated' -Message 'Changing a project-owned C# input did not invalidate the cache identity.' }
    $status = if ($script:Issues.Count -eq 0) { 'passed' } else { 'failed' }
} catch { Add-Issue -Code 'script.failed' -Message $_.Exception.ToString() }
finally {
    if (Test-Path -LiteralPath $Project -PathType Container) { try { $sourceAfter = Get-SourceHash ([IO.Path]::GetFullPath($Project)) } catch { } }
    if ($null -ne $sourceBefore -and $null -ne $sourceAfter -and $sourceBefore -ne $sourceAfter) { Add-Issue -Code 'source.modified' -Message 'The source project changed during the cache probe.'; $status = 'failed' }
    $receipt = [ordered]@{
        schema = 'noemancer.managed-compile-cache-evidence/0.1'; success = ($status -eq 'passed' -and $script:Issues.Count -eq 0); status = $status
        project = $Project; runtime = $RuntimePath; sourceSha256 = [ordered]@{ before = $sourceBefore; after = $sourceAfter; unchanged = ($sourceBefore -and $sourceBefore -eq $sourceAfter) }
        runs = $runs; cachePolicy = [ordered]@{ maxFilesPerEntry = 4096; maxBytesPerEntry = 512MB; maxEntries = 32; maxBytes = 256MB; atomic = $true; hidden = $true }
        issueCount = $script:Issues.Count; issues = @($script:Issues)
    }
    try { Write-Json -Path ([IO.Path]::GetFullPath($OutputPath)) -Value $receipt } catch { Write-Error $_ }
    if ($null -ne $script:StageRoot -and (Test-Path -LiteralPath $script:StageRoot -PathType Container)) { try { Remove-Item -LiteralPath $script:StageRoot -Recurse -Force -ErrorAction Stop } catch { } }
}
if ($script:Issues.Count -gt 0) { exit 5 }
exit 0
