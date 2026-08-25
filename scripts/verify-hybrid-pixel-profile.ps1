[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$ProjectRoot = $(if ($env:NOEMANCER_PLATFORMER_PROJECT) { $env:NOEMANCER_PLATFORMER_PROJECT } else { Join-Path ([IO.Path]::GetPathRoot($PSScriptRoot)) '3D\NoemancerPlatformer' }),

    # The verifier never builds the native engine.  When omitted, use the
    # same configuration-specific path as scripts/engine.ps1.
    [string]$RuntimePath = '',

    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Release',

    [ValidateRange(1, 60)]
    [int]$Frames = 3,

    [ValidateRange(10, 600)]
    [int]$TimeoutSeconds = 120,

    [string]$OutputRoot = ''
)

# Exit contract:
#   0 = the copied project, package Player, and requested Hybrid Pixel evidence passed
#   2 = invalid project/runtime/output invocation
#   3 = a required contract or bounded process probe failed
#   1 = unexpected verifier failure

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:EngineRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$script:Issues = [System.Collections.Generic.List[object]]::new()
$script:MaxIssues = 128
# A real Release Package receipt includes the complete app-local .NET/file
# closure and is currently about 600 KiB.  Keep it bounded, but large enough
# to parse the authoritative JSON before reducing it to the receipt summary.
$script:MaxCapturedBytes = 4MB
$script:OutputRootPath = $null
$script:TemporaryProjectRoot = $null
$script:DiagnosticsRoot = $null
$script:SourceManifestHashBefore = ''
$script:SourceManifestHashAfter = ''
$script:SourceProjectHashBefore = ''
$script:SourceProjectHashAfter = ''
$script:ExpectedProfile = $null
$script:SourceHeadless = $null
$script:Package = $null
$script:PackageHeadless = $null
$script:ShaderClosure = $null
$script:GameProfileContract = $null
$script:ExpectedShaderCount = 37
$script:SourceCaptures = [System.Collections.Generic.List[object]]::new()
$script:PackageCaptures = [System.Collections.Generic.List[object]]::new()

$script:VirtualWidth = 320
$script:VirtualHeight = 180
$script:PixelsPerUnit = 16.0
$script:WindowSizes = @(
    [ordered]@{ id = 'letterbox-1440x900'; width = 1440; height = 900 },
    [ordered]@{ id = 'odd-letterbox-1439x899'; width = 1439; height = 899 },
    [ordered]@{ id = 'exact-1920x1080'; width = 1920; height = 1080 }
)
$script:Backends = @('direct3d12', 'vulkan')

function Get-PropertyValue {
    param(
        [AllowNull()]$Object,
        [Parameter(Mandatory = $true)][string]$Name
    )

    if ($null -eq $Object) { return $null }
    if ($Object -is [System.Collections.IDictionary] -and $Object.Contains($Name)) {
        return $Object[$Name]
    }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}

function Add-Issue {
    param(
        [Parameter(Mandatory = $true)][string]$Code,
        [Parameter(Mandatory = $true)][string]$Stage,
        [Parameter(Mandatory = $true)][string]$Message,
        [string]$Path = ''
    )

    if ($script:Issues.Count -ge $script:MaxIssues) { return }
    $issue = [ordered]@{
        code = $Code
        stage = $Stage
        message = $Message
    }
    if (-not [string]::IsNullOrWhiteSpace($Path)) { $issue.path = $Path }
    [void]$script:Issues.Add([pscustomobject]$issue)
}

function Get-FullPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    return [IO.Path]::GetFullPath($Path)
}

function Get-Sha256 {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return '' }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-TextSha256 {
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Text)

    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [Text.UTF8Encoding]::new($false).GetBytes($Text)
        return ([BitConverter]::ToString($algorithm.ComputeHash($bytes)) -replace '-', '').ToLowerInvariant()
    }
    finally {
        $algorithm.Dispose()
    }
}

function Get-ProjectTreeHash {
    param(
        [Parameter(Mandatory = $true)][string]$ProjectPath,
        [Parameter(Mandatory = $true)][string]$Stage
    )

    if (-not (Test-Path -LiteralPath $ProjectPath -PathType Container)) { return '' }
    $ignoredPattern = '(?i)[\\/](?:\.git|bin|obj|generated)[\\/]'
    $maximumFiles = 4096
    $maximumBytes = 512MB
    try {
        $files = @(
            Get-ChildItem -LiteralPath $ProjectPath -Recurse -File -Force -ErrorAction Stop |
                Where-Object { $_.FullName -notmatch $ignoredPattern } |
                Sort-Object FullName
        )
        if ($files.Count -gt $maximumFiles) {
            Add-Issue -Code 'project.hash-file-limit' -Stage $Stage -Message "Project hash contains $($files.Count) files; the bounded limit is $maximumFiles."
            return ''
        }
        $totalBytes = [int64]0
        $lines = [System.Collections.Generic.List[string]]::new()
        foreach ($file in $files) {
            $totalBytes += [int64]$file.Length
            if ($totalBytes -gt $maximumBytes) {
                Add-Issue -Code 'project.hash-byte-limit' -Stage $Stage -Message "Project hash exceeds the bounded $maximumBytes byte limit."
                return ''
            }
            $relative = $file.FullName.Substring($ProjectPath.TrimEnd('\', '/').Length).TrimStart('\', '/')
            $relative = $relative.Replace('\', '/')
            [void]$lines.Add("$relative|$($file.Length)|$(Get-Sha256 $file.FullName)")
        }
        return Get-TextSha256 (($lines | Sort-Object) -join "`n")
    }
    catch {
        Add-Issue -Code 'project.hash-failed' -Stage $Stage -Message "Could not hash the source project tree: $($_.Exception.Message)"
        return ''
    }
}

function Write-Utf8 {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Text
    )

    $encoding = [Text.UTF8Encoding]::new($false)
    [IO.File]::WriteAllText($Path, $Text, $encoding)
}

function Get-RelativeArtifactPath {
    param([AllowNull()][string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path)) { return $null }
    if ($null -eq $script:OutputRootPath) { return $Path.Replace('\', '/') }
    $full = Get-FullPath $Path
    $root = $script:OutputRootPath.TrimEnd('\', '/')
    if ($full.Equals($root, [StringComparison]::OrdinalIgnoreCase)) { return '.' }
    $prefix = $root + [IO.Path]::DirectorySeparatorChar
    if ($full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        return $full.Substring($prefix.Length).Replace('\', '/')
    }
    return $full.Replace('\', '/')
}

function Resolve-PackageArtifactPath {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$RelativePath,
        [Parameter(Mandatory = $true)][string]$Stage,
        [Parameter(Mandatory = $true)][string]$Field
    )

    if ([string]::IsNullOrWhiteSpace($RelativePath) -or
        [IO.Path]::IsPathRooted(($RelativePath -replace '/', '\')) -or
        $RelativePath -match '(^|[\\/])\.\.(?:[\\/]|$)') {
        Add-Issue -Code 'artifact.path-invalid' -Stage $Stage -Path $Field -Message "Package artifact path '$RelativePath' is not a safe relative path."
        return $null
    }
    $rootFull = Get-FullPath $Root
    $candidate = Get-FullPath (Join-Path $rootFull ($RelativePath -replace '/', '\'))
    $prefix = $rootFull.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if (-not $candidate.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        Add-Issue -Code 'artifact.path-escape' -Stage $Stage -Path $Field -Message "Package artifact path '$RelativePath' escapes the package root."
        return $null
    }
    return $candidate
}

function Read-JsonFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Stage,
        [Parameter(Mandatory = $true)][string]$Label
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        Add-Issue -Code 'file.missing' -Stage $Stage -Path (Get-RelativeArtifactPath $Path) -Message "$Label is missing."
        return $null
    }
    try {
        $raw = [IO.File]::ReadAllText($Path)
        if ([string]::IsNullOrWhiteSpace($raw)) {
            Add-Issue -Code 'json.empty' -Stage $Stage -Path (Get-RelativeArtifactPath $Path) -Message "$Label is empty."
            return $null
        }
        return ConvertFrom-Json -InputObject $raw -Depth 100
    }
    catch {
        Add-Issue -Code 'json.invalid' -Stage $Stage -Path (Get-RelativeArtifactPath $Path) -Message "$Label is not valid JSON: $($_.Exception.Message)"
        return $null
    }
}

function Limit-Text {
    param([AllowNull()][string]$Text)

    if ($null -eq $Text) { $Text = '' }
    $encoding = [Text.UTF8Encoding]::new($false)
    $bytes = $encoding.GetByteCount($Text)
    if ($bytes -le $script:MaxCapturedBytes) {
        return [ordered]@{ text = $Text; bytes = $bytes; truncated = $false }
    }

    $characters = [Math]::Min($Text.Length, $script:MaxCapturedBytes)
    while ($characters -gt 0 -and $encoding.GetByteCount($Text.Substring(0, $characters)) -gt $script:MaxCapturedBytes) {
        $characters--
    }
    $limited = if ($characters -gt 0) { $Text.Substring(0, $characters) } else { '' }
    return [ordered]@{ text = $limited; bytes = $bytes; truncated = $true }
}

function Quote-CommandToken {
    param([Parameter(Mandatory = $true)][string]$Token)
    if ($Token -notmatch '[\s"]') { return $Token }
    return '"' + ($Token -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1') + '"'
}

function ConvertTo-CommandLine {
    param(
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )
    return ((@($Executable) + @($Arguments)) | ForEach-Object { Quote-CommandToken ([string]$_) }) -join ' '
}

function Copy-ProjectSnapshot {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination
    )

    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    $ignored = @('.git', 'bin', 'obj', 'generated')
    foreach ($child in @(Get-ChildItem -LiteralPath $Source -Force)) {
        if ($ignored -contains $child.Name) { continue }
        Copy-Item -LiteralPath $child.FullName -Destination (Join-Path $Destination $child.Name) -Recurse -Force
    }
}

function Inject-HybridPixelProfile {
    param([Parameter(Mandatory = $true)][string]$ProjectPath)

    $manifestPath = Join-Path $ProjectPath 'noemancer.project.json'
    $manifest = Read-JsonFile -Path $manifestPath -Stage 'profileInjection' -Label 'Copied project manifest'
    if ($null -eq $manifest) { return $null }

    $profile = [pscustomobject][ordered]@{
        schema = 'noemancer.hybrid-pixel-profile/0.1'
        profileId = 'lumen-run-hybrid-pixel-core'
        enabled = $true
        virtualWidth = $script:VirtualWidth
        virtualHeight = $script:VirtualHeight
        pixelsPerUnit = $script:PixelsPerUnit
        integerScaling = $true
        snapCamera = $true
        snapSprites = $true
        presentationFilter = 'nearest'
    }
    $manifest.schema = 'noemancer.project/0.2'
    $existing = $manifest.PSObject.Properties['hybridPixelProfile']
    if ($null -ne $existing) {
        $manifest.hybridPixelProfile = $profile
    }
    else {
        Add-Member -InputObject $manifest -MemberType NoteProperty -Name hybridPixelProfile -Value $profile
    }
    Write-Utf8 -Path $manifestPath -Text ($manifest | ConvertTo-Json -Depth 100)
    return $profile
}

function Test-ProjectNativeBoundary {
    param([Parameter(Mandatory = $true)][string]$ProjectPath)

    $nativeExtensions = @('.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp', '.hxx', '.inl', '.ixx', '.m', '.mm', '.cmake', '.vcxproj')
    $ignoredPattern = '(?i)[\\/](?:\.git|bin|obj|generated)[\\/]'
    $nativeFiles = @(
        Get-ChildItem -LiteralPath $ProjectPath -Recurse -File -Force -ErrorAction SilentlyContinue |
            Where-Object {
                $_.FullName -notmatch $ignoredPattern -and
                ($nativeExtensions -contains $_.Extension.ToLowerInvariant() -or $_.Name -eq 'CMakeLists.txt')
            } |
            ForEach-Object { $_.FullName }
    )
    foreach ($file in $nativeFiles) {
        Add-Issue -Code 'native.game-source' -Stage 'nativeBoundary' -Path (Get-RelativeArtifactPath $file) -Message 'The copied Lumen Run project contains native C/C++ or native build source.'
    }

    $referenceExtensions = @('.cs', '.csproj', '.props', '.targets', '.cmake', '.sln', '.vcxproj')
    $referencePatterns = @(
        '(?i)\b(?:DllImport|LibraryImport|UnmanagedCallersOnly|Marshal\.GetDelegateForFunctionPointer)\b',
        '(?i)\b(?:cmake_minimum_required|add_subdirectory|target_link_libraries|add_library|add_executable)\b',
        '(?i)(?:src[\\/]engine|src[\\/]runtime|src[\\/]editor|noemancer[._-]?native|noemancer[._-]?engine)'
    )
    $nativeReferences = [System.Collections.Generic.List[object]]::new()
    foreach ($file in @(Get-ChildItem -LiteralPath $ProjectPath -Recurse -File -Force -ErrorAction SilentlyContinue)) {
        if ($file.FullName -match $ignoredPattern -or $referenceExtensions -notcontains $file.Extension.ToLowerInvariant()) { continue }
        if ($file.Length -gt 2MB) { continue }
        try { $text = [IO.File]::ReadAllText($file.FullName) } catch { continue }
        foreach ($pattern in $referencePatterns) {
            $match = [regex]::Match($text, $pattern)
            if (-not $match.Success) { continue }
            $line = ($text.Substring(0, $match.Index) -split "`r?`n").Count
            $reference = [ordered]@{
                file = Get-RelativeArtifactPath $file.FullName
                line = $line
                sample = $match.Value
            }
            [void]$nativeReferences.Add([pscustomobject]$reference)
            Add-Issue -Code 'native.game-reference' -Stage 'nativeBoundary' -Path $reference.file -Message "Project source contains a native engine/build reference at line ${line}: $($match.Value)"
            break
        }
    }
    return [ordered]@{
        pass = ($nativeFiles.Count -eq 0 -and $nativeReferences.Count -eq 0)
        nativeFileCount = $nativeFiles.Count
        nativeFiles = @($nativeFiles | ForEach-Object { Get-RelativeArtifactPath $_ })
        nativeReferenceCount = $nativeReferences.Count
        nativeReferences = @($nativeReferences)
    }
}

function Invoke-BoundedProcess {
    param(
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [Parameter(Mandatory = $true)][string]$StdoutPath,
        [Parameter(Mandatory = $true)][string]$StderrPath,
        [Parameter(Mandatory = $true)][string]$Stage
    )

    $started = [DateTimeOffset]::UtcNow
    $commandLine = ConvertTo-CommandLine -Executable $Executable -Arguments $Arguments
    $stdoutText = ''
    $stderrText = ''
    $exitCode = -1
    $timedOut = $false
    $startError = ''
    $process = $null
    try {
        $info = [Diagnostics.ProcessStartInfo]::new()
        $info.FileName = $Executable
        $info.WorkingDirectory = $WorkingDirectory
        $info.UseShellExecute = $false
        $info.CreateNoWindow = $true
        $info.RedirectStandardOutput = $true
        $info.RedirectStandardError = $true
        $info.Environment['NOEMANCER_DIAGNOSTICS_DIR'] = $script:DiagnosticsRoot
        foreach ($argument in $Arguments) { [void]$info.ArgumentList.Add([string]$argument) }

        $process = [Diagnostics.Process]::new()
        $process.StartInfo = $info
        if (-not $process.Start()) { throw 'Process.Start returned false.' }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            $timedOut = $true
            try { $process.Kill($true) } catch { }
            try { [void]$process.WaitForExit(5000) } catch { }
        }
        try { $stdoutText = $stdoutTask.GetAwaiter().GetResult() } catch { $stdoutText = '' }
        try { $stderrText = $stderrTask.GetAwaiter().GetResult() } catch { $stderrText = '' }
        if ($process.HasExited) { $exitCode = $process.ExitCode }
    }
    catch {
        $startError = $_.Exception.Message
    }
    finally {
        if ($null -ne $process) { $process.Dispose() }
    }

    $stdoutLimited = Limit-Text $stdoutText
    $stderrLimited = Limit-Text $stderrText
    Write-Utf8 -Path $StdoutPath -Text $stdoutLimited.text
    Write-Utf8 -Path $StderrPath -Text $stderrLimited.text
    if ($stdoutLimited.truncated -or $stderrLimited.truncated) {
        Add-Issue -Code 'process.output-truncated' -Stage $Stage -Message 'A bounded process log exceeded the receipt capture limit; the probe is rejected.'
    }
    if (-not [string]::IsNullOrWhiteSpace($startError)) {
        Add-Issue -Code 'process.start-failed' -Stage $Stage -Message $startError
    }
    if ($timedOut) {
        Add-Issue -Code 'process.timeout' -Stage $Stage -Message "The process exceeded the $TimeoutSeconds second timeout and was terminated."
    }

    return [ordered]@{
        stage = $Stage
        executable = $Executable
        arguments = @($Arguments)
        commandLine = $commandLine
        workingDirectory = $WorkingDirectory
        exitCode = $exitCode
        timedOut = $timedOut
        durationMs = [int]([DateTimeOffset]::UtcNow.Subtract($started).TotalMilliseconds)
        stdoutText = $stdoutLimited.text
        stderrText = $stderrLimited.text
        stdoutPath = $StdoutPath
        stderrPath = $StderrPath
        stdoutBytes = $stdoutLimited.bytes
        stderrBytes = $stderrLimited.bytes
    }
}

function Get-ProcessEvidence {
    param([Parameter(Mandatory = $true)]$ProcessResult)
    return [ordered]@{
        stage = $ProcessResult.stage
        executable = $ProcessResult.executable
        arguments = @($ProcessResult.arguments)
        commandLine = $ProcessResult.commandLine
        workingDirectory = $ProcessResult.workingDirectory
        exitCode = $ProcessResult.exitCode
        timedOut = $ProcessResult.timedOut
        durationMs = $ProcessResult.durationMs
        stdout = [ordered]@{
            path = Get-RelativeArtifactPath $ProcessResult.stdoutPath
            bytes = $ProcessResult.stdoutBytes
            sha256 = Get-Sha256 $ProcessResult.stdoutPath
        }
        stderr = [ordered]@{
            path = Get-RelativeArtifactPath $ProcessResult.stderrPath
            bytes = $ProcessResult.stderrBytes
            sha256 = Get-Sha256 $ProcessResult.stderrPath
        }
    }
}

function Get-JsonLogEvents {
    param([Parameter(Mandatory = $true)][string]$Text)

    $events = [System.Collections.Generic.List[object]]::new()
    foreach ($line in ($Text -split "`r?`n")) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        try { $envelope = ConvertFrom-Json -InputObject $line -Depth 100 } catch { continue }
        $eventName = [string](Get-PropertyValue $envelope 'event')
        if ([string]::IsNullOrWhiteSpace($eventName)) { continue }
        $message = [string](Get-PropertyValue $envelope 'message')
        $payload = $null
        if (-not [string]::IsNullOrWhiteSpace($message)) {
            try { $payload = ConvertFrom-Json -InputObject $message -Depth 100 } catch { }
        }
        [void]$events.Add([pscustomobject]@{
            event = $eventName
            level = [string](Get-PropertyValue $envelope 'level')
            payload = $payload
            message = $message
        })
    }
    return @($events)
}

function Get-LastEvent {
    param(
        [Parameter(Mandatory = $true)]$Events,
        [Parameter(Mandatory = $true)][string]$Name
    )
    $matches = @($Events | Where-Object { $_.event -eq $Name })
    if ($matches.Count -eq 0) { return $null }
    return $matches[$matches.Count - 1]
}

function Invoke-HeadlessProbe {
    param(
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [Parameter(Mandatory = $true)][string]$Stage,
        [Parameter(Mandatory = $true)][bool]$RequirePlayerEvents
    )

    $stdoutPath = Join-Path $script:OutputRootPath ($Stage + '.stdout.jsonl')
    $stderrPath = Join-Path $script:OutputRootPath ($Stage + '.stderr.log')
    $process = Invoke-BoundedProcess -Executable $Executable -Arguments $Arguments -WorkingDirectory $WorkingDirectory `
        -StdoutPath $stdoutPath -StderrPath $stderrPath -Stage $Stage
    $events = Get-JsonLogEvents $process.stdoutText
    $stop = Get-LastEvent -Events $events -Name 'runtime.stop'
    $playerGeometry = Get-LastEvent -Events $events -Name 'player.geometry_loading'
    $playerScripting = Get-LastEvent -Events $events -Name 'player.scripting'
    $errorEvents = @($events | Where-Object { $_.level -eq 'error' })
    if ($process.exitCode -ne 0) {
        Add-Issue -Code 'headless.exit-failed' -Stage $Stage -Message "Headless probe exited with code $($process.exitCode)."
    }
    if ($null -eq $stop) {
        Add-Issue -Code 'headless.stop-missing' -Stage $Stage -Message 'Headless probe did not publish runtime.stop.'
    }
    if ($errorEvents.Count -gt 0) {
        Add-Issue -Code 'headless.error-event' -Stage $Stage -Message "Headless probe published $($errorEvents.Count) error event(s)."
    }
    if ($RequirePlayerEvents -and $null -eq $playerGeometry) {
        Add-Issue -Code 'headless.player-loading-missing' -Stage $Stage -Message 'Packaged Player headless probe did not publish player.geometry_loading.'
    }
    $result = [ordered]@{
        requested = $true
        pass = ($process.exitCode -eq 0 -and $null -ne $stop -and $errorEvents.Count -eq 0 -and (-not $RequirePlayerEvents -or $null -ne $playerGeometry))
        player = $RequirePlayerEvents
        process = Get-ProcessEvidence $process
        runtimeStop = if ($null -ne $stop) { [ordered]@{ event = $stop.event; message = $stop.message } } else { $null }
        playerGeometryLoading = if ($null -ne $playerGeometry) { $playerGeometry.payload } else { $null }
        playerScriptingPresent = ($null -ne $playerScripting)
    }
    return $result
}

function Get-ExpectedPresentation {
    param(
        [Parameter(Mandatory = $true)][int]$Width,
        [Parameter(Mandatory = $true)][int]$Height
    )

    if ($Width -ge $script:VirtualWidth -and $Height -ge $script:VirtualHeight) {
        $scale = [int][Math]::Min(
            [Math]::Floor([double]$Width / $script:VirtualWidth),
            [Math]::Floor([double]$Height / $script:VirtualHeight))
        $contentWidth = $script:VirtualWidth * $scale
        $contentHeight = $script:VirtualHeight * $scale
        $remainingWidth = $Width - $contentWidth
        $remainingHeight = $Height - $contentHeight
        $left = [int][Math]::Floor($remainingWidth / 2.0)
        $top = [int][Math]::Floor($remainingHeight / 2.0)
        return [ordered]@{
            status = if ($remainingWidth -eq 0 -and $remainingHeight -eq 0) { 'exact' } else { 'letterboxed' }
            code = if ($remainingWidth -eq 0 -and $remainingHeight -eq 0) { 'pixel-presentation.ok' } else { 'pixel-presentation.letterboxed' }
            integerScale = $scale
            physicalOutput = [ordered]@{ width = $Width; height = $Height }
            virtualExtent = [ordered]@{ width = $script:VirtualWidth; height = $script:VirtualHeight }
            contentRect = [ordered]@{ x = $left; y = $top; width = $contentWidth; height = $contentHeight }
            virtualRect = [ordered]@{ x = 0; y = 0; width = $script:VirtualWidth; height = $script:VirtualHeight }
            letterbox = [ordered]@{ left = $left; top = $top; right = $remainingWidth - $left; bottom = $remainingHeight - $top }
        }
    }

    $sourceWidth = [Math]::Min($script:VirtualWidth, $Width)
    $sourceHeight = [Math]::Min($script:VirtualHeight, $Height)
    return [ordered]@{
        status = 'undersized'
        code = 'pixel-presentation.undersized'
        integerScale = 1
        physicalOutput = [ordered]@{ width = $Width; height = $Height }
        virtualExtent = [ordered]@{ width = $script:VirtualWidth; height = $script:VirtualHeight }
        contentRect = [ordered]@{ x = 0; y = 0; width = $Width; height = $Height }
        virtualRect = [ordered]@{
            x = [int][Math]::Floor(($script:VirtualWidth - $sourceWidth) / 2.0)
            y = [int][Math]::Floor(($script:VirtualHeight - $sourceHeight) / 2.0)
            width = $sourceWidth
            height = $sourceHeight
        }
        letterbox = [ordered]@{ left = 0; top = 0; right = 0; bottom = 0 }
    }
}

function Assert-Scalar {
    param(
        [AllowNull()]$Actual,
        [Parameter(Mandatory = $true)]$Expected,
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Stage
    )
    if ($null -eq $Actual) {
        Add-Issue -Code 'renderer.field-missing' -Stage $Stage -Path $Path -Message "Required Renderer Status field '$Path' is missing."
        return $false
    }
    if ([string]$Actual -ne [string]$Expected) {
        Add-Issue -Code 'renderer.field-mismatch' -Stage $Stage -Path $Path -Message "Renderer Status field '$Path' was '$Actual'; expected '$Expected'."
        return $false
    }
    return $true
}

function Get-HybridRendererSnapshot {
    param(
        [Parameter(Mandatory = $true)]$Status,
        [Parameter(Mandatory = $true)][string]$Stage,
        [Parameter(Mandatory = $true)][int]$Width,
        [Parameter(Mandatory = $true)][int]$Height
    )

    $statusVersion = [string](Get-PropertyValue $Status 'schemaVersion')
    if ($statusVersion -ne 'noemancer.renderer-status.v25') {
        Add-Issue -Code 'renderer.schema-invalid' -Stage $Stage -Path '/schemaVersion' -Message "Expected noemancer.renderer-status.v25, got '$statusVersion'."
    }
    $hybrid = Get-PropertyValue $Status 'hybridPixel'
    if ($null -eq $hybrid) {
        Add-Issue -Code 'renderer.hybrid-pixel-missing' -Stage $Stage -Path '/hybridPixel' -Message 'Renderer Status does not expose the Hybrid Pixel authority projection.'
        return [ordered]@{ pass = $false; schemaVersion = $statusVersion; profile = $null; presentation = $null; projection = $null }
    }
    $active = Get-PropertyValue $hybrid 'active'
    Assert-Scalar $active $true '/hybridPixel/active' $Stage | Out-Null
    $profile = Get-PropertyValue $hybrid 'profile'
    $presentation = Get-PropertyValue $hybrid 'presentation'
    $projection = Get-PropertyValue $hybrid 'projection'
    if ($null -eq $profile -or $null -eq $presentation -or $null -eq $projection) {
        Add-Issue -Code 'renderer.hybrid-pixel-fields-missing' -Stage $Stage -Path '/hybridPixel' -Message 'Hybrid Pixel profile, presentation and projection fields are all required for this acceptance.'
        return [ordered]@{ pass = $false; schemaVersion = $statusVersion; profile = $profile; presentation = $presentation; projection = $projection }
    }

    $before = $script:Issues.Count
    Assert-Scalar (Get-PropertyValue $profile 'schema') 'noemancer.hybrid-pixel-profile/0.1' '/hybridPixel/profile/schema' $Stage | Out-Null
    Assert-Scalar (Get-PropertyValue $profile 'profileId') $script:ExpectedProfile.profileId '/hybridPixel/profile/profileId' $Stage | Out-Null
    Assert-Scalar (Get-PropertyValue $profile 'enabled') $true '/hybridPixel/profile/enabled' $Stage | Out-Null
    Assert-Scalar (Get-PropertyValue $profile 'virtualWidth') $script:VirtualWidth '/hybridPixel/profile/virtualWidth' $Stage | Out-Null
    Assert-Scalar (Get-PropertyValue $profile 'virtualHeight') $script:VirtualHeight '/hybridPixel/profile/virtualHeight' $Stage | Out-Null
    Assert-Scalar (Get-PropertyValue $profile 'pixelsPerUnit') $script:PixelsPerUnit '/hybridPixel/profile/pixelsPerUnit' $Stage | Out-Null
    Assert-Scalar (Get-PropertyValue $profile 'integerScaling') $true '/hybridPixel/profile/integerScaling' $Stage | Out-Null
    Assert-Scalar (Get-PropertyValue $profile 'snapCamera') $true '/hybridPixel/profile/snapCamera' $Stage | Out-Null
    Assert-Scalar (Get-PropertyValue $profile 'snapSprites') $true '/hybridPixel/profile/snapSprites' $Stage | Out-Null
    Assert-Scalar (Get-PropertyValue $profile 'presentationFilter') 'nearest' '/hybridPixel/profile/presentationFilter' $Stage | Out-Null

    $expected = Get-ExpectedPresentation -Width $Width -Height $Height
    Assert-Scalar (Get-PropertyValue $presentation 'valid') $true '/hybridPixel/presentation/valid' $Stage | Out-Null
    Assert-Scalar (Get-PropertyValue $presentation 'status') $expected.status '/hybridPixel/presentation/status' $Stage | Out-Null
    Assert-Scalar (Get-PropertyValue $presentation 'code') $expected.code '/hybridPixel/presentation/code' $Stage | Out-Null
    Assert-Scalar (Get-PropertyValue $presentation 'integerScale') $expected.integerScale '/hybridPixel/presentation/integerScale' $Stage | Out-Null
    foreach ($group in @('physicalOutput', 'virtualExtent', 'contentRect', 'virtualRect', 'letterbox')) {
        $actualGroup = Get-PropertyValue $presentation $group
        $expectedGroup = $expected[$group]
        if ($null -eq $actualGroup) {
            Add-Issue -Code 'renderer.presentation-field-missing' -Stage $Stage -Path "/hybridPixel/presentation/$group" -Message "Renderer Status presentation group '$group' is missing."
            continue
        }
        foreach ($field in $expectedGroup.Keys) {
            Assert-Scalar (Get-PropertyValue $actualGroup $field) $expectedGroup[$field] "/hybridPixel/presentation/$group/$field" $Stage | Out-Null
        }
    }

    Assert-Scalar (Get-PropertyValue $projection 'enabled') $true '/hybridPixel/projection/enabled' $Stage | Out-Null
    Assert-Scalar (Get-PropertyValue $projection 'valid') $true '/hybridPixel/projection/valid' $Stage | Out-Null
    Assert-Scalar (Get-PropertyValue $projection 'code') 'ok' '/hybridPixel/projection/code' $Stage | Out-Null
    Assert-Scalar (Get-PropertyValue $projection 'orthographicCamera') $true '/hybridPixel/projection/orthographicCamera' $Stage | Out-Null
    Assert-Scalar (Get-PropertyValue $projection 'cameraSnapped') $true '/hybridPixel/projection/cameraSnapped' $Stage | Out-Null
    $spritesSnapped = Get-PropertyValue $projection 'spritesSnapped'
    $tileCellsSnapped = Get-PropertyValue $projection 'tileCellsSnapped'
    if ($null -eq $spritesSnapped) { Add-Issue -Code 'renderer.snapping-field-missing' -Stage $Stage -Path '/hybridPixel/projection/spritesSnapped' -Message 'Sprite snapping count is required.' }
    if ($null -eq $tileCellsSnapped) { Add-Issue -Code 'renderer.snapping-field-missing' -Stage $Stage -Path '/hybridPixel/projection/tileCellsSnapped' -Message 'Tile-cell snapping count is required.' }
    if ($null -ne $spritesSnapped -and [int64]$spritesSnapped -lt 0) { Add-Issue -Code 'renderer.snapping-count-invalid' -Stage $Stage -Path '/hybridPixel/projection/spritesSnapped' -Message 'Sprite snapping count cannot be negative.' }
    if ($null -ne $tileCellsSnapped -and [int64]$tileCellsSnapped -lt 0) { Add-Issue -Code 'renderer.snapping-count-invalid' -Stage $Stage -Path '/hybridPixel/projection/tileCellsSnapped' -Message 'Tile-cell snapping count cannot be negative.' }

    return [ordered]@{
        pass = ($script:Issues.Count -eq $before)
        schemaVersion = $statusVersion
        active = [bool]$active
        profile = $profile
        presentation = $presentation
        projection = $projection
    }
}

function Invoke-RendererCapture {
    param(
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string[]]$PrefixArguments,
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [Parameter(Mandatory = $true)][string]$Stage,
        [Parameter(Mandatory = $true)][string]$Backend,
        [Parameter(Mandatory = $true)][string]$SizeId,
        [Parameter(Mandatory = $true)][int]$Width,
        [Parameter(Mandatory = $true)][int]$Height
    )

    $imagePath = Join-Path $script:OutputRootPath ($Stage + '.bmp')
    $arguments = @($PrefixArguments + @(
        '--format', 'json', '--gpu-backend', $Backend,
        '--capture-frame', $imagePath, '--frames', [string]$Frames,
        '--window-width', [string]$Width, '--window-height', [string]$Height
    ))
    $stdoutPath = Join-Path $script:OutputRootPath ($Stage + '.stdout.jsonl')
    $stderrPath = Join-Path $script:OutputRootPath ($Stage + '.stderr.log')
    $process = Invoke-BoundedProcess -Executable $Executable -Arguments $arguments -WorkingDirectory $WorkingDirectory `
        -StdoutPath $stdoutPath -StderrPath $stderrPath -Stage $Stage
    $events = Get-JsonLogEvents $process.stdoutText
    $finalEvent = Get-LastEvent -Events $events -Name 'render.scene.final'
    if ($null -eq $finalEvent) {
        $finalEvent = Get-LastEvent -Events $events -Name 'render.scene'
        Add-Issue -Code 'renderer.final-status-missing' -Stage $Stage -Message 'Capture did not publish render.scene.final; the fallback initial status is not sufficient for this acceptance.'
    }
    $status = if ($null -ne $finalEvent) { $finalEvent.payload } else { $null }
    if ($null -eq $status) {
        Add-Issue -Code 'renderer.status-invalid' -Stage $Stage -Message 'Renderer Status event payload was missing or not JSON.'
    }
    $snapshot = if ($null -ne $status) {
        Get-HybridRendererSnapshot -Status $status -Stage $Stage -Width $Width -Height $Height
    } else { [ordered]@{ pass = $false; profile = $null; presentation = $null; projection = $null } }
    if ($process.exitCode -ne 0) {
        Add-Issue -Code 'renderer.capture-failed' -Stage $Stage -Message "Hidden $Backend capture exited with code $($process.exitCode)."
    }
    if (-not (Test-Path -LiteralPath $imagePath -PathType Leaf)) {
        Add-Issue -Code 'renderer.capture-missing' -Stage $Stage -Message "Hidden $Backend capture did not produce an image."
    }
    return [ordered]@{
        backend = $Backend
        sizeId = $SizeId
        width = $Width
        height = $Height
        pass = ($process.exitCode -eq 0 -and (Test-Path -LiteralPath $imagePath -PathType Leaf) -and [bool]$snapshot.pass)
        process = Get-ProcessEvidence $process
        status = $snapshot
        capture = [ordered]@{
            path = Get-RelativeArtifactPath $imagePath
            bytes = if (Test-Path -LiteralPath $imagePath -PathType Leaf) { (Get-Item -LiteralPath $imagePath).Length } else { 0 }
            sha256 = Get-Sha256 $imagePath
        }
    }
}

function Test-ProfileAuthority {
    param(
        [Parameter(Mandatory = $true)]$Profile,
        [Parameter(Mandatory = $true)][string]$Stage
    )
    if ($null -eq $Profile) {
        Add-Issue -Code 'profile.missing' -Stage $Stage -Message 'Game Profile did not contain hybridPixelProfile.'
        return $false
    }
    $before = $script:Issues.Count
    foreach ($field in @('schema', 'profileId', 'enabled', 'virtualWidth', 'virtualHeight', 'pixelsPerUnit', 'integerScaling', 'snapCamera', 'snapSprites', 'presentationFilter')) {
        if ($null -eq (Get-PropertyValue $Profile $field)) {
            Add-Issue -Code 'profile.field-missing' -Stage $Stage -Path "/hybridPixelProfile/$field" -Message "Game Profile Hybrid Pixel field '$field' is required."
        }
    }
    if ($script:Issues.Count -ne $before) { return $false }
    $expected = $script:ExpectedProfile
    foreach ($field in $expected.PSObject.Properties.Name) {
        Assert-Scalar (Get-PropertyValue $Profile $field) (Get-PropertyValue $expected $field) "/hybridPixelProfile/$field" $Stage | Out-Null
    }
    return ($script:Issues.Count -eq $before)
}

function Test-GameProfileContract {
    param(
        [Parameter(Mandatory = $true)]$Profile,
        [Parameter(Mandatory = $true)][string]$Stage
    )

    $before = $script:Issues.Count
    if ($null -eq $Profile) {
        Add-Issue -Code 'game-profile.missing' -Stage $Stage -Path 'config/game-profile.json' -Message 'Packaged Game Profile is missing.'
        return $false
    }

    Assert-Scalar (Get-PropertyValue $Profile 'schema') 'noemancer.game-profile/0.4' '/schema' $Stage | Out-Null
    foreach ($field in @(
            'id', 'displayName', 'platform', 'architecture', 'configuration',
            'executable', 'projectId', 'targetProfile', 'startupScene',
            'startupSceneGuid', 'managedConfiguration', 'assetRegistry')) {
        $value = Get-PropertyValue $Profile $field
        if ($null -eq $value -or [string]::IsNullOrWhiteSpace([string]$value)) {
            Add-Issue -Code 'game-profile.field-missing' -Stage $Stage -Path "/$field" -Message "Game Profile 0.4 field '$field' is required."
        }
    }
    Assert-Scalar (Get-PropertyValue $Profile 'platform') 'windows' '/platform' $Stage | Out-Null
    Assert-Scalar (Get-PropertyValue $Profile 'architecture') 'x64' '/architecture' $Stage | Out-Null
    Assert-Scalar (Get-PropertyValue $Profile 'configuration') 'release' '/configuration' $Stage | Out-Null
    Assert-Scalar (Get-PropertyValue $Profile 'managedConfiguration') 'Release' '/managedConfiguration' $Stage | Out-Null
    Assert-Scalar (Get-PropertyValue $Profile 'targetProfile') 'windows-x64-release' '/targetProfile' $Stage | Out-Null
    Assert-Scalar (Get-PropertyValue $Profile 'assetRegistry') 'content/assets/registry.json' '/assetRegistry' $Stage | Out-Null

    $runtimeRequirements = Get-PropertyValue $Profile 'runtimeRequirements'
    $packagedAssets = Get-PropertyValue $Profile 'packagedAssets'
    $inputActions = Get-PropertyValue $Profile 'inputActions'
    if ($null -eq $runtimeRequirements -or -not ($runtimeRequirements -is [array])) {
        Add-Issue -Code 'game-profile.runtime-requirements-invalid' -Stage $Stage -Path '/runtimeRequirements' -Message 'Game Profile runtimeRequirements must be an array.'
    }
    if ($null -eq $packagedAssets -or -not ($packagedAssets -is [array])) {
        Add-Issue -Code 'game-profile.packaged-assets-invalid' -Stage $Stage -Path '/packagedAssets' -Message 'Game Profile packagedAssets must be an array.'
    }
    if ($null -eq $inputActions -or -not ($inputActions -is [array])) {
        Add-Issue -Code 'game-profile.input-actions-invalid' -Stage $Stage -Path '/inputActions' -Message 'Game Profile inputActions must be an array.'
    }

    $profileAuthorityPass = Test-ProfileAuthority -Profile (Get-PropertyValue $Profile 'hybridPixelProfile') -Stage $Stage
    return ($script:Issues.Count -eq $before -and $profileAuthorityPass)
}

function Invoke-PackageBuild {
    param(
        [Parameter(Mandatory = $true)][string]$Runtime,
        [Parameter(Mandatory = $true)][string]$CopiedProject,
        [Parameter(Mandatory = $true)][string]$PackageRoot,
        [Parameter(Mandatory = $true)][string]$TargetProfile
    )

    # The package host commits through a sibling staging directory and
    # intentionally rejects an already-existing destination.
    if (Test-Path -LiteralPath $PackageRoot) {
        throw "Package output must not already exist: $PackageRoot"
    }
    $arguments = @('package', '--project', $CopiedProject, '--output', $PackageRoot,
        '--target-profile', $TargetProfile, '--format', 'json')
    $stdoutPath = Join-Path $script:OutputRootPath 'package-build.stdout.json'
    $stderrPath = Join-Path $script:OutputRootPath 'package-build.stderr.log'
    $process = Invoke-BoundedProcess -Executable $Runtime -Arguments $arguments -WorkingDirectory $CopiedProject `
        -StdoutPath $stdoutPath -StderrPath $stderrPath -Stage 'packageBuild'
    $envelope = $null
    try { $envelope = ConvertFrom-Json -InputObject $process.stdoutText -Depth 100 } catch { }
    if ($process.exitCode -ne 0 -or $null -eq $envelope -or -not [bool](Get-PropertyValue $envelope 'success')) {
        Add-Issue -Code 'package.build-failed' -Stage 'packageBuild' -Message "Package CLI did not return success (exit=$($process.exitCode))."
    }
    $profilePath = Join-Path $PackageRoot 'config\game-profile.json'
    $profile = Read-JsonFile -Path $profilePath -Stage 'packageProfile' -Label 'Packaged Game Profile'
    if ($null -eq $profile) { return [ordered]@{ pass = $false; process = Get-ProcessEvidence $process; profile = $null; profilePath = $profilePath; packageRoot = $PackageRoot; playerPath = $null } }
    $profilePass = Test-GameProfileContract -Profile $profile -Stage 'packageProfile'
    $executableName = [string](Get-PropertyValue $profile 'executable')
    if ([string]::IsNullOrWhiteSpace($executableName) -or [IO.Path]::IsPathRooted($executableName) -or $executableName -match '(^|[\\/])\.\.(?:[\\/]|$)') {
        Add-Issue -Code 'package.player-path-invalid' -Stage 'packageProfile' -Path '/executable' -Message 'Packaged Game Profile executable must be a package-relative file name.'
        $playerPath = $null
    }
    else {
        $playerPath = (Join-Path $PackageRoot (Join-Path 'bin' $executableName))
        if (-not (Test-Path -LiteralPath $playerPath -PathType Leaf)) {
            Add-Issue -Code 'package.player-missing' -Stage 'packageProfile' -Path '/executable' -Message "Packaged Player executable is missing: $executableName"
            $playerPath = $null
        }
    }
    return [ordered]@{
        pass = ($process.exitCode -eq 0 -and $null -ne $envelope -and [bool](Get-PropertyValue $envelope 'success') -and $profilePass -and $null -ne $playerPath)
        process = Get-ProcessEvidence $process
        profile = $profile
        profilePath = $profilePath
        packageRoot = $PackageRoot
        playerPath = $playerPath
        packageEnvelope = if ($null -ne $envelope) { [ordered]@{ schema = Get-PropertyValue $envelope 'schema'; code = Get-PropertyValue $envelope 'code'; success = Get-PropertyValue $envelope 'success' } } else { $null }
    }
}

function Test-ShaderDistributionClosure {
    param([Parameter(Mandatory = $true)][string]$PackageRoot)

    $before = $script:Issues.Count
    $shaderRoot = Join-Path $PackageRoot 'shaders'
    $manifestPath = Join-Path $shaderRoot 'shader-artifact-manifest.json'
    $reflectionPath = Join-Path $shaderRoot 'shader-artifact-reflection.json'
    $contractPath = Join-Path $script:EngineRoot 'assets\shaders\shader-artifact-contract.json'
    if (-not (Test-Path -LiteralPath $shaderRoot -PathType Container)) {
        Add-Issue -Code 'shader.closure-root-missing' -Stage 'shaderClosure' -Path 'shaders' -Message 'Package shader directory is missing.'
        return [ordered]@{
            pass = $false
            expectedShaderCount = $script:ExpectedShaderCount
            packageShaderRoot = 'shaders'
            manifest = $null
            reflection = $null
            files = @()
        }
    }

    $contract = Read-JsonFile -Path $contractPath -Stage 'shaderClosure' -Label 'Engine shader source contract'
    $manifest = Read-JsonFile -Path $manifestPath -Stage 'shaderClosure' -Label 'Packaged shader artifact manifest'
    $reflection = Read-JsonFile -Path $reflectionPath -Stage 'shaderClosure' -Label 'Packaged shader reflection proof'
    $contractShaders = if ($null -ne $contract) { @(Get-PropertyValue $contract 'shaders') } else { @() }
    $manifestShaders = if ($null -ne $manifest) { @(Get-PropertyValue $manifest 'shaders') } else { @() }
    $reflectionArtifacts = if ($null -ne $reflection) { @(Get-PropertyValue $reflection 'artifacts') } else { @() }

    $contractSchema = if ($null -ne $contract) { [string](Get-PropertyValue $contract 'schema') } else { '' }
    if ($contractSchema -ne 'noemancer.shader-artifact-source-contract/0.1') {
        Add-Issue -Code 'shader.contract-schema-invalid' -Stage 'shaderClosure' -Path 'assets/shaders/shader-artifact-contract.json/schema' -Message "Expected noemancer.shader-artifact-source-contract/0.1, got '$contractSchema'."
    }
    if ($contractShaders.Count -ne $script:ExpectedShaderCount) {
        Add-Issue -Code 'shader.contract-count-invalid' -Stage 'shaderClosure' -Path 'assets/shaders/shader-artifact-contract.json/shaders' -Message "Expected exactly $($script:ExpectedShaderCount) shader contract entries, got $($contractShaders.Count)."
    }
    $expectedStems = @($contractShaders | ForEach-Object { [string](Get-PropertyValue $_ 'stem') } | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Sort-Object -Unique)
    if ($expectedStems.Count -ne $script:ExpectedShaderCount) {
        Add-Issue -Code 'shader.contract-stems-invalid' -Stage 'shaderClosure' -Message "Shader contract must expose $($script:ExpectedShaderCount) unique non-empty stems."
    }

    $manifestSchema = if ($null -ne $manifest) { [string](Get-PropertyValue $manifest 'schema') } else { '' }
    if ($manifestSchema -ne 'noemancer.shader-artifact-manifest/0.1') {
        Add-Issue -Code 'shader.manifest-schema-invalid' -Stage 'shaderClosure' -Path 'shaders/shader-artifact-manifest.json/schema' -Message "Expected noemancer.shader-artifact-manifest/0.1, got '$manifestSchema'."
    }
    $manifestCount = if ($null -ne $manifest) { [int](Get-PropertyValue $manifest 'shaderCount') } else { 0 }
    if ($manifestCount -ne $script:ExpectedShaderCount -or $manifestShaders.Count -ne $script:ExpectedShaderCount) {
        Add-Issue -Code 'shader.manifest-count-invalid' -Stage 'shaderClosure' -Path 'shaders/shader-artifact-manifest.json/shaderCount' -Message "Shader manifest must declare and contain exactly $($script:ExpectedShaderCount) entries (declared=$manifestCount, actual=$($manifestShaders.Count))."
    }
    $manifestStems = @($manifestShaders | ForEach-Object { [string](Get-PropertyValue $_ 'stem') } | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Sort-Object -Unique)
    foreach ($difference in @(Compare-Object -ReferenceObject $expectedStems -DifferenceObject $manifestStems)) {
        $side = if ($difference.SideIndicator -eq '<=') { 'missing from manifest' } else { 'not in source contract' }
        Add-Issue -Code 'shader.manifest-set-mismatch' -Stage 'shaderClosure' -Path 'shaders/shader-artifact-manifest.json/shaders' -Message "Shader stem '$($difference.InputObject)' is $side."
    }

    $reflectionSchema = if ($null -ne $reflection) { [string](Get-PropertyValue $reflection 'schema') } else { '' }
    if ($reflectionSchema -ne 'noemancer.shader-artifact-reflection-summary/0.1') {
        Add-Issue -Code 'shader.reflection-schema-invalid' -Stage 'shaderClosure' -Path 'shaders/shader-artifact-reflection.json/schema' -Message "Expected noemancer.shader-artifact-reflection-summary/0.1, got '$reflectionSchema'."
    }
    if ($null -ne $reflection -and [string](Get-PropertyValue $reflection 'status') -ne 'passed') {
        Add-Issue -Code 'shader.reflection-status-invalid' -Stage 'shaderClosure' -Path 'shaders/shader-artifact-reflection.json/status' -Message 'Packaged shader reflection proof must have status passed.'
    }
    if ($reflectionArtifacts.Count -ne $script:ExpectedShaderCount) {
        Add-Issue -Code 'shader.reflection-count-invalid' -Stage 'shaderClosure' -Path 'shaders/shader-artifact-reflection.json/artifacts' -Message "Shader reflection proof must contain exactly $($script:ExpectedShaderCount) artifacts, got $($reflectionArtifacts.Count)."
    }
    $reflectionNames = @($reflectionArtifacts | ForEach-Object { [string](Get-PropertyValue $_ 'name') } | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Sort-Object -Unique)
    foreach ($difference in @(Compare-Object -ReferenceObject $expectedStems -DifferenceObject $reflectionNames)) {
        $side = if ($difference.SideIndicator -eq '<=') { 'missing from reflection proof' } else { 'not in source contract' }
        Add-Issue -Code 'shader.reflection-set-mismatch' -Stage 'shaderClosure' -Path 'shaders/shader-artifact-reflection.json/artifacts' -Message "Shader stem '$($difference.InputObject)' is $side."
    }

    $sourceContract = if ($null -ne $manifest) { Get-PropertyValue $manifest 'sourceContract' } else { $null }
    $declaredContractHash = if ($null -ne $sourceContract) { [string](Get-PropertyValue $sourceContract 'sha256') } else { '' }
    $actualContractHash = Get-Sha256 $contractPath
    if ([string]::IsNullOrWhiteSpace($declaredContractHash) -or
        $declaredContractHash -replace '^sha256:', '' -ne $actualContractHash) {
        Add-Issue -Code 'shader.source-contract-hash-invalid' -Stage 'shaderClosure' -Path 'shaders/shader-artifact-manifest.json/sourceContract/sha256' -Message 'Packaged shader manifest source contract hash does not match the engine contract.'
    }

    $packageFiles = @(Get-ChildItem -LiteralPath $shaderRoot -File -Force | Sort-Object Name)
    $expectedMetadata = @('shader-artifact-manifest.json', 'shader-artifact-reflection.json')
    foreach ($file in $packageFiles) {
        $isExpectedBinary = $file.Extension.ToLowerInvariant() -in @('.dxil', '.spv')
        if (-not $isExpectedBinary -and $expectedMetadata -notcontains $file.Name) {
            Add-Issue -Code 'shader.unexpected-package-file' -Stage 'shaderClosure' -Path ("shaders/" + $file.Name) -Message 'Package shader closure contains an unexpected file.'
        }
    }
    foreach ($metadata in $expectedMetadata) {
        if (-not (Test-Path -LiteralPath (Join-Path $shaderRoot $metadata) -PathType Leaf)) {
            Add-Issue -Code 'shader.metadata-missing' -Stage 'shaderClosure' -Path ("shaders/" + $metadata) -Message 'Required shader metadata is missing from the package.'
        }
    }

    $actualDxil = @(Get-ChildItem -LiteralPath $shaderRoot -File -Filter '*.dxil' | ForEach-Object { $_.BaseName } | Sort-Object -Unique)
    $actualSpv = @(Get-ChildItem -LiteralPath $shaderRoot -File -Filter '*.spv' | ForEach-Object { $_.BaseName } | Sort-Object -Unique)
    if ($actualDxil.Count -ne $script:ExpectedShaderCount -or $actualSpv.Count -ne $script:ExpectedShaderCount) {
        Add-Issue -Code 'shader.binary-count-invalid' -Stage 'shaderClosure' -Path 'shaders' -Message "Package must contain exactly $($script:ExpectedShaderCount) DXIL and $($script:ExpectedShaderCount) SPIR-V shader binaries (dxil=$($actualDxil.Count), spv=$($actualSpv.Count))."
    }
    foreach ($difference in @(Compare-Object -ReferenceObject $expectedStems -DifferenceObject $actualDxil)) {
        $side = if ($difference.SideIndicator -eq '<=') { 'missing DXIL' } else { 'unexpected DXIL' }
        Add-Issue -Code 'shader.dxil-set-mismatch' -Stage 'shaderClosure' -Path 'shaders' -Message "Shader stem '$($difference.InputObject)' has $side."
    }
    foreach ($difference in @(Compare-Object -ReferenceObject $expectedStems -DifferenceObject $actualSpv)) {
        $side = if ($difference.SideIndicator -eq '<=') { 'missing SPIR-V' } else { 'unexpected SPIR-V' }
        Add-Issue -Code 'shader.spv-set-mismatch' -Stage 'shaderClosure' -Path 'shaders' -Message "Shader stem '$($difference.InputObject)' has $side."
    }

    $checkedBinaries = [System.Collections.Generic.List[object]]::new()
    foreach ($shader in $manifestShaders) {
        $stem = [string](Get-PropertyValue $shader 'stem')
        if ([string]::IsNullOrWhiteSpace($stem)) { continue }
        foreach ($backend in @('dxil', 'spv')) {
            $artifact = Get-PropertyValue $shader $backend
            $relativePath = if ($null -ne $artifact) { [string](Get-PropertyValue $artifact 'path') } else { '' }
            $declaredHash = if ($null -ne $artifact) { [string](Get-PropertyValue $artifact 'sha256') } else { '' }
            $binaryPath = Resolve-PackageArtifactPath -Root $shaderRoot -RelativePath $relativePath -Stage 'shaderClosure' -Field ("/shaders/$stem/$backend/path")
            if ($null -eq $binaryPath) { continue }
            if (-not (Test-Path -LiteralPath $binaryPath -PathType Leaf)) {
                Add-Issue -Code 'shader.binary-missing' -Stage 'shaderClosure' -Path (Get-RelativeArtifactPath $binaryPath) -Message "Manifest-declared $backend binary for '$stem' is missing."
                continue
            }
            $expectedName = "$stem.$backend"
            if ([IO.Path]::GetFileName($binaryPath) -ne $expectedName) {
                Add-Issue -Code 'shader.binary-name-mismatch' -Stage 'shaderClosure' -Path (Get-RelativeArtifactPath $binaryPath) -Message "Manifest-declared $backend binary for '$stem' does not use the canonical filename '$expectedName'."
            }
            if ([string]::IsNullOrWhiteSpace($declaredHash) -or
                ($declaredHash -replace '^sha256:', '') -ne (Get-Sha256 $binaryPath)) {
                Add-Issue -Code 'shader.binary-hash-mismatch' -Stage 'shaderClosure' -Path (Get-RelativeArtifactPath $binaryPath) -Message "Packaged $backend binary for '$stem' does not match its manifest hash."
            }
            [void]$checkedBinaries.Add([pscustomobject]@{
                stem = $stem
                backend = $backend
                path = Get-RelativeArtifactPath $binaryPath
                bytes = (Get-Item -LiteralPath $binaryPath).Length
                sha256 = Get-Sha256 $binaryPath
            })
        }
    }

    return [ordered]@{
        pass = ($script:Issues.Count -eq $before)
        expectedShaderCount = $script:ExpectedShaderCount
        packageShaderRoot = Get-RelativeArtifactPath $shaderRoot
        manifest = [ordered]@{
            path = Get-RelativeArtifactPath $manifestPath
            schema = $manifestSchema
            declaredShaderCount = $manifestCount
            shaderCount = $manifestShaders.Count
            sha256 = Get-Sha256 $manifestPath
        }
        reflection = [ordered]@{
            path = Get-RelativeArtifactPath $reflectionPath
            schema = $reflectionSchema
            status = if ($null -ne $reflection) { Get-PropertyValue $reflection 'status' } else { $null }
            artifactCount = $reflectionArtifacts.Count
            sha256 = Get-Sha256 $reflectionPath
        }
        contract = [ordered]@{
            path = 'assets/shaders/shader-artifact-contract.json'
            schema = $contractSchema
            shaderCount = $contractShaders.Count
            sha256 = $actualContractHash
        }
        binaries = [ordered]@{
            dxilCount = @($packageFiles | Where-Object Extension -eq '.dxil').Count
            spvCount = @($packageFiles | Where-Object Extension -eq '.spv').Count
            checked = @($checkedBinaries)
        }
    }
}

function Compare-CaptureStability {
    param(
        [Parameter(Mandatory = $true)]$SourceCaptures,
        [Parameter(Mandatory = $true)]$PackageCaptures
    )

    $before = $script:Issues.Count
    $signatures = [System.Collections.Generic.List[object]]::new()
    foreach ($size in $script:WindowSizes) {
        $items = @($SourceCaptures | Where-Object { $_.sizeId -eq $size.id })
        if ($items.Count -ne $script:Backends.Count) {
            Add-Issue -Code 'stability.capture-set-incomplete' -Stage 'stability' -Message "Expected one source capture per backend for $($size.id)."
            continue
        }
        $validItems = @($items | Where-Object { $_.pass -and $null -ne $_.status.presentation })
        if ($validItems.Count -ne $items.Count) { continue }
        $first = $validItems[0]
        $presentation = $first.status.presentation
        $signature = [ordered]@{
            sizeId = $size.id
            integerScale = $presentation.integerScale
            status = $presentation.status
            contentRect = $presentation.contentRect
            virtualRect = $presentation.virtualRect
            letterbox = $presentation.letterbox
            cameraSnapped = $first.status.projection.cameraSnapped
            spritesSnapped = $first.status.projection.spritesSnapped
            tileCellsSnapped = $first.status.projection.tileCellsSnapped
        }
        foreach ($item in $validItems | Select-Object -Skip 1) {
            $other = [ordered]@{
                sizeId = $size.id
                integerScale = $item.status.presentation.integerScale
                status = $item.status.presentation.status
                contentRect = $item.status.presentation.contentRect
                virtualRect = $item.status.presentation.virtualRect
                letterbox = $item.status.presentation.letterbox
                cameraSnapped = $item.status.projection.cameraSnapped
                spritesSnapped = $item.status.projection.spritesSnapped
                tileCellsSnapped = $item.status.projection.tileCellsSnapped
            }
            if (($signature | ConvertTo-Json -Depth 20 -Compress) -ne ($other | ConvertTo-Json -Depth 20 -Compress)) {
                Add-Issue -Code 'stability.backend-mismatch' -Stage 'stability' -Message "D3D12 and Vulkan Hybrid Pixel geometry/snapping differ for $($size.id)."
            }
        }
        [void]$signatures.Add([pscustomobject]$signature)
    }

    $letterbox = @($signatures | Where-Object { $_.sizeId -eq 'letterbox-1440x900' }) | Select-Object -First 1
    $odd = @($signatures | Where-Object { $_.sizeId -eq 'odd-letterbox-1439x899' }) | Select-Object -First 1
    if ($null -ne $letterbox -and $null -ne $odd) {
        if ($letterbox.integerScale -ne $odd.integerScale -or
            $letterbox.contentRect.width -ne $odd.contentRect.width -or
            $letterbox.contentRect.height -ne $odd.contentRect.height) {
            Add-Issue -Code 'stability.resize-scale-jump' -Stage 'stability' -Message 'A one-pixel window resize changed the integer scale or virtual content dimensions.'
        }
        if ($odd.letterbox.left -ne 79 -or $odd.letterbox.top -ne 89 -or
            $odd.letterbox.right -ne 80 -or $odd.letterbox.bottom -ne 90) {
            Add-Issue -Code 'stability.odd-remainder-invalid' -Stage 'stability' -Message 'Odd resize remainder did not use deterministic integer centering.'
        }
    }

    foreach ($packageCapture in @($PackageCaptures)) {
        if (-not $packageCapture.pass -or $null -eq $packageCapture.status.presentation) { continue }
        $sourceMatch = @($SourceCaptures | Where-Object {
            $_.backend -eq $packageCapture.backend -and $_.sizeId -eq $packageCapture.sizeId -and $_.pass
        }) | Select-Object -First 1
        if ($null -eq $sourceMatch) { continue }
        $packageSignature = $packageCapture.status.presentation | ConvertTo-Json -Depth 20 -Compress
        $sourceSignature = $sourceMatch.status.presentation | ConvertTo-Json -Depth 20 -Compress
        if ($packageSignature -ne $sourceSignature) {
            Add-Issue -Code 'stability.source-player-mismatch' -Stage 'stability' -Message "Source and packaged Player presentation differ for $($packageCapture.backend)/$($packageCapture.sizeId)."
        }
    }
    return [ordered]@{
        pass = ($script:Issues.Count -eq $before)
        sourceSignatures = @($signatures)
        comparedPackageCaptures = @($PackageCaptures | Where-Object { $_.pass } | ForEach-Object { "$($_.backend)/$($_.sizeId)" })
    }
}

function Write-Receipt {
    param([Parameter(Mandatory = $true)][bool]$Success)

    $receiptPath = Join-Path $script:OutputRootPath 'hybrid-pixel-core-receipt.json'
    $receipt = [ordered]@{
        schemaVersion = 'noemancer.hybrid-pixel-core-receipt/0.1'
        capturedAt = [DateTimeOffset]::UtcNow.ToString('o')
        success = $Success
        sourceProject = $ProjectRoot
        copiedProject = Get-RelativeArtifactPath $script:TemporaryProjectRoot
        outputRoot = $script:OutputRootPath
        sourceProjectManifest = [ordered]@{
            path = $ProjectRoot
            sha256Before = $script:SourceManifestHashBefore
            sha256After = $script:SourceManifestHashAfter
            unchanged = (-not [string]::IsNullOrWhiteSpace($script:SourceManifestHashBefore) -and
                $script:SourceManifestHashBefore -eq $script:SourceManifestHashAfter)
        }
        sourceProjectTree = [ordered]@{
            path = $ProjectRoot
            sha256Before = $script:SourceProjectHashBefore
            sha256After = $script:SourceProjectHashAfter
            unchanged = (-not [string]::IsNullOrWhiteSpace($script:SourceProjectHashBefore) -and
                $script:SourceProjectHashBefore -eq $script:SourceProjectHashAfter)
        }
        runtime = [ordered]@{
            path = $RuntimePath
            configuration = $Config
            targetProfile = 'windows-x64-release'
            sha256 = if (Test-Path -LiteralPath $RuntimePath -PathType Leaf) { Get-Sha256 $RuntimePath } else { '' }
        }
        expectedProfile = $script:ExpectedProfile
        nativeBoundary = $script:NativeBoundary
        sourceHeadless = $script:SourceHeadless
        package = $script:Package
        shaderClosure = $script:ShaderClosure
        packageHeadless = $script:PackageHeadless
        sourceCaptures = @($script:SourceCaptures)
        packageCaptures = @($script:PackageCaptures)
        stability = $script:Stability
        issueCount = $script:Issues.Count
        issues = @($script:Issues)
        receipt = 'hybrid-pixel-core-receipt.json'
    }
    $json = $receipt | ConvertTo-Json -Depth 40
    Write-Utf8 -Path $receiptPath -Text $json
    [Console]::Out.WriteLine($json)
    return $receiptPath
}

$script:NativeBoundary = $null
$script:Stability = $null
$sourceManifestPath = $null
$resolvedProject = $null
$exitCode = 3
try {
    $stamp = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
    if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
        $OutputRoot = Join-Path $script:EngineRoot ("generated\acceptance\hybrid-pixel-core-" + $stamp)
    }
    $script:OutputRootPath = Get-FullPath $OutputRoot
    if (Test-Path -LiteralPath $script:OutputRootPath) { throw "Evidence output already exists: $script:OutputRootPath" }
    New-Item -ItemType Directory -Path $script:OutputRootPath -Force | Out-Null
    $script:DiagnosticsRoot = Join-Path $script:OutputRootPath 'diagnostics'
    New-Item -ItemType Directory -Path $script:DiagnosticsRoot -Force | Out-Null

    $resolvedProject = Get-FullPath $ProjectRoot
    if (-not (Test-Path -LiteralPath $resolvedProject -PathType Container)) {
        Add-Issue -Code 'project-root-invalid' -Stage 'project' -Message "Project root is not a directory: $ProjectRoot"
    }
    $sourceManifestPath = Join-Path $resolvedProject 'noemancer.project.json'
    if (-not (Test-Path -LiteralPath $sourceManifestPath -PathType Leaf)) {
        Add-Issue -Code 'project-manifest-missing' -Stage 'project' -Message 'The source Lumen Run project manifest is missing.'
    }
    if ([string]::IsNullOrWhiteSpace($RuntimePath)) {
        $RuntimePath = Join-Path $script:EngineRoot ("build\windows-msvc-debug\src\runtime\$Config\noemancer.exe")
    }
    $RuntimePath = Get-FullPath $RuntimePath
    if ($Config -ne 'Release') {
        Add-Issue -Code 'release.required' -Stage 'runtime' -Message 'Hybrid Pixel package acceptance requires the Release Runtime and windows-x64-release Game Profile.'
    }
    if (-not (Test-Path -LiteralPath $RuntimePath -PathType Leaf)) {
        Add-Issue -Code 'runtime-missing' -Stage 'runtime' -Message "Runtime executable is missing: $RuntimePath"
    }

    if (Test-Path -LiteralPath $sourceManifestPath -PathType Leaf) {
        $script:SourceManifestHashBefore = Get-Sha256 $sourceManifestPath
    }
    if (Test-Path -LiteralPath $resolvedProject -PathType Container) {
        $script:SourceProjectHashBefore = Get-ProjectTreeHash -ProjectPath $resolvedProject -Stage 'projectBoundary'
    }
    $script:TemporaryProjectRoot = Join-Path $script:OutputRootPath 'lumen-run-project'
    if ($script:Issues.Count -eq 0) {
        Copy-ProjectSnapshot -Source $resolvedProject -Destination $script:TemporaryProjectRoot
        $script:ExpectedProfile = Inject-HybridPixelProfile -ProjectPath $script:TemporaryProjectRoot
        if ($null -eq $script:ExpectedProfile) {
            Add-Issue -Code 'profile-injection-failed' -Stage 'profileInjection' -Message 'Hybrid Pixel profile could not be injected into the copied project.'
        }
        else {
            $script:NativeBoundary = Test-ProjectNativeBoundary -ProjectPath $script:TemporaryProjectRoot
        }
    }

    $targetProfile = 'windows-x64-release'
    if ($script:Issues.Count -eq 0) {
        $script:SourceHeadless = Invoke-HeadlessProbe -Executable $RuntimePath `
            -Arguments @('run', '--headless', '--frames', [string]$Frames, '--format', 'json', '--project', $script:TemporaryProjectRoot) `
            -WorkingDirectory $script:TemporaryProjectRoot -Stage 'source-headless' -RequirePlayerEvents:$false
    }
    $packageRoot = Join-Path $script:OutputRootPath 'package'
    if ($script:Issues.Count -eq 0) {
        $script:Package = Invoke-PackageBuild -Runtime $RuntimePath -CopiedProject $script:TemporaryProjectRoot `
            -PackageRoot $packageRoot -TargetProfile $targetProfile
    }
    if ($script:Issues.Count -eq 0 -and $null -ne $script:Package -and [bool]$script:Package.pass) {
        $script:ShaderClosure = Test-ShaderDistributionClosure -PackageRoot $packageRoot
    }
    if ($script:Issues.Count -eq 0 -and $null -ne $script:Package -and $null -ne $script:Package.playerPath) {
        $profilePath = $script:Package.profilePath
        $script:PackageHeadless = Invoke-HeadlessProbe -Executable $script:Package.playerPath `
            -Arguments @('player', '--profile', $profilePath, '--headless', '--frames', [string]$Frames, '--format', 'json') `
            -WorkingDirectory $packageRoot -Stage 'package-player-headless' -RequirePlayerEvents:$true
    }
    foreach ($size in $script:WindowSizes) {
        if ($script:Issues.Count -ne 0) { break }
        foreach ($backend in $script:Backends) {
            if ($script:Issues.Count -ne 0) { break }
            $stage = "source-$backend-$($size.id)"
            $capture = Invoke-RendererCapture -Executable $RuntimePath -PrefixArguments @('run', '--project', $script:TemporaryProjectRoot) `
                -WorkingDirectory $script:TemporaryProjectRoot -Stage $stage -Backend $backend -SizeId $size.id `
                -Width $size.width -Height $size.height
            [void]$script:SourceCaptures.Add([pscustomobject]$capture)
        }
    }
    if ($script:Issues.Count -eq 0 -and $null -ne $script:Package -and $null -ne $script:Package.playerPath) {
        foreach ($packageSize in $script:WindowSizes) {
            if ($script:Issues.Count -ne 0) { break }
        foreach ($backend in $script:Backends) {
            if ($script:Issues.Count -ne 0) { break }
            $stage = "package-$backend-$($packageSize.id)"
            $capture = Invoke-RendererCapture -Executable $script:Package.playerPath `
                -PrefixArguments @('player', '--profile', $script:Package.profilePath) -WorkingDirectory $packageRoot `
                -Stage $stage -Backend $backend -SizeId $packageSize.id -Width $packageSize.width -Height $packageSize.height
            [void]$script:PackageCaptures.Add([pscustomobject]$capture)
        }
        }
    }
    if ($script:Issues.Count -eq 0) {
        $script:Stability = Compare-CaptureStability -SourceCaptures @($script:SourceCaptures) -PackageCaptures @($script:PackageCaptures)
    }
    $exitCode = if ($script:Issues.Count -eq 0) { 0 } else { 3 }
}
catch {
    Add-Issue -Code 'script.unexpected-error' -Stage 'script' -Message $_.Exception.ToString()
    $exitCode = 1
}
finally {
    if ($null -ne $sourceManifestPath -and (Test-Path -LiteralPath $sourceManifestPath -PathType Leaf)) {
        $script:SourceManifestHashAfter = Get-Sha256 $sourceManifestPath
        if (-not [string]::IsNullOrWhiteSpace($script:SourceManifestHashBefore) -and
            $script:SourceManifestHashAfter -ne $script:SourceManifestHashBefore) {
            Add-Issue -Code 'project.source-modified' -Stage 'projectBoundary' -Message 'The source Lumen Run manifest changed during verification; the acceptance is rejected.'
            $exitCode = 3
        }
    }
    if ($null -ne $resolvedProject -and (Test-Path -LiteralPath $resolvedProject -PathType Container)) {
        $script:SourceProjectHashAfter = Get-ProjectTreeHash -ProjectPath $resolvedProject -Stage 'projectBoundary'
        if (-not [string]::IsNullOrWhiteSpace($script:SourceProjectHashBefore) -and
            $script:SourceProjectHashAfter -ne $script:SourceProjectHashBefore) {
            Add-Issue -Code 'project.tree-modified' -Stage 'projectBoundary' -Message 'The source Lumen Run project tree changed during verification; the acceptance is rejected.'
            $exitCode = 3
        }
    }
    if ($null -ne $script:OutputRootPath) {
        try {
            $success = ($exitCode -eq 0 -and $script:Issues.Count -eq 0)
            [void](Write-Receipt -Success:$success)
        }
        catch {
            [Console]::Error.WriteLine("Could not write Hybrid Pixel receipt: $($_.Exception.Message)")
            $exitCode = 1
        }
    }
}
exit $exitCode
