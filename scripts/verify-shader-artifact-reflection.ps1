#requires -Version 7.0

[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(Mandatory = $true)]
    [string]$ManifestPath,

    [Parameter(Mandatory = $true)]
    [string]$CompilerPath,

    [string]$OutputPath,

    [switch]$Quiet
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# This verifier intentionally has no dependency on SPIRV-Tools.  The pinned DXC
# used by the repository can reflect DXIL with -dumpbin, but rejects SPIR-V as
# bitcode.  SPIR-V reflection below therefore parses the binary module itself.
$script:MaxManifestBytes = 16MB
$script:MaxShaderBytes = 64MB
$script:MaxJsonDepth = 100
$script:MaxArtifacts = 1024
$script:MaxResources = 4096
$script:MaxInstructions = 1000000
$script:MaxCompilerOutputChars = 16MB
$script:MaxCompilerProbeMs = 60000
$script:Issues = [System.Collections.Generic.List[object]]::new()
$script:CompilerVersion = $null
$script:CompilerSpirvDumpbin = $null
$script:ManifestFullPath = $null

function Add-Issue {
    param(
        [Parameter(Mandatory = $true)][ValidateSet('error', 'warning')][string]$Severity,
        [Parameter(Mandatory = $true)][string]$Code,
        [Parameter(Mandatory = $true)][string]$Message,
        [string]$Artifact,
        [string]$Backend
    )

    $entry = [ordered]@{
        severity = $Severity
        code = $Code
        message = $Message
    }
    if ($Artifact) { $entry.artifact = $Artifact }
    if ($Backend) { $entry.backend = $Backend }
    [void]$script:Issues.Add([pscustomobject]$entry)
}

function Get-FieldValue {
    param(
        [AllowNull()][object]$Object,
        [Parameter(Mandatory = $true)][string[]]$Names
    )

    if ($null -eq $Object) { return $null }
    foreach ($name in $Names) {
        $property = $Object.PSObject.Properties[$name]
        if ($null -ne $property) { return $property.Value }
    }
    return $null
}

function Test-FieldPresent {
    param(
        [AllowNull()][object]$Object,
        [Parameter(Mandatory = $true)][string[]]$Names
    )

    if ($null -eq $Object) { return $false }
    foreach ($name in $Names) {
        if ($null -ne $Object.PSObject.Properties[$name]) { return $true }
    }
    return $false
}

function Get-RequiredText {
    param(
        [AllowNull()][object]$Object,
        [Parameter(Mandatory = $true)][string[]]$Names,
        [Parameter(Mandatory = $true)][string]$Context
    )

    $value = Get-FieldValue -Object $Object -Names $Names
    if ($null -eq $value -or [string]::IsNullOrWhiteSpace([string]$value)) {
        throw "$Context is missing a non-empty property ($($Names -join ', '))."
    }
    return ([string]$value).Trim()
}

function Convert-ToBoundedInteger {
    param(
        [AllowNull()][object]$Value,
        [Parameter(Mandatory = $true)][string]$Context,
        [int64]$Minimum = 0,
        [int64]$Maximum = [int64]::MaxValue
    )

    if ($null -eq $Value -or $Value -is [bool]) {
        throw "$Context must be an integer."
    }
    try {
        $number = [decimal]$Value
    } catch {
        throw "$Context must be an integer."
    }
    if ($number -ne [decimal]::Truncate($number) -or
        $number -lt [decimal]$Minimum -or $number -gt [decimal]$Maximum) {
        throw "$Context is outside the allowed integer range [$Minimum, $Maximum]."
    }
    return [int64]$number
}

function Get-FileSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Resolve-ExistingFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Context,
        [int64]$MaximumBytes = [int64]::MaxValue
    )

    $fullPath = [System.IO.Path]::GetFullPath($Path)
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
        throw "$Context does not exist: $fullPath"
    }
    $file = Get-Item -LiteralPath $fullPath -Force
    if ($file.Length -gt $MaximumBytes) {
        throw "$Context is too large ($($file.Length) bytes; maximum is $MaximumBytes): $fullPath"
    }
    return $file
}

function Get-RelativeOrAbsolutePath {
    param(
        [Parameter(Mandatory = $true)][string]$ManifestDirectory,
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Context,
        [int64]$MaximumBytes = [int64]::MaxValue
    )

    $candidate = if ([System.IO.Path]::IsPathRooted($Path)) {
        $Path
    } else {
        Join-Path $ManifestDirectory $Path
    }
    return Resolve-ExistingFile -Path $candidate -Context $Context -MaximumBytes $MaximumBytes
}

function Resolve-ManifestSourceFile {
    param(
        [Parameter(Mandatory = $true)][string]$ManifestDirectory,
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Context
    )

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return Resolve-ExistingFile -Path $Path -Context $Context -MaximumBytes $script:MaxShaderBytes
    }
    $manifestCandidate = Join-Path $ManifestDirectory $Path
    if (Test-Path -LiteralPath $manifestCandidate -PathType Leaf) {
        return Resolve-ExistingFile -Path $manifestCandidate -Context $Context -MaximumBytes $script:MaxShaderBytes
    }
    # Generator 0.1 writes source identities relative to the repository root,
    # while DXIL/SPIR-V identities are relative to the artifact directory.
    $repositoryRoot = Split-Path -Parent $PSScriptRoot
    $repositoryCandidate = Join-Path $repositoryRoot $Path
    if (Test-Path -LiteralPath $repositoryCandidate -PathType Leaf) {
        return Resolve-ExistingFile -Path $repositoryCandidate -Context $Context -MaximumBytes $script:MaxShaderBytes
    }
    $shaderCandidate = Join-Path (Join-Path $repositoryRoot 'assets/shaders') $Path
    return Resolve-ExistingFile -Path $shaderCandidate -Context $Context -MaximumBytes $script:MaxShaderBytes
}

function Invoke-ProcessCapture {
    param(
        [Parameter(Mandatory = $true)][string]$Executable,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [int]$TimeoutMilliseconds = $script:MaxCompilerProbeMs
    )

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $Executable
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    foreach ($argument in $Arguments) {
        [void]$startInfo.ArgumentList.Add($argument)
    }

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    try {
        if (-not $process.Start()) { throw "Unable to start process '$Executable'." }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($TimeoutMilliseconds)) {
            try { $process.Kill($true) } catch { }
            throw "Process timed out after $TimeoutMilliseconds ms: $Executable"
        }
        $stdout = $stdoutTask.GetAwaiter().GetResult()
        $stderr = $stderrTask.GetAwaiter().GetResult()
        if ($stdout.Length -gt $script:MaxCompilerOutputChars -or
            $stderr.Length -gt $script:MaxCompilerOutputChars) {
            throw "Compiler output exceeded the bounded diagnostic limit."
        }
        return [pscustomobject]@{
            ExitCode = $process.ExitCode
            Stdout = $stdout
            Stderr = $stderr
        }
    } finally {
        $process.Dispose()
    }
}

function Normalize-Stage {
    param([Parameter(Mandatory = $true)][string]$Stage)

    switch -Regex ($Stage.Trim().ToLowerInvariant()) {
        '^(vs|vs_[0-9_]+|vertex|vertexshader)$' { return 'vertex' }
        '^(ps|ps_[0-9_]+|pixel|fragment|fs|fragmentshader)$' { return 'fragment' }
        '^(cs|cs_[0-9_]+|compute|computeshader)$' { return 'compute' }
        '^(gs|gs_[0-9_]+|geometry)$' { return 'geometry' }
        '^(hs|hs_[0-9_]+|hull|tessellationcontrol)$' { return 'hull' }
        '^(ds|ds_[0-9_]+|domain|tessellationevaluation)$' { return 'domain' }
        '^(ms|ms_[0-9_]+|mesh)$' { return 'mesh' }
        '^(as|as_[0-9_]+|amplification|task)$' { return 'task' }
        default { throw "Unsupported shader stage '$Stage'." }
    }
}

function Normalize-ResourceKind {
    param(
        [Parameter(Mandatory = $true)][string]$Kind,
        [string]$Format,
        [string]$Dimension
    )

    switch -Regex ($Kind.Trim().ToLowerInvariant()) {
        '^(cbuffer|constantbuffer|uniform)$' { return 'cbuffer' }
        '^(sampler|samplerstate|samplercomparisonstate)$' { return 'sampler' }
        '^(uav|rw|storage|storagebuffer|rwtexture|rwstructuredbuffer|rwbyteaddressbuffer)$' { return 'uav' }
        '^(texture|srv|structuredbuffer|byteaddressbuffer|buffer|sampledimage)$' { return 'texture' }
        default {
            throw "Unsupported reflected resource kind '$Kind' (format '$Format', dimension '$Dimension')."
        }
    }
}

function Convert-ResourceSignature {
    param(
        [Parameter(Mandatory = $true)][string]$Kind,
        [Parameter(Mandatory = $true)][int64]$Register,
        [Parameter(Mandatory = $true)][int64]$Space,
        [Parameter(Mandatory = $true)][int64]$Count,
        [string]$Category,
        [string]$Name
    )

    $normalizedKind = Normalize-ResourceKind -Kind $Kind
    $registerClass = switch ($normalizedKind) {
        'cbuffer' { 'b' }
        'sampler' { 's' }
        'texture' { 't' }
        'uav' { 'u' }
        default { throw "Cannot map resource kind '$normalizedKind' to an ABI register class." }
    }
    $result = [ordered]@{
        kind = $normalizedKind
        registerClass = $registerClass
        register = $Register
        space = $Space
        count = $Count
    }
    if ($Category) { $result.category = $Category }
    if ($Name) { $result.name = $Name }
    return [pscustomobject]$result
}

function Get-ManifestBackendSpec {
    param(
        [Parameter(Mandatory = $true)][object]$Artifact,
        [Parameter(Mandatory = $true)][string]$Backend
    )

    $names = if ($Backend -eq 'dxil') {
        @('dxil', 'DXIL', 'direct3d12', 'd3d12')
    } else {
        @('spirv', 'SPIR-V', 'spv', 'vulkan')
    }
    $value = Get-FieldValue -Object $Artifact -Names $names
    if ($null -eq $value) {
        $container = Get-FieldValue -Object $Artifact -Names @('backends', 'backendArtifacts', 'artifacts', 'outputs')
        $value = Get-FieldValue -Object $container -Names $names
    }
    if ($null -eq $value) {
        throw "Shader artifact is missing the $Backend artifact specification."
    }

    if ($value -is [string]) {
        return [pscustomobject]@{ path = [string]$value }
    }
    return $value
}

function Get-BackendArtifactPathAndHash {
    param(
        [Parameter(Mandatory = $true)][object]$Spec,
        [Parameter(Mandatory = $true)][string]$Backend,
        [Parameter(Mandatory = $true)][string]$ManifestDirectory,
        [Parameter(Mandatory = $true)][string]$ArtifactName
    )

    $pathValue = Get-FieldValue -Object $Spec -Names @('path', 'file', 'artifact', 'artifactPath', 'filename', 'uri')
    if ($pathValue -is [object] -and $pathValue -isnot [string]) {
        $pathValue = Get-FieldValue -Object $pathValue -Names @('path', 'file', 'value')
    }
    if ($null -eq $pathValue -or [string]::IsNullOrWhiteSpace([string]$pathValue)) {
        throw "$ArtifactName.$Backend is missing an artifact path."
    }
    $file = Get-RelativeOrAbsolutePath -ManifestDirectory $ManifestDirectory -Path ([string]$pathValue) -Context "$ArtifactName $Backend artifact" -MaximumBytes $script:MaxShaderBytes
    $hashValue = Get-FieldValue -Object $Spec -Names @('sha256', 'hash', 'sha256Hash', 'artifactSha256')
    if ($hashValue -is [object] -and $hashValue -isnot [string]) {
        $hashValue = Get-FieldValue -Object $hashValue -Names @('sha256', 'value')
    }
    if ($null -eq $hashValue) {
        throw "$ArtifactName.$Backend is missing the required SHA-256 hash."
    }
    $expectedHash = ([string]$hashValue).Trim().ToLowerInvariant()
    if ($expectedHash.StartsWith('sha256:')) { $expectedHash = $expectedHash.Substring(7) }
    if ($expectedHash -notmatch '^[0-9a-f]{64}$') {
        throw "$ArtifactName.$Backend SHA-256 must be exactly 64 lowercase/uppercase hexadecimal characters."
    }
    $declaredBytes = Get-FieldValue -Object $Spec -Names @('bytes', 'byteLength', 'size')
    if ($null -ne $declaredBytes) {
        $declaredBytes = Convert-ToBoundedInteger -Value $declaredBytes -Context "$ArtifactName.$Backend.bytes" -Minimum 1 -Maximum $script:MaxShaderBytes
        if ($declaredBytes -ne $file.Length) {
            throw "$ArtifactName.$Backend declares $declaredBytes bytes but the artifact is $($file.Length) bytes."
        }
    }
    return [pscustomobject]@{
        file = $file
        expectedHash = $expectedHash
    }
}

function Get-ExpectedReflection {
    param(
        [Parameter(Mandatory = $true)][object]$Artifact,
        [Parameter(Mandatory = $true)][object]$BackendSpec
    )

    $reflection = Get-FieldValue -Object $BackendSpec -Names @('reflection', 'abi', 'contract', 'expected')
    if ($null -eq $reflection) {
        $reflection = Get-FieldValue -Object $Artifact -Names @('reflection', 'abi', 'contract', 'expected')
    }
    if ($null -eq $reflection) {
        # The repository's 0.1 manifest records the ABI as category counts and
        # threadGroup beside each shader rather than repeating a binding list.
        $resourceCounts = Get-FieldValue -Object $Artifact -Names @('resources', 'resourceCounts')
        $threadGroup = Get-FieldValue -Object $Artifact -Names @('threadGroup', 'threadGroupSize')
        if ($null -ne $resourceCounts -or (Test-FieldPresent -Object $Artifact -Names @('threadGroup', 'threadGroupSize'))) {
            if ($resourceCounts -is [System.Collections.IEnumerable] -and $resourceCounts -isnot [string] -and $resourceCounts -isnot [pscustomobject]) {
                $reflection = [pscustomobject]@{ resourceBindings = @($resourceCounts); threadGroupSize = $threadGroup }
            } else {
                $reflection = [pscustomobject]@{ resourceCounts = $resourceCounts; threadGroupSize = $threadGroup }
            }
        }
    }
    return $reflection
}

function Get-ExpectedResources {
    param(
        [AllowNull()][object]$Reflection,
        [Parameter(Mandatory = $true)][string]$Context
    )

    if ($null -eq $Reflection) { return $null }
    $value = Get-FieldValue -Object $Reflection -Names @('resourceBindings', 'bindings')
    if ($null -eq $value) { return $null }
    $items = @($value)
    if ($items.Count -gt $script:MaxResources) { throw "$Context contains too many expected resource bindings." }
    return $items
}

function Get-ActualResourceCategoryCounts {
    param([Parameter(Mandatory = $true)][object]$Actual)

    $counts = [ordered]@{
        samplers = [int64]0
        uniformBuffers = [int64]0
        storageBuffers = [int64]0
        readonlyStorageBuffers = [int64]0
        readwriteStorageBuffers = [int64]0
    }
    foreach ($resource in @($Actual.resources)) {
        $quantity = [int64]$resource.count
        switch ([string]$resource.category) {
            'sampler' { $counts.samplers += $quantity }
            'uniformBuffer' { $counts.uniformBuffers += $quantity }
            'readonlyStorageBuffer' { $counts.readonlyStorageBuffers += $quantity }
            'readwriteStorageBuffer' { $counts.readwriteStorageBuffers += $quantity }
            'storageBuffer' { $counts.storageBuffers += $quantity }
            default { }
        }
    }
    return [pscustomobject]$counts
}

function Compare-ExpectedResourceCategoryCounts {
    param(
        [Parameter(Mandatory = $true)][object]$Actual,
        [AllowNull()][object]$Reflection,
        [Parameter(Mandatory = $true)][string]$Context,
        [Parameter(Mandatory = $true)][string]$Artifact,
        [Parameter(Mandatory = $true)][string]$Backend
    )

    if ($null -eq $Reflection) { return $false }
    $expectedCounts = Get-FieldValue -Object $Reflection -Names @('resourceCounts', 'resourceCategories')
    if ($null -eq $expectedCounts) {
        $resourceValue = Get-FieldValue -Object $Reflection -Names @('resources')
        if ($null -ne $resourceValue -and (Test-FieldPresent -Object $resourceValue -Names @('samplers', 'uniformBuffers', 'storageBuffers', 'readonlyStorageBuffers', 'readwriteStorageBuffers'))) {
            $expectedCounts = $resourceValue
        }
    }
    if ($null -eq $expectedCounts) { return $false }
    $actualCounts = Get-ActualResourceCategoryCounts -Actual $Actual
    $expectedStorage = if (Test-FieldPresent -Object $expectedCounts -Names @('storageBuffers')) {
        Convert-ToBoundedInteger -Value (Get-FieldValue -Object $expectedCounts -Names @('storageBuffers')) -Context "$Context.resources.storageBuffers" -Minimum 0 -Maximum $script:MaxResources
    } else { [int64]0 }
    $expectedReadonly = if (Test-FieldPresent -Object $expectedCounts -Names @('readonlyStorageBuffers')) {
        Convert-ToBoundedInteger -Value (Get-FieldValue -Object $expectedCounts -Names @('readonlyStorageBuffers')) -Context "$Context.resources.readonlyStorageBuffers" -Minimum 0 -Maximum $script:MaxResources
    } else { [int64]0 }
    $expectedReadwrite = if (Test-FieldPresent -Object $expectedCounts -Names @('readwriteStorageBuffers')) {
        Convert-ToBoundedInteger -Value (Get-FieldValue -Object $expectedCounts -Names @('readwriteStorageBuffers')) -Context "$Context.resources.readwriteStorageBuffers" -Minimum 0 -Maximum $script:MaxResources
    } else { [int64]0 }
    foreach ($field in @('samplers', 'uniformBuffers', 'storageBuffers', 'readonlyStorageBuffers', 'readwriteStorageBuffers')) {
        if (-not (Test-FieldPresent -Object $expectedCounts -Names @($field))) {
            Add-Issue -Severity error -Code 'manifest-resource-category-missing' -Message "$Context resource category '$field' is missing." -Artifact $Artifact -Backend $Backend
            continue
        }
        $expectedValue = Convert-ToBoundedInteger -Value (Get-FieldValue -Object $expectedCounts -Names @($field)) -Context "$Context.resources.$field" -Minimum 0 -Maximum $script:MaxResources
        # Graphics entries use the legacy generic storageBuffers count for
        # read-only StructuredBuffer declarations.  Compute entries use the
        # explicit read-only/read-write fields instead.
        if ($field -eq 'readonlyStorageBuffers' -and $expectedStorage -gt 0 -and $expectedReadonly -eq 0 -and $expectedReadwrite -eq 0) { continue }
        $actualValue = if ($field -eq 'storageBuffers' -and ($expectedStorage -gt 0 -or ($expectedStorage -eq 0 -and $expectedReadonly -eq 0 -and $expectedReadwrite -eq 0))) {
            # Source-contract 0.1 uses storageBuffers for graphics SRV
            # StructuredBuffer declarations, while compute contracts split
            # read-only and read-write storage buffers explicitly.
            [int64]$actualCounts.storageBuffers + [int64]$actualCounts.readonlyStorageBuffers
        } else {
            [int64](Get-FieldValue -Object $actualCounts -Names @($field))
        }
        if ($expectedValue -ne $actualValue) {
            Add-Issue -Severity error -Code 'resource-category-count-mismatch' -Message "$Context resources.$field declares $expectedValue but reflection found $actualValue." -Artifact $Artifact -Backend $Backend
        }
    }
    return $true
}

function Get-ExpectedResourceCount {
    param(
        [AllowNull()][object]$Reflection,
        [Parameter(Mandatory = $true)][string]$Context
    )

    if ($null -eq $Reflection) { return $null }
    $value = Get-FieldValue -Object $Reflection -Names @('resourceBindingCount', 'bindingCount', 'resourceCount')
    if ($null -eq $value) {
        $resources = Get-ExpectedResources -Reflection $Reflection -Context $Context
        if ($null -ne $resources) { return [int64]$resources.Count }
        return $null
    }
    return Convert-ToBoundedInteger -Value $value -Context "$Context.resourceBindingCount" -Minimum 0 -Maximum $script:MaxResources
}

function Get-ExpectedThreadGroup {
    param(
        [AllowNull()][object]$Reflection,
        [Parameter(Mandatory = $true)][string]$Context
    )

    if ($null -eq $Reflection) { return $null }
    $value = Get-FieldValue -Object $Reflection -Names @('threadGroupSize', 'threadGroup', 'numThreads', 'localSize', 'workgroupSize')
    if ($null -eq $value) { return $null }
    $values = @($value)
    if ($values.Count -eq 1 -and $value -is [object] -and $value -isnot [string]) {
        $values = @(
            (Get-FieldValue -Object $value -Names @('x', 'X', 'width')),
            (Get-FieldValue -Object $value -Names @('y', 'Y', 'height')),
            (Get-FieldValue -Object $value -Names @('z', 'Z', 'depth'))
        )
    }
    if ($values.Count -ne 3 -or $null -in $values) { throw "$Context thread group size must contain exactly three integers." }
    return @(
        (Convert-ToBoundedInteger -Value $values[0] -Context "$Context.threadGroupSize[0]" -Minimum 1 -Maximum 1024),
        (Convert-ToBoundedInteger -Value $values[1] -Context "$Context.threadGroupSize[1]" -Minimum 1 -Maximum 1024),
        (Convert-ToBoundedInteger -Value $values[2] -Context "$Context.threadGroupSize[2]" -Minimum 1 -Maximum 1024)
    )
}

function Convert-ExpectedResource {
    param(
        [Parameter(Mandatory = $true)][object]$Value,
        [Parameter(Mandatory = $true)][string]$Context
    )

    if ($Value -is [string]) {
        throw "$Context expected resource must be an object, not a string."
    }
    $kindValue = Get-FieldValue -Object $Value -Names @('kind', 'resourceKind', 'resourceType', 'type')
    $classValue = Get-FieldValue -Object $Value -Names @('registerClass', 'class', 'registerType', 'spaceClass')
    $registerValue = Get-FieldValue -Object $Value -Names @('register', 'slot', 'binding')
    $spaceValue = Get-FieldValue -Object $Value -Names @('space', 'descriptorSet', 'set')
    $countValue = Get-FieldValue -Object $Value -Names @('count', 'arrayCount', 'descriptorCount')

    $result = [ordered]@{}
    if ($null -ne $kindValue) {
        $kindText = ([string]$kindValue).Trim()
        try { $result.kind = Normalize-ResourceKind -Kind $kindText } catch { throw "$Context.kind is invalid: $($_.Exception.Message)" }
    }
    if ($null -ne $classValue) {
        $classText = ([string]$classValue).Trim().ToLowerInvariant()
        if ($classText -eq 'cb') { $classText = 'b' }
        if ($classText -notmatch '^[btsu]$') { throw "$Context.registerClass must be one of b, t, s, u." }
        $result.registerClass = $classText
    }
    if ($null -ne $registerValue) { $result.register = Convert-ToBoundedInteger -Value $registerValue -Context "$Context.register" -Minimum 0 -Maximum 65535 }
    if ($null -ne $spaceValue) { $result.space = Convert-ToBoundedInteger -Value $spaceValue -Context "$Context.space" -Minimum 0 -Maximum 65535 }
    if ($null -ne $countValue) { $result.count = Convert-ToBoundedInteger -Value $countValue -Context "$Context.count" -Minimum 1 -Maximum $script:MaxResources }
    $nameValue = Get-FieldValue -Object $Value -Names @('name', 'resourceName', 'identifier')
    if ($null -ne $nameValue -and -not [string]::IsNullOrWhiteSpace([string]$nameValue)) { $result.name = [string]$nameValue }
    if ($result.Count -eq 0) { throw "$Context has no verifiable binding fields." }
    return [pscustomobject]$result
}

function Compare-ExpectedReflection {
    param(
        [Parameter(Mandatory = $true)][object]$Actual,
        [AllowNull()][object]$Reflection,
        [Parameter(Mandatory = $true)][string]$Context,
        [Parameter(Mandatory = $true)][string]$Artifact,
        [Parameter(Mandatory = $true)][string]$Backend
    )

    if ($null -eq $Reflection) {
        Add-Issue -Severity error -Code 'manifest-reflection-missing' -Message "$Context has no reflection contract." -Artifact $Artifact -Backend $Backend
        return
    }

    $hasCategoryCounts = Compare-ExpectedResourceCategoryCounts -Actual $Actual -Reflection $Reflection -Context $Context -Artifact $Artifact -Backend $Backend

    $expectedCount = Get-ExpectedResourceCount -Reflection $Reflection -Context $Context
    $expectedResources = Get-ExpectedResources -Reflection $Reflection -Context $Context
    if (-not $hasCategoryCounts -and $null -eq $expectedCount -and $null -eq $expectedResources) {
        Add-Issue -Severity error -Code 'manifest-resource-contract-missing' -Message "$Context has neither resourceBindingCount nor resourceBindings/resources/bindings." -Artifact $Artifact -Backend $Backend
    } elseif ($null -ne $expectedCount -and $expectedCount -ne $Actual.resourceBindingCount) {
        Add-Issue -Severity error -Code 'resource-binding-count-mismatch' -Message "$Context declares $expectedCount resource bindings but reflection found $($Actual.resourceBindingCount)." -Artifact $Artifact -Backend $Backend
    }

    if ($null -ne $expectedResources) {
        if ($expectedResources.Count -ne $Actual.resources.Count) {
            Add-Issue -Severity error -Code 'resource-list-count-mismatch' -Message "$Context declares $($expectedResources.Count) resource entries but reflection found $($Actual.resources.Count)." -Artifact $Artifact -Backend $Backend
        } else {
            $actualSorted = @($Actual.resources | Sort-Object registerClass, space, register)
            $expectedNormalized = [System.Collections.Generic.List[object]]::new()
            for ($index = 0; $index -lt $expectedResources.Count; $index++) {
                [void]$expectedNormalized.Add((Convert-ExpectedResource -Value $expectedResources[$index] -Context "$Context.resources[$index]"))
            }
            $expectedSorted = @($expectedNormalized | Sort-Object registerClass, space, register)
            for ($index = 0; $index -lt $expectedSorted.Count; $index++) {
                $expectedItem = $expectedSorted[$index]
                $actualItem = $actualSorted[$index]
                foreach ($field in @('kind', 'registerClass', 'register', 'space', 'count')) {
                    if (Test-FieldPresent -Object $expectedItem -Names @($field)) {
                        if ((Get-FieldValue -Object $expectedItem -Names @($field)) -ne (Get-FieldValue -Object $actualItem -Names @($field))) {
                            Add-Issue -Severity error -Code 'resource-binding-mismatch' -Message "$Context resource[$index].$field expected '$((Get-FieldValue -Object $expectedItem -Names @($field)))' but found '$((Get-FieldValue -Object $actualItem -Names @($field)))'." -Artifact $Artifact -Backend $Backend
                        }
                    }
                }
            }
        }
    }

    $expectedThread = Get-ExpectedThreadGroup -Reflection $Reflection -Context $Context
    if ($Actual.stage -eq 'compute') {
        if ($null -eq $Actual.threadGroupSize) {
            Add-Issue -Severity error -Code 'compute-thread-group-missing' -Message "$Context reflection did not expose a concrete compute thread group." -Artifact $Artifact -Backend $Backend
        }
        if ($null -eq $expectedThread) {
            Add-Issue -Severity error -Code 'manifest-thread-group-missing' -Message "$Context is a compute shader but has no threadGroupSize/localSize contract." -Artifact $Artifact -Backend $Backend
        } else {
            $expectedThreadText = (@($expectedThread) -join ',')
            $actualThreadText = (@($Actual.threadGroupSize) -join ',')
            if ($expectedThreadText -ne $actualThreadText) {
            Add-Issue -Severity error -Code 'thread-group-mismatch' -Message "$Context declares thread group [$(@($expectedThread) -join ',')] but reflection found [$(@($Actual.threadGroupSize) -join ',')]." -Artifact $Artifact -Backend $Backend
            }
        }
    } elseif ($null -ne $expectedThread) {
        Add-Issue -Severity error -Code 'noncompute-thread-group' -Message "$Context declares a thread group for non-compute stage '$($Actual.stage)'." -Artifact $Artifact -Backend $Backend
    }
}

function Read-Utf8SpirvString {
    param(
        [Parameter(Mandatory = $true)][uint32[]]$Words,
        [Parameter(Mandatory = $true)][int]$Start,
        [Parameter(Mandatory = $true)][int]$End,
        [Parameter(Mandatory = $true)][string]$Context
    )

    $bytes = [System.Collections.Generic.List[byte]]::new()
    $terminated = $false
    for ($wordIndex = $Start; $wordIndex -lt $End; $wordIndex++) {
        $word = $Words[$wordIndex]
        for ($byteIndex = 0; $byteIndex -lt 4; $byteIndex++) {
            $byte = [byte](($word -shr ($byteIndex * 8)) -band 0xff)
            if ($byte -eq 0) {
                $terminated = $true
                break
            }
            [void]$bytes.Add($byte)
            if ($bytes.Count -gt 4096) { throw "$Context string exceeds the 4096-byte limit." }
        }
        if ($terminated) { break }
    }
    if (-not $terminated) { throw "$Context string is not NUL terminated." }
    $encoding = [System.Text.UTF8Encoding]::new($false, $true)
    try { $value = $encoding.GetString($bytes.ToArray()) } catch { throw "$Context is not valid UTF-8." }
    return [pscustomobject]@{ Value = $value; Next = ($wordIndex + 1) }
}

function Get-SpirvType {
    param([Parameter(Mandatory = $true)][hashtable]$Types, [Parameter(Mandatory = $true)][uint32]$Id)
    if (-not $Types.ContainsKey($Id)) { throw "SPIR-V references unknown type ID $Id." }
    return $Types[$Id]
}

function Test-Decoration {
    param(
        [Parameter(Mandatory = $true)][hashtable]$Decorations,
        [Parameter(Mandatory = $true)][uint32]$Id,
        [Parameter(Mandatory = $true)][int]$Decoration
    )
    if (-not $Decorations.ContainsKey($Id)) { return $false }
    foreach ($item in @($Decorations[$Id])) {
        if ([int]$item.decoration -eq $Decoration) { return $true }
    }
    return $false
}

function Get-DecorationValue {
    param(
        [Parameter(Mandatory = $true)][hashtable]$Decorations,
        [Parameter(Mandatory = $true)][uint32]$Id,
        [Parameter(Mandatory = $true)][int]$Decoration,
        [Parameter(Mandatory = $true)][string]$Context
    )
    if (-not $Decorations.ContainsKey($Id)) { throw "$Context is missing SPIR-V decoration $Decoration." }
    $matches = @($Decorations[$Id] | Where-Object { [int]$_.decoration -eq $Decoration })
    $operandValues = @($matches[0].operands)
    if ($matches.Count -ne 1 -or $operandValues.Count -lt 1) { throw "$Context must have exactly one SPIR-V decoration $Decoration with a value." }
    return [uint32]$operandValues[0]
}

function Get-SpirvArrayBaseAndCount {
    param(
        [Parameter(Mandatory = $true)][hashtable]$Types,
        [Parameter(Mandatory = $true)][hashtable]$Constants,
        [Parameter(Mandatory = $true)][uint32]$TypeId,
        [Parameter(Mandatory = $true)][string]$Context
    )

    $count = [int64]1
    $current = $TypeId
    while ($true) {
        $type = Get-SpirvType -Types $Types -Id $current
        if ([int]$type.opcode -eq 28) {
            if ($type.args.Count -lt 3) { throw "$Context has a malformed OpTypeArray." }
            $lengthId = [uint32]$type.args[2]
            if (-not $Constants.ContainsKey($lengthId)) { throw "$Context has an OpTypeArray with an unresolved length." }
            $length = Convert-ToBoundedInteger -Value $Constants[$lengthId] -Context "$Context.arrayLength" -Minimum 1 -Maximum $script:MaxResources
            if ($count -gt [int64]($script:MaxResources / $length)) { throw "$Context descriptor array count exceeds the limit." }
            $count *= $length
            $current = [uint32]$type.args[1]
            continue
        }
        if ([int]$type.opcode -eq 29) { throw "$Context uses an unbounded descriptor array, which is not verifiable." }
        break
    }
    return [pscustomobject]@{ TypeId = $current; Count = $count }
}

function Get-SpirvResourceKind {
    param(
        [Parameter(Mandatory = $true)][hashtable]$Types,
        [Parameter(Mandatory = $true)][hashtable]$Decorations,
        [Parameter(Mandatory = $true)][hashtable]$MemberDecorations,
        [Parameter(Mandatory = $true)][hashtable]$Constants,
        [Parameter(Mandatory = $true)][uint32]$VariableTypeId,
        [Parameter(Mandatory = $true)][int]$StorageClass,
        [Parameter(Mandatory = $true)][string]$Context
    )

    $pointer = Get-SpirvType -Types $Types -Id $VariableTypeId
    if ([int]$pointer.opcode -ne 32 -or $pointer.args.Count -lt 3) { throw "$Context has a variable type that is not an OpTypePointer." }
    $pointeeId = [uint32]$pointer.args[2]
    $arrayInfo = Get-SpirvArrayBaseAndCount -Types $Types -Constants $Constants -TypeId $pointeeId -Context $Context
    $type = Get-SpirvType -Types $Types -Id $arrayInfo.TypeId
    $kind = $null
    $category = $null

    switch ([int]$type.opcode) {
        25 {
            # OpTypeImage: operands are result id, sampled type, dimension,
            # depth, arrayed, multisample, sampled, image format.
            if ($type.args.Count -lt 7) { throw "$Context has a malformed OpTypeImage." }
            $sampled = [int]$type.args[6]
            $kind = if ($sampled -eq 2) { 'uav' } else { 'texture' }
            $category = if ($sampled -eq 2) { 'uav' } else { 'texture' }
        }
        26 { $kind = 'sampler'; $category = 'sampler' }
        27 { $kind = 'texture'; $category = 'texture' }
        30 {
            $isBlock = Test-Decoration -Decorations $Decorations -Id $arrayInfo.TypeId -Decoration 2
            $isBufferBlock = Test-Decoration -Decorations $Decorations -Id $arrayInfo.TypeId -Decoration 3
            $isNonWritable = Test-Decoration -Decorations $Decorations -Id $arrayInfo.TypeId -Decoration 24
            if ($MemberDecorations.ContainsKey($arrayInfo.TypeId)) {
                foreach ($item in @($MemberDecorations[$arrayInfo.TypeId])) {
                    if ([int]$item.decoration -eq 24) { $isNonWritable = $true }
                }
            }
            if ($isBlock) { $kind = 'cbuffer'; $category = 'uniformBuffer' }
            elseif ($isBufferBlock) {
                $kind = if ($isNonWritable) { 'texture' } else { 'uav' }
                $category = if ($isNonWritable) { 'readonlyStorageBuffer' } else { 'readwriteStorageBuffer' }
            }
        }
        default { }
    }
    if ($StorageClass -eq 12) {
        $isNonWritable = Test-Decoration -Decorations $Decorations -Id $arrayInfo.TypeId -Decoration 24
        $kind = if ($isNonWritable) { 'texture' } else { 'uav' }
        $category = if ($isNonWritable) { 'readonlyStorageBuffer' } else { 'readwriteStorageBuffer' }
    }
    if ($null -eq $kind) { throw "$Context has an unsupported descriptor type (SPIR-V opcode $($type.opcode), storage class $StorageClass)." }
    return [pscustomobject]@{ Kind = $kind; Category = $category; Count = $arrayInfo.Count }
}

function Read-SpirvReflection {
    param(
        [Parameter(Mandatory = $true)][System.IO.FileInfo]$File,
        [Parameter(Mandatory = $true)][string]$ExpectedStage,
        [Parameter(Mandatory = $true)][string]$ExpectedEntryPoint,
        [Parameter(Mandatory = $true)][string]$Context
    )

    $bytes = [System.IO.File]::ReadAllBytes($File.FullName)
    if ($bytes.Length -lt 20 -or ($bytes.Length % 4) -ne 0) { throw "$Context is not a complete SPIR-V word stream." }
    if ([BitConverter]::ToUInt32($bytes, 0) -ne [uint32]0x07230203) { throw "$Context has an invalid SPIR-V magic number." }
    $words = [uint32[]]::new([int]($bytes.Length / 4))
    [Buffer]::BlockCopy($bytes, 0, $words, 0, $bytes.Length)
    $version = $words[1]
    $major = [int](($version -shr 16) -band 0xff)
    $minor = [int](($version -shr 8) -band 0xff)
    if ($major -ne 1 -or $minor -gt 6) { throw "$Context uses unsupported SPIR-V version $major.$minor." }
    $bound = [uint32]$words[3]
    if ($bound -lt 1 -or $bound -gt 10000000) { throw "$Context has an invalid SPIR-V ID bound $bound." }
    if ($words[4] -ne 0) { throw "$Context has a non-zero SPIR-V schema word." }

    $types = @{}
    $constants = @{}
    $variables = [System.Collections.Generic.List[object]]::new()
    $decorations = @{}
    $memberDecorations = @{}
    $names = @{}
    $entryPoints = [System.Collections.Generic.List[object]]::new()
    $executionModes = [System.Collections.Generic.List[object]]::new()
    $hasMemoryModel = $false
    $instructionCount = 0
    $wordIndex = 5
    while ($wordIndex -lt $words.Count) {
        $instructionWord = [uint32]$words[$wordIndex]
        $wordCount = [int](($instructionWord -shr 16) -band 0xffff)
        $opcode = [int]($instructionWord -band 0xffff)
        if ($wordCount -lt 1 -or $wordIndex + $wordCount -gt $words.Count) { throw "$Context has a malformed SPIR-V instruction at word $wordIndex." }
        $instructionCount++
        if ($instructionCount -gt $script:MaxInstructions) { throw "$Context exceeds the SPIR-V instruction limit." }
        $operandCount = $wordCount - 1
        $operands = [uint32[]]::new($operandCount)
        if ($operandCount -gt 0) { [Array]::Copy($words, $wordIndex + 1, $operands, 0, $operandCount) }
        switch ($opcode) {
            14 { $hasMemoryModel = $true }
            5 {
                if ($operands.Count -lt 2) { throw "$Context has malformed OpName." }
                $parsed = Read-Utf8SpirvString -Words $words -Start ($wordIndex + 2) -End ($wordIndex + $wordCount) -Context "$Context OpName"
                $names[[uint32]$operands[0]] = $parsed.Value
            }
            15 {
                if ($operands.Count -lt 3) { throw "$Context has malformed OpEntryPoint." }
                $parsed = Read-Utf8SpirvString -Words $words -Start ($wordIndex + 3) -End ($wordIndex + $wordCount) -Context "$Context OpEntryPoint"
                [void]$entryPoints.Add([pscustomobject]@{
                    executionModel = [int]$operands[0]
                    id = [uint32]$operands[1]
                    name = $parsed.Value
                })
            }
            16 {
                if ($operands.Count -lt 2) { throw "$Context has malformed OpExecutionMode." }
                $modeValues = if ($operands.Count -gt 2) { @($operands[2..($operands.Count - 1)]) } else { @() }
                [void]$executionModes.Add([pscustomobject]@{ id = [uint32]$operands[0]; mode = [int]$operands[1]; values = $modeValues })
            }
            331 {
                if ($operands.Count -lt 2) { throw "$Context has malformed OpExecutionModeId." }
                $modeValueIds = if ($operands.Count -gt 2) { @($operands[2..($operands.Count - 1)]) } else { @() }
                [void]$executionModes.Add([pscustomobject]@{ id = [uint32]$operands[0]; mode = [int]$operands[1]; valueIds = $modeValueIds })
            }
            21 { if ($operands.Count -ge 1) { $types[[uint32]$operands[0]] = [pscustomobject]@{ opcode = $opcode; args = @($operands) } } }
            22 { if ($operands.Count -ge 1) { $types[[uint32]$operands[0]] = [pscustomobject]@{ opcode = $opcode; args = @($operands) } } }
            23 { if ($operands.Count -ge 1) { $types[[uint32]$operands[0]] = [pscustomobject]@{ opcode = $opcode; args = @($operands) } } }
            24 { if ($operands.Count -ge 1) { $types[[uint32]$operands[0]] = [pscustomobject]@{ opcode = $opcode; args = @($operands) } } }
            25 { if ($operands.Count -ge 1) { $types[[uint32]$operands[0]] = [pscustomobject]@{ opcode = $opcode; args = @($operands) } } }
            26 { if ($operands.Count -ge 1) { $types[[uint32]$operands[0]] = [pscustomobject]@{ opcode = $opcode; args = @($operands) } } }
            27 { if ($operands.Count -ge 1) { $types[[uint32]$operands[0]] = [pscustomobject]@{ opcode = $opcode; args = @($operands) } } }
            28 { if ($operands.Count -ge 1) { $types[[uint32]$operands[0]] = [pscustomobject]@{ opcode = $opcode; args = @($operands) } } }
            29 { if ($operands.Count -ge 1) { $types[[uint32]$operands[0]] = [pscustomobject]@{ opcode = $opcode; args = @($operands) } } }
            30 { if ($operands.Count -ge 1) { $types[[uint32]$operands[0]] = [pscustomobject]@{ opcode = $opcode; args = @($operands) } } }
            32 { if ($operands.Count -ge 1) { $types[[uint32]$operands[0]] = [pscustomobject]@{ opcode = $opcode; args = @($operands) } } }
            43 { if ($operands.Count -ge 3) { $constants[[uint32]$operands[1]] = [uint32]$operands[2] } }
            50 { if ($operands.Count -ge 3) { $constants[[uint32]$operands[1]] = [uint32]$operands[2] } }
            59 { if ($operands.Count -ge 3) { [void]$variables.Add([pscustomobject]@{ typeId = [uint32]$operands[0]; id = [uint32]$operands[1]; storageClass = [int]$operands[2] }) } }
            71 {
                if ($operands.Count -lt 2) { throw "$Context has malformed OpDecorate." }
                $target = [uint32]$operands[0]
                if (-not $decorations.ContainsKey($target)) { $decorations[$target] = [System.Collections.Generic.List[object]]::new() }
                $decorationOperands = if ($operands.Count -gt 2) { @($operands[2..($operands.Count - 1)]) } else { @() }
                [void]$decorations[$target].Add([pscustomobject]@{ decoration = [int]$operands[1]; operands = $decorationOperands })
            }
            72 {
                if ($operands.Count -lt 3) { throw "$Context has malformed OpMemberDecorate." }
                $target = [uint32]$operands[0]
                if (-not $memberDecorations.ContainsKey($target)) { $memberDecorations[$target] = [System.Collections.Generic.List[object]]::new() }
                $memberOperands = if ($operands.Count -gt 3) { @($operands[3..($operands.Count - 1)]) } else { @() }
                [void]$memberDecorations[$target].Add([pscustomobject]@{ member = [int]$operands[1]; decoration = [int]$operands[2]; operands = $memberOperands })
            }
        }
        $wordIndex += $wordCount
    }
    if (-not $hasMemoryModel) { throw "$Context has no OpMemoryModel." }
    if ($entryPoints.Count -eq 0) { throw "$Context has no OpEntryPoint." }

    $expectedExecutionModel = switch ($ExpectedStage) {
        'vertex' { 0 }
        'hull' { 1 }
        'domain' { 2 }
        'geometry' { 3 }
        'fragment' { 4 }
        'compute' { 5 }
        'task' { 5364 }
        'mesh' { 5365 }
        default { throw "SPIR-V stage mapping for '$ExpectedStage' is not implemented." }
    }
    $selectedEntries = @($entryPoints | Where-Object { $_.name -eq $ExpectedEntryPoint -and $_.executionModel -eq $expectedExecutionModel })
    if ($selectedEntries.Count -ne 1) { throw "$Context does not contain exactly one '$ExpectedEntryPoint' entry point for stage '$ExpectedStage'." }
    $selectedEntry = $selectedEntries[0]

    $threadGroup = $null
    $localModes = @($executionModes | Where-Object { $_.id -eq $selectedEntry.id -and $_.mode -in @(17, 38) })
    if ($ExpectedStage -eq 'compute') {
        $localMode = $localModes | Where-Object { $_.mode -eq 17 } | Select-Object -First 1
        if ($null -ne $localMode) {
            if ($localMode.values.Count -ne 3) { throw "$Context OpExecutionMode LocalSize does not have three literals." }
            $threadGroup = @(
                (Convert-ToBoundedInteger -Value $localMode.values[0] -Context "$Context localSize[0]" -Minimum 1 -Maximum 1024),
                (Convert-ToBoundedInteger -Value $localMode.values[1] -Context "$Context localSize[1]" -Minimum 1 -Maximum 1024),
                (Convert-ToBoundedInteger -Value $localMode.values[2] -Context "$Context localSize[2]" -Minimum 1 -Maximum 1024)
            )
        } else {
            $localIdMode = $localModes | Where-Object { $_.mode -eq 38 } | Select-Object -First 1
            if ($null -ne $localIdMode -and $localIdMode.PSObject.Properties['valueIds']) {
                if ($localIdMode.valueIds.Count -ne 3) { throw "$Context OpExecutionModeId LocalSizeId does not have three IDs." }
                $threadGroup = @(
                    (Convert-ToBoundedInteger -Value $constants[[uint32]$localIdMode.valueIds[0]] -Context "$Context localSizeId[0]" -Minimum 1 -Maximum 1024),
                    (Convert-ToBoundedInteger -Value $constants[[uint32]$localIdMode.valueIds[1]] -Context "$Context localSizeId[1]" -Minimum 1 -Maximum 1024),
                    (Convert-ToBoundedInteger -Value $constants[[uint32]$localIdMode.valueIds[2]] -Context "$Context localSizeId[2]" -Minimum 1 -Maximum 1024)
                )
            }
        }
    }

    $resources = [System.Collections.Generic.List[object]]::new()
    foreach ($variable in $variables) {
        $hasSet = Test-Decoration -Decorations $decorations -Id $variable.id -Decoration 34
        $hasBinding = Test-Decoration -Decorations $decorations -Id $variable.id -Decoration 33
        if (-not $hasSet -and -not $hasBinding) { continue }
        if (-not $hasSet -or -not $hasBinding) { throw "$Context descriptor variable ID $($variable.id) has only one of DescriptorSet/Binding." }
        $set = Get-DecorationValue -Decorations $decorations -Id $variable.id -Decoration 34 -Context "$Context resource $($variable.id).descriptorSet"
        $binding = Get-DecorationValue -Decorations $decorations -Id $variable.id -Decoration 33 -Context "$Context resource $($variable.id).binding"
        $kindInfo = Get-SpirvResourceKind -Types $types -Decorations $decorations -MemberDecorations $memberDecorations -Constants $constants -VariableTypeId $variable.typeId -StorageClass $variable.storageClass -Context "$Context resource $($variable.id)"
        $name = if ($names.ContainsKey($variable.id)) { [string]$names[$variable.id] } else { $null }
        [void]$resources.Add((Convert-ResourceSignature -Kind $kindInfo.Kind -Register ([int64]$binding) -Space ([int64]$set) -Count ([int64]$kindInfo.Count) -Category $kindInfo.Category -Name $name))
    }
    if ($resources.Count -gt $script:MaxResources) { throw "$Context has too many reflected resources." }
    $signatures = @($resources | ForEach-Object { "$($_.registerClass):$($_.space):$($_.register):$($_.count)" })
    if (@($signatures | Sort-Object -Unique).Count -ne @($signatures).Count) { throw "$Context contains duplicate resource bindings." }
    return [pscustomobject]@{
        stage = $ExpectedStage
        entrypoint = $selectedEntry.name
        threadGroupSize = $threadGroup
        resourceBindingCount = $resources.Count
        resources = @($resources | Sort-Object registerClass, space, register)
        container = [ordered]@{
            format = 'SPIR-V'
            bytes = $bytes.Length
            version = "$major.$minor"
            bound = $bound
            instructionCount = $instructionCount
        }
    }
}

function Read-DxilReflection {
    param(
        [Parameter(Mandatory = $true)][System.IO.FileInfo]$File,
        [Parameter(Mandatory = $true)][string]$Compiler,
        [Parameter(Mandatory = $true)][string]$ExpectedStage,
        [Parameter(Mandatory = $true)][string]$ExpectedEntryPoint,
        [Parameter(Mandatory = $true)][string]$Context
    )

    $bytes = [System.IO.File]::ReadAllBytes($File.FullName)
    if ($bytes.Length -lt 32) { throw "$Context is too small to be a DXIL container." }
    if ([Text.Encoding]::ASCII.GetString($bytes, 0, 4) -ne 'DXBC') { throw "$Context does not start with DXBC." }
    $totalSize = [int64][BitConverter]::ToUInt32($bytes, 24)
    $chunkCount = [int64][BitConverter]::ToUInt32($bytes, 28)
    if ($totalSize -ne $bytes.Length) { throw "$Context DXBC total size $totalSize does not equal file size $($bytes.Length)." }
    if ($chunkCount -lt 1 -or $chunkCount -gt 128 -or 32 + 4 * $chunkCount -gt $bytes.Length) { throw "$Context has an invalid DXBC chunk count $chunkCount." }
    $ranges = [System.Collections.Generic.List[object]]::new()
    $tags = [System.Collections.Generic.List[string]]::new()
    for ($index = 0; $index -lt $chunkCount; $index++) {
        $offset = [int64][BitConverter]::ToUInt32($bytes, 32 + 4 * $index)
        if ($offset -lt 32 + 4 * $chunkCount -or $offset + 8 -gt $bytes.Length) { throw "$Context has an out-of-range DXBC chunk offset $offset." }
        $chunkSize = [int64][BitConverter]::ToUInt32($bytes, [int]($offset + 4))
        $end = $offset + 8 + $chunkSize
        if ($end -gt $bytes.Length -or $chunkSize -gt $script:MaxShaderBytes) { throw "$Context has an out-of-range DXBC chunk size $chunkSize." }
        foreach ($range in @($ranges)) {
            if ($offset -lt $range.end -and $end -gt $range.start) { throw "$Context has overlapping DXBC chunks." }
        }
        [void]$ranges.Add([pscustomobject]@{ start = $offset; end = $end })
        [void]$tags.Add([Text.Encoding]::ASCII.GetString($bytes, [int]$offset, 4))
    }
    if ('DXIL' -notin $tags) { throw "$Context is missing a DXIL chunk." }
    if ('PSV0' -notin $tags) { throw "$Context is missing the PSV0 reflection chunk." }

    $dump = Invoke-ProcessCapture -Executable $Compiler -Arguments @('-dumpbin', $File.FullName)
    if ($dump.ExitCode -ne 0) { throw "$Context dxc -dumpbin failed with exit code $($dump.ExitCode): $($dump.Stderr.Trim())" }
    $lines = @($dump.Stdout -split "`r?`n")
    $stage = $null
    foreach ($line in $lines) {
        if ($line -match ';\s*Vertex Shader\s*$') { $stage = 'vertex' }
        elseif ($line -match ';\s*(Pixel|Fragment) Shader\s*$') { $stage = 'fragment' }
        elseif ($line -match ';\s*Compute Shader\s*$') { $stage = 'compute' }
        elseif ($line -match ';\s*Geometry Shader\s*$') { $stage = 'geometry' }
        elseif ($line -match ';\s*Hull Shader\s*$') { $stage = 'hull' }
        elseif ($line -match ';\s*Domain Shader\s*$') { $stage = 'domain' }
        elseif ($line -match ';\s*(Mesh|Amplification|Task) Shader\s*$') { $stage = if ($line -match 'Mesh') { 'mesh' } else { 'task' } }
    }
    if ($null -eq $stage) { throw "$Context dxc -dumpbin output did not expose a shader stage." }
    $entryMatches = @($lines | Where-Object { $_ -match 'EntryFunctionName:\s*(?<entry>[^\s;]+)' } | ForEach-Object { [regex]::Match($_, 'EntryFunctionName:\s*(?<entry>[^\s;]+)').Groups['entry'].Value })
    if ($entryMatches.Count -ne 1 -or $entryMatches[0] -ne $ExpectedEntryPoint) { throw "$Context dxc reflection entry point does not match '$ExpectedEntryPoint'." }
    $threadGroup = $null
    $threadMatch = $lines | Where-Object { $_ -match 'NumThreads=\((?<x>\d+),(?<y>\d+),(?<z>\d+)\)' } | Select-Object -First 1
    if ($null -ne $threadMatch) {
        $match = [regex]::Match($threadMatch, 'NumThreads=\((?<x>\d+),(?<y>\d+),(?<z>\d+)\)')
        $threadGroup = @(
            (Convert-ToBoundedInteger -Value $match.Groups['x'].Value -Context "$Context NumThreads.x" -Minimum 1 -Maximum 1024),
            (Convert-ToBoundedInteger -Value $match.Groups['y'].Value -Context "$Context NumThreads.y" -Minimum 1 -Maximum 1024),
            (Convert-ToBoundedInteger -Value $match.Groups['z'].Value -Context "$Context NumThreads.z" -Minimum 1 -Maximum 1024)
        )
    }

    $resourceHeaderSeen = $false
    $resources = [System.Collections.Generic.List[object]]::new()
    $rowPattern = '^\s*;\s*(?:(?<name>\S+)\s+)?(?<kind>cbuffer|sampler|texture|UAV|uav|tbuffer|structured|byteaddress)\s+(?<format>\S+)\s+(?<dimension>\S+)\s+(?<id>\S+)\s+(?<binding>(?:cb|[tsu])\d+(?:,space\d+)?)\s+(?<count>\d+)\s*$'
    foreach ($line in $lines) {
        if ($line -match ';\s*Resource Bindings:\s*$') { $resourceHeaderSeen = $true; continue }
        if (-not $resourceHeaderSeen) { continue }
        $match = [regex]::Match($line, $rowPattern)
        if (-not $match.Success) { continue }
        $bindingText = $match.Groups['binding'].Value
        $bindingMatch = [regex]::Match($bindingText, '^(?<prefix>cb|[tsu])(?<register>\d+)(?:,space(?<space>\d+))?$')
        if (-not $bindingMatch.Success) { throw "$Context contains an invalid DXIL binding '$bindingText'." }
        $kindText = $match.Groups['kind'].Value
        $formatText = $match.Groups['format'].Value
        $kind = Normalize-ResourceKind -Kind $kindText -Format $formatText -Dimension $match.Groups['dimension'].Value
        $count = Convert-ToBoundedInteger -Value $match.Groups['count'].Value -Context "$Context resource count" -Minimum 1 -Maximum $script:MaxResources
        $register = Convert-ToBoundedInteger -Value $bindingMatch.Groups['register'].Value -Context "$Context resource register" -Minimum 0 -Maximum 65535
        $space = if ($bindingMatch.Groups['space'].Success) { Convert-ToBoundedInteger -Value $bindingMatch.Groups['space'].Value -Context "$Context resource space" -Minimum 0 -Maximum 65535 } else { [int64]0 }
        $name = $match.Groups['name'].Value
        $category = switch ($kind) {
            'cbuffer' { 'uniformBuffer' }
            'sampler' { 'sampler' }
            'texture' { if ($formatText -in @('struct', 'byte')) { 'readonlyStorageBuffer' } else { 'texture' } }
            'uav' { if ($formatText -in @('struct', 'byte')) { 'readwriteStorageBuffer' } else { 'uav' } }
            default { $null }
        }
        [void]$resources.Add((Convert-ResourceSignature -Kind $kind -Register $register -Space $space -Count $count -Category $category -Name $name))
    }
    if (-not $resourceHeaderSeen) { throw "$Context dxc -dumpbin output did not expose Resource Bindings." }
    if ($resources.Count -gt $script:MaxResources) { throw "$Context has too many reflected resources." }
    $signatures = @($resources | ForEach-Object { "$($_.registerClass):$($_.space):$($_.register):$($_.count)" })
    if (@($signatures | Sort-Object -Unique).Count -ne @($signatures).Count) { throw "$Context contains duplicate resource bindings." }
    return [pscustomobject]@{
        stage = $stage
        entrypoint = $entryMatches[0]
        threadGroupSize = $threadGroup
        resourceBindingCount = $resources.Count
        resources = @($resources | Sort-Object registerClass, space, register)
        container = [ordered]@{
            format = 'DXIL'
            bytes = $bytes.Length
            chunks = @($tags)
            totalSize = $totalSize
        }
    }
}

function Compare-BackendAbi {
    param(
        [Parameter(Mandatory = $true)][object]$Dxil,
        [Parameter(Mandatory = $true)][object]$Spirv,
        [Parameter(Mandatory = $true)][string]$Artifact
    )

    if ($Dxil.stage -ne $Spirv.stage) { Add-Issue -Severity error -Code 'backend-stage-mismatch' -Message "DXIL stage '$($Dxil.stage)' and SPIR-V stage '$($Spirv.stage)' differ." -Artifact $Artifact }
    if ($Dxil.entrypoint -ne $Spirv.entrypoint) { Add-Issue -Severity error -Code 'backend-entrypoint-mismatch' -Message "DXIL entry point '$($Dxil.entrypoint)' and SPIR-V entry point '$($Spirv.entrypoint)' differ." -Artifact $Artifact }
    if ($Dxil.resourceBindingCount -ne $Spirv.resourceBindingCount) { Add-Issue -Severity error -Code 'backend-resource-count-mismatch' -Message "DXIL reflects $($Dxil.resourceBindingCount) bindings but SPIR-V reflects $($Spirv.resourceBindingCount)." -Artifact $Artifact }
    if (($null -eq $Dxil.threadGroupSize) -xor ($null -eq $Spirv.threadGroupSize)) { Add-Issue -Severity error -Code 'backend-thread-group-presence-mismatch' -Message 'Only one backend exposes a compute thread group.' -Artifact $Artifact }
    elseif ($null -ne $Dxil.threadGroupSize) {
        $dxilThreadText = (@($Dxil.threadGroupSize) -join ',')
        $spirvThreadText = (@($Spirv.threadGroupSize) -join ',')
        if ($dxilThreadText -ne $spirvThreadText) { Add-Issue -Severity error -Code 'backend-thread-group-mismatch' -Message "DXIL thread group [$(@($Dxil.threadGroupSize) -join ',')] and SPIR-V thread group [$(@($Spirv.threadGroupSize) -join ',')] differ." -Artifact $Artifact }
    }
    $dxilResources = @($Dxil.resources | Sort-Object registerClass, space, register)
    $spirvResources = @($Spirv.resources | Sort-Object registerClass, space, register)
    if ($dxilResources.Count -eq $spirvResources.Count) {
        for ($index = 0; $index -lt $dxilResources.Count; $index++) {
            $left = $dxilResources[$index]
            $right = $spirvResources[$index]
            foreach ($field in @('kind', 'registerClass', 'register', 'space', 'count')) {
                if ((Get-FieldValue -Object $left -Names @($field)) -ne (Get-FieldValue -Object $right -Names @($field))) {
                    Add-Issue -Severity error -Code 'backend-resource-abi-mismatch' -Message "Binding $index differs: DXIL $field='$((Get-FieldValue -Object $left -Names @($field)))', SPIR-V '$((Get-FieldValue -Object $right -Names @($field)))'." -Artifact $Artifact
                }
            }
        }
    }
}

function Convert-SummaryJson {
    param([Parameter(Mandatory = $true)][object]$Summary)
    return $Summary | ConvertTo-Json -Depth 30
}

$summary = $null
try {
    if ([string]::IsNullOrWhiteSpace($ManifestPath)) { throw 'ManifestPath cannot be empty.' }
    if ([string]::IsNullOrWhiteSpace($CompilerPath)) { throw 'CompilerPath cannot be empty.' }
    $manifestFile = Resolve-ExistingFile -Path $ManifestPath -Context 'Manifest' -MaximumBytes $script:MaxManifestBytes
    $script:ManifestFullPath = $manifestFile.FullName
    $compilerFile = Resolve-ExistingFile -Path $CompilerPath -Context 'Compiler' -MaximumBytes $script:MaxShaderBytes
    if ([System.IO.Path]::GetExtension($compilerFile.FullName).ToLowerInvariant() -ne '.exe') { throw "CompilerPath must point to a Windows executable: $($compilerFile.FullName)" }

    $compilerProbe = Invoke-ProcessCapture -Executable $compilerFile.FullName -Arguments @('--version')
    if ($compilerProbe.ExitCode -ne 0 -or [string]::IsNullOrWhiteSpace($compilerProbe.Stdout)) { throw "DXC --version failed with exit code $($compilerProbe.ExitCode): $($compilerProbe.Stderr.Trim())" }
    $script:CompilerVersion = ($compilerProbe.Stdout -split "`r?`n" | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Select-Object -First 1).Trim()

    $manifestText = [System.IO.File]::ReadAllText($manifestFile.FullName, [System.Text.UTF8Encoding]::new($false, $true))
    try { $manifest = $manifestText | ConvertFrom-Json -Depth $script:MaxJsonDepth } catch { throw "Manifest is not valid UTF-8 JSON: $($_.Exception.Message)" }
    $schemaValue = Get-FieldValue -Object $manifest -Names @('schema', 'schemaId', 'schemaVersion', 'format')
    if ($null -eq $schemaValue -or ([string]$schemaValue -notmatch '^noemancer\.shader-artifact-manifest/0\.1$')) { throw "Manifest schema must be noemancer.shader-artifact-manifest/0.1." }
    $artifactValue = Get-FieldValue -Object $manifest -Names @('artifacts', 'shaders', 'entries', 'shaderArtifacts')
    if ($null -eq $artifactValue) { throw 'Manifest has no artifacts/shaders/entries array.' }
    $artifactItems = @($artifactValue)
    if ($artifactItems.Count -lt 1 -or $artifactItems.Count -gt $script:MaxArtifacts) { throw "Manifest artifact count must be between 1 and $script:MaxArtifacts." }
    $declaredShaderCount = Get-FieldValue -Object $manifest -Names @('shaderCount', 'artifactCount')
    if ($null -ne $declaredShaderCount) {
        $declaredShaderCount = Convert-ToBoundedInteger -Value $declaredShaderCount -Context 'Manifest.shaderCount' -Minimum 1 -Maximum $script:MaxArtifacts
        if ($declaredShaderCount -ne $artifactItems.Count) { throw "Manifest shaderCount $declaredShaderCount does not match the artifact array count $($artifactItems.Count)." }
    }
    $toolchain = Get-FieldValue -Object $manifest -Names @('toolchain', 'compiler')
    if ($null -ne $toolchain) {
        $toolchainHashValue = Get-FieldValue -Object $toolchain -Names @('sha256', 'hash', 'compilerSha256')
        if ($null -eq $toolchainHashValue) { throw 'Manifest toolchain is present but has no compiler SHA-256.' }
        $toolchainHash = ([string]$toolchainHashValue).Trim().ToLowerInvariant()
        if ($toolchainHash.StartsWith('sha256:')) { $toolchainHash = $toolchainHash.Substring(7) }
        if ($toolchainHash -notmatch '^[0-9a-f]{64}$') { throw 'Manifest toolchain SHA-256 is not a 64-character hexadecimal value.' }
        if ((Get-FileSha256 -Path $compilerFile.FullName) -ne $toolchainHash) { throw 'Compiler SHA-256 does not match the manifest toolchain identity.' }
    }
    $manifestDirectory = Split-Path -Parent $manifestFile.FullName
    $artifactSummaries = [System.Collections.Generic.List[object]]::new()
    $artifactNames = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
    $firstSpirvPath = $null

    foreach ($artifact in $artifactItems) {
        $name = Get-RequiredText -Object $artifact -Names @('name', 'stem', 'id', 'shader', 'key') -Context 'Shader artifact'
        if (-not $artifactNames.Add($name)) { throw "Manifest contains duplicate shader artifact '$name'." }
        $stageText = Get-RequiredText -Object $artifact -Names @('stage', 'shaderStage') -Context "$name"
        $stage = Normalize-Stage -Stage $stageText
        $entryPoint = Get-RequiredText -Object $artifact -Names @('entryPoint', 'entrypoint', 'entry') -Context "$name"
        if ($entryPoint.Length -gt 256 -or $entryPoint -notmatch '^[A-Za-z_][A-Za-z0-9_\.]*$') { throw "$name entry point '$entryPoint' is invalid or too long." }

        $sourceSpec = Get-FieldValue -Object $artifact -Names @('source', 'sourcePath', 'hlsl', 'sourceFile')
        if ($null -ne $sourceSpec) {
            $sourcePathValue = if ($sourceSpec -is [string]) { [string]$sourceSpec } else { Get-FieldValue -Object $sourceSpec -Names @('path', 'file', 'sourcePath') }
            if ($sourcePathValue) {
                $sourceFile = Resolve-ManifestSourceFile -ManifestDirectory $manifestDirectory -Path ([string]$sourcePathValue) -Context "$name source"
                $sourceHashValue = if ($sourceSpec -is [string]) { $null } else { Get-FieldValue -Object $sourceSpec -Names @('sha256', 'hash', 'sourceSha256') }
                if ($sourceHashValue) {
                    $sourceHash = ([string]$sourceHashValue).Trim().ToLowerInvariant()
                    if ($sourceHash.StartsWith('sha256:')) { $sourceHash = $sourceHash.Substring(7) }
                    if ($sourceHash -notmatch '^[0-9a-f]{64}$' -or (Get-FileSha256 -Path $sourceFile.FullName) -ne $sourceHash) { throw "$name source SHA-256 does not match." }
                }
            }
        }
        $dxilSpec = Get-ManifestBackendSpec -Artifact $artifact -Backend 'dxil'
        $spirvSpec = Get-ManifestBackendSpec -Artifact $artifact -Backend 'spirv'
        $dxilArtifact = Get-BackendArtifactPathAndHash -Spec $dxilSpec -Backend 'dxil' -ManifestDirectory $manifestDirectory -ArtifactName $name
        $spirvArtifact = Get-BackendArtifactPathAndHash -Spec $spirvSpec -Backend 'spirv' -ManifestDirectory $manifestDirectory -ArtifactName $name
        if ($null -eq $firstSpirvPath) { $firstSpirvPath = $spirvArtifact.file.FullName }
        if ((Get-FileSha256 -Path $dxilArtifact.file.FullName) -ne $dxilArtifact.expectedHash) { throw "$name DXIL SHA-256 does not match manifest." }
        if ((Get-FileSha256 -Path $spirvArtifact.file.FullName) -ne $spirvArtifact.expectedHash) { throw "$name SPIR-V SHA-256 does not match manifest." }

        $dxilReflection = Read-DxilReflection -File $dxilArtifact.file -Compiler $compilerFile.FullName -ExpectedStage $stage -ExpectedEntryPoint $entryPoint -Context "$name DXIL"
        $spirvReflection = Read-SpirvReflection -File $spirvArtifact.file -ExpectedStage $stage -ExpectedEntryPoint $entryPoint -Context "$name SPIR-V"
        $dxilReflection | Add-Member -NotePropertyName hash -NotePropertyValue $dxilArtifact.expectedHash
        $spirvReflection | Add-Member -NotePropertyName hash -NotePropertyValue $spirvArtifact.expectedHash
        $dxilExpected = Get-ExpectedReflection -Artifact $artifact -BackendSpec $dxilSpec
        $spirvExpected = Get-ExpectedReflection -Artifact $artifact -BackendSpec $spirvSpec
        Compare-ExpectedReflection -Actual $dxilReflection -Reflection $dxilExpected -Context "$name DXIL" -Artifact $name -Backend 'dxil'
        Compare-ExpectedReflection -Actual $spirvReflection -Reflection $spirvExpected -Context "$name SPIR-V" -Artifact $name -Backend 'spirv'
        Compare-BackendAbi -Dxil $dxilReflection -Spirv $spirvReflection -Artifact $name
        [void]$artifactSummaries.Add([ordered]@{
            name = $name
            stage = $stage
            entrypoint = $entryPoint
            dxil = $dxilReflection
            spirv = $spirvReflection
            abi = [ordered]@{
                resourceBindingCount = $dxilReflection.resourceBindingCount
                resources = @($dxilReflection.resources)
                crossBackendEqual = @($script:Issues | Where-Object { $_.artifact -eq $name -and $_.code -like 'backend-*' -and $_.severity -eq 'error' }).Count -eq 0
            }
        })
    }

    if ($firstSpirvPath) {
        $spirvProbe = Invoke-ProcessCapture -Executable $compilerFile.FullName -Arguments @('-dumpbin', $firstSpirvPath)
        $script:CompilerSpirvDumpbin = [ordered]@{
            attempted = $true
            supported = ($spirvProbe.ExitCode -eq 0)
            exitCode = $spirvProbe.ExitCode
            diagnostic = if ($spirvProbe.ExitCode -eq 0) {
                'DXC accepted -dumpbin for SPIR-V; verifier still uses its independent parser.'
            } else {
                $probeDiagnostic = ([string]$spirvProbe.Stderr).Trim()
                if ([string]::IsNullOrWhiteSpace($probeDiagnostic)) { $probeDiagnostic = ([string]$spirvProbe.Stdout).Trim() }
                $probeDiagnostic = ($probeDiagnostic -replace '\s+', ' ')
                if ($probeDiagnostic.Length -gt 512) { $probeDiagnostic.Substring(0, 512) } else { $probeDiagnostic }
            }
        }
    }

    $toolchainDisplayPath = if ($null -ne $toolchain) {
        $declaredToolchainPath = Get-FieldValue -Object $toolchain -Names @('path', 'file', 'executable')
        if ($declaredToolchainPath) { [string]$declaredToolchainPath } else { $compilerFile.Name }
    } else { $compilerFile.Name }
    $summary = [ordered]@{
        schema = 'noemancer.shader-artifact-reflection-summary/0.1'
        status = if (@($script:Issues | Where-Object severity -eq 'error').Count -eq 0) { 'passed' } else { 'failed' }
        manifest = [ordered]@{ path = $manifestFile.Name; schema = [string]$schemaValue; bytes = $manifestFile.Length }
        compiler = [ordered]@{ path = $toolchainDisplayPath; version = $script:CompilerVersion; dxcSpirvDumpbin = $script:CompilerSpirvDumpbin }
        artifacts = @($artifactSummaries)
        issues = @($script:Issues)
    }
} catch {
    Add-Issue -Severity error -Code 'fatal' -Message $_.Exception.Message
    $summary = [ordered]@{
        schema = 'noemancer.shader-artifact-reflection-summary/0.1'
        status = 'failed'
        manifest = if ($script:ManifestFullPath) { [ordered]@{ path = [System.IO.Path]::GetFileName($script:ManifestFullPath) } } else { $null }
        compiler = [ordered]@{ path = [System.IO.Path]::GetFileName($CompilerPath); version = $script:CompilerVersion; dxcSpirvDumpbin = $script:CompilerSpirvDumpbin }
        artifacts = @()
        issues = @($script:Issues)
    }
}

$json = Convert-SummaryJson -Summary $summary
if ($OutputPath) {
    try {
        $outputFullPath = [System.IO.Path]::GetFullPath($OutputPath)
        $parent = Split-Path -Parent $outputFullPath
        if ($parent) { [System.IO.Directory]::CreateDirectory($parent) | Out-Null }
        [System.IO.File]::WriteAllText($outputFullPath, $json + [Environment]::NewLine, [System.Text.UTF8Encoding]::new($false))
    } catch {
        Write-Error "Unable to write OutputPath '$OutputPath': $($_.Exception.Message)"
        exit 1
    }
}
if (-not $Quiet) { Write-Output $json }
if ($summary.status -ne 'passed') { exit 1 }
exit 0
