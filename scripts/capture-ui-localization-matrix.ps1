[CmdletBinding(PositionalBinding = $false)]
param(
    [switch]$SkipBuild,
    [string]$OutputRoot = '',
    [string]$ProjectPath = 'D:\3D\NoemancerPlatformer',
    [ValidateRange(1, 120)][int]$Frames = 3,
    [ValidateRange(10, 600)][int]$TimeoutSeconds = 180
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$schema = 'noemancer.ui-localization-visual-matrix/0.1'
$qualitySchema = 'noemancer.render-quality.v1'
$logicalWidth = 1440
$logicalHeight = 900
$scales = @(1.0, 1.5, 2.0)
$locales = @('en-US', 'zh-CN', 'ar-SA')
$requiredScripts = @{ 'en-US' = 'Latin'; 'zh-CN' = 'Han'; 'ar-SA' = 'Arabic' }
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$projectRoot = [IO.Path]::GetFullPath($ProjectPath)
$runtime = Join-Path $repoRoot 'build\windows-msvc-debug\src\runtime\Release\noemancer.exe'
$runId = "$(Get-Date -Format 'yyyyMMdd-HHmmss')-$([guid]::NewGuid().ToString('N').Substring(0, 8))"
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $repoRoot "generated\acceptance\ui-localization-matrix-$runId"
}
$evidenceRoot = [IO.Path]::GetFullPath($OutputRoot)
$manifestPath = Join-Path $evidenceRoot 'manifest.json'
$issues = [System.Collections.Generic.List[object]]::new()
$runs = [System.Collections.Generic.List[object]]::new()
$exitCode = 1

function Add-Issue {
    param([string]$Code, [string]$Stage, [string]$Detail, [string]$RunId = '')
    $item = [ordered]@{ code = $Code; stage = $Stage; detail = $Detail }
    if (-not [string]::IsNullOrWhiteSpace($RunId)) { $item.runId = $RunId }
    [void]$issues.Add([pscustomobject]$item)
}

function Write-Utf8Atomic {
    param([string]$Path, [string]$Text)
    $parent = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
        [void](New-Item -ItemType Directory -Path $parent -Force)
    }
    $temporary = "$Path.tmp-$([guid]::NewGuid().ToString('N'))"
    try {
        [IO.File]::WriteAllText($temporary, $Text, [Text.UTF8Encoding]::new($false))
        Move-Item -LiteralPath $temporary -Destination $Path -Force
    }
    finally {
        if (Test-Path -LiteralPath $temporary -PathType Leaf) {
            Remove-Item -LiteralPath $temporary -Force
        }
    }
}

function Write-JsonAtomic {
    param([string]$Path, $Value)
    Write-Utf8Atomic -Path $Path -Text (($Value | ConvertTo-Json -Depth 100) + "`n")
}

function Get-Value {
    param([AllowNull()]$Object, [string]$Name)
    if ($null -eq $Object) { return $null }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}

function Get-RetainedProbeHeight {
    param([AllowNull()]$Observation)
    $nodes = Get-Value $Observation 'nodes'
    if ($null -eq $nodes) { return $null }
    foreach ($node in @($nodes)) {
        if ([string](Get-Value $node 'role') -ceq 'group' -and [bool](Get-Value $node 'visible')) {
            $layout = Get-Value $node 'layout'
            $height = Get-Value $layout 'height'
            if ($null -ne $height -and [double]$height -gt 0.0) { return [double]$height }
        }
    }
    return $null
}

function Get-RelativeEvidencePath {
    param([string]$Path)
    return [IO.Path]::GetRelativePath($evidenceRoot, [IO.Path]::GetFullPath($Path)).Replace('\', '/')
}

function Get-Artifact {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    $item = Get-Item -LiteralPath $Path
    return [ordered]@{
        path = Get-RelativeEvidencePath $Path
        bytes = $item.Length
        sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

function Get-ProjectFingerprint {
    param([string]$Root)
    $records = [System.Collections.Generic.List[string]]::new()
    $files = Get-ChildItem -LiteralPath $Root -Recurse -File -Force -ErrorAction Stop |
        Where-Object {
            $relative = [IO.Path]::GetRelativePath($Root, $_.FullName).Replace('\', '/')
            $parts = $relative -split '/'
            $parts -notcontains '.git' -and $parts -notcontains 'bin' -and
                $parts -notcontains 'obj' -and $parts -notcontains 'generated' -and
                $parts -notcontains 'package'
        } | Sort-Object FullName
    foreach ($file in $files) {
        $relative = [IO.Path]::GetRelativePath($Root, $file.FullName).Replace('\', '/')
        $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        [void]$records.Add("$relative|$($file.Length)|$hash")
    }
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes(($records -join "`n"))
    $hasher = [Security.Cryptography.SHA256]::Create()
    try { return ([Convert]::ToHexString($hasher.ComputeHash($bytes))).ToLowerInvariant() }
    finally { $hasher.Dispose() }
}

function Invoke-HiddenProcess {
    param(
        [string]$Executable,
        [string[]]$Arguments,
        [string]$WorkingDirectory,
        [string]$StdoutPath,
        [string]$StderrPath,
        [int]$Timeout
    )
    $psi = [Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = [IO.Path]::GetFullPath($Executable)
    $psi.WorkingDirectory = [IO.Path]::GetFullPath($WorkingDirectory)
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    foreach ($argument in $Arguments) { [void]$psi.ArgumentList.Add([string]$argument) }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $psi
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $timedOut = $false
    $exit = $null
    $stdout = ''
    $stderr = ''
    try {
        if (-not $process.Start()) { throw "Could not start $Executable" }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($Timeout * 1000)) {
            $timedOut = $true
            try { $process.Kill($true) } catch { }
            [void]$process.WaitForExit(5000)
        }
        $stdout = $stdoutTask.GetAwaiter().GetResult()
        $stderr = $stderrTask.GetAwaiter().GetResult()
        if (-not $timedOut) { $exit = $process.ExitCode }
    }
    finally {
        $watch.Stop()
        $process.Dispose()
    }
    Write-Utf8Atomic -Path $StdoutPath -Text $stdout
    Write-Utf8Atomic -Path $StderrPath -Text $stderr
    return [pscustomobject]@{
        exitCode = $exit
        timedOut = $timedOut
        durationMs = [int]$watch.ElapsedMilliseconds
        stdout = $stdout
        stderr = $stderr
    }
}

function Read-JsonLines {
    param([string]$Text)
    $items = [System.Collections.Generic.List[object]]::new()
    $invalid = 0
    foreach ($line in ($Text -split "`r?`n")) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        try { [void]$items.Add(($line | ConvertFrom-Json -Depth 100)) }
        catch { $invalid++ }
    }
    return [pscustomobject]@{ items = @($items.ToArray()); invalidLines = $invalid }
}

function Get-LastEventMessage {
    param([object[]]$Items, [string[]]$Names)
    $event = $null
    foreach ($item in $Items) {
        if ($Names -contains [string](Get-Value $item 'event')) { $event = $item }
    }
    if ($null -eq $event) { return [pscustomobject]@{ status = 'missing'; event = $null; value = $null } }
    $message = Get-Value $event 'message'
    if ($message -is [string]) {
        try { $message = $message | ConvertFrom-Json -Depth 100 }
        catch { return [pscustomobject]@{ status = 'invalid'; event = $event; value = $null } }
    }
    return [pscustomobject]@{ status = 'present'; event = $event; value = $message }
}

function Get-BmpDimensions {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 26 -or $bytes[0] -ne 0x42 -or $bytes[1] -ne 0x4D) { return $null }
    return [pscustomobject]@{
        width = [BitConverter]::ToInt32($bytes, 18)
        height = [Math]::Abs([BitConverter]::ToInt32($bytes, 22))
    }
}

function Test-FallbackFace {
    param($Capabilities, [string]$Script)
    if ($null -eq $Capabilities) { return $false }
    if (-not [bool](Get-Value $Capabilities 'requiredScriptFallbackAvailable')) { return $false }
    $fontSelection = Get-Value $Capabilities 'fontSelection'
    if ($null -eq $fontSelection -or -not [bool](Get-Value $fontSelection 'platformResolved')) { return $false }
    foreach ($face in @(Get-Value $Capabilities 'platformFallbackFaces')) {
        if (-not [bool](Get-Value $face 'available')) { continue }
        foreach ($covered in @(Get-Value $face 'scripts')) {
            if ([string]$covered -ceq $Script -or ($Script -ceq 'Latin' -and [string]$covered -ceq 'Latin')) {
                return $true
            }
        }
    }
    return $Script -ceq 'Latin'
}

$manifest = [ordered]@{
    schemaVersion = $schema
    status = 'failed'
    pass = $false
    capturedAt = [DateTime]::UtcNow.ToString('o')
    project = [ordered]@{ path = $projectRoot; fingerprintBefore = $null; fingerprintAfter = $null; unchanged = $false }
    runtime = [ordered]@{ path = $runtime; configuration = 'Release'; build = $null }
    logicalCanvas = [ordered]@{ width = $logicalWidth; height = $logicalHeight }
    requestedMatrix = [ordered]@{ scales = $scales; locales = $locales; runCount = $scales.Count * $locales.Count }
    runs = @()
    issues = @()
}

try {
    if (Test-Path -LiteralPath $manifestPath -PathType Leaf) { throw "Refusing to overwrite $manifestPath" }
    if (-not (Test-Path -LiteralPath $projectRoot -PathType Container)) { throw "Project does not exist: $projectRoot" }
    [void](New-Item -ItemType Directory -Path (Join-Path $evidenceRoot 'runs') -Force)
    [void](New-Item -ItemType Directory -Path (Join-Path $evidenceRoot 'logs') -Force)
    $manifest.project.fingerprintBefore = Get-ProjectFingerprint $projectRoot

    if (-not $SkipBuild) {
        $shell = (Get-Process -Id $PID).Path
        $buildStdout = Join-Path $evidenceRoot 'logs\build.stdout.log'
        $buildStderr = Join-Path $evidenceRoot 'logs\build.stderr.log'
        $build = Invoke-HiddenProcess -Executable $shell -Arguments @(
            '-NoLogo', '-NoProfile', '-NonInteractive', '-File', (Join-Path $repoRoot 'scripts\engine.ps1'),
            'build', '-Config', 'Release', '-Target', 'noemancer') -WorkingDirectory $repoRoot `
            -StdoutPath $buildStdout -StderrPath $buildStderr -Timeout 600
        $manifest.runtime.build = [ordered]@{
            skipped = $false; exitCode = $build.exitCode; timedOut = $build.timedOut; durationMs = $build.durationMs
            stdout = Get-Artifact $buildStdout; stderr = Get-Artifact $buildStderr
        }
        if ($build.timedOut -or $build.exitCode -ne 0) {
            Add-Issue 'matrix.build-failed' 'build' "Release noemancer build failed or timed out (exit=$($build.exitCode))."
            throw 'Release runtime build failed.'
        }
    }
    else { $manifest.runtime.build = [ordered]@{ skipped = $true } }
    if (-not (Test-Path -LiteralPath $runtime -PathType Leaf)) { throw "Release runtime does not exist: $runtime" }
    $manifest.runtime.sha256 = (Get-FileHash -LiteralPath $runtime -Algorithm SHA256).Hash.ToLowerInvariant()

    foreach ($scale in $scales) {
        foreach ($locale in $locales) {
            $scaleText = $scale.ToString('0.0', [Globalization.CultureInfo]::InvariantCulture)
            $id = "scale-$($scaleText.Replace('.', '_'))-$locale"
            $runRoot = Join-Path $evidenceRoot "runs\$id"
            [void](New-Item -ItemType Directory -Path $runRoot -Force)
            $image = Join-Path $runRoot 'editor.bmp'
            $qualityPath = "$image.quality.json"
            $stdoutPath = Join-Path $runRoot 'stdout.jsonl'
            $stderrPath = Join-Path $runRoot 'stderr.log'
            $arguments = @(
                'run', '--format', 'json', '--frames', [string]$Frames,
                '--project', $projectRoot, '--gpu-backend', 'direct3d12',
                '--capture-editor-frame', $image,
                '--window-width', [string]$logicalWidth, '--window-height', [string]$logicalHeight,
                '--ui-scale', $scaleText, '--ui-locale', $locale
            )
            $process = Invoke-HiddenProcess -Executable $runtime -Arguments $arguments -WorkingDirectory $repoRoot `
                -StdoutPath $stdoutPath -StderrPath $stderrPath -Timeout $TimeoutSeconds
            $jsonLines = Read-JsonLines $process.stdout
            $display = Get-LastEventMessage @($jsonLines.items) @('ui.display_configuration')
            $text = Get-LastEventMessage @($jsonLines.items) @('ui.text_capabilities')
            $retained = Get-LastEventMessage @($jsonLines.items) @(
                'ui.retained_observation', 'ui.retained.observation', 'ui.retained_ui_observation')
            $retainedSurfaces = Get-LastEventMessage @($jsonLines.items) @('ui.retained_surface_observations')
            $editorSemantic = Get-LastEventMessage @($jsonLines.items) @('editor.semantic_snapshot')
            $quality = $null
            if (Test-Path -LiteralPath $qualityPath -PathType Leaf) {
                try { $quality = Get-Content -LiteralPath $qualityPath -Raw -Encoding UTF8 | ConvertFrom-Json -Depth 100 }
                catch { Add-Issue 'matrix.quality-invalid' 'quality' $_.Exception.Message $id }
            }
            $dimensions = Get-BmpDimensions $image
            $requiredScript = [string]$requiredScripts[$locale]
            $displayValue = $display.value
            $textValue = $text.value
            $retainedValue = $retained.value
            $displayScale = Get-Value $displayValue 'requestedUiScale'
            $displayLocale = [string](Get-Value $displayValue 'locale')
            $windowPixels = Get-Value $displayValue 'windowPixels'
            $displayWidth = Get-Value $windowPixels 'width'
            $displayHeight = Get-Value $windowPixels 'height'
            $layoutDiagnostics = Get-Value $retainedValue 'layoutDiagnostics'
            $actionableOverflowCount = Get-Value $layoutDiagnostics 'actionableOverflowCount'
            $segmentation = Get-Value $textValue 'segmentation'
            $surfaceValue = $retainedSurfaces.value
            $surfaceInspector = Get-Value $surfaceValue 'inspector'
            $surfaceViewport = Get-Value $surfaceInspector 'viewport'
            $surfaceDensity = Get-Value $surfaceViewport 'densityIndependentPixelRatio'
            $surfaceText = Get-Value $surfaceInspector 'text'
            $surfaceShaping = Get-Value $surfaceText 'shapingStats'
            $surfaceOverflows = @()
            $surfaceObservationsValid = $true
            foreach ($surfaceName in @('inspector', 'outliner', 'assetBrowser')) {
                $surface = Get-Value $surfaceValue $surfaceName
                $diagnostics = Get-Value $surface 'layoutDiagnostics'
                $surfaceOverflow = Get-Value $diagnostics 'actionableOverflowCount'
                if ($null -eq $surface -or -not [bool](Get-Value $surface 'valid') -or $null -eq $surfaceOverflow) {
                    $surfaceObservationsValid = $false
                }
                else { $surfaceOverflows += [int]$surfaceOverflow }
            }
            $semanticValue = $editorSemantic.value
            $semanticChrome = Get-Value $semanticValue 'editorChrome'
            $semanticPanels = Get-Value $semanticChrome 'retainedPanels'
            $semanticInspector = Get-Value $semanticPanels 'inspector'
            $semanticLocalization = Get-Value $semanticInspector 'localizationDiagnostics'
            $retainedProbeHeight = Get-RetainedProbeHeight $surfaceInspector
            $checks = [ordered]@{
                processExitZero = -not $process.timedOut -and $process.exitCode -eq 0
                stdoutJsonLinesValid = $jsonLines.invalidLines -eq 0
                imageExists = Test-Path -LiteralPath $image -PathType Leaf
                qualityExists = Test-Path -LiteralPath $qualityPath -PathType Leaf
                imageDimensions = $null -ne $dimensions -and $dimensions.width -eq $logicalWidth -and $dimensions.height -eq $logicalHeight
                qualityPassed = $null -ne $quality -and [string](Get-Value $quality 'schemaVersion') -eq $qualitySchema -and
                    [bool](Get-Value $quality 'pass') -and [bool](Get-Value $quality 'dimensionsMatch') -and
                    [int](Get-Value $quality 'width') -eq $logicalWidth -and [int](Get-Value $quality 'height') -eq $logicalHeight
                displayObserved = $display.status -eq 'present'
                displaySchemaMatched = [string](Get-Value $displayValue 'schemaVersion') -ceq 'noemancer.ui-display-configuration/0.1'
                localeMatched = $display.status -eq 'present' -and $displayLocale -ceq $locale
                scaleMatched = $display.status -eq 'present' -and $null -ne $displayScale -and
                    [Math]::Abs([double]$displayScale - [double]$scale) -lt 0.001
                logicalCanvasMatched = $display.status -eq 'present' -and
                    [int]$displayWidth -eq $logicalWidth -and [int]$displayHeight -eq $logicalHeight
                textCapabilitiesObserved = $text.status -eq 'present'
                requiredScriptMatched = $text.status -eq 'present' -and
                    [string](Get-Value $textValue 'locale') -ceq $locale -and
                    [string](Get-Value $textValue 'requiredScript') -ceq $requiredScript
                arabicBidiReady = $locale -cne 'ar-SA' -or
                    ($text.status -eq 'present' -and [bool](Get-Value $segmentation 'bidirectionalLayout'))
                fontFallbackReady = $text.status -eq 'present' -and (Test-FallbackFace $textValue $requiredScript)
                retainedFallbackReady = $retainedSurfaces.status -eq 'present' -and
                    (($requiredScript -ceq 'Latin') -or
                     ($requiredScript -ceq 'Han' -and [bool](Get-Value $surfaceText 'cjkFallbackReady')) -or
                     ($requiredScript -ceq 'Arabic' -and [bool](Get-Value $surfaceText 'complexScriptFallbackReady')))
                retainedShapingSucceeded = $retainedSurfaces.status -eq 'present' -and $null -ne $surfaceShaping -and
                    [int64](Get-Value $surfaceShaping 'failures') -eq 0 -and
                    (($requiredScript -ceq 'Latin') -or [int64](Get-Value $surfaceShaping 'fallbackRuns') -gt 0)
                retainedObservationObserved = $retained.status -eq 'present'
                overflowFree = $retained.status -eq 'present' -and $null -ne $actionableOverflowCount -and
                    [int]$actionableOverflowCount -eq 0
                retainedSurfacesObserved = $retainedSurfaces.status -eq 'present'
                retainedSurfacesSchemaMatched = [string](Get-Value $surfaceValue 'schemaVersion') -ceq
                    'noemancer.ui-retained-surface-observations/0.1'
                surfaceDensityMatched = $retainedSurfaces.status -eq 'present' -and $null -ne $surfaceDensity -and
                    [Math]::Abs([double]$surfaceDensity - [double]$scale) -lt 0.001
                editorSurfacesOverflowFree = $retainedSurfaces.status -eq 'present' -and
                    $surfaceObservationsValid -and $surfaceOverflows.Count -eq 3 -and
                    @($surfaceOverflows | Where-Object { $_ -ne 0 }).Count -eq 0
                editorSemanticObserved = $editorSemantic.status -eq 'present'
                inspectorLocaleMatched = $editorSemantic.status -eq 'present' -and
                    [string](Get-Value $semanticInspector 'locale') -ceq $locale -and
                    [string](Get-Value $semanticLocalization 'requiredScript') -ceq $requiredScript
                inspectorLocalized = $editorSemantic.status -eq 'present' -and
                    ($locale -ceq 'en-US' -or [int](Get-Value $semanticLocalization 'localizedCount') -gt 0)
                inspectorDirectionMatched = $editorSemantic.status -eq 'present' -and
                    (($locale -ceq 'ar-SA' -and [string](Get-Value $semanticInspector 'textDirection') -ceq 'rtl') -or
                     ($locale -cne 'ar-SA' -and [string](Get-Value $semanticInspector 'textDirection') -ceq 'ltr'))
            }
            $runPass = -not ($checks.Values -contains $false)
            if (-not $runPass) {
                foreach ($check in $checks.GetEnumerator()) {
                    if (-not [bool]$check.Value) {
                        Add-Issue "matrix.$($check.Key)" 'run-gate' "Gate '$($check.Key)' failed." $id
                    }
                }
            }
            [void]$runs.Add([pscustomobject][ordered]@{
                runId = $id
                pass = $runPass
                requested = [ordered]@{ scale = $scale; locale = $locale; requiredScript = $requiredScript;
                    logicalWidth = $logicalWidth; logicalHeight = $logicalHeight }
                command = [ordered]@{ executable = $runtime; arguments = $arguments; workingDirectory = $repoRoot;
                    hidden = $true; computerUse = $false }
                process = [ordered]@{ exitCode = $process.exitCode; timedOut = $process.timedOut;
                    durationMs = $process.durationMs; invalidJsonLines = $jsonLines.invalidLines }
                artifacts = [ordered]@{ image = Get-Artifact $image; quality = Get-Artifact $qualityPath;
                    stdout = Get-Artifact $stdoutPath; stderr = Get-Artifact $stderrPath }
                dimensions = $dimensions
                metrics = [ordered]@{ retainedProbeHeight = $retainedProbeHeight }
                observations = [ordered]@{
                    displayConfiguration = [ordered]@{ status = $display.status; value = $display.value }
                    textCapabilities = [ordered]@{ status = $text.status; value = $text.value }
                    retained = [ordered]@{ status = $retained.status; value = $retained.value }
                    retainedSurfaces = [ordered]@{ status = $retainedSurfaces.status; value = $retainedSurfaces.value }
                    finalEditorSemantic = [ordered]@{ status = $editorSemantic.status; value = $editorSemantic.value }
                }
                checks = $checks
            })
        }
    }

    $manifest.project.fingerprintAfter = Get-ProjectFingerprint $projectRoot
    $manifest.project.unchanged = $manifest.project.fingerprintBefore -ceq $manifest.project.fingerprintAfter
    if (-not $manifest.project.unchanged) {
        Add-Issue 'matrix.project-modified' 'integrity' 'The source project changed during the hidden capture matrix.'
    }
    foreach ($locale in $locales) {
        $baseline = $runs | Where-Object { $_.requested.locale -ceq $locale -and [double]$_.requested.scale -eq 1.0 } |
            Select-Object -First 1
        $double = $runs | Where-Object { $_.requested.locale -ceq $locale -and [double]$_.requested.scale -eq 2.0 } |
            Select-Object -First 1
        $densityLayoutPassed = $null -ne $baseline -and $null -ne $double -and
            $null -ne $baseline.metrics.retainedProbeHeight -and $null -ne $double.metrics.retainedProbeHeight -and
            [double]$double.metrics.retainedProbeHeight -gt [double]$baseline.metrics.retainedProbeHeight * 1.6
        if (-not $densityLayoutPassed) {
            Add-Issue 'matrix.retained-density-no-layout-effect' 'cross-run-gate' `
                "Retained group geometry did not grow meaningfully from UI scale 1.0 to 2.0 for $locale."
        }
    }
    $allRunsPassed = $runs.Count -eq ($scales.Count * $locales.Count) -and
        @($runs | Where-Object { -not $_.pass }).Count -eq 0
    $manifest.pass = $allRunsPassed -and $manifest.project.unchanged -and $issues.Count -eq 0
    $manifest.status = if ($manifest.pass) { 'passed' } else { 'failed' }
    $exitCode = if ($manifest.pass) { 0 } else { 4 }
}
catch {
    Add-Issue 'matrix.exception' 'verifier' $_.Exception.Message
    if ($null -eq $manifest.project.fingerprintAfter -and (Test-Path -LiteralPath $projectRoot -PathType Container)) {
        try {
            $manifest.project.fingerprintAfter = Get-ProjectFingerprint $projectRoot
            $manifest.project.unchanged = $manifest.project.fingerprintBefore -ceq $manifest.project.fingerprintAfter
        } catch { }
    }
    $exitCode = 2
}
finally {
    $manifest.runs = @($runs.ToArray())
    $manifest.issues = @($issues.ToArray())
    $manifest.pass = $exitCode -eq 0
    $manifest.status = if ($manifest.pass) { 'passed' } else { 'failed' }
    try { Write-JsonAtomic -Path $manifestPath -Value $manifest }
    catch {
        [Console]::Error.WriteLine("Could not write UI localization matrix manifest: $($_.Exception.Message)")
        $exitCode = 3
    }
}

Write-Output $manifestPath
exit $exitCode
