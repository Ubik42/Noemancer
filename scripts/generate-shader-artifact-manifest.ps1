[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(Mandatory = $true)]
    [string]$SourceContract,

    [Parameter(Mandatory = $true)]
    [string]$SourceRoot,

    [Parameter(Mandatory = $true)]
    [string]$ArtifactRoot,

    [Parameter(Mandatory = $true)]
    [string]$OutputPath,

    [Parameter(Mandatory = $true)]
    [string]$CompilerPath,

    [Parameter(Mandatory = $true)]
    [string]$CompilerVersion
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$SourceContractSchema = 'noemancer.shader-artifact-source-contract/0.1'
$ManifestSchema = 'noemancer.shader-artifact-manifest/0.1'
$ExpectedShaderCount = 29
$ResourceNames = @(
    'samplers',
    'uniformBuffers',
    'storageBuffers',
    'readonlyStorageBuffers',
    'readwriteStorageBuffers'
)

function Resolve-RequiredFile {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Description
    )

    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw "$Description path is empty."
    }
    $fullPath = [System.IO.Path]::GetFullPath($Path)
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
        throw "$Description is missing: $fullPath"
    }
    return $fullPath
}

function Resolve-RequiredDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Description
    )

    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw "$Description path is empty."
    }
    $fullPath = [System.IO.Path]::GetFullPath($Path)
    if (-not (Test-Path -LiteralPath $fullPath -PathType Container)) {
        throw "$Description is missing: $fullPath"
    }
    return $fullPath
}

function Get-RequiredProperty {
    param(
        [Parameter(Mandatory = $true)][object]$Object,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Context
    )

    if ($null -eq $Object) {
        throw "$Context is missing; expected property '$Name'."
    }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) {
        throw "$Context is missing required property '$Name'."
    }
    return $property.Value
}

function Get-OptionalProperty {
    param(
        [Parameter(Mandatory = $true)][object]$Object,
        [Parameter(Mandatory = $true)][string]$Name
    )

    if ($null -eq $Object) { return $null }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}

function Get-RequiredString {
    param(
        [Parameter(Mandatory = $true)][object]$Object,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Context
    )

    $value = Get-RequiredProperty -Object $Object -Name $Name -Context $Context
    if ($value -isnot [string] -or [string]::IsNullOrWhiteSpace($value)) {
        throw "$Context.$Name must be a non-empty string."
    }
    return [string]$value
}

function ConvertTo-NonNegativeInteger {
    param(
        [Parameter(Mandatory = $true)][object]$Value,
        [Parameter(Mandatory = $true)][string]$Context
    )

    if ($Value -is [bool] -or $Value -isnot [System.ValueType]) {
        throw "$Context must be a non-negative integer."
    }
    try {
        $number = [int64]$Value
    } catch {
        throw "$Context must be a non-negative integer."
    }
    if ($number -lt 0 -or ([double]$Value -ne [double]$number)) {
        throw "$Context must be a non-negative integer."
    }
    return $number
}

function Get-FileIdentity {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Description,
        [string]$RelativePath
    )

    $item = Get-Item -LiteralPath $Path -ErrorAction Stop
    if (-not ($item -is [System.IO.FileInfo])) {
        throw "$Description is not a regular file: $Path"
    }
    $hash = 'sha256:' + (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    $displayPath = if ([string]::IsNullOrWhiteSpace($RelativePath)) {
        [System.IO.Path]::GetFileName($item.FullName)
    } else {
        $RelativePath.Replace('\', '/')
    }
    return [ordered]@{
        path = $displayPath
        bytes = [int64]$item.Length
        sha256 = $hash
    }
}

function Get-ExpectedStage {
    param([Parameter(Mandatory = $true)][string]$Stem)

    $suffix = ($Stem -split '\.')[-1]
    switch ($suffix) {
        'vert' { return 'vertex' }
        'frag' { return 'fragment' }
        'comp' { return 'compute' }
        default { throw "Shader stem '$Stem' must end in .vert, .frag, or .comp." }
    }
}

function Get-ExpectedProfile {
    param([Parameter(Mandatory = $true)][string]$Stage)

    switch ($Stage) {
        'vertex' { return 'vs_6_0' }
        'fragment' { return 'ps_6_0' }
        'compute' { return 'cs_6_0' }
        default { throw "Unsupported shader stage '$Stage'." }
    }
}

function ConvertTo-PortableRelativePath {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$Path
    )

    try {
        $relative = [System.IO.Path]::GetRelativePath($Root, $Path)
    } catch {
        return [System.IO.Path]::GetFileName($Path).Replace('\', '/')
    }
    return $relative.Replace('\', '/')
}

try {
    $contractPath = Resolve-RequiredFile -Path $SourceContract -Description 'Shader source contract'
    $sourceRootPath = Resolve-RequiredDirectory -Path $SourceRoot -Description 'Shader source root'
    # CMake passes the repository root so that the command remains relocatable;
    # direct callers may pass assets/shaders. Normalize both forms before the
    # contract/source set comparison.
    $directSourceFiles = @(Get-ChildItem -LiteralPath $sourceRootPath -File -Filter '*.hlsl' -ErrorAction SilentlyContinue)
    if ($directSourceFiles.Count -eq 0) {
        $nestedShaderRoot = Join-Path $sourceRootPath 'assets/shaders'
        if (Test-Path -LiteralPath $nestedShaderRoot -PathType Container) {
            $sourceRootPath = [System.IO.Path]::GetFullPath($nestedShaderRoot)
        }
    }
    $artifactRootPath = Resolve-RequiredDirectory -Path $ArtifactRoot -Description 'Shader artifact root'
    $compilerPath = Resolve-RequiredFile -Path $CompilerPath -Description 'DXC compiler'

    if ([string]::IsNullOrWhiteSpace($CompilerVersion)) {
        throw 'CompilerVersion must be a non-empty toolchain identity.'
    }

    try {
        $contract = Get-Content -LiteralPath $contractPath -Raw -Encoding UTF8 | ConvertFrom-Json
    } catch {
        throw "Shader source contract is not valid JSON: $contractPath ($($_.Exception.Message))"
    }
    if ($null -eq $contract) {
        throw "Shader source contract is empty: $contractPath"
    }

    $contractSchema = Get-OptionalProperty -Object $contract -Name 'schema'
    if ($null -eq $contractSchema) {
        $contractSchema = Get-OptionalProperty -Object $contract -Name 'schemaVersion'
    }
    if ($contractSchema -ne $SourceContractSchema) {
        throw "Shader source contract schema '$contractSchema' is unsupported; expected '$SourceContractSchema'."
    }
    $sourceExtension = Get-RequiredString -Object $contract -Name 'sourceExtension' -Context 'Shader source contract'
    if ($sourceExtension -ne '.hlsl') {
        throw "Shader source contract.sourceExtension must be '.hlsl'; found '$sourceExtension'."
    }

    $contractShaders = Get-RequiredProperty -Object $contract -Name 'shaders' -Context 'Shader source contract'
    if ($contractShaders -is [string] -or $contractShaders -isnot [System.Collections.IEnumerable]) {
        throw 'Shader source contract.shaders must be an array.'
    }
    $entries = @($contractShaders)
    if ($entries.Count -ne $ExpectedShaderCount) {
        throw "Shader source contract must contain exactly $ExpectedShaderCount shaders; found $($entries.Count)."
    }

    $sourceFiles = @(Get-ChildItem -LiteralPath $sourceRootPath -File -Filter '*.hlsl' -ErrorAction Stop)
    if ($sourceFiles.Count -ne $ExpectedShaderCount) {
        throw "Shader source root must contain exactly $ExpectedShaderCount HLSL files; found $($sourceFiles.Count): $sourceRootPath"
    }
    $sourceStems = @($sourceFiles | ForEach-Object {
        $_.Name.Substring(0, $_.Name.Length - '.hlsl'.Length)
    } | Sort-Object)

    $contractStems = [System.Collections.Generic.List[string]]::new()
    foreach ($entry in $entries) {
        $stem = Get-RequiredString -Object $entry -Name 'stem' -Context 'Shader source contract entry'
        if ($stem -notmatch '^[A-Za-z0-9_-]+\.(vert|frag|comp)$') {
            throw "Shader stem '$stem' must be a simple .vert, .frag, or .comp filename stem."
        }
        $contractStems.Add($stem)
    }
    $sortedContractStems = @($contractStems | Sort-Object)
    $uniqueContractStems = @($sortedContractStems | Select-Object -Unique)
    if ($uniqueContractStems.Count -ne $sortedContractStems.Count) {
        throw 'Shader source contract contains duplicate shader stems.'
    }
    $stemDifferences = @(Compare-Object -ReferenceObject $sourceStems -DifferenceObject $sortedContractStems)
    if ($stemDifferences.Count -ne 0) {
        $differenceText = ($stemDifferences | ForEach-Object {
            if ($_.SideIndicator -eq '<=') { "missing contract entry '$($_.InputObject)'" }
            else { "contract entry has no HLSL source '$($_.InputObject)'" }
        }) -join '; '
        throw "Shader source contract does not exactly match HLSL sources: $differenceText"
    }

    # These values are the SDL_GPU pipeline ABI in scene_renderer.cpp. Keep the
    # explicit checks here so a stale source contract cannot silently describe a
    # different compute binding layout or dispatch width.
    $expectedComputeBindings = @{
        'gpu_visibility.comp' = [ordered]@{ uniformBuffers = 1; readonlyStorageBuffers = 2; readwriteStorageBuffers = 2; threadGroupX = 64 }
        'vfx_group.comp' = [ordered]@{ uniformBuffers = 1; readonlyStorageBuffers = 0; readwriteStorageBuffers = 7; threadGroupX = 64 }
        'vfx_sim.comp' = [ordered]@{ uniformBuffers = 1; readonlyStorageBuffers = 0; readwriteStorageBuffers = 7; threadGroupX = 64 }
        'vfx_sort_alpha.comp' = [ordered]@{ uniformBuffers = 1; readonlyStorageBuffers = 0; readwriteStorageBuffers = 3; threadGroupX = 256 }
        'vfx_spawn.comp' = [ordered]@{ uniformBuffers = 1; readonlyStorageBuffers = 0; readwriteStorageBuffers = 7; threadGroupX = 64 }
    }

    $manifestShaders = [System.Collections.Generic.List[object]]::new()
    foreach ($entry in ($entries | Sort-Object { [string](Get-RequiredProperty -Object $_ -Name 'stem' -Context 'Shader source contract entry') })) {
        $stem = Get-RequiredString -Object $entry -Name 'stem' -Context 'Shader source contract entry'
        $stage = Get-RequiredString -Object $entry -Name 'stage' -Context "Shader '$stem'"
        $profile = Get-RequiredString -Object $entry -Name 'profile' -Context "Shader '$stem'"
        $entrypoint = Get-RequiredString -Object $entry -Name 'entrypoint' -Context "Shader '$stem'"
        $expectedStage = Get-ExpectedStage -Stem $stem
        $expectedProfile = Get-ExpectedProfile -Stage $expectedStage
        if ($stage -ne $expectedStage) {
            throw "Shader '$stem' declares stage '$stage'; expected '$expectedStage'."
        }
        if ($profile -ne $expectedProfile) {
            throw "Shader '$stem' declares profile '$profile'; expected '$expectedProfile'."
        }
        if ($entrypoint -ne 'main') {
            throw "Shader '$stem' must use entrypoint 'main'; found '$entrypoint'."
        }

        $resourceDocument = Get-RequiredProperty -Object $entry -Name 'resources' -Context "Shader '$stem'"
        $resources = [ordered]@{}
        foreach ($resourceName in $ResourceNames) {
            $resourceValue = Get-RequiredProperty -Object $resourceDocument -Name $resourceName -Context "Shader '$stem'.resources"
            $resources[$resourceName] = ConvertTo-NonNegativeInteger -Value $resourceValue -Context "Shader '$stem'.resources.$resourceName"
        }
        if ($stage -eq 'compute' -and $resources.storageBuffers -ne 0) {
            throw "Compute shader '$stem' must use readonlyStorageBuffers/readwriteStorageBuffers rather than storageBuffers."
        }
        if ($stage -ne 'compute' -and ($resources.readonlyStorageBuffers -ne 0 -or $resources.readwriteStorageBuffers -ne 0)) {
            throw "Graphics shader '$stem' cannot declare compute storage buffer counts."
        }

        $threadGroupValue = Get-OptionalProperty -Object $entry -Name 'threadGroup'
        $threadGroup = $null
        if ($stage -eq 'compute') {
            if ($null -eq $threadGroupValue -or $threadGroupValue -is [string] -or $threadGroupValue -isnot [System.Collections.IEnumerable]) {
                throw "Compute shader '$stem' must declare threadGroup as [x, y, z]."
            }
            $threadValues = @($threadGroupValue)
            if ($threadValues.Count -ne 3) {
                throw "Compute shader '$stem' threadGroup must contain exactly three integers."
            }
            $threadGroup = @(
                (ConvertTo-NonNegativeInteger -Value $threadValues[0] -Context "Shader '$stem'.threadGroup[0]"),
                (ConvertTo-NonNegativeInteger -Value $threadValues[1] -Context "Shader '$stem'.threadGroup[1]"),
                (ConvertTo-NonNegativeInteger -Value $threadValues[2] -Context "Shader '$stem'.threadGroup[2]")
            )
            if ($threadGroup[0] -lt 1 -or $threadGroup[1] -lt 1 -or $threadGroup[2] -lt 1) {
                throw "Compute shader '$stem' threadGroup dimensions must be positive."
            }
        } elseif ($null -ne $threadGroupValue) {
            throw "Graphics shader '$stem' must not declare a compute threadGroup."
        }

        if ($expectedComputeBindings.ContainsKey($stem)) {
            $expected = $expectedComputeBindings[$stem]
            foreach ($resourceName in @('uniformBuffers', 'readonlyStorageBuffers', 'readwriteStorageBuffers')) {
                if ($resources[$resourceName] -ne $expected[$resourceName]) {
                    throw "Shader '$stem' has resources.$resourceName=$($resources[$resourceName]); expected $($expected[$resourceName]) from the SDL_GPU pipeline call."
                }
            }
            if ($threadGroup[0] -ne $expected.threadGroupX -or $threadGroup[1] -ne 1 -or $threadGroup[2] -ne 1) {
                throw "Shader '$stem' has threadGroup [$($threadGroup -join ',')]; expected [$($expected.threadGroupX),1,1]."
            }
        }

        $sourceRelativePath = "$stem.hlsl"
        $dxilRelativePath = "$stem.dxil"
        $spvRelativePath = "$stem.spv"
        $sourcePath = Join-Path $sourceRootPath $sourceRelativePath
        $dxilPath = Join-Path $artifactRootPath $dxilRelativePath
        $spvPath = Join-Path $artifactRootPath $spvRelativePath
        $sourceIdentity = Get-FileIdentity -Path $sourcePath -Description "Shader source '$stem'" -RelativePath $sourceRelativePath
        $dxilIdentity = Get-FileIdentity -Path $dxilPath -Description "DXIL artifact '$stem'" -RelativePath $dxilRelativePath
        $spvIdentity = Get-FileIdentity -Path $spvPath -Description "SPIR-V artifact '$stem'" -RelativePath $spvRelativePath

        $manifestShaders.Add([ordered]@{
            stem = $stem
            stage = $stage
            profile = $profile
            entrypoint = $entrypoint
            resources = $resources
            threadGroup = $threadGroup
            source = $sourceIdentity
            dxil = $dxilIdentity
            spv = $spvIdentity
        })
    }

    $repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
    $contractDisplayPath = ConvertTo-PortableRelativePath -Root $repositoryRoot -Path $contractPath
    $compilerDisplayPath = ConvertTo-PortableRelativePath -Root $repositoryRoot -Path $compilerPath
    $contractIdentity = Get-FileIdentity -Path $contractPath -Description 'Shader source contract' -RelativePath $contractDisplayPath
    $compilerIdentity = Get-FileIdentity -Path $compilerPath -Description 'DXC compiler' -RelativePath $compilerDisplayPath
    $compileArguments = [ordered]@{
        dxil = @(
            '-Zpc', '-E', 'main', '-T', '{profile}', '-O3',
            '-Qstrip_debug', '-Qstrip_reflect', '-Fo', '{dxil}', '{source}'
        )
        spv = @(
            '-spirv', '-Zpc', '-fspv-target-env=vulkan1.1', '-E', 'main',
            '-T', '{profile}', '-O3', '-Fo', '{spv}', '{source}'
        )
    }
    $toolchain = [ordered]@{
        name = 'DirectXShaderCompiler'
        path = $compilerIdentity.path
        executable = [System.IO.Path]::GetFileName($compilerPath)
        version = [string]$CompilerVersion
        bytes = $compilerIdentity.bytes
        sha256 = $compilerIdentity.sha256
    }

    $manifest = [ordered]@{
        schema = $ManifestSchema
        sourceContract = [ordered]@{
            schema = $SourceContractSchema
            file = $contractIdentity.path
            bytes = $contractIdentity.bytes
            sha256 = $contractIdentity.sha256
        }
        toolchain = $toolchain
        compileArguments = $compileArguments
        shaderCount = $manifestShaders.Count
        shaders = @($manifestShaders)
    }

    $outputFullPath = [System.IO.Path]::GetFullPath($OutputPath)
    if (Test-Path -LiteralPath $outputFullPath -PathType Container) {
        throw "Manifest output path is a directory: $outputFullPath"
    }
    $outputDirectory = [System.IO.Path]::GetDirectoryName($outputFullPath)
    if ([string]::IsNullOrWhiteSpace($outputDirectory)) {
        throw "Manifest output path has no parent directory: $OutputPath"
    }
    [System.IO.Directory]::CreateDirectory($outputDirectory) | Out-Null
    $json = $manifest | ConvertTo-Json -Depth 20
    $temporaryPath = "$outputFullPath.$([System.Guid]::NewGuid().ToString('N')).tmp"
    try {
        $utf8NoBom = [System.Text.UTF8Encoding]::new($false)
        [System.IO.File]::WriteAllText($temporaryPath, $json, $utf8NoBom)
        # The temporary file is created beside the destination, so Move-Item's
        # replacement stays on one volume and is an atomic rename on Windows.
        Move-Item -LiteralPath $temporaryPath -Destination $outputFullPath -Force
        $temporaryPath = $null
    } finally {
        if ($null -ne $temporaryPath -and (Test-Path -LiteralPath $temporaryPath -PathType Leaf)) {
            Remove-Item -LiteralPath $temporaryPath -Force -ErrorAction SilentlyContinue
        }
    }

    [pscustomobject]@{
        Success = $true
        Manifest = $outputFullPath
        ShaderCount = $manifestShaders.Count
        SourceContract = $contractPath
        ArtifactRoot = $artifactRootPath
        CompilerVersion = [string]$CompilerVersion
    }
    exit 0
} catch {
    Write-Error "Shader artifact manifest generation failed: $($_.Exception.Message)"
    exit 1
}
