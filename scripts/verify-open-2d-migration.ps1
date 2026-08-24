[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$ProjectRoot = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) '_games\starfall-gauntlet'),

    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Debug',

    [ValidateRange(1, 600)]
    [int]$Frames = 3,

    [string]$PackageRoot = '',

    [string]$ReceiptPath = '',

    # This is intentionally optional.  The normal location is the already
    # built engine Runtime; the verifier never configures or builds native C++.
    [string]$RuntimePath = ''
)

# Exit contract:
#   0 = project boundary, managed build, and requested runtime probes passed
#   2 = invalid invocation or project root
#   3 = project/schema/boundary validation failed
#   4 = the managed project did not compile
#   5 = the source-project headless probe failed
#   6 = the optional packaged Player probe failed
#   7 = the requested receipt could not be written
#   1 = an unexpected verifier failure

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:EngineRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$script:Issues = [System.Collections.Generic.List[object]]::new()
$script:TemporaryRoot = $null
$script:ProjectPath = $null
$script:StartedUtc = [DateTime]::UtcNow
$script:MaxCapturedBytes = 512KB
$script:MaxJsonLines = 2048
$script:AcceptedProjectSchemas = @('noemancer.project/0.1', 'noemancer.project/0.2')
# Migration may inspect an already-created package from either generation;
# Release Closure remains the strict 0.4 gate.
$script:CompatibleGameProfileSchemas = @('noemancer.game-profile/0.3', 'noemancer.game-profile/0.4')

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

    $issue = [ordered]@{
        code    = $Code
        stage   = $Stage
        message = $Message
    }
    if (-not [string]::IsNullOrWhiteSpace($Path)) { $issue.path = $Path }
    [void]$script:Issues.Add([pscustomobject]$issue)
}

function Get-FullPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    return [IO.Path]::GetFullPath($Path)
}

function Get-RelativeProjectPath {
    param([Parameter(Mandatory = $true)][string]$Path)

    if ($null -eq $script:ProjectPath) { return ($Path -replace '\\', '/') }
    $full = Get-FullPath $Path
    $root = $script:ProjectPath.TrimEnd('\', '/')
    if ($full.Equals($root, [StringComparison]::OrdinalIgnoreCase)) { return '.' }
    $prefix = $root + [IO.Path]::DirectorySeparatorChar
    if ($full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        return $full.Substring($prefix.Length).Replace('\', '/')
    }
    return $full.Replace('\', '/')
}

function Test-ContainedPath {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Root
    )

    $full = Get-FullPath $Path
    $root = (Get-FullPath $Root).TrimEnd('\', '/')
    return $full.Equals($root, [StringComparison]::OrdinalIgnoreCase) -or
        $full.StartsWith($root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)
}

function Resolve-ProjectRelativePath {
    param(
        [AllowNull()][string]$RelativePath,
        [Parameter(Mandatory = $true)][string]$Stage,
        [Parameter(Mandatory = $true)][string]$Field,
        [ValidateSet('any', 'leaf', 'directory')]
        [string]$Kind = 'any'
    )

    if ([string]::IsNullOrWhiteSpace($RelativePath)) {
        Add-Issue -Code 'path.missing' -Stage $Stage -Path $Field -Message 'A required project-relative path is missing.'
        return $null
    }

    $candidate = $RelativePath.Replace('/', '\')
    if ([IO.Path]::IsPathRooted($candidate) -or $candidate -match '(^|\\)\.\.(\\|$)') {
        Add-Issue -Code 'path.not-project-relative' -Stage $Stage -Path $Field -Message "Path must be relative to the project and may not escape it: $RelativePath"
        return $null
    }

    try {
        $full = Get-FullPath (Join-Path $script:ProjectPath $candidate)
        if (-not (Test-ContainedPath -Path $full -Root $script:ProjectPath)) {
            Add-Issue -Code 'path.outside-project' -Stage $Stage -Path $Field -Message "Resolved path escapes the project root: $RelativePath"
            return $null
        }
        if ($Kind -eq 'leaf' -and -not (Test-Path -LiteralPath $full -PathType Leaf)) {
            Add-Issue -Code 'file.missing' -Stage $Stage -Path $Field -Message "Required project file is missing: $RelativePath"
            return $null
        }
        if ($Kind -eq 'directory' -and -not (Test-Path -LiteralPath $full -PathType Container)) {
            Add-Issue -Code 'directory.missing' -Stage $Stage -Path $Field -Message "Required project directory is missing: $RelativePath"
            return $null
        }
        return $full
    }
    catch {
        Add-Issue -Code 'path.invalid' -Stage $Stage -Path $Field -Message "Could not resolve project path '$RelativePath': $($_.Exception.Message)"
        return $null
    }
}

function Read-JsonFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Stage,
        [Parameter(Mandatory = $true)][string]$Label
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        Add-Issue -Code 'file.missing' -Stage $Stage -Path (Get-RelativeProjectPath $Path) -Message "$Label is missing."
        return $null
    }
    try {
        $raw = Get-Content -LiteralPath $Path -Raw -Encoding UTF8
        if ([string]::IsNullOrWhiteSpace($raw)) {
            Add-Issue -Code 'json.empty' -Stage $Stage -Path (Get-RelativeProjectPath $Path) -Message "$Label is empty."
            return $null
        }
        return ($raw | ConvertFrom-Json -Depth 100)
    }
    catch {
        Add-Issue -Code 'json.invalid' -Stage $Stage -Path (Get-RelativeProjectPath $Path) -Message "$Label is not valid JSON: $($_.Exception.Message)"
        return $null
    }
}

function Get-Sha256 {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return '' }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
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

function Get-TextFileExtensions {
    return @('.cs', '.csproj', '.props', '.targets', '.json', '.jsonl', '.xml', '.txt', '.md', '.cmake', '.sln', '.vcxproj')
}

function Get-ProjectFiles {
    param([Parameter(Mandatory = $true)][string]$Root)

    $ignored = @('\bin\', '\obj\', '\.git\', '\.vs\', '\generated\')
    return @(Get-ChildItem -LiteralPath $Root -Recurse -File -Force -ErrorAction SilentlyContinue | Where-Object {
        $path = $_.FullName
        foreach ($fragment in $ignored) {
            if ($path.IndexOf($fragment, [StringComparison]::OrdinalIgnoreCase) -ge 0) { return $false }
        }
        return $true
    })
}

function Find-NativeFiles {
    param([Parameter(Mandatory = $true)][string]$Root)

    $extensions = @('.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp', '.hxx', '.inl', '.ixx', '.m', '.mm', '.cmake', '.vcxproj')
    $files = @(Get-ProjectFiles -Root $Root | Where-Object {
        $extensions -contains $_.Extension.ToLowerInvariant() -or $_.Name -eq 'CMakeLists.txt'
    })
    return $files
}

function Find-NativeReferences {
    param([Parameter(Mandatory = $true)][string]$Root)

    $patterns = @(
        '(?im)^\s*#\s*include\s*[<"]',
        '(?i)\b(?:DllImport|LibraryImport|UnmanagedCallersOnly|Marshal\.GetDelegateForFunctionPointer)\b',
        '(?i)\b(?:cmake_minimum_required|add_subdirectory|target_link_libraries|add_library|add_executable)\b',
        '(?i)(?:src[\\/]engine|src[\\/]runtime|src[\\/]editor|noemancer[._-]?native|noemancer[._-]?engine)',
        '(?i)(?:[A-Z]:[\\/]|\\\\)[^\r\n"'']*(?:src[\\/](?:engine|runtime|editor)|managed[\\/]Noemancer)',
        '(?i)<(?:ProjectReference|Reference)\b[^>]*\bInclude\s*=\s*"(?:[^"$]|\$\([^)]*\))*\.vcxproj'
    )
    # Documentation may name the engine checkout in build/run instructions.
    # The clean-room gate applies to executable source and build metadata, not
    # prose, provenance notes or project-authored data.
    $extensions = @('.cs', '.csproj', '.props', '.targets', '.cmake', '.sln', '.vcxproj')
    $matches = [System.Collections.Generic.List[object]]::new()
    foreach ($file in @(Get-ProjectFiles -Root $Root)) {
        if ($extensions -notcontains $file.Extension.ToLowerInvariant() -and $file.Name -notin @('CMakeLists.txt')) { continue }
        if ($file.Length -gt 2MB) { continue }
        try { $text = Get-Content -LiteralPath $file.FullName -Raw -Encoding UTF8 } catch { continue }
        foreach ($pattern in $patterns) {
            $match = [regex]::Match($text, $pattern)
            if (-not $match.Success) { continue }
            $line = ($text.Substring(0, $match.Index) -split "`r?`n").Count
            [void]$matches.Add([pscustomobject]@{
                file = Get-RelativeProjectPath $file.FullName
                line = $line
                pattern = $pattern
                sample = (Limit-Text $match.Value).text
            })
            break
        }
    }
    return @($matches.ToArray())
}

function Invoke-ProjectValidation {
    $before = $script:Issues.Count
    $manifestPath = Join-Path $script:ProjectPath 'noemancer.project.json'
    $manifest = Read-JsonFile -Path $manifestPath -Stage 'project' -Label 'Project manifest'
    $scenePath = $null
    $scriptProjectPath = $null
    $scene = $null
    $scriptProjectText = ''
    $assetRootPaths = [System.Collections.Generic.List[string]]::new()

    if ($null -ne $manifest) {
        $schema = [string](Get-PropertyValue $manifest 'schema')
        if ($script:AcceptedProjectSchemas -notcontains $schema) {
            Add-Issue -Code 'project.schema-invalid' -Stage 'project' -Path 'noemancer.project.json/schema' -Message "Expected one of $($script:AcceptedProjectSchemas -join ', '), got '$schema'."
        }
        foreach ($field in @('projectId', 'name', 'startupScene', 'scriptProject', 'assetRoots')) {
            if ($null -eq (Get-PropertyValue $manifest $field)) {
                Add-Issue -Code 'project.field-missing' -Stage 'project' -Path "noemancer.project.json/$field" -Message "Project field '$field' is required for an open 2D migration."
            }
        }

        $startupScene = [string](Get-PropertyValue $manifest 'startupScene')
        $scenePath = Resolve-ProjectRelativePath -RelativePath $startupScene -Stage 'project' -Field '/startupScene' -Kind 'leaf'
        if ($null -ne $scenePath) {
            if (-not $startupScene.ToLowerInvariant().EndsWith('.scene.json')) {
                Add-Issue -Code 'project.startup-scene-extension' -Stage 'project' -Path '/startupScene' -Message 'The startup scene must use a .scene.json document.'
            }
            $scene = Read-JsonFile -Path $scenePath -Stage 'scene' -Label 'Startup scene'
        }

        $scriptProject = [string](Get-PropertyValue $manifest 'scriptProject')
        $scriptProjectPath = Resolve-ProjectRelativePath -RelativePath $scriptProject -Stage 'project' -Field '/scriptProject' -Kind 'leaf'
        if ($null -ne $scriptProjectPath) {
            if (-not $scriptProject.ToLowerInvariant().EndsWith('.csproj')) {
                Add-Issue -Code 'project.script-project-extension' -Stage 'project' -Path '/scriptProject' -Message 'scriptProject must point to a C# .csproj file.'
            }
            if (-not $scriptProject.Replace('\', '/').StartsWith('scripts/', [StringComparison]::OrdinalIgnoreCase)) {
                Add-Issue -Code 'project.script-project-boundary' -Stage 'project' -Path '/scriptProject' -Message 'Project gameplay C# must remain under the project scripts/ boundary.'
            }
            try { $scriptProjectText = Get-Content -LiteralPath $scriptProjectPath -Raw -Encoding UTF8 } catch { $scriptProjectText = '' }
        }

        $assetRoots = Get-PropertyValue $manifest 'assetRoots'
        if ($null -ne $assetRoots) {
            foreach ($assetRoot in @($assetRoots)) {
                $assetPath = Resolve-ProjectRelativePath -RelativePath ([string]$assetRoot) -Stage 'project' -Field '/assetRoots' -Kind 'directory'
                if ($null -ne $assetPath) { [void]$assetRootPaths.Add((Get-RelativeProjectPath $assetPath)) }
            }
            if ($assetRootPaths.Count -eq 0) {
                Add-Issue -Code 'project.asset-roots-empty' -Stage 'project' -Path '/assetRoots' -Message 'At least one project-owned asset root is required.'
            }
        }

        $hudDocument = Get-PropertyValue $manifest 'hudDocument'
        if ($null -ne $hudDocument -and -not [string]::IsNullOrWhiteSpace([string]$hudDocument)) {
            [void](Resolve-ProjectRelativePath -RelativePath ([string]$hudDocument) -Stage 'project' -Field '/hudDocument' -Kind 'leaf')
        }

        $inputActions = Get-PropertyValue $manifest 'inputActions'
        if ($null -ne $inputActions -and -not ($inputActions -is [System.Collections.IEnumerable])) {
            Add-Issue -Code 'project.input-actions-shape' -Stage 'project' -Path '/inputActions' -Message 'inputActions must be an array when present.'
        }
    }

    if ($null -ne $scene) {
        if ([string](Get-PropertyValue $scene 'schema') -ne 'noemancer.scene/0.1') {
            Add-Issue -Code 'scene.schema-invalid' -Stage 'scene' -Path (Get-RelativeProjectPath $scenePath) -Message 'Startup scene schema must be noemancer.scene/0.1.'
        }
        foreach ($field in @('sceneGuid', 'name', 'entities')) {
            if ($null -eq (Get-PropertyValue $scene $field)) {
                Add-Issue -Code 'scene.field-missing' -Stage 'scene' -Path (Get-RelativeProjectPath $scenePath) -Message "Startup scene field '$field' is required."
            }
        }
        $entities = @(Get-PropertyValue $scene 'entities')
        if ($entities.Count -eq 0) {
            Add-Issue -Code 'scene.entities-empty' -Stage 'scene' -Path (Get-RelativeProjectPath $scenePath) -Message 'Startup scene must contain at least one entity.'
        }
        $entityIds = @{}
        foreach ($entity in $entities) {
            $id = [string](Get-PropertyValue $entity 'guid')
            if ([string]::IsNullOrWhiteSpace($id)) {
                Add-Issue -Code 'scene.entity-guid-missing' -Stage 'scene' -Path (Get-RelativeProjectPath $scenePath) -Message 'Every startup scene entity requires a stable guid.'
                continue
            }
            if ($entityIds.ContainsKey($id)) {
                Add-Issue -Code 'scene.entity-guid-duplicate' -Stage 'scene' -Path $id -Message "Startup scene entity guid '$id' is duplicated."
            }
            else { $entityIds[$id] = $true }
            if ($null -eq (Get-PropertyValue $entity 'components')) {
                Add-Issue -Code 'scene.entity-components-missing' -Stage 'scene' -Path $id -Message "Startup scene entity '$id' requires a components object."
            }
        }
    }

    $scriptDetails = [ordered]@{
        path = if ($null -ne $scriptProjectPath) { Get-RelativeProjectPath $scriptProjectPath } else { $null }
        sourceCount = 0
        nativeFileCount = 0
        nativeReferenceCount = 0
        pass = $false
    }
    if ($null -ne $scriptProjectPath -and (Test-Path -LiteralPath $scriptProjectPath -PathType Leaf)) {
        $scriptDirectory = Split-Path -Parent $scriptProjectPath
        $csFiles = @(Get-ChildItem -LiteralPath $scriptDirectory -Recurse -File -Filter '*.cs' -Force -ErrorAction SilentlyContinue | Where-Object {
            $_.FullName -notmatch '(?i)[\\/]bin[\\/]|[\\/]obj[\\/]|[\\/]\.git[\\/]'
        })
        $scriptDetails.sourceCount = $csFiles.Count
        if ($csFiles.Count -eq 0) {
            Add-Issue -Code 'script.source-empty' -Stage 'scriptBoundary' -Path (Get-RelativeProjectPath $scriptProjectPath) -Message 'The C# project must contain at least one project-owned .cs source file.'
        }
        if ($scriptProjectText -notmatch '(?i)Microsoft\.NET\.Sdk') {
            Add-Issue -Code 'script.sdk-missing' -Stage 'scriptBoundary' -Path (Get-RelativeProjectPath $scriptProjectPath) -Message 'The gameplay project must use the Microsoft.NET.Sdk project model.'
        }
        if ($scriptProjectText -notmatch '(?i)Noemancer\.Managed\.csproj') {
            Add-Issue -Code 'script.public-api-reference-missing' -Stage 'scriptBoundary' -Path (Get-RelativeProjectPath $scriptProjectPath) -Message 'The gameplay project must reference the public Noemancer.Managed API.'
        }
        $projectReferences = [regex]::Matches($scriptProjectText, '(?i)<(?:ProjectReference|Reference)\b[^>]*\bInclude\s*=\s*"([^"]+)"')
        foreach ($reference in $projectReferences) {
            $include = $reference.Groups[1].Value
            $allowedSdkReference = $include -match '(?i)^\$\(NoemancerSdkRoot\)[\\/]managed[\\/]Noemancer\.Managed[\\/]Noemancer\.Managed\.csproj$'
            if (([IO.Path]::IsPathRooted($include) -or $include -match '(^|[\\/])\.\.(?:[\\/]|$)') -and -not $allowedSdkReference) {
                Add-Issue -Code 'script.external-reference' -Stage 'scriptBoundary' -Path (Get-RelativeProjectPath $scriptProjectPath) -Message "C# project reference must stay project-owned or use the public Noemancer.Managed SDK: $include"
            }
        }
    }

    $nativeFiles = @(Find-NativeFiles -Root $script:ProjectPath)
    $scriptDetails.nativeFileCount = $nativeFiles.Count
    foreach ($file in $nativeFiles) {
        Add-Issue -Code 'native.game-source' -Stage 'nativeBoundary' -Path (Get-RelativeProjectPath $file.FullName) -Message 'Open 2D gameplay must not add native engine/game source or a second native build graph.'
    }
    $nativeReferences = @(Find-NativeReferences -Root $script:ProjectPath)
    $scriptDetails.nativeReferenceCount = $nativeReferences.Count
    foreach ($reference in $nativeReferences) {
        Add-Issue -Code 'native.game-reference' -Stage 'nativeBoundary' -Path ([string]$reference.file) -Message "Project source contains a native engine/build reference at line $($reference.line): $($reference.sample)"
    }
    $scriptDetails.pass = ($scriptDetails.sourceCount -gt 0 -and $scriptDetails.nativeFileCount -eq 0 -and $scriptDetails.nativeReferenceCount -eq 0 -and $script:Issues.Count -eq $before)

    return [ordered]@{
        pass = ($script:Issues.Count -eq $before)
        issueCount = $script:Issues.Count - $before
        manifest = [ordered]@{
            path = 'noemancer.project.json'
            schema = if ($null -ne $manifest) { [string](Get-PropertyValue $manifest 'schema') } else { $null }
            acceptedSchemas = @($script:AcceptedProjectSchemas)
            sha256 = Get-Sha256 -Path $manifestPath
        }
        scene = [ordered]@{
            path = if ($null -ne $scenePath) { Get-RelativeProjectPath $scenePath } else { $null }
            schema = if ($null -ne $scene) { [string](Get-PropertyValue $scene 'schema') } else { $null }
            sha256 = if ($null -ne $scenePath) { Get-Sha256 -Path $scenePath } else { '' }
            entityCount = if ($null -ne $scene) { @((Get-PropertyValue $scene 'entities')).Count } else { 0 }
        }
        scripts = $scriptDetails
        assetRoots = @($assetRootPaths.ToArray())
    }
}

function Get-QuotedArgument {
    param([Parameter(Mandatory = $true)][string]$Value)
    if ($Value -notmatch '[\s"]') { return $Value }
    return '"' + $Value.Replace('"', '\"') + '"'
}

function Invoke-CapturedProcess {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [hashtable]$Environment = @{},
        [ValidateRange(1, 1800)][int]$TimeoutSeconds = 180
    )

    $result = [ordered]@{
        file = $FilePath
        arguments = @($Arguments)
        workingDirectory = $WorkingDirectory
        started = $false
        timedOut = $false
        exitCode = $null
        durationMs = $null
        stdout = [ordered]@{ text = ''; bytes = 0; truncated = $false }
        stderr = [ordered]@{ text = ''; bytes = 0; truncated = $false }
    }
    if (-not (Test-Path -LiteralPath $FilePath -PathType Leaf)) {
        $result.stderr = [ordered]@{ text = "Executable was not found: $FilePath"; bytes = 0; truncated = $false }
        return $result
    }

    $psi = [Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $FilePath
    $psi.WorkingDirectory = $WorkingDirectory
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    if ($null -ne $psi.ArgumentList) {
        foreach ($argument in $Arguments) { [void]$psi.ArgumentList.Add([string]$argument) }
    }
    else {
        $psi.Arguments = (($Arguments | ForEach-Object { Get-QuotedArgument ([string]$_) }) -join ' ')
    }
    foreach ($key in $Environment.Keys) { $psi.Environment[$key] = [string]$Environment[$key] }

    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $psi
    $startedAt = [DateTime]::UtcNow
    try {
        if (-not $process.Start()) {
            $result.stderr = [ordered]@{ text = 'Process.Start returned false.'; bytes = 0; truncated = $false }
            return $result
        }
        $result.started = $true
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            $result.timedOut = $true
            try { $process.Kill($true) } catch { }
            $process.WaitForExit()
        }
        $result.exitCode = $process.ExitCode
        $result.stdout = Limit-Text ($stdoutTask.GetAwaiter().GetResult())
        $result.stderr = Limit-Text ($stderrTask.GetAwaiter().GetResult())
    }
    catch {
        $result.stderr = Limit-Text ("$($_.Exception.GetType().Name): $($_.Exception.Message)")
    }
    finally {
        $result.durationMs = [int]([DateTime]::UtcNow - $startedAt).TotalMilliseconds
        $process.Dispose()
    }
    return $result
}

function Get-StructuredProcessEvents {
    param([Parameter(Mandatory = $true)]$ProcessResult)

    $events = [System.Collections.Generic.List[string]]::new()
    $errors = [System.Collections.Generic.List[string]]::new()
    $stdout = [string](Get-PropertyValue (Get-PropertyValue $ProcessResult 'stdout') 'text')
    $lineCount = 0
    foreach ($line in ($stdout -split "`r?`n")) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        $lineCount++
        if ($lineCount -gt $script:MaxJsonLines) { break }
        try {
            $item = $line | ConvertFrom-Json -Depth 30
            $eventName = [string](Get-PropertyValue $item 'event')
            if (-not [string]::IsNullOrWhiteSpace($eventName)) { [void]$events.Add($eventName) }
            if ([string](Get-PropertyValue $item 'level') -eq 'error') { [void]$errors.Add($eventName) }
        }
        catch { }
    }
    return [ordered]@{
        outputLines = $lineCount
        events = @($events.ToArray() | Select-Object -Unique)
        errorEvents = @($errors.ToArray() | Select-Object -Unique)
    }
}

function New-HeadlessProbe {
    param(
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [Parameter(Mandatory = $true)][string]$Stage
    )

    $probeRoot = Join-Path $script:TemporaryRoot (($Stage -replace '[^A-Za-z0-9_.-]', '-') + '-diagnostics')
    New-Item -ItemType Directory -Path $probeRoot -Force | Out-Null
    $process = Invoke-CapturedProcess -FilePath $Executable -Arguments $Arguments -WorkingDirectory $WorkingDirectory `
        -Environment @{
            NOEMANCER_DIAGNOSTICS_DIR = $probeRoot
            NOEMANCER_DISABLE_ERROR_DIALOGS = '1'
            SDL_VIDEODRIVER = 'dummy'
        } -TimeoutSeconds 180
    $events = Get-StructuredProcessEvents -ProcessResult $process
    $pass = [bool]$process.started -and -not [bool]$process.timedOut -and [int]$process.exitCode -eq 0 -and
        @($events.events) -contains 'runtime.stop' -and @($events.errorEvents).Count -eq 0
    if (-not $pass) {
        Add-Issue -Code "$Stage.failed" -Stage $Stage -Message "Headless Noemancer process did not complete successfully (started=$($process.started), exit=$($process.exitCode), timedOut=$($process.timedOut), events=$($events.outputLines))."
    }
    return [ordered]@{
        requested = $true
        pass = $pass
        executable = $Executable
        arguments = @($Arguments)
        frames = $Frames
        process = $process
        events = $events
    }
}

function Resolve-RuntimeExecutable {
    if (-not [string]::IsNullOrWhiteSpace($RuntimePath)) {
        try { return (Get-FullPath $RuntimePath) } catch { return $RuntimePath }
    }
    return Join-Path $script:EngineRoot ("build\windows-msvc-debug\src\runtime\{0}\noemancer.exe" -f $Config)
}

function Invoke-ManagedCompile {
    param([Parameter(Mandatory = $true)][string]$ScriptProjectPath)

    $before = $script:Issues.Count
    $dotnet = Join-Path $script:EngineRoot '_tools\dotnet\dotnet.exe'
    if (-not (Test-Path -LiteralPath $dotnet -PathType Leaf)) {
        $command = Get-Command dotnet.exe -ErrorAction SilentlyContinue
        if ($null -ne $command) { $dotnet = $command.Source }
    }
    $details = [ordered]@{
        pass = $false
        tool = $dotnet
        project = Get-RelativeProjectPath $ScriptProjectPath
        configuration = $Config
        process = $null
        assembly = $null
    }
    if (-not (Test-Path -LiteralPath $dotnet -PathType Leaf)) {
        Add-Issue -Code 'compile.tool-missing' -Stage 'compile' -Message 'Noemancer pinned dotnet SDK or a system dotnet.exe is available; native engine build was not attempted.'
        $details.process = [ordered]@{ started = $false; exitCode = $null; timedOut = $false; stdout = ''; stderr = 'dotnet executable not found.' }
        $details.issueCount = $script:Issues.Count - $before
        return $details
    }

    $arguments = @(
        'build', $ScriptProjectPath, '--configuration', $Config, '--nologo', '--tl:off', '--verbosity:minimal',
        '-p:GenerateFullPaths=true', '-p:PreferredUILang=en-US',
        ('-p:NoemancerSdkRoot={0}' -f $script:EngineRoot)
    )
    $process = Invoke-CapturedProcess -FilePath $dotnet -Arguments $arguments -WorkingDirectory (Split-Path -Parent $ScriptProjectPath) `
        -Environment @{
            DOTNET_CLI_TELEMETRY_OPTOUT = '1'
            DOTNET_NOLOGO = '1'
            DOTNET_SKIP_FIRST_TIME_EXPERIENCE = '1'
            NOEMANCER_DISABLE_ERROR_DIALOGS = '1'
        } -TimeoutSeconds 600
    $details.process = $process
    if (-not $process.started -or $process.timedOut -or [int]$process.exitCode -ne 0) {
        Add-Issue -Code 'compile.failed' -Stage 'compile' -Path (Get-RelativeProjectPath $ScriptProjectPath) -Message "C# project compilation failed (started=$($process.started), exit=$($process.exitCode), timedOut=$($process.timedOut))."
    }
    else {
        $binRoot = Join-Path (Split-Path -Parent $ScriptProjectPath) ("bin\{0}" -f $Config)
        $assemblyName = ([IO.Path]::GetFileNameWithoutExtension($ScriptProjectPath) + '.dll')
        $assemblies = @(Get-ChildItem -LiteralPath $binRoot -Recurse -File -Filter $assemblyName -ErrorAction SilentlyContinue | Where-Object {
            $_.FullName -notmatch '(?i)[\\/]ref(?:int)?[\\/]'
        })
        if ($assemblies.Count -eq 0) {
            Add-Issue -Code 'compile.assembly-missing' -Stage 'compile' -Path (Get-RelativeProjectPath $binRoot) -Message "Compilation exited successfully but did not produce $assemblyName under the project bin/$Config output."
        }
        else {
            $selected = $assemblies | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
            $details.assembly = [ordered]@{
                path = Get-RelativeProjectPath $selected.FullName
                sha256 = Get-Sha256 $selected.FullName
                bytes = $selected.Length
            }
        }
    }
    $details.pass = ($script:Issues.Count -eq $before)
    $details.issueCount = $script:Issues.Count - $before
    return $details
}

function Invoke-PackageProbe {
    param([Parameter(Mandatory = $true)][string]$Root)

    $before = $script:Issues.Count
    $profilePath = Join-Path $Root 'config\game-profile.json'
    $profile = Read-JsonFile -Path $profilePath -Stage 'package' -Label 'Package Game Profile'
    $playerPath = $null
    $profileExecutable = ''
    if ($null -ne $profile) {
        $profileSchema = [string](Get-PropertyValue $profile 'schema')
        if ($script:CompatibleGameProfileSchemas -notcontains $profileSchema) {
            Add-Issue -Code 'package.profile-schema-invalid' -Stage 'package' -Path 'config/game-profile.json' -Message "Package probe requires one of $($script:CompatibleGameProfileSchemas -join ', '), got '$profileSchema'."
        }
        $profileExecutable = [string](Get-PropertyValue $profile 'executable')
        if ([string]::IsNullOrWhiteSpace($profileExecutable) -or [IO.Path]::IsPathRooted($profileExecutable) -or $profileExecutable -match '(^|[\\/])\.\.(?:[\\/]|$)') {
            Add-Issue -Code 'package.player-path-invalid' -Stage 'package' -Path 'config/game-profile.json/executable' -Message 'Game Profile executable must be a package-relative file name.'
        }
        else {
            $playerPath = Get-FullPath (Join-Path $Root (Join-Path 'bin' $profileExecutable))
            if (-not (Test-ContainedPath -Path $playerPath -Root $Root) -or -not (Test-Path -LiteralPath $playerPath -PathType Leaf)) {
                Add-Issue -Code 'package.player-missing' -Stage 'package' -Path ('bin/' + $profileExecutable) -Message 'The packaged Player executable is missing or escapes the package root.'
                $playerPath = $null
            }
        }
    }

    $result = [ordered]@{
        requested = $true
        pass = $false
        packageRoot = $Root
        profile = if ($null -ne $profile) {
            $actualSchema = [string](Get-PropertyValue $profile 'schema')
            [ordered]@{
                path = 'config/game-profile.json'
                schema = $actualSchema
                acceptedSchemas = @($script:CompatibleGameProfileSchemas)
                compatibilityPath = if ($actualSchema -eq 'noemancer.game-profile/0.3') { 'legacy-read' } else { 'current-read' }
                projectId = [string](Get-PropertyValue $profile 'projectId')
            }
        } else { $null }
        player = $null
    }
    if ($null -ne $playerPath) {
        $result.player = New-HeadlessProbe -Executable $playerPath -Arguments @('player', '--profile', $profilePath, '--headless', '--frames', [string]$Frames, '--format', 'json') `
            -WorkingDirectory $Root -Stage 'packagePlayer'
    }
    $result.pass = ($script:Issues.Count -eq $before)
    $result.issueCount = $script:Issues.Count - $before
    return $result
}

function Convert-ReceiptProcess {
    param([AllowNull()]$Process)
    if ($null -eq $Process) { return $null }
    return [ordered]@{
        file = $Process.file
        arguments = @($Process.arguments)
        workingDirectory = $Process.workingDirectory
        started = $Process.started
        timedOut = $Process.timedOut
        exitCode = $Process.exitCode
        durationMs = $Process.durationMs
        stdout = $Process.stdout
        stderr = $Process.stderr
    }
}

function Write-ReceiptAndExit {
    param(
        [Parameter(Mandatory = $true)]$Receipt,
        [Parameter(Mandatory = $true)][int]$ExitCode
    )

    $json = $Receipt | ConvertTo-Json -Depth 40
    $writeFailed = $false
    if (-not [string]::IsNullOrWhiteSpace($ReceiptPath)) {
        try {
            $parent = Split-Path -Parent $ReceiptPath
            if (-not [string]::IsNullOrWhiteSpace($parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
            Set-Content -LiteralPath $ReceiptPath -Value $json -Encoding UTF8
        }
        catch {
            $writeFailed = $true
            [Console]::Error.WriteLine("Could not write migration receipt '$ReceiptPath': $($_.Exception.Message)")
        }
    }
    [Console]::Out.WriteLine($json)
    if ($writeFailed) { exit 7 }
    exit $ExitCode
}

try {
    try {
        $script:ProjectPath = (Resolve-Path -LiteralPath $ProjectRoot -ErrorAction Stop).Path
        $script:ProjectPath = Get-FullPath $script:ProjectPath
        if (-not (Test-Path -LiteralPath $script:ProjectPath -PathType Container)) { throw 'Project root is not a directory.' }
    }
    catch {
        $early = [ordered]@{
            schema = 'noemancer.open-2d-migration-receipt/0.1'
            success = $false
            projectRoot = $ProjectRoot
            configuration = $Config
            frames = $Frames
            issues = @([ordered]@{ code = 'project-root-invalid'; stage = 'project'; message = $_.Exception.Message })
        }
        Write-ReceiptAndExit -Receipt $early -ExitCode 2
    }

    $script:TemporaryRoot = Join-Path ([IO.Path]::GetTempPath()) ('noemancer-open-2d-migration-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $script:TemporaryRoot -Force | Out-Null

    $validation = Invoke-ProjectValidation
    $compile = $null
    $projectHeadless = [ordered]@{ requested = $false; pass = $true; skipped = $true; reason = 'validation-failed-or-runtime-not-found' }
    $packageProbe = $null

    $scriptProjectRelative = $null
    $manifestPath = Join-Path $script:ProjectPath 'noemancer.project.json'
    if (Test-Path -LiteralPath $manifestPath -PathType Leaf) {
        $manifestForCompile = Read-JsonFile -Path $manifestPath -Stage 'compile' -Label 'Project manifest for compilation'
        if ($null -ne $manifestForCompile) {
            $scriptProjectRelative = [string](Get-PropertyValue $manifestForCompile 'scriptProject')
        }
    }
    $scriptProjectPath = if (-not [string]::IsNullOrWhiteSpace($scriptProjectRelative)) {
        Resolve-ProjectRelativePath -RelativePath $scriptProjectRelative -Stage 'compile' -Field '/scriptProject' -Kind 'leaf'
    } else { $null }

    if ($validation.pass -and $null -ne $scriptProjectPath) {
        $compile = Invoke-ManagedCompile -ScriptProjectPath $scriptProjectPath
    }
    else {
        $compile = [ordered]@{ pass = $false; skipped = $true; reason = 'project-validation-failed-or-script-project-missing'; process = $null; issueCount = 0 }
    }

    $runtime = Resolve-RuntimeExecutable
    if ($validation.pass -and $compile.pass -and (Test-Path -LiteralPath $runtime -PathType Leaf)) {
        $projectHeadless = New-HeadlessProbe -Executable $runtime -Arguments @('run', '--headless', '--frames', [string]$Frames, '--format', 'json', '--project', $script:ProjectPath) `
            -WorkingDirectory $script:EngineRoot -Stage 'projectHeadless'
    }
    elseif (-not (Test-Path -LiteralPath $runtime -PathType Leaf)) {
        $projectHeadless = [ordered]@{ requested = $false; pass = $true; skipped = $true; reason = 'runtime-executable-not-found'; executable = $runtime }
    }

    if (-not [string]::IsNullOrWhiteSpace($PackageRoot)) {
        try {
            $resolvedPackage = (Resolve-Path -LiteralPath $PackageRoot -ErrorAction Stop).Path
            if (-not (Test-Path -LiteralPath $resolvedPackage -PathType Container)) { throw 'Package root is not a directory.' }
            $packageProbe = Invoke-PackageProbe -Root (Get-FullPath $resolvedPackage)
        }
        catch {
            Add-Issue -Code 'package-root-invalid' -Stage 'package' -Message $_.Exception.Message
            $packageProbe = [ordered]@{ requested = $true; pass = $false; packageRoot = $PackageRoot; issueCount = 1 }
        }
    }
    else {
        $packageProbe = [ordered]@{ requested = $false; pass = $true; skipped = $true; reason = 'package-root-not-supplied' }
    }

    $failureCode = 0
    if (-not $validation.pass) { $failureCode = 3 }
    elseif (-not $compile.pass) { $failureCode = 4 }
    elseif (-not $projectHeadless.pass) { $failureCode = 5 }
    elseif (-not $packageProbe.pass) { $failureCode = 6 }

    $receipt = [ordered]@{
        schema = 'noemancer.open-2d-migration-receipt/0.1'
        success = ($failureCode -eq 0 -and $script:Issues.Count -eq 0)
        projectRoot = $script:ProjectPath
        configuration = $Config
        frames = $Frames
        startedUtc = $script:StartedUtc.ToString('o')
        completedUtc = [DateTime]::UtcNow.ToString('o')
        stages = [ordered]@{
            project = $validation
            compile = $compile
            projectHeadless = $projectHeadless
            packagePlayer = $packageProbe
        }
        issues = @($script:Issues.ToArray())
    }
    # Process objects are intentionally normalized at the final boundary so
    # callers receive a stable JSON shape and no live .NET handles.
    $compileProcess = Get-PropertyValue $receipt.stages.compile 'process'
    if ($null -ne $compileProcess) {
        $receipt.stages.compile.process = Convert-ReceiptProcess $compileProcess
    }
    $projectProcess = Get-PropertyValue $receipt.stages.projectHeadless 'process'
    if ($null -ne $projectProcess) {
        $receipt.stages.projectHeadless.process = Convert-ReceiptProcess $projectProcess
    }
    $packagePlayer = Get-PropertyValue $receipt.stages.packagePlayer 'player'
    $packageProcess = Get-PropertyValue $packagePlayer 'process'
    if ($null -ne $packageProcess) {
        $packagePlayer.process = Convert-ReceiptProcess $packageProcess
    }
    Write-ReceiptAndExit -Receipt $receipt -ExitCode $failureCode
}
catch {
    $unexpected = [ordered]@{
        schema = 'noemancer.open-2d-migration-receipt/0.1'
        success = $false
        projectRoot = $ProjectRoot
        configuration = $Config
        frames = $Frames
        issues = @([ordered]@{ code = 'verifier.unexpected'; stage = 'verifier'; message = "$($_.Exception.GetType().Name): $($_.Exception.Message)" })
    }
    Write-ReceiptAndExit -Receipt $unexpected -ExitCode 1
}
finally {
    if ($null -ne $script:TemporaryRoot -and (Test-Path -LiteralPath $script:TemporaryRoot -PathType Container)) {
        Remove-Item -LiteralPath $script:TemporaryRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
