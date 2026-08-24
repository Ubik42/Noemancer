[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$Project = $(if ($env:NOEMANCER_PLATFORMER_PROJECT) { $env:NOEMANCER_PLATFORMER_PROJECT } else { Join-Path ([IO.Path]::GetPathRoot($PSScriptRoot)) '3D\NoemancerPlatformer' }),
    [string]$RuntimePath = '',
    [Alias('OutputPath', 'ReceiptPath')]
    [string]$EvidencePath = '',
    [ValidateRange(1, 120)]
    [int]$Frames = 3,
    [ValidateRange(640, 7680)]
    [int]$WindowWidth = 1600,
    [ValidateRange(360, 4320)]
    [int]$WindowHeight = 900,
    [ValidateRange(5, 600)]
    [int]$TimeoutSeconds = 180,
    [switch]$KeepStaging
)

# Hidden, engine-owned visual acceptance for the Project UI authoring surface.
#
# The verifier deliberately edits only a disposable copy of the requested
# project HUD.  The native runtime owns the window, ImGui dockspace, Scene
# surface, retained Inspector surface and Project UI Authoring panel; no
# desktop automation or Computer Use is involved.  The semantic snapshot and
# the GPU-produced quality sidecar are correlated with the same invocation.
#
# Exit contract:
#   0 = hidden capture, semantic fixture and quality contract passed
#   2 = invalid invocation or source project precondition
#   3 = staging fixture or semantic authoring contract failed
#   4 = hidden runtime capture failed or timed out
#   5 = image/quality/artifact contract failed
#   6 = source project changed while the verifier was running
#   7 = receipt/hash artifact could not be written
#   1 = unexpected verifier failure

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:EvidenceSchema = 'noemancer.project-ui-authoring-editor-capture/0.1'
$script:HashSchema = 'noemancer.project-ui-authoring-hashes/0.1'
$script:QualitySchema = 'noemancer.render-quality.v1'
$script:AcceptedProjectSchemas = @('noemancer.project/0.1', 'noemancer.project/0.2')
$script:MaxLogBytes = 2MB
$script:MaxJsonLineBytes = 1MB
$script:MaxJsonObjects = 4096
$script:SourceRoot = ''
$script:SourceManifest = ''
$script:SourceHud = ''
$script:SourceTreeHashBefore = ''
$script:SourceTreeHashAfter = ''
$script:StagingPath = ''
$script:StagingKept = $false
$script:EvidenceRoot = ''
$script:EvidenceFile = ''
$script:Issues = [System.Collections.Generic.List[object]]::new()
$script:Commands = [System.Collections.Generic.List[object]]::new()
$script:Logs = [System.Collections.Generic.List[object]]::new()

function Get-FullPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    return [IO.Path]::GetFullPath($Path)
}

function Get-Sha256 {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Required file does not exist: $Path"
    }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-RelativePath {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Root
    )
    $full = Get-FullPath $Path
    $rootFull = (Get-FullPath $Root).TrimEnd('\', '/')
    if ($full.Equals($rootFull, [StringComparison]::OrdinalIgnoreCase)) { return '.' }
    $prefix = $rootFull + [IO.Path]::DirectorySeparatorChar
    if ($full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        return $full.Substring($prefix.Length).Replace('\', '/')
    }
    return $full.Replace('\', '/')
}

function Add-Issue {
    param(
        [Parameter(Mandatory = $true)][string]$Code,
        [Parameter(Mandatory = $true)][string]$Stage,
        [Parameter(Mandatory = $true)][string]$Message,
        [string]$Path = ''
    )
    $issue = [ordered]@{ code = $Code; stage = $Stage; message = $Message }
    if (-not [string]::IsNullOrWhiteSpace($Path)) { $issue.path = $Path }
    [void]$script:Issues.Add([pscustomobject]$issue)
}

function Get-PropertyValue {
    param([AllowNull()]$Object, [Parameter(Mandatory = $true)][string]$Name)
    if ($null -eq $Object) { return $null }
    if ($Object -is [System.Collections.IDictionary] -and $Object.Contains($Name)) { return $Object[$Name] }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}

function Read-JsonFile {
    param([Parameter(Mandatory = $true)][string]$Path, [Parameter(Mandatory = $true)][string]$Label)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "$Label does not exist: $Path" }
    $raw = Get-Content -LiteralPath $Path -Raw -Encoding UTF8
    if ([string]::IsNullOrWhiteSpace($raw)) { throw "$Label is empty: $Path" }
    $parsed = $raw | ConvertFrom-Json -Depth 100
    if ($null -eq $parsed) { throw "$Label did not parse as JSON: $Path" }
    return $parsed
}

function Write-Utf8Atomic {
    param([Parameter(Mandatory = $true)][string]$Path, [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Text)
    $directory = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $directory -PathType Container)) {
        [void](New-Item -ItemType Directory -Path $directory -Force)
    }
    $temporary = "$Path.tmp-$([guid]::NewGuid().ToString('N'))"
    try {
        [IO.File]::WriteAllText($temporary, $Text, [Text.UTF8Encoding]::new($false))
        Move-Item -LiteralPath $temporary -Destination $Path -Force
    }
    finally {
        if (Test-Path -LiteralPath $temporary -PathType Leaf) { Remove-Item -LiteralPath $temporary -Force }
    }
}

function Write-JsonAtomic {
    param([Parameter(Mandatory = $true)][string]$Path, [Parameter(Mandatory = $true)]$Value)
    Write-Utf8Atomic -Path $Path -Text (($Value | ConvertTo-Json -Depth 100) + "`n")
}

function Limit-Text {
    param([AllowNull()][string]$Text, [int]$MaxBytes = $script:MaxLogBytes)
    if ($null -eq $Text) { $Text = '' }
    $encoding = [Text.UTF8Encoding]::new($false)
    $bytes = $encoding.GetByteCount($Text)
    if ($bytes -le $MaxBytes) { return [pscustomobject]@{ text = $Text; bytes = $bytes; truncated = $false } }
    $characters = [Math]::Min($Text.Length, $MaxBytes)
    while ($characters -gt 0 -and $encoding.GetByteCount($Text.Substring(0, $characters)) -gt $MaxBytes) { $characters-- }
    $limited = if ($characters -gt 0) { $Text.Substring(0, $characters) } else { '' }
    return [pscustomobject]@{ text = $limited; bytes = $bytes; truncated = $true }
}

function Write-BoundedTextFile {
    param([Parameter(Mandatory = $true)][string]$Path, [AllowNull()][string]$Text)
    $limited = Limit-Text $Text
    Write-Utf8Atomic -Path $Path -Text $limited.text
    $record = [ordered]@{
        path = Get-RelativePath -Path $Path -Root $script:EvidenceRoot
        bytes = (Get-Item -LiteralPath $Path).Length
        sha256 = Get-Sha256 $Path
        sourceBytes = $limited.bytes
        truncated = [bool]$limited.truncated
    }
    [void]$script:Logs.Add([pscustomobject]$record)
    return [pscustomobject]@{ path = $Path; bytes = $record.bytes; sha256 = $record.sha256; truncated = $record.truncated }
}

function Get-JsonLines {
    param([AllowNull()][string]$Text)
    $items = [System.Collections.Generic.List[object]]::new()
    $invalid = 0
    $oversize = 0
    if ([string]::IsNullOrEmpty($Text)) {
        return [pscustomobject]@{ items = @(); invalidLines = 0; oversizeLines = 0 }
    }
    foreach ($line in ($Text -split "`r?`n")) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        if ([Text.Encoding]::UTF8.GetByteCount($line) -gt $script:MaxJsonLineBytes) { $oversize++; continue }
        try {
            if ($items.Count -lt $script:MaxJsonObjects) { [void]$items.Add(($line | ConvertFrom-Json -Depth 100)) }
        }
        catch { $invalid++ }
    }
    return [pscustomobject]@{ items = @($items.ToArray()); invalidLines = $invalid; oversizeLines = $oversize }
}

function Invoke-HiddenRuntime {
    param(
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [Parameter(Mandatory = $true)][string]$LogStem
    )
    if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) { throw "Runtime executable does not exist: $Executable" }
    $stdoutPath = Join-Path $script:EvidenceRoot "logs\$LogStem.stdout.jsonl"
    $stderrPath = Join-Path $script:EvidenceRoot "logs\$LogStem.stderr.log"
    $command = [ordered]@{
        executable = Get-FullPath $Executable
        arguments = @($Arguments)
        workingDirectory = Get-FullPath $WorkingDirectory
        stdout = Get-RelativePath -Path $stdoutPath -Root $script:EvidenceRoot
        stderr = Get-RelativePath -Path $stderrPath -Root $script:EvidenceRoot
    }
    [void]$script:Commands.Add([pscustomobject]$command)

    $watch = [Diagnostics.Stopwatch]::StartNew()
    $psi = [Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = Get-FullPath $Executable
    $psi.WorkingDirectory = Get-FullPath $WorkingDirectory
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    foreach ($argument in $Arguments) { [void]$psi.ArgumentList.Add([string]$argument) }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $psi
    $timedOut = $false
    $stdout = ''
    $stderr = ''
    $exitCode = $null
    try {
        if (-not $process.Start()) { throw "Process failed to start: $Executable" }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            $timedOut = $true
            try { $process.Kill($true) } catch { }
            [void]$process.WaitForExit(5000)
        }
        $stdout = $stdoutTask.GetAwaiter().GetResult()
        $stderr = $stderrTask.GetAwaiter().GetResult()
        if (-not $timedOut) { $exitCode = $process.ExitCode }
    }
    finally {
        $watch.Stop()
        $process.Dispose()
    }
    $stdoutLog = Write-BoundedTextFile -Path $stdoutPath -Text $stdout
    $stderrLog = Write-BoundedTextFile -Path $stderrPath -Text $stderr
    return [pscustomobject]@{
        exitCode = $exitCode
        timedOut = $timedOut
        durationMs = [int]$watch.ElapsedMilliseconds
        stdout = Get-JsonLines $stdout
        stdoutLog = $stdoutLog
        stderrLog = $stderrLog
    }
}

function Get-SourceTreeHash {
    param([Parameter(Mandatory = $true)][string]$Root)
    $records = [System.Collections.Generic.List[string]]::new()
    $files = Get-ChildItem -LiteralPath $Root -Recurse -File -Force -ErrorAction SilentlyContinue |
        Where-Object {
            $relative = Get-RelativePath -Path $_.FullName -Root $Root
            $parts = $relative -split '/'
            $parts -notcontains '.git' -and $parts -notcontains 'bin' -and $parts -notcontains 'obj' -and
                $parts -notcontains 'package' -and $parts -notcontains 'generated'
        } | Sort-Object { Get-RelativePath -Path $_.FullName -Root $Root }
    foreach ($file in $files) {
        [void]$records.Add("$(Get-RelativePath -Path $file.FullName -Root $Root)|$(Get-Sha256 $file.FullName)|$($file.Length)")
    }
    $material = ($records -join "`n") + "`n"
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes($material)
    $digest = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($digest.ComputeHash($bytes)) -replace '-', '').ToLowerInvariant() }
    finally { $digest.Dispose() }
}

function Assert-SafeTempTarget {
    param([Parameter(Mandatory = $true)][string]$Target)
    $tempRoot = (Get-FullPath ([IO.Path]::GetTempPath())).TrimEnd('\', '/')
    $full = (Get-FullPath $Target).TrimEnd('\', '/')
    if ($full -eq $tempRoot -or -not $full.StartsWith("$tempRoot\", [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove a staging path outside the temporary directory: $Target"
    }
}

function Remove-StagingSafely {
    if ([string]::IsNullOrWhiteSpace($script:StagingPath) -or $script:StagingKept) { return $false }
    if (-not (Test-Path -LiteralPath $script:StagingPath)) { return $true }
    Assert-SafeTempTarget $script:StagingPath
    Remove-Item -LiteralPath $script:StagingPath -Recurse -Force
    return $true
}

function Get-Event {
    param([AllowEmptyCollection()][object[]]$Items, [Parameter(Mandatory = $true)][string]$Name)
    $candidate = $null
    foreach ($item in $Items) {
        if ($null -ne $item -and [string](Get-PropertyValue $item 'event') -ceq $Name) { $candidate = $item }
    }
    return $candidate
}

function Convert-EventMessage {
    param([AllowNull()]$Event)
    if ($null -eq $Event) { return $null }
    $message = Get-PropertyValue $Event 'message'
    if ($message -isnot [string]) { return $message }
    try { return ($message | ConvertFrom-Json -Depth 100) } catch { return $null }
}

function New-AuthoringFixture {
    param([Parameter(Mandatory = $true)][string]$HudPath)
    # These are source fields only.  Runtime-derived fields such as fingerprint,
    # bindingState and componentChain are intentionally absent and are produced
    # by the engine projection, never written back into the source fixture.
    $fixture = @'
{
  "schemaVersion": "noemancer.ui-document/0.1",
  "valid": true,
  "code": "ok",
  "documentId": "noemancer.project-ui.authoring-capture",
  "surface": "game",
  "kind": "hud",
  "revision": 41,
  "locale": "en-US",
  "roots": ["ui.authoring.root"],
  "designTokens": {
    "surfaceColor": "#0b111bea",
    "groupColor": "#15243ae8",
    "textColor": "#e8f1ff",
    "accentColor": "#6ee7d8",
    "dangerColor": "#ff6b78",
    "surfaceWidthPx": 420
  },
  "capabilities": {
    "semanticQuery": true,
    "transactionActions": true,
    "layoutEvidence": true
  },
  "components": [
    {
      "id": "ui.authoring.root",
      "role": "group",
      "label": "Production UI Authoring",
      "presentation": {
        "control": "group",
        "layout": {"flow": "column", "gap": 8},
        "constraints": {"width": 420, "minHeight": 240}
      },
      "state": {"visible": true, "enabled": true, "editable": false}
    },
    {
      "id": "ui.authoring.property",
      "role": "property",
      "label": "Production property",
      "presentation": {"control": "label", "constraints": {"width": 360}},
      "state": {"visible": true, "enabled": true, "editable": false}
    },
    {
      "id": "ui.authoring.disabled-action",
      "extends": "ui.authoring.root",
      "role": "button",
      "label": "Apply changes",
      "presentation": {"control": "button", "constraints": {"width": 220}},
      "state": {"visible": true, "enabled": false, "editable": true}
    }
  ],
  "nodes": [
    {
      "id": "ui.authoring.root",
      "parentId": null,
      "role": "group",
      "label": "Project UI Authoring / Production",
      "componentRef": "ui.authoring.root",
      "state": {"visible": true, "enabled": true, "editable": false},
      "presentation": {"control": "group", "layout": {"flow": "column", "gap": 8}},
      "actions": []
    },
    {
      "id": "ui.authoring.binding-error",
      "parentId": "ui.authoring.root",
      "role": "property",
      "label": "Diagnostics / unavailable binding",
      "componentRef": "ui.authoring.property",
      "binding": {"kind": "script-state", "instanceId": "script.missing-for-evidence", "member": "Unavailable", "fallback": "n/a"},
      "value": "n/a",
      "state": {"visible": true, "enabled": true, "editable": false, "error": "Binding source is not available."},
      "actions": []
    },
    {
      "id": "ui.authoring.disabled-action",
      "parentId": "ui.authoring.root",
      "role": "button",
      "label": "Apply changes (disabled)",
      "componentRef": "ui.authoring.disabled-action",
      "value": "Apply",
      "state": {"visible": true, "enabled": false, "editable": true},
      "actions": []
    },
    {
      "id": "ui.authoring.production-value",
      "parentId": "ui.authoring.root",
      "role": "property",
      "label": "Production field / revision",
      "componentRef": "ui.authoring.property",
      "value": "Revision 41 / componentRef",
      "state": {"visible": true, "enabled": true, "editable": false},
      "actions": []
    }
  ]
}
'@
    Write-Utf8Atomic -Path $HudPath -Text ($fixture.Trim() + "`n")
    return (Read-JsonFile -Path $HudPath -Label 'Staging HUD fixture')
}

function Get-ArtifactRecord {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    $item = Get-Item -LiteralPath $Path
    $relativeRoot = if ([string]::IsNullOrWhiteSpace($script:EvidenceRoot)) { $script:SourceRoot } else { $script:EvidenceRoot }
    return [ordered]@{ path = Get-RelativePath -Path $Path -Root $relativeRoot; bytes = $item.Length; sha256 = Get-Sha256 $Path }
}

$evidence = [ordered]@{
    schemaVersion = $script:EvidenceSchema
    status = 'failed'
    pass = $false
    capturedAt = [DateTime]::UtcNow.ToString('o')
    sourceProject = [ordered]@{ root = $Project; manifest = $null; hudDocument = $null; schema = $null; treeSha256Before = $null; treeSha256After = $null; unchanged = $false }
    staging = [ordered]@{ path = $null; kept = [bool]$KeepStaging; cleaned = $false; fixture = $null }
    runtime = $null
    capture = [ordered]@{ image = $null; quality = $null; semanticSnapshot = $null }
    quality = [ordered]@{ pass = $false; sidecar = $null; checks = [ordered]@{} }
    hashes = $null
    commands = @()
    logs = @()
    issues = @()
    evidenceFile = $null
}

$exitCode = 1
try {
    $repoRoot = Get-FullPath (Join-Path $PSScriptRoot '..')
    $script:SourceRoot = Get-FullPath $Project
    if (-not (Test-Path -LiteralPath $script:SourceRoot -PathType Container)) {
        throw "Source project directory does not exist: $Project"
    }
    $script:SourceManifest = Join-Path $script:SourceRoot 'noemancer.project.json'
    if (-not (Test-Path -LiteralPath $script:SourceManifest -PathType Leaf)) {
        throw "Source project manifest does not exist: $($script:SourceManifest)"
    }
    $sourceDocument = Read-JsonFile -Path $script:SourceManifest -Label 'Source project manifest'
    $projectSchema = [string](Get-PropertyValue $sourceDocument 'schema')
    if ($script:AcceptedProjectSchemas -notcontains $projectSchema) {
        throw "Unsupported source project schema '$projectSchema'."
    }
    $hudRelative = [string](Get-PropertyValue $sourceDocument 'hudDocument')
    if ([string]::IsNullOrWhiteSpace($hudRelative) -or [IO.Path]::IsPathRooted($hudRelative) -or $hudRelative -match '(^|[\\/])\.\.') {
        throw 'Source project hudDocument must be a non-empty project-relative path.'
    }
    $script:SourceHud = Get-FullPath (Join-Path $script:SourceRoot ($hudRelative.Replace('/', '\')))
    if (-not (Test-Path -LiteralPath $script:SourceHud -PathType Leaf)) { throw "Source HUD document does not exist: $($script:SourceHud)" }
    $sourceHudDocument = Read-JsonFile -Path $script:SourceHud -Label 'Source HUD document'
    $sourceTreeBefore = Get-SourceTreeHash -Root $script:SourceRoot
    $script:SourceTreeHashBefore = $sourceTreeBefore
    $evidence.sourceProject.root = $script:SourceRoot
    $evidence.sourceProject.manifest = Get-ArtifactRecord -Path $script:SourceManifest
    $evidence.sourceProject.hudDocument = Get-ArtifactRecord -Path $script:SourceHud
    $evidence.sourceProject.schema = $projectSchema
    $evidence.sourceProject.treeSha256Before = $sourceTreeBefore

    if ([string]::IsNullOrWhiteSpace($RuntimePath)) {
        $RuntimePath = Join-Path $repoRoot 'build\windows-msvc-debug\src\runtime\Debug\noemancer.exe'
    }
    $RuntimePath = Get-FullPath $RuntimePath
    if (-not (Test-Path -LiteralPath $RuntimePath -PathType Leaf)) { throw "Runtime executable does not exist: $RuntimePath" }

    if ([string]::IsNullOrWhiteSpace($EvidencePath)) {
        $runId = "$(Get-Date -Format 'yyyyMMdd-HHmmss')-$([guid]::NewGuid().ToString('N').Substring(0, 8))"
        $script:EvidenceRoot = Join-Path $repoRoot "generated\acceptance\project-ui-authoring-$runId"
        $script:EvidenceFile = Join-Path $script:EvidenceRoot 'evidence.json'
    }
    else {
        $script:EvidenceFile = Get-FullPath $EvidencePath
        if (Test-Path -LiteralPath $script:EvidenceFile -PathType Container) { throw 'EvidencePath must name a JSON file, not a directory.' }
        $script:EvidenceRoot = Split-Path -Parent $script:EvidenceFile
    }
    if (Test-Path -LiteralPath $script:EvidenceFile) { throw "Refusing to overwrite existing evidence: $($script:EvidenceFile)" }
    [void](New-Item -ItemType Directory -Path (Join-Path $script:EvidenceRoot 'logs') -Force)
    $evidence.evidenceFile = $script:EvidenceFile

    $tempRoot = (Get-FullPath ([IO.Path]::GetTempPath())).TrimEnd('\', '/')
    $script:StagingPath = Join-Path $tempRoot "noemancer-project-ui-authoring-$([guid]::NewGuid().ToString('N'))"
    [void](New-Item -ItemType Directory -Path $script:StagingPath -Force)
    $evidence.staging.path = $script:StagingPath
    foreach ($child in Get-ChildItem -LiteralPath $script:SourceRoot -Force) {
        if ($child.Name -ceq '.git') { continue }
        Copy-Item -LiteralPath $child.FullName -Destination $script:StagingPath -Recurse -Force
    }
    $stagingManifest = Join-Path $script:StagingPath 'noemancer.project.json'
    if (-not (Test-Path -LiteralPath $stagingManifest -PathType Leaf)) { throw 'Staging copy did not contain noemancer.project.json.' }
    $stagingDocument = Read-JsonFile -Path $stagingManifest -Label 'Staging project manifest'
    $stagingHudRelative = [string](Get-PropertyValue $stagingDocument 'hudDocument')
    $stagingHud = Get-FullPath (Join-Path $script:StagingPath ($stagingHudRelative.Replace('/', '\')))
    $fixture = New-AuthoringFixture -HudPath $stagingHud
    $fixtureComponents = @($fixture.components)
    $fixtureNodes = @($fixture.nodes)
    $fixtureIds = @($fixtureNodes | ForEach-Object { [string]$_.id })
    $fixtureComponentRefs = @($fixtureNodes | Where-Object { $null -ne $_.componentRef } | ForEach-Object { [string]$_.componentRef })
    $fixturePass = [string]$fixture.schemaVersion -eq 'noemancer.ui-document/0.1' -and
        [bool]$fixture.valid -and [int]$fixture.revision -eq 41 -and $fixtureComponents.Count -eq 3 -and
        $fixtureNodes.Count -eq 4 -and $fixtureIds -contains 'ui.authoring.root' -and
        $fixtureComponentRefs -contains 'ui.authoring.disabled-action' -and
        $fixtureComponentRefs -contains 'ui.authoring.property' -and
        [string]$fixture.nodes[1].binding.instanceId -eq 'script.missing-for-evidence' -and
        [bool]$fixture.nodes[2].state.enabled -eq $false
    $evidence.staging.fixture = [ordered]@{
        pass = $fixturePass
        hudDocument = Get-RelativePath -Path $stagingHud -Root $script:StagingPath
        revision = [int]$fixture.revision
        componentCount = $fixtureComponents.Count
        nodeCount = $fixtureNodes.Count
        componentRefs = @($fixtureComponentRefs)
        errorBindingNode = 'ui.authoring.binding-error'
        disabledNode = 'ui.authoring.disabled-action'
        sourceSha256 = Get-Sha256 $stagingHud
    }
    if (-not $fixturePass) {
        Add-Issue -Code 'fixture.contract-failed' -Stage 'staging-fixture' -Message 'The disposable HUD fixture did not contain the expected reusable components, error binding and disabled control.'
        $exitCode = 3
        throw 'Staging fixture contract failed.'
    }

    $captureImage = Join-Path $script:EvidenceRoot 'project-ui-authoring-editor.bmp'
    $runtimeArgs = @(
        'run', '--format', 'json', '--frames', [string]$Frames,
        '--project', $script:StagingPath, '--editor-project-settings',
        '--capture-editor-frame', $captureImage,
        '--window-width', [string]$WindowWidth, '--window-height', [string]$WindowHeight
    )
    $run = Invoke-HiddenRuntime -Executable $RuntimePath -Arguments $runtimeArgs -WorkingDirectory $repoRoot -LogStem 'editor-project-ui-authoring'
    $evidence.runtime = [ordered]@{
        executable = Get-FullPath $RuntimePath
        sha256 = Get-Sha256 $RuntimePath
        arguments = @($runtimeArgs)
        workingDirectory = $repoRoot
        exitCode = $run.exitCode
        timedOut = [bool]$run.timedOut
        durationMs = $run.durationMs
        stdout = $run.stdoutLog
        stderr = $run.stderrLog
        invalidJsonLines = $run.stdout.invalidLines
        oversizeJsonLines = $run.stdout.oversizeLines
    }
    if ($run.timedOut -or $run.exitCode -ne 0) {
        Add-Issue -Code 'runtime.capture-failed' -Stage 'hidden-runtime' -Message "The hidden editor capture exited with code '$($run.exitCode)' or timed out."
        $exitCode = 4
    }

    $snapshotEvent = Get-Event -Items @($run.stdout.items) -Name 'editor.semantic_snapshot'
    $snapshot = Convert-EventMessage $snapshotEvent
    $projectUi = Get-PropertyValue $snapshot 'projectUiAuthoring'
    $snapshotNodes = @((Get-PropertyValue $projectUi 'nodes'))
    $snapshotComponents = @((Get-PropertyValue $projectUi 'components'))
    $snapshotHasComponentRef = $false
    $snapshotHasDisabledField = $false
    $snapshotHasFields = $false
    foreach ($node in $snapshotNodes) {
        if (-not [string]::IsNullOrWhiteSpace([string](Get-PropertyValue $node 'componentRef'))) { $snapshotHasComponentRef = $true }
        $fields = @(Get-PropertyValue $node 'fields')
        if ($fields.Count -gt 0) { $snapshotHasFields = $true }
        foreach ($field in $fields) { if ([bool](Get-PropertyValue $field 'disabled')) { $snapshotHasDisabledField = $true } }
    }
    $semanticPass = $null -ne $snapshotEvent -and $null -ne $projectUi -and
        [bool](Get-PropertyValue $projectUi 'valid') -and $snapshotComponents.Count -eq 3 -and
        $snapshotNodes.Count -eq 4 -and $snapshotHasComponentRef -and $snapshotHasFields -and $snapshotHasDisabledField
    $evidence.capture.semanticSnapshot = [ordered]@{
        pass = $semanticPass
        eventObserved = $null -ne $snapshotEvent
        valid = if ($null -ne $projectUi) { [bool](Get-PropertyValue $projectUi 'valid') } else { $false }
        componentCount = $snapshotComponents.Count
        nodeCount = $snapshotNodes.Count
        componentRefObserved = $snapshotHasComponentRef
        fieldStatesObserved = $snapshotHasFields
        disabledFieldObserved = $snapshotHasDisabledField
        expectedRevision = 41
        observedRevision = if ($null -ne $projectUi) { Get-PropertyValue $projectUi 'revision' } else { $null }
    }
    if (-not $semanticPass) {
        Add-Issue -Code 'semantic.snapshot-contract-failed' -Stage 'semantic-snapshot' -Message 'The hidden run did not publish a valid Project UI Authoring snapshot containing components, componentRef and disabled field states.'
        if ($exitCode -eq 1) { $exitCode = 3 }
    }

    $qualityPath = "$captureImage.quality.json"
    $imageExists = Test-Path -LiteralPath $captureImage -PathType Leaf
    $quality = $null
    if (Test-Path -LiteralPath $qualityPath -PathType Leaf) { $quality = Read-JsonFile -Path $qualityPath -Label 'Editor capture quality sidecar' }
    $qualityPass = $imageExists -and $null -ne $quality -and
        [string](Get-PropertyValue $quality 'schemaVersion') -eq $script:QualitySchema -and
        [bool](Get-PropertyValue $quality 'pass') -and [bool](Get-PropertyValue $quality 'dimensionsMatch') -and
        [int](Get-PropertyValue $quality 'width') -gt 0 -and [int](Get-PropertyValue $quality 'height') -gt 0
    $evidence.capture.image = Get-ArtifactRecord -Path $captureImage
    $evidence.capture.quality = Get-ArtifactRecord -Path $qualityPath
    $evidence.quality = [ordered]@{
        pass = $qualityPass
        sidecar = Get-ArtifactRecord -Path $qualityPath
        checks = [ordered]@{
            imageExists = $imageExists
            schema = if ($null -ne $quality) { [string](Get-PropertyValue $quality 'schemaVersion') } else { $null }
            runtimePass = if ($null -ne $quality) { [bool](Get-PropertyValue $quality 'pass') } else { $false }
            dimensionsMatch = if ($null -ne $quality) { [bool](Get-PropertyValue $quality 'dimensionsMatch') } else { $false }
            width = if ($null -ne $quality) { Get-PropertyValue $quality 'width' } else { $null }
            height = if ($null -ne $quality) { Get-PropertyValue $quality 'height' } else { $null }
            metrics = if ($null -ne $quality) { Get-PropertyValue $quality 'metrics' } else { $null }
        }
    }
    if (-not $qualityPass) {
        Add-Issue -Code 'capture.quality-contract-failed' -Stage 'quality' -Message 'The hidden editor image or its versioned quality sidecar did not pass.'
        if ($exitCode -eq 1 -or $exitCode -eq 4) { $exitCode = 5 }
    }

    $script:SourceTreeHashAfter = Get-SourceTreeHash -Root $script:SourceRoot
    $evidence.sourceProject.treeSha256After = $script:SourceTreeHashAfter
    $evidence.sourceProject.manifest = [ordered]@{
        path = Get-RelativePath -Path $script:SourceManifest -Root $script:SourceRoot
        sha256Before = (Get-PropertyValue $evidence.sourceProject.manifest 'sha256')
        sha256After = Get-Sha256 $script:SourceManifest
    }
    $evidence.sourceProject.hudDocument = [ordered]@{
        path = Get-RelativePath -Path $script:SourceHud -Root $script:SourceRoot
        sha256Before = (Get-PropertyValue $evidence.sourceProject.hudDocument 'sha256')
        sha256After = Get-Sha256 $script:SourceHud
    }
    $sourceUnchanged = $script:SourceTreeHashBefore -eq $script:SourceTreeHashAfter -and
        $evidence.sourceProject.manifest.sha256Before -eq $evidence.sourceProject.manifest.sha256After -and
        $evidence.sourceProject.hudDocument.sha256Before -eq $evidence.sourceProject.hudDocument.sha256After
    $evidence.sourceProject.unchanged = $sourceUnchanged
    if (-not $sourceUnchanged) {
        Add-Issue -Code 'source.integrity-changed' -Stage 'source-integrity' -Message 'The source project changed during the hidden acceptance run.'
        $exitCode = 6
    }

    $hashesPath = Join-Path $script:EvidenceRoot 'hashes.json'
    $runtimeHash = [ordered]@{ name = 'runtime'; path = $evidence.runtime.executable; sha256 = $evidence.runtime.sha256 }
    $hashFiles = [System.Collections.Generic.List[object]]::new()
    [void]$hashFiles.Add($runtimeHash)
    if ($null -ne $evidence.capture.image) { [void]$hashFiles.Add($evidence.capture.image) }
    if ($null -ne $evidence.capture.quality) { [void]$hashFiles.Add($evidence.capture.quality) }
    [void]$hashFiles.Add($run.stdoutLog)
    [void]$hashFiles.Add($run.stderrLog)
    $hashes = [ordered]@{
        schemaVersion = $script:HashSchema
        capturedAt = [DateTime]::UtcNow.ToString('o')
        files = @($hashFiles.ToArray())
        source = [ordered]@{ treeSha256Before = $script:SourceTreeHashBefore; treeSha256After = $script:SourceTreeHashAfter; unchanged = $sourceUnchanged }
    }
    Write-JsonAtomic -Path $hashesPath -Value $hashes
    $evidence.hashes = Get-ArtifactRecord -Path $hashesPath

    $evidence.commands = @($script:Commands.ToArray())
    $evidence.logs = @($script:Logs.ToArray())
    $evidence.issues = @($script:Issues.ToArray())
    $evidence.pass = $exitCode -eq 1 -and $fixturePass -and $semanticPass -and $qualityPass -and $sourceUnchanged
    if ($evidence.pass) { $exitCode = 0 }
    elseif ($exitCode -eq 1) { $exitCode = 3 }
}
catch {
    if ($exitCode -eq 1) {
        $exitCode = if ($script:SourceRoot -eq '') { 2 } else { 3 }
    }
    Add-Issue -Code 'verifier.exception' -Stage 'verifier' -Message $_.Exception.Message
}
finally {
    try {
        $script:StagingKept = [bool]$KeepStaging
        $evidence.staging.kept = $script:StagingKept
        if (-not $script:StagingKept -and -not [string]::IsNullOrWhiteSpace($script:StagingPath)) {
            $evidence.staging.cleaned = Remove-StagingSafely
        }
        elseif ($script:StagingKept) {
            $evidence.staging.cleaned = $false
        }
    }
    catch {
        Add-Issue -Code 'staging.cleanup-failed' -Stage 'cleanup' -Message $_.Exception.Message
        if ($exitCode -eq 0) { $exitCode = 7 }
    }

    try {
        $evidence.status = if ($exitCode -eq 0) { 'passed' } else { 'failed' }
        $evidence.pass = $exitCode -eq 0
        $evidence.commands = @($script:Commands.ToArray())
        $evidence.logs = @($script:Logs.ToArray())
        $evidence.issues = @($script:Issues.ToArray())
        if (-not [string]::IsNullOrWhiteSpace($script:EvidenceFile)) {
            if (-not (Test-Path -LiteralPath $script:EvidenceRoot -PathType Container)) {
                [void](New-Item -ItemType Directory -Path $script:EvidenceRoot -Force)
            }
            if (-not (Test-Path -LiteralPath $script:EvidenceFile -PathType Leaf)) {
                Write-JsonAtomic -Path $script:EvidenceFile -Value $evidence
            }
            else {
                # A pre-existing receipt is never overwritten; this branch is
                # only reachable when the caller's path raced with the run.
                throw "Refusing to overwrite raced evidence file: $($script:EvidenceFile)"
            }
        }
    }
    catch {
        [Console]::Error.WriteLine("Could not write Project UI evidence receipt: $($_.Exception.Message)")
        $exitCode = 7
    }
}

if (-not [string]::IsNullOrWhiteSpace($script:EvidenceFile)) { Write-Output $script:EvidenceFile }
exit $exitCode
