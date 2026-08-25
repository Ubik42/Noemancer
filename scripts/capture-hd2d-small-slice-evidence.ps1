[CmdletBinding()]
param(
    [string]$Project = 'D:\3D\NoemancerProjects\NoemancerHd2dSlice',
    [string]$RuntimePath = (Join-Path $PSScriptRoot '..\build\windows-msvc-debug\src\runtime\Debug\noemancer.exe'),
    [string]$OutputPath = (Join-Path $PSScriptRoot '..\generated\acceptance\hd2d-small-slice-evidence.json'),
    [ValidateSet('windows-x64-release', 'windows-x64-debug')]
    [string]$TargetProfile = 'windows-x64-release',
    [ValidateRange(10, 900)]
    [int]$TimeoutSeconds = 180,
    [switch]$KeepStaging
)

# This lane is intentionally a verifier, not a project generator.  It only
# copies the requested project to a temporary root and lets the existing
# Editor/Runtime/package contracts report what is actually implemented.
$ErrorActionPreference = 'Stop'
$script:ExitCode = 0
$script:Issues = [System.Collections.Generic.List[object]]::new()
$script:ProjectRoot = $null
$script:StageRoot = $null
$script:PackageRoot = $null
$script:OutputRoot = $null
$script:StageKept = $false
$script:SourceTreeBefore = $null
$script:SourceTreeAfter = $null
$script:StaticState = $null
$script:SourceRun = $null
$script:SourceServe = $null
$script:SourceVisual = $null
$script:Package = $null
$script:PackageRun = $null
$script:PackageVisual = $null
$script:PackagePerformance = $null
$script:Tools = $null
$script:StateComparison = $null

function Get-PropertyValue {
    param(
        [AllowNull()][object]$Object,
        [Parameter(Mandatory = $true)][string]$Name
    )
    if ($null -eq $Object) { return $null }
    if ($Object -is [System.Collections.IDictionary]) {
        if ($Object.Contains($Name)) { return $Object[$Name] }
        return $null
    }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}

function Add-Issue {
    param(
        [Parameter(Mandatory = $true)][string]$Code,
        [Parameter(Mandatory = $true)][string]$Stage,
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$Message,
        [ValidateRange(1, 124)][int]$ExitCode = 5
    )
    [void]$script:Issues.Add([ordered]@{ code = $Code; stage = $Stage; message = $Message })
    if ($script:ExitCode -eq 0) { $script:ExitCode = $ExitCode }
}

function Convert-ToJsonSafeObject {
    param([AllowNull()][object]$Value)
    if ($null -eq $Value -or $Value -is [string] -or $Value.GetType().IsPrimitive -or $Value -is [decimal]) { return $Value }
    if ($Value -is [System.Collections.IDictionary]) {
        $object = [ordered]@{}
        foreach ($key in $Value.Keys) { $object[[string]$key] = Convert-ToJsonSafeObject -Value $Value[$key] }
        return $object
    }
    if ($Value -is [System.Collections.IEnumerable]) {
        $array = [System.Collections.Generic.List[object]]::new()
        foreach ($item in $Value) { [void]$array.Add((Convert-ToJsonSafeObject -Value $item)) }
        return @($array)
    }
    $properties = @($Value.PSObject.Properties)
    if ($properties.Count -gt 0) {
        $object = [ordered]@{}
        foreach ($property in $properties) { $object[$property.Name] = Convert-ToJsonSafeObject -Value $property.Value }
        return $object
    }
    return $Value
}

function Write-Utf8Json {
    param([Parameter(Mandatory = $true)][string]$Path, [Parameter(Mandatory = $true)]$Value)
    $parent = Split-Path -Parent $Path
    if (-not [string]::IsNullOrWhiteSpace($parent)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    $text = if ($Value -is [string]) { $Value } else { (Convert-ToJsonSafeObject -Value $Value) | ConvertTo-Json -Depth 100 }
    [IO.File]::WriteAllText($Path, $text + "`n", [Text.UTF8Encoding]::new($false))
}

function Read-JsonFile {
    param([Parameter(Mandatory = $true)][string]$Path, [Parameter(Mandatory = $true)][string]$Stage)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        Add-Issue -Code 'file.missing' -Stage $Stage -Message "Required JSON file is missing: $Path"
        return $null
    }
    try {
        return (Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json -Depth 100)
    } catch {
        Add-Issue -Code 'json.invalid' -Stage $Stage -Message "Could not parse JSON '$Path': $($_.Exception.Message)"
        return $null
    }
}

function Get-Sha256 {
    param([Parameter(Mandatory = $true)][string]$Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-TreeFingerprint {
    param([Parameter(Mandatory = $true)][string]$Root)
    $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
    $rows = [System.Collections.Generic.List[string]]::new()
    if (-not (Test-Path -LiteralPath $rootFull -PathType Container)) {
        return [ordered]@{ sha256 = $null; fileCount = 0 }
    }
    foreach ($file in (Get-ChildItem -LiteralPath $rootFull -Recurse -File -Force | Where-Object {
        $_.FullName -notmatch '\\(\.git|bin|obj|generated)(\\|$)' } | Sort-Object FullName)) {
        $relative = [IO.Path]::GetRelativePath($rootFull, $file.FullName).Replace('\', '/')
        [void]$rows.Add($relative + '|' + (Get-Sha256 -Path $file.FullName))
    }
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes(($rows -join "`n"))
    $digest = [Security.Cryptography.SHA256]::HashData($bytes)
    return [ordered]@{
        sha256 = ([BitConverter]::ToString($digest).Replace('-', '').ToLowerInvariant())
        fileCount = $rows.Count
    }
}

function Get-RelativePath {
    param([Parameter(Mandatory = $true)][string]$Root, [Parameter(Mandatory = $true)][string]$Path)
    return [IO.Path]::GetRelativePath([IO.Path]::GetFullPath($Root), [IO.Path]::GetFullPath($Path)).Replace('\', '/')
}

function Copy-ProjectSnapshot {
    param([Parameter(Mandatory = $true)][string]$Source, [Parameter(Mandatory = $true)][string]$Destination)
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    foreach ($entry in (Get-ChildItem -LiteralPath $Source -Force | Where-Object {
        $_.Name -notin @('.git', 'bin', 'obj', '.vs', 'generated') })) {
        Copy-Item -LiteralPath $entry.FullName -Destination (Join-Path $Destination $entry.Name) -Recurse -Force
    }
}

function Invoke-HiddenProcess {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [Parameter(Mandatory = $true)][int]$Timeout,
        [string]$InputText = '',
        [Parameter(Mandatory = $true)][string]$StdoutPath,
        [Parameter(Mandatory = $true)][string]$StderrPath
    )
    $parent = Split-Path -Parent $StdoutPath
    if (-not [string]::IsNullOrWhiteSpace($parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $FilePath
    $info.WorkingDirectory = $WorkingDirectory
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    foreach ($argument in $Arguments) { [void]$info.ArgumentList.Add([string]$argument) }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $info
    $startedAt = [Diagnostics.Stopwatch]::GetTimestamp()
    try {
        if (-not $process.Start()) { throw "Unable to start process '$FilePath'." }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not [string]::IsNullOrEmpty($InputText)) { $process.StandardInput.Write($InputText) }
        $process.StandardInput.Close()
        $timedOut = -not $process.WaitForExit($Timeout * 1000)
        if ($timedOut) {
            try { $process.Kill($true) } catch { }
            $process.WaitForExit()
        }
        $stdout = $stdoutTask.GetAwaiter().GetResult()
        $stderr = $stderrTask.GetAwaiter().GetResult()
        [IO.File]::WriteAllText($StdoutPath, $stdout, [Text.UTF8Encoding]::new($false))
        [IO.File]::WriteAllText($StderrPath, $stderr, [Text.UTF8Encoding]::new($false))
        return [ordered]@{
            exitCode = if ($timedOut) { 124 } else { $process.ExitCode }
            timedOut = $timedOut
            durationMs = [math]::Round(([Diagnostics.Stopwatch]::GetTimestamp() - $startedAt) * 1000.0 / [Diagnostics.Stopwatch]::Frequency, 2)
            stdout = $stdout
            stderr = $stderr
            stdoutPath = $StdoutPath
            stderrPath = $StderrPath
        }
    } finally {
        $process.Dispose()
    }
}

function Get-JsonFromText {
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Text)
    try { return (ConvertFrom-Json -InputObject $Text -Depth 100) } catch { }
    foreach ($line in ($Text -split "`r?`n" | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })) {
        try { return (ConvertFrom-Json -InputObject $line -Depth 100) } catch { }
    }
    return $null
}

function Get-LogEvents {
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Text)
    $events = [System.Collections.Generic.List[object]]::new()
    foreach ($line in ($Text -split "`r?`n")) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        try { $envelope = ConvertFrom-Json -InputObject $line -Depth 100 } catch { continue }
        $eventName = [string](Get-PropertyValue $envelope 'event')
        if ([string]::IsNullOrWhiteSpace($eventName)) { continue }
        $message = [string](Get-PropertyValue $envelope 'message')
        $payload = $null
        if (-not [string]::IsNullOrWhiteSpace($message)) { $payload = Get-JsonFromText -Text $message }
        [void]$events.Add([ordered]@{
            event = $eventName
            level = [string](Get-PropertyValue $envelope 'level')
            payload = $payload
        })
    }
    return @($events)
}

function Get-LastEvent {
    param([Parameter(Mandatory = $true)]$Events, [Parameter(Mandatory = $true)][string]$Name)
    $matches = @($Events | Where-Object { $_.event -eq $Name })
    if ($matches.Count -eq 0) { return $null }
    return $matches[$matches.Count - 1]
}

function Get-RecursivePropertyCount {
    param([AllowNull()][object]$Value, [Parameter(Mandatory = $true)][string[]]$Names)
    if ($null -eq $Value) { return 0 }
    if ($Value -is [string] -or $Value.GetType().IsPrimitive) { return 0 }
    $count = 0
    if ($Value -is [System.Collections.IEnumerable] -and $Value -isnot [pscustomobject]) {
        foreach ($item in $Value) { $count += Get-RecursivePropertyCount -Value $item -Names $Names }
        return $count
    }
    foreach ($property in $Value.PSObject.Properties) {
        if ($Names -contains $property.Name) { $count++ }
        $count += Get-RecursivePropertyCount -Value $property.Value -Names $Names
    }
    return $count
}

function Get-FeatureCounts {
    param([Parameter(Mandatory = $true)]$Scene)
    return [ordered]@{
        spriteRenderer = Get-RecursivePropertyCount -Value $Scene -Names @('SpriteRenderer')
        tilemap = Get-RecursivePropertyCount -Value $Scene -Names @('Tilemap', 'TilemapRenderer', 'Tilemap2D')
        meshRenderer = Get-RecursivePropertyCount -Value $Scene -Names @('MeshRenderer')
        camera = Get-RecursivePropertyCount -Value $Scene -Names @('Camera')
        lights = Get-RecursivePropertyCount -Value $Scene -Names @('DirectionalLight', 'PointLight', 'SpotLight', 'Light')
        vfx = Get-RecursivePropertyCount -Value $Scene -Names @('Vfx', 'VFX', 'VfxEmitter', 'ParticleSystem', 'ParticleEmitter')
        managedScripts = Get-RecursivePropertyCount -Value $Scene -Names @('ManagedScript')
    }
}

function Get-TextD3D12Matches {
    param([Parameter(Mandatory = $true)][string]$Root)
    $hits = [System.Collections.Generic.List[object]]::new()
    $extensions = @('.json', '.cs', '.csproj', '.scene', '.hlsl', '.shader', '.rcss', '.md')
    foreach ($file in (Get-ChildItem -LiteralPath $Root -Recurse -File -Force | Where-Object {
        $extensions -contains $_.Extension.ToLowerInvariant() -and $_.FullName -notmatch '\\(bin|obj|generated)(\\|$)' })) {
        try {
            $line = Select-String -LiteralPath $file.FullName -Pattern 'd3d12' -SimpleMatch -CaseSensitive:$false | Select-Object -First 1
            if ($null -ne $line) {
                [void]$hits.Add([ordered]@{ path = Get-RelativePath -Root $Root -Path $file.FullName; line = $line.LineNumber })
            }
        } catch { }
    }
    return @($hits)
}

function Test-NativeBoundary {
    param([Parameter(Mandatory = $true)][string]$Root)
    $extensions = @('.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp', '.hxx', '.ixx')
    $files = @(Get-ChildItem -LiteralPath $Root -Recurse -File -Force | Where-Object {
        $extensions -contains $_.Extension.ToLowerInvariant() -or $_.Name -in @('CMakeLists.txt', 'CMakePresets.json') })
    return @($files | ForEach-Object { Get-RelativePath -Root $Root -Path $_.FullName })
}

function Test-RequiredRelativePath {
    param([Parameter(Mandatory = $true)][string]$Root, [AllowNull()][object]$Value, [Parameter(Mandatory = $true)][string]$Field)
    if ([string]::IsNullOrWhiteSpace([string]$Value)) {
        Add-Issue -Code 'project.field-missing' -Stage 'project-static' -Message "Project field '$Field' is missing."
        return $false
    }
    $candidate = [IO.Path]::GetFullPath((Join-Path $Root ([string]$Value)))
    $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd('\', '/') + '\'
    if (-not $candidate.StartsWith($rootFull, [StringComparison]::OrdinalIgnoreCase) -or -not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
        Add-Issue -Code 'project.path-invalid' -Stage 'project-static' -Message "Project field '$Field' does not point to a readable relative file: $Value"
        return $false
    }
    return $true
}

function Test-StaticProject {
    param([Parameter(Mandatory = $true)][string]$Root)
    $manifestPath = Join-Path $Root 'noemancer.project.json'
    $manifest = Read-JsonFile -Path $manifestPath -Stage 'project-static'
    if ($null -eq $manifest) { return [ordered]@{ pass = $false; manifest = $null; features = $null; registry = $null; sourceD3D12 = @(); nativeCpp = @() } }
    $schema = [string](Get-PropertyValue $manifest 'schema')
    if ($schema -ne 'noemancer.project/0.2') {
        Add-Issue -Code 'project.schema-not-hybrid-pixel' -Stage 'project-static' -Message "Hybrid Pixel project requires noemancer.project/0.2; got '$schema'."
    }
    foreach ($field in @('startupScene', 'assetRoots', 'scriptProject', 'hudDocument', 'inputActions', 'hybridPixelProfile')) {
        if ($null -eq (Get-PropertyValue $manifest $field)) {
            Add-Issue -Code 'project.field-missing' -Stage 'project-static' -Message "Project field '$field' is required for this small slice."
        }
    }
    $hybrid = Get-PropertyValue $manifest 'hybridPixelProfile'
    $hybridFields = @('schema', 'profileId', 'enabled', 'virtualWidth', 'virtualHeight', 'pixelsPerUnit', 'integerScaling', 'snapCamera', 'snapSprites', 'presentationFilter')
    if ($null -ne $hybrid) {
        foreach ($field in $hybridFields) {
            if ($null -eq (Get-PropertyValue $hybrid $field)) { Add-Issue -Code 'hybrid-pixel.field-missing' -Stage 'project-static' -Message "Hybrid Pixel field '$field' is missing." }
        }
        if ([string](Get-PropertyValue $hybrid 'schema') -ne 'noemancer.hybrid-pixel-profile/0.1') { Add-Issue -Code 'hybrid-pixel.schema-invalid' -Stage 'project-static' -Message 'Hybrid Pixel profile schema is not noemancer.hybrid-pixel-profile/0.1.' }
        if ((Get-PropertyValue $hybrid 'enabled') -ne $true) { Add-Issue -Code 'hybrid-pixel.disabled' -Stage 'project-static' -Message 'Hybrid Pixel profile is not enabled.' }
    }
    $actions = @(Get-PropertyValue $manifest 'inputActions')
    if ($actions.Count -eq 0) { Add-Issue -Code 'input.actions-empty' -Stage 'project-static' -Message 'Project inputActions must contain at least one action.' }
    $inputIds = @($actions | ForEach-Object { [string](Get-PropertyValue $_ 'id') } | Where-Object { $_ })

    $startupScene = [string](Get-PropertyValue $manifest 'startupScene')
    $scene = $null
    if (Test-RequiredRelativePath -Root $Root -Value $startupScene -Field 'startupScene') {
        $scene = Read-JsonFile -Path ([IO.Path]::GetFullPath((Join-Path $Root $startupScene))) -Stage 'project-scene'
    }
    $features = if ($null -ne $scene) { Get-FeatureCounts -Scene $scene } else { [ordered]@{} }
    if ([int](Get-PropertyValue $features 'spriteRenderer') -le 0 -and [int](Get-PropertyValue $features 'tilemap') -le 0) {
        Add-Issue -Code 'render.2d-content-missing' -Stage 'project-scene' -Message 'Startup scene has neither SpriteRenderer nor Tilemap content.'
    }
    if ([int](Get-PropertyValue $features 'meshRenderer') -le 0 -or [int](Get-PropertyValue $features 'camera') -le 0) {
        Add-Issue -Code 'render.mixed-2d-3d-missing' -Stage 'project-scene' -Message 'Startup scene must contain both 2D content and a 3D render path (MeshRenderer and Camera).'
    }
    # A project may author a VFX graph as an asset rather than placing an
    # emitter in the startup scene.  The registry check below supplies that
    # second, still-structured evidence path.

    $scriptProject = [string](Get-PropertyValue $manifest 'scriptProject')
    $scriptProjectPath = $null
    if (Test-RequiredRelativePath -Root $Root -Value $scriptProject -Field 'scriptProject') { $scriptProjectPath = [IO.Path]::GetFullPath((Join-Path $Root $scriptProject)) }
    $csFiles = @()
    if ($null -ne $scriptProjectPath) { $csFiles = @(Get-ChildItem -LiteralPath (Split-Path -Parent $scriptProjectPath) -Recurse -File -Filter '*.cs' -ErrorAction SilentlyContinue) }
    if ($csFiles.Count -eq 0) { Add-Issue -Code 'scripting.cs-source-missing' -Stage 'project-static' -Message 'scriptProject has no C# source files.' }

    $hudPath = [string](Get-PropertyValue $manifest 'hudDocument')
    $hud = $null
    if (Test-RequiredRelativePath -Root $Root -Value $hudPath -Field 'hudDocument') { $hud = Read-JsonFile -Path ([IO.Path]::GetFullPath((Join-Path $Root $hudPath))) -Stage 'project-ui' }
    if ($null -ne $hud) {
        if ([string](Get-PropertyValue $hud 'schemaVersion') -ne 'noemancer.ui-document/0.1' -or (Get-PropertyValue $hud 'valid') -ne $true) {
            Add-Issue -Code 'ui.document-invalid' -Stage 'project-ui' -Message 'HUD document is not a valid noemancer.ui-document/0.1 document.'
        }
        if (@(Get-PropertyValue $hud 'nodes').Count -eq 0) { Add-Issue -Code 'ui.nodes-empty' -Stage 'project-ui' -Message 'HUD document has no nodes.' }
    }

    $assetRoots = @(Get-PropertyValue $manifest 'assetRoots')
    $registryPath = $null
    foreach ($assetRoot in $assetRoots) {
        $candidate = Join-Path $Root ([string]$assetRoot) 'registry.json'
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { $registryPath = $candidate; break }
    }
    $registry = if ($null -ne $registryPath) { Read-JsonFile -Path $registryPath -Stage 'asset-registry' } else { $null }
    $assetRecords = if ($null -ne $registry) { @((Get-PropertyValue $registry 'assets')) } else { @() }
    $kindCounts = [ordered]@{
        sprite = @($assetRecords | Where-Object { [string](Get-PropertyValue $_ 'kind') -eq 'Sprite' }).Count
        tilemap = @($assetRecords | Where-Object { [string](Get-PropertyValue $_ 'kind') -eq 'Tilemap' }).Count
        tilePalette = @($assetRecords | Where-Object { [string](Get-PropertyValue $_ 'kind') -eq 'TilePalette' }).Count
        vfxGraph = @($assetRecords | Where-Object { [string](Get-PropertyValue $_ 'kind') -in @('VfxGraph', 'VFX', 'ParticleSystem') }).Count
    }
    if ($kindCounts.sprite -le 0) { Add-Issue -Code 'asset.sprite-missing' -Stage 'asset-registry' -Message 'Asset Registry contains no Sprite record.' }
    if ($kindCounts.tilemap -le 0) { Add-Issue -Code 'asset.tilemap-missing' -Stage 'asset-registry' -Message 'Asset Registry contains no Tilemap record.' }
    if ([int](Get-PropertyValue $features 'vfx') -le 0 -and $kindCounts.vfxGraph -le 0) {
        Add-Issue -Code 'vfx.structure-missing' -Stage 'project-scene' -Message 'No declared VFX/particle component or VFX graph asset is present; no VFX pass is claimed.'
    }

    $nativeCpp = Test-NativeBoundary -Root $Root
    if ($nativeCpp.Count -gt 0) { Add-Issue -Code 'project.native-cpp-present' -Stage 'native-boundary' -Message "Project contains native C/C++ files: $($nativeCpp -join ', ')" }
    $d3d12 = Get-TextD3D12Matches -Root $Root
    if ($d3d12.Count -gt 0) { Add-Issue -Code 'project.d3d12-source-visible' -Stage 'native-boundary' -Message 'Project source contains D3D12-specific text; backend details must remain engine-owned.' }

    return [ordered]@{
        pass = ($script:Issues.Count -eq 0)
        manifest = [ordered]@{ path = Get-RelativePath -Root $Root -Path $manifestPath; schema = $schema; projectId = [string](Get-PropertyValue $manifest 'projectId'); name = [string](Get-PropertyValue $manifest 'name'); hybridPixelProfile = $hybrid; inputActionIds = $inputIds; startupScene = $startupScene; scriptProject = $scriptProject; hudDocument = $hudPath }
        features = $features
        registry = [ordered]@{ path = if ($null -ne $registryPath) { Get-RelativePath -Root $Root -Path $registryPath } else { $null }; schema = if ($null -ne $registry) { [string](Get-PropertyValue $registry 'schema') } else { $null }; kindCounts = $kindCounts; assetIds = @($assetRecords | ForEach-Object { [string](Get-PropertyValue $_ 'id') }) }
        nativeCpp = $nativeCpp
        sourceD3D12 = $d3d12
        scriptSourceCount = $csFiles.Count
        hud = if ($null -ne $hud) { [ordered]@{ schemaVersion = Get-PropertyValue $hud 'schemaVersion'; valid = Get-PropertyValue $hud 'valid'; documentId = Get-PropertyValue $hud 'documentId'; nodeCount = @((Get-PropertyValue $hud 'nodes')).Count } } else { $null }
    }
}

function Invoke-SourceRun {
    param([Parameter(Mandatory = $true)][string]$Root, [Parameter(Mandatory = $true)][string]$Label)
    $stdoutPath = Join-Path $script:OutputRoot ($Label + '.stdout.jsonl')
    $stderrPath = Join-Path $script:OutputRoot ($Label + '.stderr.log')
    $result = Invoke-HiddenProcess -FilePath $RuntimePath -Arguments @('run', '--headless', '--frames', '3', '--format', 'json', '--editor-project-settings', '--project', $Root) -WorkingDirectory $Root -Timeout $TimeoutSeconds -StdoutPath $stdoutPath -StderrPath $stderrPath
    $events = Get-LogEvents -Text $result.stdout
    $production = Get-LastEvent -Events $events -Name 'runtime.production_state'
    $errors = @($events | Where-Object { $_.level -eq 'error' })
    if ($result.timedOut) { Add-Issue -Code 'runtime.timeout' -Stage $Label -Message 'Hidden Runtime probe exceeded its timeout.' -ExitCode 4 }
    if ($result.exitCode -ne 0) { Add-Issue -Code 'runtime.exit-failed' -Stage $Label -Message "Runtime probe exited with code $($result.exitCode)." -ExitCode 4 }
    if ($null -eq $production) { Add-Issue -Code 'runtime.production-state-missing' -Stage $Label -Message 'Runtime did not publish runtime.production_state.' -ExitCode 4 }
    if ($errors.Count -gt 0) { Add-Issue -Code 'runtime.error-event' -Stage $Label -Message "Runtime published $($errors.Count) error event(s)." -ExitCode 4 }
    $payload = if ($null -ne $production) { $production.payload } else { $null }
    $scripting = Get-PropertyValue $payload 'scripting'
    if ($null -ne $payload -and (Get-PropertyValue $scripting 'ready') -ne $true) { Add-Issue -Code 'scripting.host-not-ready' -Stage $Label -Message 'Managed scripting host did not report ready.' }
    if ($null -ne $payload -and @((Get-PropertyValue $scripting 'instances')).Count -eq 0) { Add-Issue -Code 'scripting.no-instances' -Stage $Label -Message 'Runtime did not create any managed script instance.' }
    $executed = Get-PropertyValue (Get-PropertyValue $scripting 'lastManagedResult') 'projectCodeExecuted'
    if ($null -ne $payload -and $executed -ne $true) { Add-Issue -Code 'scripting.project-code-not-executed' -Stage $Label -Message 'Runtime did not prove project C# code execution.' }
    return [ordered]@{
        pass = ($result.exitCode -eq 0 -and -not $result.timedOut -and $null -ne $production -and $errors.Count -eq 0)
        process = [ordered]@{ exitCode = $result.exitCode; timedOut = $result.timedOut; durationMs = $result.durationMs; stdout = Get-RelativePath -Root $script:OutputRoot -Path $stdoutPath; stderr = Get-RelativePath -Root $script:OutputRoot -Path $stderrPath }
        eventCount = $events.Count
        productionState = $payload
    }
}

function Invoke-ToolsProbe {
    $stdoutPath = Join-Path $script:OutputRoot 'tools.stdout.json'
    $stderrPath = Join-Path $script:OutputRoot 'tools.stderr.log'
    $result = Invoke-HiddenProcess -FilePath $RuntimePath -Arguments @('tools', 'list', '--format', 'json') -WorkingDirectory (Split-Path -Parent $RuntimePath) -Timeout $TimeoutSeconds -StdoutPath $stdoutPath -StderrPath $stderrPath
    $manifest = Get-JsonFromText -Text $result.stdout
    if ($result.exitCode -ne 0 -or $null -eq $manifest) { Add-Issue -Code 'tools.manifest-unavailable' -Stage 'tools' -Message 'Runtime tools list did not return valid JSON.' -ExitCode 4 }
    $tools = @((Get-PropertyValue $manifest 'tools'))
    return [ordered]@{ pass = ($result.exitCode -eq 0 -and $null -ne $manifest); process = [ordered]@{ exitCode = $result.exitCode; timedOut = $result.timedOut; durationMs = $result.durationMs; stdout = Get-RelativePath -Root $script:OutputRoot -Path $stdoutPath; stderr = Get-RelativePath -Root $script:OutputRoot -Path $stderrPath }; protocolVersion = Get-PropertyValue $manifest 'protocolVersion'; toolNames = @($tools | ForEach-Object { [string](Get-PropertyValue $_ 'name') }) }
}

function Invoke-ServeProbe {
    param([Parameter(Mandatory = $true)][string]$Root, [Parameter(Mandatory = $true)][string]$Label)
    $requests = @(
        ([ordered]@{ id = 'registry'; name = 'asset.registry'; arguments = [ordered]@{ refresh = $false } }),
        ([ordered]@{ id = 'sprites'; name = 'asset.query'; arguments = [ordered]@{ kind = 'Sprite'; limit = 256 } }),
        ([ordered]@{ id = 'tilemaps'; name = 'asset.query'; arguments = [ordered]@{ kind = 'Tilemap'; limit = 256 } }),
        ([ordered]@{ id = 'render'; name = 'render.observe'; arguments = [ordered]@{} }),
        ([ordered]@{ id = 'vfx'; name = 'vfx.observe'; arguments = [ordered]@{} }),
        ([ordered]@{ id = 'ui'; name = 'ui.project.observe'; arguments = [ordered]@{} })
    )
    $inputText = (($requests | ForEach-Object { $_ | ConvertTo-Json -Depth 20 -Compress }) -join "`n") + "`n"
    $stdoutPath = Join-Path $script:OutputRoot ($Label + '.stdout.jsonl')
    $stderrPath = Join-Path $script:OutputRoot ($Label + '.stderr.log')
    $result = Invoke-HiddenProcess -FilePath $RuntimePath -Arguments @('serve', '--project', $Root, '--format', 'jsonl') -WorkingDirectory $Root -Timeout $TimeoutSeconds -InputText $inputText -StdoutPath $stdoutPath -StderrPath $stderrPath
    $responses = @()
    foreach ($line in ($result.stdout -split "`r?`n" | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })) {
        try { $responses += ConvertFrom-Json -InputObject $line -Depth 100 } catch { }
    }
    if ($result.exitCode -ne 0 -or $result.timedOut) { Add-Issue -Code 'serve.probe-failed' -Stage $Label -Message "Hidden serve probe failed (exit=$($result.exitCode), timeout=$($result.timedOut))." -ExitCode 4 }
    foreach ($id in @('registry', 'sprites', 'tilemaps', 'render', 'vfx', 'ui')) {
        $response = @($responses | Where-Object { [string](Get-PropertyValue $_ 'id') -eq $id } | Select-Object -First 1)
        if ($response.Count -eq 0 -or (Get-PropertyValue (Get-PropertyValue $response[0] 'response') 'ok') -eq $false) { Add-Issue -Code "serve.$id-unavailable" -Stage $Label -Message "Serve did not return a successful '$id' response." }
    }
    return [ordered]@{
        pass = ($result.exitCode -eq 0 -and -not $result.timedOut -and $responses.Count -ge 6)
        process = [ordered]@{ exitCode = $result.exitCode; timedOut = $result.timedOut; durationMs = $result.durationMs; stdout = Get-RelativePath -Root $script:OutputRoot -Path $stdoutPath; stderr = Get-RelativePath -Root $script:OutputRoot -Path $stderrPath }
        responses = @($responses | ForEach-Object { [ordered]@{ id = Get-PropertyValue $_ 'id'; exitCode = Get-PropertyValue $_ 'exitCode'; response = Get-PropertyValue $_ 'response' } })
    }
}

function Get-NormalizedState {
    param([AllowNull()][object]$Production, [AllowNull()][object]$Manifest, [AllowNull()][object]$Static)
    $hybrid = Get-PropertyValue $Production 'hybridPixelProfile'
    if ($null -eq $hybrid) { $hybrid = Get-PropertyValue $Manifest 'hybridPixelProfile' }
    $input = Get-PropertyValue $Production 'input'
    $scripting = Get-PropertyValue $Production 'scripting'
    $ui = Get-PropertyValue $Production 'projectUi'
    $instances = @((Get-PropertyValue $scripting 'instances'))
    return [ordered]@{
        hybridPixel = if ($null -ne $hybrid) { [ordered]@{ schema = Get-PropertyValue $hybrid 'schema'; profileId = Get-PropertyValue $hybrid 'profileId'; enabled = Get-PropertyValue $hybrid 'enabled'; virtualWidth = Get-PropertyValue $hybrid 'virtualWidth'; virtualHeight = Get-PropertyValue $hybrid 'virtualHeight'; pixelsPerUnit = Get-PropertyValue $hybrid 'pixelsPerUnit'; integerScaling = Get-PropertyValue $hybrid 'integerScaling'; snapCamera = Get-PropertyValue $hybrid 'snapCamera'; snapSprites = Get-PropertyValue $hybrid 'snapSprites'; presentationFilter = Get-PropertyValue $hybrid 'presentationFilter' } } else { $null }
        inputActionIds = @((Get-PropertyValue $input 'actions') | ForEach-Object { [string](Get-PropertyValue $_ 'id') } | Sort-Object)
        ui = [ordered]@{ schemaVersion = Get-PropertyValue $ui 'schemaVersion'; documentId = Get-PropertyValue $ui 'documentId'; valid = Get-PropertyValue $ui 'valid'; nodeCount = @((Get-PropertyValue $ui 'nodes')).Count }
        scripting = [ordered]@{ ready = Get-PropertyValue $scripting 'ready'; status = Get-PropertyValue $scripting 'status'; instanceCount = $instances.Count; activeInstanceCount = @($instances | Where-Object { [string](Get-PropertyValue $_ 'state') -eq 'active' }).Count; projectCodeExecuted = Get-PropertyValue (Get-PropertyValue $scripting 'lastManagedResult') 'projectCodeExecuted' }
        staticFeatures = if ($null -ne $Static) { Get-PropertyValue $Static 'features' } else { $null }
    }
}

function Compare-NormalizedState {
    param([Parameter(Mandatory = $true)]$Source, [Parameter(Mandatory = $true)]$Package)
    $comparisons = [System.Collections.Generic.List[object]]::new()
    foreach ($field in @('hybridPixel', 'inputActionIds', 'ui', 'scripting')) {
        $sourceValue = Get-PropertyValue $Source $field
        $packageValue = Get-PropertyValue $Package $field
        if ($null -eq $sourceValue -or $null -eq $packageValue) {
            [void]$comparisons.Add([ordered]@{ field = $field; comparable = $false; equal = $false; reason = 'missing-on-one-side' })
            Add-Issue -Code 'package.state-field-missing' -Stage 'source-package-compare' -Message "Normalized state field '$field' is missing on one side of the source/package comparison."
            continue
        }
        $sourceJson = $sourceValue | ConvertTo-Json -Depth 40 -Compress
        $packageJson = $packageValue | ConvertTo-Json -Depth 40 -Compress
        $equal = $sourceJson -eq $packageJson
        [void]$comparisons.Add([ordered]@{ field = $field; comparable = $true; equal = $equal })
        if (-not $equal) { Add-Issue -Code 'package.state-drift' -Stage 'source-package-compare' -Message "Normalized state field '$field' differs between source and packaged Player." }
    }
    return [ordered]@{ pass = ($comparisons.Count -gt 0 -and @($comparisons | Where-Object { -not $_.equal }).Count -eq 0); comparisons = @($comparisons) }
}

function Get-BmpEvidence {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][int]$ExpectedWidth,
        [Parameter(Mandatory = $true)][int]$ExpectedHeight,
        [Parameter(Mandatory = $true)][string]$Stage
    )
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        Add-Issue -Code 'visual.capture-missing' -Stage $Stage -Message "Hidden visual probe did not produce a BMP: $Path" -ExitCode 4
        return [ordered]@{ pass = $false; path = $null; bytes = 0; sha256 = $null; width = 0; height = 0 }
    }
    try {
        $bytes = [IO.File]::ReadAllBytes($Path)
        if ($bytes.Length -lt 54 -or $bytes[0] -ne 0x42 -or $bytes[1] -ne 0x4D) { throw 'BMP header is missing.' }
        $width = [BitConverter]::ToInt32($bytes, 18)
        $height = [Math]::Abs([BitConverter]::ToInt32($bytes, 22))
        if ($width -ne $ExpectedWidth -or $height -ne $ExpectedHeight) { throw "Expected ${ExpectedWidth}x${ExpectedHeight}, got ${width}x${height}." }
        return [ordered]@{ pass = $true; path = Get-RelativePath -Root $script:OutputRoot -Path $Path; bytes = $bytes.Length; sha256 = Get-Sha256 -Path $Path; width = $width; height = $height }
    } catch {
        Add-Issue -Code 'visual.capture-invalid' -Stage $Stage -Message $_.Exception.Message -ExitCode 4
        return [ordered]@{ pass = $false; path = Get-RelativePath -Root $script:OutputRoot -Path $Path; bytes = (Get-Item -LiteralPath $Path).Length; sha256 = Get-Sha256 -Path $Path; width = 0; height = 0 }
    }
}

function Invoke-HiddenVisualProbe {
    param(
        [Parameter(Mandatory = $true)][string]$Label,
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string[]]$PrefixArguments,
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [ValidateRange(3, 240)][int]$Frames = 3,
        [ValidateRange(1, 240)][int]$VfxRespawnInterval = 1
    )
    $stage = $Label
    $width = 960
    $height = 540
    $imagePath = Join-Path $script:OutputRoot ($stage + '.bmp')
    $stdoutPath = Join-Path $script:OutputRoot ($stage + '.stdout.jsonl')
    $stderrPath = Join-Path $script:OutputRoot ($stage + '.stderr.log')
    $before = $script:Issues.Count
    $arguments = @($PrefixArguments) + @(
        '--format', 'json', '--gpu-backend', 'direct3d12',
        '--capture-frame', $imagePath, '--frames', [string]$Frames,
        '--vfx-respawn-interval', [string]$VfxRespawnInterval,
        '--window-width', [string]$width, '--window-height', [string]$height
    )
    $process = Invoke-HiddenProcess -FilePath $Executable -Arguments $arguments `
        -WorkingDirectory $WorkingDirectory -Timeout $TimeoutSeconds -StdoutPath $stdoutPath -StderrPath $stderrPath
    $events = Get-LogEvents -Text $process.stdout
    $finalEvent = Get-LastEvent -Events $events -Name 'render.scene.final'
    if ($null -eq $finalEvent) { $finalEvent = Get-LastEvent -Events $events -Name 'render.scene' }
    $status = if ($null -ne $finalEvent) { $finalEvent.payload } else { $null }
    if ($process.timedOut) { Add-Issue -Code 'visual.process-timeout' -Stage $stage -Message 'Hidden packaged Player visual probe exceeded its timeout.' -ExitCode 4 }
    if ($process.exitCode -ne 0) { Add-Issue -Code 'visual.process-failed' -Stage $stage -Message "Hidden packaged Player visual probe exited with code $($process.exitCode)." -ExitCode 4 }
    if ($null -eq $status) { Add-Issue -Code 'visual.renderer-status-missing' -Stage $stage -Message 'Visual probe did not publish render.scene.final or render.scene.' -ExitCode 4 }
    $checks = [System.Collections.Generic.List[object]]::new()
    $addCheck = {
        param([string]$Field, [AllowNull()][object]$Actual, [bool]$Pass)
        [void]$checks.Add([ordered]@{ field = $Field; actual = $Actual; pass = $Pass })
        if (-not $Pass) { Add-Issue -Code 'visual.renderer-contract-failed' -Stage $stage -Message "Renderer contract '$Field' was not satisfied." }
    }
    $hybrid = Get-PropertyValue $status 'hybridPixel'
    $profile = Get-PropertyValue $hybrid 'profile'
    $projection = Get-PropertyValue $hybrid 'projection'
    $vfxPolicy = Get-PropertyValue $hybrid 'vfx'
    $sprites = Get-PropertyValue $status 'sprites'
    $tilemaps = Get-PropertyValue $status 'tilemaps'
    $vfxGpu = Get-PropertyValue $status 'vfxGpu'
    $submission = Get-PropertyValue $status 'submission'
    & $addCheck 'schemaVersion' (Get-PropertyValue $status 'schemaVersion') ([string](Get-PropertyValue $status 'schemaVersion') -eq 'noemancer.renderer-status.v26')
    & $addCheck 'hybridPixel.active' (Get-PropertyValue $hybrid 'active') ((Get-PropertyValue $hybrid 'active') -eq $true)
    & $addCheck 'hybridPixel.profile.profileId' (Get-PropertyValue $profile 'profileId') ([string](Get-PropertyValue $profile 'profileId') -eq [string](Get-PropertyValue (Get-PropertyValue $script:StaticState 'manifest') 'hybridPixelProfile').profileId)
    & $addCheck 'hybridPixel.projection.valid' (Get-PropertyValue $projection 'valid') ((Get-PropertyValue $projection 'valid') -eq $true)
    & $addCheck 'hybridPixel.vfx.sameGpuLifecycle' (Get-PropertyValue $vfxPolicy 'sameGpuLifecycle') ((Get-PropertyValue $vfxPolicy 'sameGpuLifecycle') -eq $true)
    & $addCheck 'sprites.pipelineCreated' (Get-PropertyValue $sprites 'pipelineCreated') ((Get-PropertyValue $sprites 'pipelineCreated') -eq $true)
    & $addCheck 'sprites.instancesSubmitted' (Get-PropertyValue $sprites 'instancesSubmitted') ([int](Get-PropertyValue $sprites 'instancesSubmitted') -gt 0)
    & $addCheck 'tilemaps.count' (Get-PropertyValue $tilemaps 'count') ([int](Get-PropertyValue $tilemaps 'count') -gt 0)
    & $addCheck 'vfxGpu.outputConsumedByDraw' (Get-PropertyValue $vfxGpu 'outputConsumedByDraw') ((Get-PropertyValue $vfxGpu 'outputConsumedByDraw') -eq $true)
    & $addCheck 'submission.opaqueInstances' (Get-PropertyValue $submission 'opaqueInstances') ([int](Get-PropertyValue $submission 'opaqueInstances') -gt 0)
    $image = Get-BmpEvidence -Path $imagePath -ExpectedWidth $width -ExpectedHeight $height -Stage $stage
    return [ordered]@{
        pass = ($process.exitCode -eq 0 -and -not $process.timedOut -and $null -ne $status -and $image.pass -and $script:Issues.Count -eq $before)
        backend = 'direct3d12'
        frames = $Frames
        dimensions = [ordered]@{ width = $width; height = $height }
        process = [ordered]@{ exitCode = $process.exitCode; timedOut = $process.timedOut; durationMs = $process.durationMs; stdout = Get-RelativePath -Root $script:OutputRoot -Path $stdoutPath; stderr = Get-RelativePath -Root $script:OutputRoot -Path $stderrPath }
        image = $image
        eventCount = $events.Count
        checks = @($checks)
        renderer = if ($null -ne $status) { [ordered]@{ schemaVersion = Get-PropertyValue $status 'schemaVersion'; deviceBackend = Get-PropertyValue (Get-PropertyValue $status 'device') 'backend'; hybridPixel = [ordered]@{ active = Get-PropertyValue $hybrid 'active'; profileId = Get-PropertyValue $profile 'profileId'; projectionValid = Get-PropertyValue $projection 'valid'; spritesSnapped = Get-PropertyValue $projection 'spritesSnapped'; tileCellsSnapped = Get-PropertyValue $projection 'tileCellsSnapped' }; sprites = [ordered]@{ instancesSubmitted = Get-PropertyValue $sprites 'instancesSubmitted'; litInstances = Get-PropertyValue $sprites 'litInstances'; pipelineCreated = Get-PropertyValue $sprites 'pipelineCreated' }; tilemaps = [ordered]@{ count = Get-PropertyValue $tilemaps 'count'; cellInstancesSubmitted = Get-PropertyValue $tilemaps 'cellInstancesSubmitted' }; vfxGpu = [ordered]@{ pipelineCreated = Get-PropertyValue $vfxGpu 'pipelineCreated'; outputConsumedByDraw = Get-PropertyValue $vfxGpu 'outputConsumedByDraw'; abi = Get-PropertyValue $vfxGpu 'abi' }; submission = [ordered]@{ opaqueInstances = Get-PropertyValue $submission 'opaqueInstances'; vfxParticles = Get-PropertyValue $submission 'vfxParticles' }; renderWorld = Get-PropertyValue $status 'renderWorld' } } else { $null }
    }
}

function Invoke-HiddenPerformanceProbe {
    param(
        [Parameter(Mandatory = $true)][string]$PlayerPath,
        [Parameter(Mandatory = $true)][string]$ProfilePath,
        [Parameter(Mandatory = $true)][string]$PackageRoot
    )
    $stage = 'package-player-performance'
    $evidencePath = Join-Path $script:OutputRoot ($stage + '.json')
    $stdoutPath = Join-Path $script:OutputRoot ($stage + '.stdout.jsonl')
    $stderrPath = Join-Path $script:OutputRoot ($stage + '.stderr.log')
    $process = Invoke-HiddenProcess -FilePath $PlayerPath -Arguments @(
        'player', '--profile', $ProfilePath, '--gpu-backend', 'direct3d12',
        '--performance-evidence', $evidencePath, '--performance-hidden',
        '--performance-workload', 'noemancer.hd2d-small-slice/0.1',
        '--performance-warmup-frames', '30', '--performance-sample-frames', '60',
        '--window-width', '960', '--window-height', '540', '--format', 'json'
    ) -WorkingDirectory $PackageRoot -Timeout $TimeoutSeconds -StdoutPath $stdoutPath -StderrPath $stderrPath
    $evidence = if (Test-Path -LiteralPath $evidencePath -PathType Leaf) { Read-JsonFile -Path $evidencePath -Stage $stage } else { $null }
    $frame = Get-PropertyValue (Get-PropertyValue $evidence 'cpu') 'frameTime'
    $sampleCount = [int](Get-PropertyValue $frame 'sampleCount')
    $p95 = [double](Get-PropertyValue $frame 'p95')
    $validP95 = [double]::IsFinite($p95) -and $p95 -gt 0.0
    if ($process.timedOut -or $process.exitCode -ne 0 -or $null -eq $evidence) { Add-Issue -Code 'performance.process-failed' -Stage $stage -Message "Hidden packaged Player performance probe failed (exit=$($process.exitCode), timeout=$($process.timedOut))." -ExitCode 4 }
    if ($null -ne $evidence -and [string](Get-PropertyValue $evidence 'schemaVersion') -ne 'noemancer.performance-evidence/0.1') { Add-Issue -Code 'performance.schema-invalid' -Stage $stage -Message 'Performance evidence schema is not noemancer.performance-evidence/0.1.' }
    if ($sampleCount -ne 60) { Add-Issue -Code 'performance.sample-count-invalid' -Stage $stage -Message "Expected 60 measured frames, got $sampleCount." }
    if (-not $validP95 -or $p95 -gt 20.0) { Add-Issue -Code 'performance.frame-budget-failed' -Stage $stage -Message "Measured CPU frame p95 '$p95' ms is outside the (0, 20] ms probe budget." }
    return [ordered]@{
        pass = ($process.exitCode -eq 0 -and -not $process.timedOut -and $null -ne $evidence -and $sampleCount -eq 60 -and $validP95 -and $p95 -le 20.0)
        backend = 'direct3d12'
        workload = 'noemancer.hd2d-small-slice/0.1'
        budgetMilliseconds = 20.0
        frameTime = [ordered]@{ sampleCount = $sampleCount; p95 = $p95; mean = [double](Get-PropertyValue $frame 'mean'); max = [double](Get-PropertyValue $frame 'max') }
        evidence = [ordered]@{ path = Get-RelativePath -Root $script:OutputRoot -Path $evidencePath; bytes = if (Test-Path -LiteralPath $evidencePath) { (Get-Item -LiteralPath $evidencePath).Length } else { 0 }; sha256 = if (Test-Path -LiteralPath $evidencePath) { Get-Sha256 -Path $evidencePath } else { $null } }
        process = [ordered]@{ exitCode = $process.exitCode; timedOut = $process.timedOut; durationMs = $process.durationMs; stdout = Get-RelativePath -Root $script:OutputRoot -Path $stdoutPath; stderr = Get-RelativePath -Root $script:OutputRoot -Path $stderrPath }
    }
}

function Invoke-PackageProbe {
    param([Parameter(Mandatory = $true)][string]$Root)
    $packagePath = Join-Path ([IO.Path]::GetTempPath()) ('noemancer-hd2d-package-' + [IO.Path]::GetFileName($script:StageRoot))
    $script:PackageRoot = $packagePath
    if (Test-Path -LiteralPath $packagePath) { Remove-Item -LiteralPath $packagePath -Recurse -Force }
    $stdoutPath = Join-Path $script:OutputRoot 'package.stdout.json'
    $stderrPath = Join-Path $script:OutputRoot 'package.stderr.log'
    $result = Invoke-HiddenProcess -FilePath $RuntimePath -Arguments @('package', '--project', $Root, '--output', $packagePath, '--target-profile', $TargetProfile, '--format', 'json') -WorkingDirectory $Root -Timeout $TimeoutSeconds -StdoutPath $stdoutPath -StderrPath $stderrPath
    $envelope = Get-JsonFromText -Text $result.stdout
    if ($result.exitCode -ne 0 -or $result.timedOut -or $null -eq $envelope -or (Get-PropertyValue $envelope 'success') -ne $true) {
        Add-Issue -Code 'package.build-failed' -Stage 'package' -Message "Package command did not complete successfully for $TargetProfile (exit=$($result.exitCode), timeout=$($result.timedOut))." -ExitCode 4
    }
    $receipt = Get-PropertyValue $envelope 'receipt'
    if ($null -ne $receipt) {
        if ((Get-PropertyValue $receipt 'committed') -ne $true -or (Get-PropertyValue $receipt 'atomic') -ne $true) { Add-Issue -Code 'package.not-atomic' -Stage 'package' -Message 'Package receipt did not prove committed=true and atomic=true.' }
    }
    $profilePath = Join-Path $packagePath 'config\game-profile.json'
    $profile = if (Test-Path -LiteralPath $profilePath -PathType Leaf) { Read-JsonFile -Path $profilePath -Stage 'package-profile' } else { $null }
    $playerPath = $null
    if ($null -ne $profile) {
        $executable = [string](Get-PropertyValue $profile 'executable')
        if ([string]::IsNullOrWhiteSpace($executable) -or [IO.Path]::IsPathRooted($executable) -or $executable -match '(^|[\\/])\.\.(?:[\\/]|$)') { Add-Issue -Code 'package.player-path-invalid' -Stage 'package-profile' -Message 'Packaged executable is not a package-relative path.' }
        else {
            $playerPath = Join-Path $packagePath ('bin\' + $executable)
            if (-not (Test-Path -LiteralPath $playerPath -PathType Leaf)) { Add-Issue -Code 'package.player-missing' -Stage 'package-profile' -Message "Packaged Player executable is missing: $executable" }
        }
    }
    $registryPath = Join-Path $packagePath 'content\assets\registry.json'
    $packageRegistry = if (Test-Path -LiteralPath $registryPath -PathType Leaf) { Read-JsonFile -Path $registryPath -Stage 'package-registry' } else { $null }
    if ($null -eq $packageRegistry) { Add-Issue -Code 'package.registry-missing' -Stage 'package' -Message 'Packaged content Asset Registry is missing.' }
    $pathLeaks = [System.Collections.Generic.List[object]]::new()
    if (Test-Path -LiteralPath $packagePath -PathType Container) {
        foreach ($file in (Get-ChildItem -LiteralPath $packagePath -Recurse -File -Force | Where-Object { $_.Extension.ToLowerInvariant() -in @('.json', '.txt', '.cs', '.csproj', '.scene', '.ui') })) {
            try {
                $match = Select-String -LiteralPath $file.FullName -Pattern '(?<![A-Za-z])(?:[A-Za-z]:[\\/]|\\\\)' -CaseSensitive:$false | Select-Object -First 1
                if ($null -ne $match) { [void]$pathLeaks.Add([ordered]@{ path = Get-RelativePath -Root $packagePath -Path $file.FullName; line = $match.LineNumber }) }
            } catch { }
        }
    }
    if ($pathLeaks.Count -gt 0) { Add-Issue -Code 'package.absolute-path-leak' -Stage 'package' -Message 'Package text files contain absolute Windows paths.' }
    $playerRun = $null
    if ($null -ne $playerPath -and (Test-Path -LiteralPath $playerPath -PathType Leaf)) {
        $playerStdout = Join-Path $script:OutputRoot 'package-player.stdout.jsonl'
        $playerStderr = Join-Path $script:OutputRoot 'package-player.stderr.log'
        $playerProcess = Invoke-HiddenProcess -FilePath $playerPath -Arguments @('player', '--profile', $profilePath, '--headless', '--frames', '3', '--format', 'json') -WorkingDirectory $packagePath -Timeout $TimeoutSeconds -StdoutPath $playerStdout -StderrPath $playerStderr
        $events = Get-LogEvents -Text $playerProcess.stdout
        $production = Get-LastEvent -Events $events -Name 'runtime.production_state'
        if ($playerProcess.exitCode -ne 0 -or $playerProcess.timedOut -or $null -eq $production) { Add-Issue -Code 'package.player-run-failed' -Stage 'package-player' -Message "Packaged Player hidden run failed (exit=$($playerProcess.exitCode), timeout=$($playerProcess.timedOut))." -ExitCode 4 }
        $playerRun = [ordered]@{ pass = ($playerProcess.exitCode -eq 0 -and -not $playerProcess.timedOut -and $null -ne $production); process = [ordered]@{ exitCode = $playerProcess.exitCode; timedOut = $playerProcess.timedOut; durationMs = $playerProcess.durationMs; stdout = Get-RelativePath -Root $script:OutputRoot -Path $playerStdout; stderr = Get-RelativePath -Root $script:OutputRoot -Path $playerStderr }; productionState = if ($null -ne $production) { $production.payload } else { $null } }
    }
    $profileHybrid = Get-PropertyValue $profile 'hybridPixelProfile'
    if ($null -eq $profileHybrid) { Add-Issue -Code 'package.hybrid-pixel-profile-missing' -Stage 'package-profile' -Message 'Packaged game profile omitted hybridPixelProfile.' }
    $visual = $null
    $performance = $null
    if ($null -ne $playerPath -and (Test-Path -LiteralPath $playerPath -PathType Leaf)) {
        $visual = Invoke-HiddenVisualProbe -Label 'package-player-visual' -Executable $playerPath `
            -PrefixArguments @('player', '--profile', $profilePath) -WorkingDirectory $packagePath
        $performance = Invoke-HiddenPerformanceProbe -PlayerPath $playerPath -ProfilePath $profilePath -PackageRoot $packagePath
    }
    $receiptSummary = $null
    if ($null -ne $receipt) {
        $receiptSummary = [ordered]@{
            schema = Get-PropertyValue $receipt 'schema'
            success = Get-PropertyValue $receipt 'success'
            committed = Get-PropertyValue $receipt 'committed'
            atomic = Get-PropertyValue $receipt 'atomic'
            contentHash = Get-PropertyValue $receipt 'contentHash'
            entryCount = @((Get-PropertyValue $receipt 'entries')).Count
        }
    }
    return [ordered]@{
        pass = ($result.exitCode -eq 0 -and -not $result.timedOut -and $null -ne $envelope -and (Get-PropertyValue $envelope 'success') -eq $true -and $null -ne $playerRun -and [bool]$playerRun.pass -and $null -ne $visual -and [bool]$visual.pass -and $null -ne $performance -and [bool]$performance.pass)
        process = [ordered]@{ exitCode = $result.exitCode; timedOut = $result.timedOut; durationMs = $result.durationMs; stdout = Get-RelativePath -Root $script:OutputRoot -Path $stdoutPath; stderr = Get-RelativePath -Root $script:OutputRoot -Path $stderrPath }
        envelope = if ($null -ne $envelope) { [ordered]@{ schema = Get-PropertyValue $envelope 'schema'; success = Get-PropertyValue $envelope 'success'; code = Get-PropertyValue $envelope 'code'; receipt = $receiptSummary } } else { $null }
        profile = if ($null -ne $profile) { [ordered]@{ schema = Get-PropertyValue $profile 'schema'; targetProfile = Get-PropertyValue $profile 'targetProfile'; executable = Get-PropertyValue $profile 'executable'; startupScene = Get-PropertyValue $profile 'startupScene'; hybridPixelProfile = $profileHybrid; inputActions = Get-PropertyValue $profile 'inputActions' } } else { $null }
        profilePath = $profilePath
        packageRoot = $packagePath
        playerPath = $playerPath
        pathLeaks = @($pathLeaks)
        registry = if ($null -ne $packageRegistry) { [ordered]@{ schema = Get-PropertyValue $packageRegistry 'schema'; assetCount = @((Get-PropertyValue $packageRegistry 'assets')).Count } } else { $null }
        playerRun = $playerRun
        visual = $visual
        performance = $performance
    }
}

function Write-Receipt {
    param([Parameter(Mandatory = $true)][string]$Status)
    $receipt = [ordered]@{
        schema = 'noemancer.hd2d-small-slice-evidence/0.1'
        success = ($Status -eq 'passed' -and $script:ExitCode -eq 0 -and $script:Issues.Count -eq 0)
        status = $Status
        exitCode = $script:ExitCode
        project = $Project
        targetProfile = $TargetProfile
        runtime = [ordered]@{ path = $RuntimePath; exists = (Test-Path -LiteralPath $RuntimePath -PathType Leaf) }
        sourceTree = [ordered]@{ before = $script:SourceTreeBefore; after = $script:SourceTreeAfter; unchanged = ($null -ne $script:SourceTreeBefore -and $null -ne $script:SourceTreeAfter -and $script:SourceTreeBefore.sha256 -eq $script:SourceTreeAfter.sha256) }
        staging = [ordered]@{ path = $script:StageRoot; kept = $script:StageKept }
        static = $script:StaticState
        tools = $script:Tools
        sourceRuntime = $script:SourceRun
        sourceServe = $script:SourceServe
        sourceVisual = $script:SourceVisual
        package = $script:Package
        packageRuntime = $script:PackageRun
        stateComparison = $script:StateComparison
        issueCount = $script:Issues.Count
        issues = @($script:Issues)
    }
    Write-Utf8Json -Path $OutputPath -Value $receipt
    $qualityPath = [IO.Path]::ChangeExtension($OutputPath, '.quality.json')
    Write-Utf8Json -Path $qualityPath -Value ([ordered]@{
        schema = 'noemancer.hd2d-small-slice-quality/0.1'
        success = $receipt.success
        status = $Status
        issueCount = $script:Issues.Count
        sourceUnchanged = $receipt.sourceTree.unchanged
        hiddenProcessPolicy = [ordered]@{ createNoWindow = $true; windowStyle = 'Hidden'; timeoutSeconds = $TimeoutSeconds; computerUse = $false }
        contractSummary = [ordered]@{
            nativeCpp = if ($null -ne $script:StaticState) { @($script:StaticState.nativeCpp).Count -eq 0 } else { $false }
            hybridPixel = if ($null -ne $script:StaticState) { $null -ne (Get-PropertyValue $script:StaticState.manifest 'hybridPixelProfile') } else { $false }
            spriteTilemap = if ($null -ne $script:StaticState) { [int]$script:StaticState.registry.kindCounts.sprite -gt 0 -and [int]$script:StaticState.registry.kindCounts.tilemap -gt 0 } else { $false }
            mixed2D3D = if ($null -ne $script:StaticState) { [int]$script:StaticState.features.meshRenderer -gt 0 -and [int]$script:StaticState.features.camera -gt 0 } else { $false }
            vfx = if ($null -ne $script:StaticState) { [int]$script:StaticState.features.vfx -gt 0 -or [int]$script:StaticState.registry.kindCounts.vfxGraph -gt 0 } else { $false }
            csharpExecution = if ($null -ne $script:SourceRun) { [bool]$script:SourceRun.pass } else { $false }
            sourceD3D12 = if ($null -ne $script:SourceVisual) { [bool]$script:SourceVisual.pass } else { $false }
            packageAndPlayer = if ($null -ne $script:Package) { [bool]$script:Package.pass } else { $false }
        }
    })
    return $receipt
}

$status = 'failed'
$resolvedSource = $null
try {
    $script:OutputRoot = [IO.Path]::GetFullPath((Split-Path -Parent $OutputPath))
    New-Item -ItemType Directory -Path $script:OutputRoot -Force | Out-Null
    $OutputPath = [IO.Path]::GetFullPath($OutputPath)
    $RuntimePath = [IO.Path]::GetFullPath($RuntimePath)
    if (-not (Test-Path -LiteralPath $RuntimePath -PathType Leaf)) { Add-Issue -Code 'runtime.missing' -Stage 'input' -Message "Runtime executable is missing: $RuntimePath" -ExitCode 2 }
    try { $resolvedSource = [IO.Path]::GetFullPath((Resolve-Path -LiteralPath $Project -ErrorAction Stop).Path) } catch { }
    if ($null -eq $resolvedSource -or -not (Test-Path -LiteralPath $resolvedSource -PathType Container)) {
        Add-Issue -Code 'input.project-missing' -Stage 'input' -Message "HD2D small-slice project is not present: $Project" -ExitCode 2
    } else {
        $script:ProjectRoot = $resolvedSource
        $script:SourceTreeBefore = Get-TreeFingerprint -Root $resolvedSource
        $script:StageRoot = Join-Path ([IO.Path]::GetTempPath()) ('noemancer-hd2d-small-slice-' + [Guid]::NewGuid().ToString('N'))
        $stagedProject = Join-Path $script:StageRoot 'project'
        New-Item -ItemType Directory -Path $script:StageRoot -Force | Out-Null
        Copy-ProjectSnapshot -Source $resolvedSource -Destination $stagedProject
        $script:StaticState = Test-StaticProject -Root $stagedProject
        if (Test-Path -LiteralPath $RuntimePath -PathType Leaf) {
            $script:Tools = Invoke-ToolsProbe
        }
        if ($script:Issues.Count -eq 0) {
            $script:SourceRun = Invoke-SourceRun -Root $stagedProject -Label 'source-runtime'
            $script:SourceServe = Invoke-ServeProbe -Root $stagedProject -Label 'source-serve'
        }
        if ($script:Issues.Count -eq 0) {
            $script:SourceVisual = Invoke-HiddenVisualProbe -Label 'source-project-visual' `
                -Executable $RuntimePath -PrefixArguments @('run', '--project', $stagedProject) `
                -WorkingDirectory $stagedProject -Frames 64 -VfxRespawnInterval 60
        }
        if ($script:Issues.Count -eq 0) {
            $script:Package = Invoke-PackageProbe -Root $stagedProject
            if ($null -ne $script:Package.playerRun) { $script:PackageRun = $script:Package.playerRun }
        }
        if ($null -ne $script:SourceRun -and $null -ne $script:PackageRun) {
            $manifest = Read-JsonFile -Path (Join-Path $stagedProject 'noemancer.project.json') -Stage 'source-package-compare'
            $script:StateComparison = Compare-NormalizedState -Source (Get-NormalizedState -Production $script:SourceRun.productionState -Manifest $manifest -Static $script:StaticState) -Package (Get-NormalizedState -Production $script:PackageRun.productionState -Manifest $script:Package.profile -Static $null)
        }
    }
    if ($null -ne $script:ProjectRoot) { $script:SourceTreeAfter = Get-TreeFingerprint -Root $script:ProjectRoot }
    if ($null -ne $script:SourceTreeBefore -and $null -ne $script:SourceTreeAfter -and $script:SourceTreeBefore.sha256 -ne $script:SourceTreeAfter.sha256) {
        Add-Issue -Code 'source.tree-modified' -Stage 'source-boundary' -Message 'Source project tree changed during acceptance.' -ExitCode 6
    }
    if ($script:Issues.Count -eq 0) { $status = 'passed' }
}
catch {
    Add-Issue -Code 'script.unexpected-error' -Stage 'script' -Message $_.Exception.ToString() -ExitCode 7
}
finally {
    if ($null -ne $script:StageRoot -and (Test-Path -LiteralPath $script:StageRoot -PathType Container)) {
        if ($KeepStaging) { $script:StageKept = $true }
        else {
            try { Remove-Item -LiteralPath $script:StageRoot -Recurse -Force -ErrorAction Stop } catch { Add-Issue -Code 'cleanup.failed' -Stage 'cleanup' -Message $_.Exception.Message -ExitCode 7 }
        }
    }
    if (-not $KeepStaging -and $null -ne $script:PackageRoot -and (Test-Path -LiteralPath $script:PackageRoot -PathType Container)) {
        try { Remove-Item -LiteralPath $script:PackageRoot -Recurse -Force -ErrorAction Stop } catch { Add-Issue -Code 'cleanup.package-failed' -Stage 'cleanup' -Message $_.Exception.Message -ExitCode 7 }
    }
    try { [void](Write-Receipt -Status $status) } catch { [Console]::Error.WriteLine("Could not write HD2D evidence receipt: $($_.Exception.Message)"); if ($script:ExitCode -eq 0) { $script:ExitCode = 7 } }
}
exit $script:ExitCode
