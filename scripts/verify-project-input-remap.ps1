[CmdletBinding()]
param(
    [string]$Project = $(if ($env:NOEMANCER_PLATFORMER_PROJECT) { $env:NOEMANCER_PLATFORMER_PROJECT } else { Join-Path ([IO.Path]::GetPathRoot($PSScriptRoot)) '3D\NoemancerPlatformer' }),
    [string]$RuntimePath = '',
    [string]$OutputPath = '',
    [ValidateRange(1, 120)]
    [int]$Frames = 1,
    [ValidateRange(5, 600)]
    [int]$TimeoutSeconds = 180,
    [switch]$KeepStaging
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# This verifier deliberately does not use the interactive application.  It makes a
# disposable project copy, edits one input binding, and exercises only the same CLI
# paths that a CI/headless build can use.
$script:EvidenceSchema = 'noemancer.project-input-remap-evidence/0.1'
$script:AcceptedProjectSchemas = @('noemancer.project/0.1', 'noemancer.project/0.2')
$script:GeneratedGameProfileSchema = 'noemancer.game-profile/0.4'
$script:MaxLogBytes = 16MB
$script:MaxJsonLineBytes = 1MB
$script:MaxJsonObjects = 512
$script:ActionId = 'gameplay.jump'
$script:OldSource = 'keyboard.space'
$script:NewSource = 'keyboard.q'
$script:Commands = [System.Collections.Generic.List[object]]::new()
$script:Issues = [System.Collections.Generic.List[object]]::new()
$script:Logs = [System.Collections.Generic.List[object]]::new()
$script:StagingPath = ''
$script:StagingKept = $false
$script:EvidenceRoot = ''
$script:EvidencePath = ''
$script:SourceProjectFile = ''
$script:SourceProjectHashBefore = ''
$script:SourceProjectHashAfter = ''

function Get-FullPath {
    param([Parameter(Mandatory)][string]$Path)

    return [System.IO.Path]::GetFullPath($Path)
}

function Get-Sha256 {
    param([Parameter(Mandatory)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Required file does not exist: $Path"
    }

    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Add-Issue {
    param(
        [Parameter(Mandatory)][string]$Code,
        [Parameter(Mandatory)][string]$Message,
        [ValidateSet('error', 'warning', 'info')]
        [string]$Severity = 'error',
        [string]$Stage = ''
    )

    $issue = [ordered]@{
        code     = $Code
        message  = $Message
        severity = $Severity
    }
    if (-not [string]::IsNullOrWhiteSpace($Stage)) {
        $issue.stage = $Stage
    }
    [void]$script:Issues.Add([pscustomobject]$issue)
}

function Get-RelativeEvidencePath {
    param([Parameter(Mandatory)][string]$Path)

    $full = Get-FullPath $Path
    $root = (Get-FullPath $script:EvidenceRoot).TrimEnd('\') + '\'
    if ($full.StartsWith($root, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $full.Substring($root.Length).Replace('\', '/')
    }
    return $full
}

function Limit-Text {
    param(
        [AllowNull()][string]$Text,
        [int]$MaxBytes = $script:MaxLogBytes
    )

    if ($null -eq $Text) {
        return [pscustomobject]@{ text = ''; truncated = $false; bytes = 0 }
    }

    $encoding = [System.Text.UTF8Encoding]::new($false)
    $bytes = $encoding.GetByteCount($Text)
    if ($bytes -le $MaxBytes) {
        return [pscustomobject]@{ text = $Text; truncated = $false; bytes = $bytes }
    }

    # Keep the bounded log valid UTF-8 and avoid cutting a surrogate pair.
    $keepChars = [Math]::Min($Text.Length, $MaxBytes)
    while ($keepChars -gt 0 -and $encoding.GetByteCount($Text.Substring(0, $keepChars)) -gt $MaxBytes) {
        $keepChars--
    }
    $limited = if ($keepChars -gt 0) { $Text.Substring(0, $keepChars) } else { '' }
    return [pscustomobject]@{ text = $limited; truncated = $true; bytes = $bytes }
}

function Write-BoundedTextFile {
    param(
        [Parameter(Mandatory)][string]$Path,
        [AllowNull()][string]$Text
    )

    $limited = Limit-Text $Text
    $utf8 = [System.Text.UTF8Encoding]::new($false)
    [System.IO.File]::WriteAllText($Path, $limited.text, $utf8)
    $hash = Get-Sha256 $Path
    $record = [ordered]@{
        path      = Get-RelativeEvidencePath $Path
        bytes     = (Get-Item -LiteralPath $Path).Length
        sha256    = $hash
        truncated = $limited.truncated
    }
    [void]$script:Logs.Add([pscustomobject]$record)
    return [pscustomobject]@{
        path      = $Path
        bytes     = [int64]$record.bytes
        sha256    = $hash
        truncated = [bool]$limited.truncated
    }
}

function Get-JsonLines {
    param([AllowNull()][string]$Text)

    $items = [System.Collections.Generic.List[object]]::new()
    $invalidLines = 0
    $oversizeLines = 0
    if ([string]::IsNullOrEmpty($Text)) {
        return [pscustomobject]@{ items = @(); invalidLines = 0; oversizeLines = 0 }
    }

    foreach ($line in ($Text -split "`r?`n")) {
        if ([string]::IsNullOrWhiteSpace($line)) {
            continue
        }
        if ([System.Text.Encoding]::UTF8.GetByteCount($line) -gt $script:MaxJsonLineBytes) {
            $oversizeLines++
            continue
        }
        try {
            $value = $line | ConvertFrom-Json -Depth 64
            if ($items.Count -lt $script:MaxJsonObjects) {
                [void]$items.Add($value)
            }
        }
        catch {
            $invalidLines++
        }
    }
    return [pscustomobject]@{
        items        = @($items.ToArray())
        invalidLines = $invalidLines
        oversizeLines = $oversizeLines
    }
}

function Get-Event {
    param(
        [AllowEmptyCollection()][object[]]$Items,
        [Parameter(Mandatory)][string]$Name
    )

    foreach ($item in $Items) {
        if ($null -ne $item -and [string]$item.event -eq $Name) {
            return $item
        }
    }
    return $null
}

function Get-InputSources {
    param([Parameter(Mandatory)]$Document)

    $sources = [System.Collections.Generic.List[string]]::new()
    foreach ($action in @($Document.inputActions)) {
        foreach ($binding in @($action.bindings)) {
            if ($null -ne $binding -and $null -ne $binding.source) {
                [void]$sources.Add([string]$binding.source)
            }
        }
    }
    return @($sources.ToArray())
}

function Get-Action {
    param(
        [Parameter(Mandatory)]$Document,
        [Parameter(Mandatory)][string]$Id
    )

    foreach ($action in @($Document.inputActions)) {
        if ($null -ne $action -and [string]$action.id -eq $Id) {
            return $action
        }
    }
    return $null
}

function Test-ContainsString {
    param(
        [Parameter(Mandatory)][object[]]$Values,
        [Parameter(Mandatory)][string]$Value
    )
    foreach ($candidate in $Values) {
        if ([string]$candidate -ceq $Value) {
            return $true
        }
    }
    return $false
}

function Invoke-CapturedRuntime {
    param(
        [Parameter(Mandatory)][string]$Executable,
        [Parameter(Mandatory)][string[]]$Arguments,
        [Parameter(Mandatory)][string]$WorkingDirectory,
        [Parameter(Mandatory)][string]$LogStem
    )

    if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
        throw "Runtime executable does not exist: $Executable"
    }
    if (-not (Test-Path -LiteralPath $WorkingDirectory -PathType Container)) {
        throw "Working directory does not exist: $WorkingDirectory"
    }

    $stdoutPath = Join-Path $script:EvidenceRoot "logs\$LogStem.stdout.jsonl"
    $stderrPath = Join-Path $script:EvidenceRoot "logs\$LogStem.stderr.log"
    $command = [ordered]@{
        executable      = (Get-FullPath $Executable)
        arguments       = @($Arguments)
        workingDirectory = (Get-FullPath $WorkingDirectory)
        stdout          = Get-RelativeEvidencePath $stdoutPath
        stderr          = Get-RelativeEvidencePath $stderrPath
    }
    [void]$script:Commands.Add([pscustomobject]$command)

    $start = [System.Diagnostics.Stopwatch]::StartNew()
    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = Get-FullPath $Executable
    $psi.WorkingDirectory = Get-FullPath $WorkingDirectory
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    foreach ($argument in $Arguments) {
        [void]$psi.ArgumentList.Add([string]$argument)
    }

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $psi
    try {
        if (-not $process.Start()) {
            throw "Process failed to start: $Executable"
        }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        $completed = $process.WaitForExit($TimeoutSeconds * 1000)
        $timedOut = -not $completed
        if ($timedOut) {
            try { $process.Kill($true) } catch { }
            [void]$process.WaitForExit(5000)
        }
        $stdout = $stdoutTask.GetAwaiter().GetResult()
        $stderr = $stderrTask.GetAwaiter().GetResult()
        $exitCode = if ($timedOut) { $null } else { $process.ExitCode }
    }
    finally {
        $start.Stop()
        $process.Dispose()
    }

    $stdoutLog = Write-BoundedTextFile -Path $stdoutPath -Text $stdout
    $stderrLog = Write-BoundedTextFile -Path $stderrPath -Text $stderr
    $stdoutJson = Get-JsonLines $stdout
    return [pscustomobject]@{
        exitCode       = $exitCode
        timedOut       = $timedOut
        durationMs     = [int]$start.ElapsedMilliseconds
        stdout         = $stdoutJson
        stderrText     = $stderr
        stdoutLog      = $stdoutLog
        stderrLog      = $stderrLog
        stdoutBytes    = $stdoutLog.bytes
        stderrBytes    = $stderrLog.bytes
    }
}

function Write-JsonAtomic {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)]$Value
    )

    $directory = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $directory -PathType Container)) {
        [void](New-Item -ItemType Directory -Path $directory -Force)
    }
    $tempPath = "$Path.tmp-$([guid]::NewGuid().ToString('N'))"
    $utf8 = [System.Text.UTF8Encoding]::new($false)
    try {
        [System.IO.File]::WriteAllText($tempPath, ($Value | ConvertTo-Json -Depth 64), $utf8)
        Move-Item -LiteralPath $tempPath -Destination $Path -Force
    }
    finally {
        if (Test-Path -LiteralPath $tempPath -PathType Leaf) {
            Remove-Item -LiteralPath $tempPath -Force
        }
    }
}

function Assert-SafeTempTarget {
    param([Parameter(Mandatory)][string]$Target)

    $tempRoot = (Get-FullPath ([System.IO.Path]::GetTempPath())).TrimEnd('\')
    $full = (Get-FullPath $Target).TrimEnd('\')
    if ($full -eq $tempRoot -or -not $full.StartsWith("$tempRoot\", [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove a staging path outside the temporary directory: $Target"
    }
}

function Remove-StagingSafely {
    if ([string]::IsNullOrWhiteSpace($script:StagingPath)) {
        return
    }
    if (-not (Test-Path -LiteralPath $script:StagingPath)) {
        return
    }
    Assert-SafeTempTarget $script:StagingPath
    Remove-Item -LiteralPath $script:StagingPath -Recurse -Force
}

function Read-ProjectJson {
    param([Parameter(Mandatory)][string]$Path)
    return (Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json -Depth 64)
}

function Get-ModuleState {
    param(
        [Parameter(Mandatory)]$Event,
        [Parameter(Mandatory)][string]$ModuleId
    )

    if ($null -eq $Event -or $null -eq $Event.message) {
        return $null
    }
    try {
        $payload = [string]$Event.message | ConvertFrom-Json -Depth 64
        foreach ($module in @($payload.modules)) {
            if ([string]$module.id -eq $ModuleId) {
                return [pscustomobject]@{
                    id    = [string]$module.id
                    state = [string]$module.state
                    detail = [string]$module.detail
                }
            }
        }
    }
    catch {
        return $null
    }
    return $null
}

function Get-PropertyValue {
    param(
        [AllowNull()]$Object,
        [Parameter(Mandatory)][string]$Name
    )

    if ($null -eq $Object) {
        return $null
    }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) {
        return $null
    }
    return $property.Value
}

function Get-InputProbeSummary {
    param(
        [Parameter(Mandatory)]$ProcessResult,
        [Parameter(Mandatory)][string]$ExpectedSource,
        [Parameter(Mandatory)][bool]$ExpectedActive,
        [Parameter(Mandatory)][string]$RunLabel
    )

    $probeEvent = Get-Event -Items @($ProcessResult.stdout.items) -Name 'runtime.input_probe'
    $payload = $null
    $parseError = ''
    if ($null -ne $probeEvent -and $null -ne (Get-PropertyValue $probeEvent 'message')) {
        try {
            $payload = [string](Get-PropertyValue $probeEvent 'message') | ConvertFrom-Json -Depth 64
        }
        catch {
            $parseError = $_.Exception.Message
        }
    }

    $actionsPayload = Get-PropertyValue $payload 'actions'
    $actionItems = @(Get-PropertyValue $actionsPayload 'actions')
    $targetAction = $null
    foreach ($action in $actionItems) {
        if ([string](Get-PropertyValue $action 'id') -ceq $script:ActionId) {
            $targetAction = $action
            break
        }
    }

    $injectionItems = @(Get-PropertyValue $payload 'injections')
    $injection = if ($injectionItems.Count -gt 0) { $injectionItems[0] } else { $null }
    $actualValue = $null
    $hasNumericValue = $false
    $rawValue = Get-PropertyValue $targetAction 'value'
    if ($null -ne $rawValue) {
        try {
            $actualValue = [double]$rawValue
            $hasNumericValue = [double]::IsFinite($actualValue)
        }
        catch {
            $hasNumericValue = $false
        }
    }
    $actualActive = $hasNumericValue -and $actualValue -gt 0.5
    $injectionSource = [string](Get-PropertyValue $injection 'source')
    $injectionSuccess = [bool](Get-PropertyValue $injection 'success')
    $probeSchema = [string](Get-PropertyValue $payload 'schemaVersion')
    $actionsSchema = [string](Get-PropertyValue $actionsPayload 'schemaVersion')
    $eventObserved = $null -ne $probeEvent
    $pass = -not $ProcessResult.timedOut -and $ProcessResult.exitCode -eq 0 -and
        $eventObserved -and [string]::IsNullOrWhiteSpace($parseError) -and
        $probeSchema -eq 'noemancer.runtime-input-probe/0.1' -and
        $actionsSchema -eq 'noemancer.input-actions/0.2' -and
        $null -ne $targetAction -and $hasNumericValue -and
        $injectionItems.Count -eq 1 -and $injectionSuccess -and
        $injectionSource -ceq $ExpectedSource -and $actualActive -eq $ExpectedActive

    return [pscustomobject][ordered]@{
        label = $RunLabel
        pass = $pass
        exitCode = $ProcessResult.exitCode
        timedOut = $ProcessResult.timedOut
        eventObserved = $eventObserved
        parseError = $parseError
        probeSchema = $probeSchema
        actionsSchema = $actionsSchema
        expectedSource = $ExpectedSource
        expectedActive = $ExpectedActive
        injectionCount = $injectionItems.Count
        injectionSuccess = $injectionSuccess
        injectionSource = $injectionSource
        actionId = $script:ActionId
        actionValue = $actualValue
        actionActive = $actualActive
        invalidJsonLines = $ProcessResult.stdout.invalidLines
        oversizeJsonLines = $ProcessResult.stdout.oversizeLines
        stdout = Get-RelativeEvidencePath $ProcessResult.stdoutLog.path
        stderr = Get-RelativeEvidencePath $ProcessResult.stderrLog.path
    }
}

function Get-ToolDescriptor {
    param(
        [AllowEmptyCollection()][object[]]$Tools,
        [Parameter(Mandatory)][string]$Name
    )
    foreach ($tool in $Tools) {
        if ([string]$tool.name -eq $Name) {
            return $tool
        }
    }
    return $null
}

$projectRoot = ''
$projectFile = ''
$stagingProjectRoot = ''
$stagingProjectFile = ''
$packageRoot = ''
$evidence = [ordered]@{
    schemaVersion = $script:EvidenceSchema
    status        = 'failed'
    pass          = $false
    capturedAt    = (Get-Date).ToUniversalTime().ToString('o')
    sourceProject = $Project
    runtime       = $null
    change        = [ordered]@{
        actionId  = $script:ActionId
        oldSource = $script:OldSource
        newSource = $script:NewSource
    }
    sourceIntegrity = [ordered]@{
        projectFile = $null
        schema = $null
        acceptedSchemas = @($script:AcceptedProjectSchemas)
        sha256Before = $null
        sha256After = $null
        unchanged = $false
    }
    staging = [ordered]@{
        path = $null
        kept = [bool]$KeepStaging
        cleaned = $false
    }
    stages = [ordered]@{}
    issues = @()
    commands = @()
    logs = @()
    evidenceFile = $null
}

try {
    $repoRoot = Get-FullPath (Join-Path $PSScriptRoot '..')
    $projectRoot = Get-FullPath $Project
    if (-not (Test-Path -LiteralPath $projectRoot -PathType Container)) {
        throw "Project directory does not exist: $Project"
    }
    $projectFile = Join-Path $projectRoot 'noemancer.project.json'
    if (-not (Test-Path -LiteralPath $projectFile -PathType Leaf)) {
        throw "Project manifest does not exist: $projectFile"
    }
    $script:SourceProjectFile = $projectFile
    $script:SourceProjectHashBefore = Get-Sha256 $projectFile
    $sourceDocument = Read-ProjectJson $projectFile
    $sourceProjectSchema = [string](Get-PropertyValue $sourceDocument 'schema')
    if ($script:AcceptedProjectSchemas -notcontains $sourceProjectSchema) {
        throw "Source project schema '$sourceProjectSchema' is unsupported; expected one of $($script:AcceptedProjectSchemas -join ', ')."
    }
    $evidence.sourceIntegrity.schema = $sourceProjectSchema
    $sourceAction = Get-Action -Document $sourceDocument -Id $script:ActionId
    if ($null -eq $sourceAction) {
        throw "Input action is missing from source project: $($script:ActionId)"
    }
    $sourceSources = @(Get-InputSources $sourceDocument)
    $sourceOldCount = @($sourceSources | Where-Object { $_ -ceq $script:OldSource }).Count
    $sourceNewCount = @($sourceSources | Where-Object { $_ -ceq $script:NewSource }).Count
    if ($sourceOldCount -ne 1 -or $sourceNewCount -ne 0) {
        throw "Source project precondition failed: expected exactly one '$($script:OldSource)' and no '$($script:NewSource)'."
    }
    $sourceBinding = @($sourceAction.bindings | Where-Object { [string]$_.source -ceq $script:OldSource })
    if ($sourceBinding.Count -ne 1) {
        throw "Source action precondition failed: expected exactly one old binding on '$($script:ActionId)'."
    }

    if ([string]::IsNullOrWhiteSpace($RuntimePath)) {
        $RuntimePath = Join-Path $repoRoot 'build\windows-msvc-debug\src\runtime\Debug\noemancer.exe'
    }
    $RuntimePath = Get-FullPath $RuntimePath
    if (-not (Test-Path -LiteralPath $RuntimePath -PathType Leaf)) {
        throw "Runtime executable does not exist: $RuntimePath"
    }
    $evidence.runtime = [ordered]@{
        executable = $RuntimePath
        sha256 = Get-Sha256 $RuntimePath
        frames = $Frames
        timeoutSeconds = $TimeoutSeconds
    }

    if ([string]::IsNullOrWhiteSpace($OutputPath)) {
        $runId = "$(Get-Date -Format 'yyyyMMdd-HHmmss')-$([guid]::NewGuid().ToString('N').Substring(0, 8))"
        $script:EvidenceRoot = Join-Path $repoRoot "generated\acceptance\project-input-remap-$runId"
        $script:EvidencePath = Join-Path $script:EvidenceRoot 'evidence.json'
    }
    else {
        $script:EvidencePath = Get-FullPath $OutputPath
        if (Test-Path -LiteralPath $script:EvidencePath -PathType Container) {
            throw "OutputPath must name a JSON file, not a directory: $OutputPath"
        }
        $script:EvidenceRoot = Split-Path -Parent $script:EvidencePath
    }
    if (Test-Path -LiteralPath $script:EvidencePath) {
        throw "Refusing to overwrite existing evidence: $($script:EvidencePath)"
    }
    [void](New-Item -ItemType Directory -Path (Join-Path $script:EvidenceRoot 'logs') -Force)
    $evidence.evidenceFile = $script:EvidencePath

    $tempRoot = (Get-FullPath ([System.IO.Path]::GetTempPath())).TrimEnd('\')
    $script:StagingPath = Join-Path $tempRoot "noemancer-project-input-remap-$([guid]::NewGuid().ToString('N'))"
    [void](New-Item -ItemType Directory -Path $script:StagingPath -Force)
    $evidence.staging.path = $script:StagingPath
    $stagingProjectRoot = $script:StagingPath
    $stagingProjectFile = Join-Path $stagingProjectRoot 'noemancer.project.json'

    # Copy source contents into a directory that was created exclusively for this run.
    foreach ($child in Get-ChildItem -LiteralPath $projectRoot -Force) {
        if ($child.Name -ceq '.git') {
            continue
        }
        Copy-Item -LiteralPath $child.FullName -Destination $stagingProjectRoot -Recurse -Force
    }
    if (-not (Test-Path -LiteralPath $stagingProjectFile -PathType Leaf)) {
        throw "Staging copy did not contain noemancer.project.json"
    }

    $stagingDocument = Read-ProjectJson $stagingProjectFile
    $stagingProjectSchema = [string](Get-PropertyValue $stagingDocument 'schema')
    if ($script:AcceptedProjectSchemas -notcontains $stagingProjectSchema) {
        throw "Staging project schema '$stagingProjectSchema' is unsupported; expected one of $($script:AcceptedProjectSchemas -join ', ')."
    }
    $stagingAction = Get-Action -Document $stagingDocument -Id $script:ActionId
    if ($null -eq $stagingAction) {
        throw "Staging project is missing input action: $($script:ActionId)"
    }
    $changed = $false
    foreach ($binding in @($stagingAction.bindings)) {
        if ([string]$binding.source -ceq $script:OldSource) {
            $binding.source = $script:NewSource
            $changed = $true
        }
    }
    if (-not $changed) {
        throw "Staging action did not contain the expected old binding."
    }
    $stagingTempManifest = "$stagingProjectFile.tmp-$([guid]::NewGuid().ToString('N'))"
    try {
        $utf8 = [System.Text.UTF8Encoding]::new($false)
        [System.IO.File]::WriteAllText($stagingTempManifest, ($stagingDocument | ConvertTo-Json -Depth 64), $utf8)
        Move-Item -LiteralPath $stagingTempManifest -Destination $stagingProjectFile -Force
    }
    finally {
        if (Test-Path -LiteralPath $stagingTempManifest -PathType Leaf) {
            Remove-Item -LiteralPath $stagingTempManifest -Force
        }
    }

    $reloadedStaging = Read-ProjectJson $stagingProjectFile
    $reloadedSources = @(Get-InputSources $reloadedStaging)
    $reloadedAction = Get-Action -Document $reloadedStaging -Id $script:ActionId
    $newCount = @($reloadedSources | Where-Object { $_ -ceq $script:NewSource }).Count
    $oldCount = @($reloadedSources | Where-Object { $_ -ceq $script:OldSource }).Count
    $actionNewCount = @($reloadedAction.bindings | Where-Object { [string]$_.source -ceq $script:NewSource }).Count
    $projectDocumentPass = $newCount -eq 1 -and $oldCount -eq 0 -and $actionNewCount -eq 1
    $evidence.stages.projectDocument = [ordered]@{
        pass = $projectDocumentPass
        schema = $stagingProjectSchema
        acceptedSchemas = @($script:AcceptedProjectSchemas)
        actionId = $script:ActionId
        oldSourceCount = $oldCount
        newSourceCount = $newCount
        actionNewSourceCount = $actionNewCount
        oldSourceAbsent = ($oldCount -eq 0)
        newSourcePresent = ($actionNewCount -eq 1)
    }
    if (-not $projectDocumentPass) {
        Add-Issue -Code 'staging-remap-not-applied' -Stage 'project-document' -Message "The isolated project manifest did not contain exactly one new binding and zero old bindings after the atomic edit."
    }

    $headless = Invoke-CapturedRuntime -Executable $RuntimePath -Arguments @('run', '--headless', '--frames', [string]$Frames, '--format', 'json', '--project', $stagingProjectRoot) -WorkingDirectory $repoRoot -LogStem 'project-headless'
    $headlessModules = Get-Event -Items @($headless.stdout.items) -Name 'engine.modules'
    $headlessStop = Get-Event -Items @($headless.stdout.items) -Name 'runtime.stop'
    $inputModule = Get-ModuleState -Event $headlessModules -ModuleId 'input.actions'
    $headlessPass = $projectDocumentPass -and -not $headless.timedOut -and $headless.exitCode -eq 0 -and $null -ne $headlessStop -and $null -ne $inputModule -and $inputModule.state -eq 'ready'
    $evidence.stages.projectHeadless = [ordered]@{
        pass = $headlessPass
        exitCode = $headless.exitCode
        timedOut = $headless.timedOut
        runtimeStopObserved = ($null -ne $headlessStop)
        inputActionsModule = $inputModule
        invalidJsonLines = $headless.stdout.invalidLines
        oversizeJsonLines = $headless.stdout.oversizeLines
        stdout = $headless.stdoutLog.path | ForEach-Object { Get-RelativeEvidencePath $_ }
        stderr = $headless.stderrLog.path | ForEach-Object { Get-RelativeEvidencePath $_ }
    }
    if (-not $headlessPass) {
        Add-Issue -Code 'project-headless-failed' -Stage 'project-headless' -Message 'The remapped isolated project did not complete the expected headless load/module lifecycle.'
    }

    # Probe the CLI descriptor as a cheap preflight, then use the actual runtime
    # input_probe event for semantic proof.  The event is emitted by the same
    # project-loaded World that consumed the remapped manifest.
    $toolsProbe = Invoke-CapturedRuntime -Executable $RuntimePath -Arguments @('tools', 'list', '--format', 'json') -WorkingDirectory $repoRoot -LogStem 'tools-capability'
    $toolsDocument = $null
    if (@($toolsProbe.stdout.items).Count -gt 0) {
        $toolsDocument = @($toolsProbe.stdout.items)[0]
    }
    $toolDescriptors = if ($null -ne $toolsDocument) { @(Get-PropertyValue $toolsDocument 'tools') } else { @() }
    $observeTool = Get-ToolDescriptor -Tools $toolDescriptors -Name 'input.actions.observe'
    $injectTool = Get-ToolDescriptor -Tools $toolDescriptors -Name 'input.source.inject'
    $runTool = Get-ToolDescriptor -Tools $toolDescriptors -Name 'run.headless'
    $projectNewInput = Invoke-CapturedRuntime -Executable $RuntimePath -Arguments @(
        'run', '--headless', '--frames', [string]$Frames, '--format', 'json',
        '--input-sample', $script:NewSource, '1', '--project', $stagingProjectRoot
    ) -WorkingDirectory $repoRoot -LogStem 'project-input-new-source'
    $projectOldInput = Invoke-CapturedRuntime -Executable $RuntimePath -Arguments @(
        'run', '--headless', '--frames', [string]$Frames, '--format', 'json',
        '--input-sample', $script:OldSource, '1', '--project', $stagingProjectRoot
    ) -WorkingDirectory $repoRoot -LogStem 'project-input-old-source'
    $projectNewSummary = Get-InputProbeSummary -ProcessResult $projectNewInput -ExpectedSource $script:NewSource -ExpectedActive $true -RunLabel 'project-new-source'
    $projectOldSummary = Get-InputProbeSummary -ProcessResult $projectOldInput -ExpectedSource $script:OldSource -ExpectedActive $false -RunLabel 'project-old-source'
    $runtimeProbePass = $projectNewSummary.pass -and $projectOldSummary.pass
    $evidence.stages.runtimeInputProbe = [ordered]@{
        pass = $runtimeProbePass
        semanticProofAvailable = $runtimeProbePass
        capabilityQuery = [ordered]@{
            exitCode = $toolsProbe.exitCode
            timedOut = $toolsProbe.timedOut
            observedTool = if ($null -ne $observeTool) { [ordered]@{ runtimeState = [string](Get-PropertyValue $observeTool 'runtimeState') } } else { $null }
            injectedTool = if ($null -ne $injectTool) { [ordered]@{ runtimeState = [string](Get-PropertyValue $injectTool 'runtimeState') } } else { $null }
            headlessTool = if ($null -ne $runTool) { [ordered]@{ runtimeState = [string](Get-PropertyValue $runTool 'runtimeState') } } else { $null }
        }
        probes = @($projectNewSummary, $projectOldSummary)
        stdout = Get-RelativeEvidencePath $toolsProbe.stdoutLog.path
        stderr = Get-RelativeEvidencePath $toolsProbe.stderrLog.path
    }
    foreach ($probe in @($projectNewSummary, $projectOldSummary)) {
        if (-not $probe.pass) {
            Add-Issue -Code 'project-input-semantic-mismatch' -Stage 'runtime-input' -Message "Project input probe '$($probe.label)' did not match the expected source/action value contract."
        }
    }

    $packageRoot = Join-Path $stagingProjectRoot 'package'
    if (Test-Path -LiteralPath $packageRoot) {
        Remove-Item -LiteralPath $packageRoot -Recurse -Force
    }
    $package = Invoke-CapturedRuntime -Executable $RuntimePath -Arguments @('package', '--project', $stagingProjectRoot, '--output', $packageRoot, '--target-profile', 'windows-x64-release', '--format', 'json') -WorkingDirectory $repoRoot -LogStem 'package'
    $packageJson = $null
    if (@($package.stdout.items).Count -gt 0) {
        $packageJson = @($package.stdout.items)[-1]
    }
    $packageSuccess = $null -ne $packageJson -and [bool]$packageJson.success
    $packagePass = -not $package.timedOut -and $package.exitCode -eq 0 -and $packageSuccess -and (Test-Path -LiteralPath $packageRoot -PathType Container)
    $evidence.stages.package = [ordered]@{
        pass = $packagePass
        exitCode = $package.exitCode
        timedOut = $package.timedOut
        successField = $packageSuccess
        schema = if ($null -ne $packageJson) { [string]$packageJson.schema } else { $null }
        output = $packageRoot
        stdout = Get-RelativeEvidencePath $package.stdoutLog.path
        stderr = Get-RelativeEvidencePath $package.stderrLog.path
    }
    if (-not $packagePass) {
        Add-Issue -Code 'package-failed' -Stage 'package' -Message 'The isolated remapped project could not be packaged into a Game Profile.'
    }

    $profilePath = Join-Path $packageRoot 'config\game-profile.json'
    $profileDocument = $null
    if (Test-Path -LiteralPath $profilePath -PathType Leaf) {
        $profileDocument = Read-ProjectJson $profilePath
    }
    $profileSources = if ($null -ne $profileDocument) { @(Get-InputSources $profileDocument) } else { @() }
    $profileAction = if ($null -ne $profileDocument) { Get-Action -Document $profileDocument -Id $script:ActionId } else { $null }
    $profileNewCount = @($profileSources | Where-Object { $_ -ceq $script:NewSource }).Count
    $profileOldCount = @($profileSources | Where-Object { $_ -ceq $script:OldSource }).Count
    $profileActionNewCount = if ($null -ne $profileAction) { @($profileAction.bindings | Where-Object { [string]$_.source -ceq $script:NewSource }).Count } else { 0 }
    $profileExecutable = if ($null -ne $profileDocument) { [string]$profileDocument.executable } else { '' }
    $profileExe = if ([string]::IsNullOrWhiteSpace($profileExecutable)) { $null } else {
        Join-Path $packageRoot (Join-Path 'bin' $profileExecutable)
    }
    $profileSchema = if ($null -ne $profileDocument) { [string]$profileDocument.schema } else { $null }
    # This verifier creates a new Windows release package, so it is an output
    # contract check rather than the legacy-profile compatibility read path.
    $profilePass = $packagePass -and $null -ne $profileDocument -and $profileSchema -eq $script:GeneratedGameProfileSchema -and $profileNewCount -eq 1 -and $profileOldCount -eq 0 -and $profileActionNewCount -eq 1 -and $null -ne $profileExe -and (Test-Path -LiteralPath $profileExe -PathType Leaf)
    $evidence.stages.gameProfile = [ordered]@{
        pass = $profilePass
        path = $profilePath
        schema = $profileSchema
        requiredSchema = $script:GeneratedGameProfileSchema
        newSourceCount = $profileNewCount
        oldSourceCount = $profileOldCount
        actionNewSourceCount = $profileActionNewCount
        newBindingPreserved = ($profileActionNewCount -eq 1)
        oldSourceAbsent = ($profileOldCount -eq 0)
        executable = $profileExe
        executablePresent = (Test-Path -LiteralPath $profileExe -PathType Leaf)
        sha256 = if (Test-Path -LiteralPath $profilePath -PathType Leaf) { Get-Sha256 $profilePath } else { $null }
    }
    if (-not $profilePass) {
        Add-Issue -Code 'game-profile-remap-not-preserved' -Stage 'game-profile' -Message "The packaged Game Profile did not produce $($script:GeneratedGameProfileSchema) while preserving exactly the staged new binding and removing the old source."
    }

    $playerPass = $false
    $playerNewSummary = $null
    $playerOldSummary = $null
    if ($profilePass) {
        $playerNewInput = Invoke-CapturedRuntime -Executable $profileExe -Arguments @(
            'player', '--profile', $profilePath, '--headless', '--frames', [string]$Frames, '--format', 'json',
            '--input-sample', $script:NewSource, '1'
        ) -WorkingDirectory $packageRoot -LogStem 'package-player-input-new-source'
        $playerOldInput = Invoke-CapturedRuntime -Executable $profileExe -Arguments @(
            'player', '--profile', $profilePath, '--headless', '--frames', [string]$Frames, '--format', 'json',
            '--input-sample', $script:OldSource, '1'
        ) -WorkingDirectory $packageRoot -LogStem 'package-player-input-old-source'
        $playerNewSummary = Get-InputProbeSummary -ProcessResult $playerNewInput -ExpectedSource $script:NewSource -ExpectedActive $true -RunLabel 'package-player-new-source'
        $playerOldSummary = Get-InputProbeSummary -ProcessResult $playerOldInput -ExpectedSource $script:OldSource -ExpectedActive $false -RunLabel 'package-player-old-source'
        $playerNewStop = Get-Event -Items @($playerNewInput.stdout.items) -Name 'runtime.stop'
        $playerOldStop = Get-Event -Items @($playerOldInput.stdout.items) -Name 'runtime.stop'
        $playerNewScript = Get-Event -Items @($playerNewInput.stdout.items) -Name 'player.scripting'
        $playerOldScript = Get-Event -Items @($playerOldInput.stdout.items) -Name 'player.scripting'
        $playerNewNative = Get-Event -Items @($playerNewInput.stdout.items) -Name 'runtime.native_dependencies'
        $playerOldNative = Get-Event -Items @($playerOldInput.stdout.items) -Name 'runtime.native_dependencies'
        $playerNewNativeComplete = $false
        $playerOldNativeComplete = $false
        if ($null -ne $playerNewNative -and $null -ne (Get-PropertyValue $playerNewNative 'message')) {
            try { $playerNewNativeComplete = [bool]([string](Get-PropertyValue $playerNewNative 'message') | ConvertFrom-Json -Depth 32).complete } catch { $playerNewNativeComplete = $false }
        }
        if ($null -ne $playerOldNative -and $null -ne (Get-PropertyValue $playerOldNative 'message')) {
            try { $playerOldNativeComplete = [bool]([string](Get-PropertyValue $playerOldNative 'message') | ConvertFrom-Json -Depth 32).complete } catch { $playerOldNativeComplete = $false }
        }
        $playerPass = $playerNewSummary.pass -and $playerOldSummary.pass -and
            $null -ne $playerNewStop -and $null -ne $playerOldStop -and
            $null -ne $playerNewScript -and $null -ne $playerOldScript
        $evidence.stages.playerHeadless = [ordered]@{
            pass = $playerPass
            semanticProofAvailable = $playerNewSummary.pass -and $playerOldSummary.pass
            probes = @($playerNewSummary, $playerOldSummary)
            newSourceHeadless = [ordered]@{
                exitCode = $playerNewInput.exitCode
                timedOut = $playerNewInput.timedOut
                runtimeStopObserved = ($null -ne $playerNewStop)
                scriptingEventObserved = ($null -ne $playerNewScript)
                nativeDependenciesComplete = $playerNewNativeComplete
            }
            oldSourceHeadless = [ordered]@{
                exitCode = $playerOldInput.exitCode
                timedOut = $playerOldInput.timedOut
                runtimeStopObserved = ($null -ne $playerOldStop)
                scriptingEventObserved = ($null -ne $playerOldScript)
                nativeDependenciesComplete = $playerOldNativeComplete
            }
        }
        foreach ($probe in @($playerNewSummary, $playerOldSummary)) {
            if (-not $probe.pass) {
                Add-Issue -Code 'package-player-input-semantic-mismatch' -Stage 'player-headless' -Message "Packaged Player input probe '$($probe.label)' did not match the expected source/action value contract."
            }
        }
    }
    else {
        $evidence.stages.playerHeadless = [ordered]@{
            pass = $false
            skipped = $true
            reason = 'Skipped because package Game Profile validation failed.'
        }
    }
    if (-not $playerPass) {
        Add-Issue -Code 'package-player-headless-failed' -Stage 'player-headless' -Message 'The packaged Player did not complete a bounded headless run with the remapped Game Profile.'
    }
}
catch {
    Add-Issue -Code 'verifier-exception' -Stage 'script' -Message $_.Exception.Message
}
finally {
    if (-not [string]::IsNullOrWhiteSpace($script:SourceProjectFile) -and (Test-Path -LiteralPath $script:SourceProjectFile -PathType Leaf)) {
        try {
            $script:SourceProjectHashAfter = Get-Sha256 $script:SourceProjectFile
            $evidence.sourceIntegrity.projectFile = $script:SourceProjectFile
            $evidence.sourceIntegrity.sha256Before = $script:SourceProjectHashBefore
            $evidence.sourceIntegrity.sha256After = $script:SourceProjectHashAfter
            $evidence.sourceIntegrity.unchanged = $script:SourceProjectHashBefore -eq $script:SourceProjectHashAfter
            if (-not $evidence.sourceIntegrity.unchanged) {
                Add-Issue -Code 'source-project-mutated' -Stage 'source-integrity' -Message 'The real source project manifest changed during verification.'
            }
        }
        catch {
            Add-Issue -Code 'source-integrity-unavailable' -Stage 'source-integrity' -Message $_.Exception.Message
        }
    }

    $evidence.commands = @($script:Commands.ToArray())
    $evidence.logs = @($script:Logs.ToArray())
    $evidence.issues = @($script:Issues.ToArray())
    $requiredStages = @('projectDocument', 'projectHeadless', 'runtimeInputProbe', 'package', 'gameProfile', 'playerHeadless')
    $allRequiredPassed = $true
    foreach ($stageName in $requiredStages) {
        if (-not $evidence.stages.Contains($stageName) -or -not [bool]$evidence.stages[$stageName].pass) {
            $allRequiredPassed = $false
        }
    }
    $sourceUnchanged = [bool]$evidence.sourceIntegrity.unchanged
    $evidence.pass = $allRequiredPassed -and $sourceUnchanged -and $script:Issues.Count -eq 0
    $evidence.status = if ($evidence.pass) { 'passed' } else { 'failed' }
    $evidence.staging.kept = [bool]$KeepStaging

    if (-not [string]::IsNullOrWhiteSpace($script:StagingPath)) {
        if ($KeepStaging) {
            $script:StagingKept = $true
            $evidence.staging.cleaned = $false
        }
        else {
            try {
                Remove-StagingSafely
                $evidence.staging.cleaned = $true
            }
            catch {
                Add-Issue -Code 'staging-cleanup-failed' -Stage 'cleanup' -Message $_.Exception.Message
                $evidence.staging.cleaned = $false
                $evidence.issues = @($script:Issues.ToArray())
                $evidence.pass = $false
                $evidence.status = 'failed'
            }
        }
    }

    # If setup failed before the output directory was created, still try to emit a
    # machine-readable failure beside the requested output when possible.
    if (-not [string]::IsNullOrWhiteSpace($script:EvidencePath)) {
        try {
            $evidence.issues = @($script:Issues.ToArray())
            Write-JsonAtomic -Path $script:EvidencePath -Value $evidence
        }
        catch {
            # There is no safe place to report a second write failure.  The process
            # exit code below remains non-zero and the original error is printed as
            # compact JSON if no evidence file could be created.
            $script:EvidencePath = ''
        }
    }
}

$summary = [ordered]@{
    schemaVersion = $script:EvidenceSchema
    status = $evidence.status
    pass = [bool]$evidence.pass
    evidencePath = $script:EvidencePath
    issues = @($evidence.issues)
}
Write-Output ($summary | ConvertTo-Json -Depth 32 -Compress)
if (-not [bool]$evidence.pass) {
    exit 1
}
exit 0
