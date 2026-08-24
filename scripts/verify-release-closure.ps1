[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$PackageRoot,

    # When supplied, the receipt is written as UTF-8 JSON at this path. Without
    # it the final receipt is emitted as one JSON document on stdout.
    [string]$ReceiptPath,

    # HeadlessProbe is deliberately opt-in: ordinary package validation must
    # never start a Player or create a visible window.
    [switch]$HeadlessProbe,

    [ValidateRange(1, 600)]
    [int]$Frames = 2,

    [ValidateRange(1, 600)]
    [int]$TimeoutSeconds = 30
)

# Exit contract:
#   0 = package closure passed (and, when requested, the headless probe passed)
#   2 = invalid invocation or package root
#   3 = package closure failed
#   4 = package shape passed but the optional headless probe failed
#   5 = the requested receipt could not be written
#   1 = unexpected script failure

$ErrorActionPreference = 'Stop'
$script:Issues = New-Object System.Collections.ArrayList
$script:RootPath = $null
$script:TemporaryProbeRoot = $null
$script:ReleaseGameProfileSchema = 'noemancer.game-profile/0.4'

function Add-Issue {
    param(
        [Parameter(Mandatory = $true)][string]$Code,
        [Parameter(Mandatory = $true)][string]$Stage,
        [Parameter(Mandatory = $true)][string]$Message,
        [string]$Path
    )

    $issue = [ordered]@{
        code = $Code
        stage = $Stage
        message = $Message
    }
    if (-not [string]::IsNullOrWhiteSpace($Path)) {
        $issue.path = $Path
    }
    [void]$script:Issues.Add($issue)
}

function Get-PropertyValue {
    param(
        [AllowNull()]$Object,
        [Parameter(Mandatory = $true)][string]$Name
    )

    if ($null -eq $Object) { return $null }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}

function Get-RelativePackagePath {
    param([Parameter(Mandatory = $true)][string]$FullPath)

    if ($null -eq $script:RootPath) { return $FullPath }
    $relative = $FullPath.Substring($script:RootPath.Length).TrimStart('\', '/')
    if ([string]::IsNullOrEmpty($relative)) { return '.' }
    return ($relative -replace '\\', '/')
}

function Resolve-PackageRelativePath {
    param(
        [Parameter(Mandatory = $true)][string]$RelativePath,
        [Parameter(Mandatory = $true)][string]$Stage,
        [Parameter(Mandatory = $true)][string]$Field
    )

    if ([string]::IsNullOrWhiteSpace($RelativePath)) {
        Add-Issue -Code 'path.missing' -Stage $Stage -Path $Field -Message 'A required package-relative path is missing.'
        return $null
    }

    $candidate = $RelativePath -replace '/', '\\'
    if ([IO.Path]::IsPathRooted($candidate) -or $candidate -match '(^|\\)\.\.(\\|$)') {
        Add-Issue -Code 'path.not-package-relative' -Stage $Stage -Path $Field -Message "Package paths must be relative and may not escape the package root: $RelativePath"
        return $null
    }

    try {
        $full = [IO.Path]::GetFullPath((Join-Path $script:RootPath $candidate))
        $rootWithSeparator = $script:RootPath.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
        if (-not ($full.Equals($script:RootPath, [StringComparison]::OrdinalIgnoreCase) -or
                $full.StartsWith($rootWithSeparator, [StringComparison]::OrdinalIgnoreCase))) {
            Add-Issue -Code 'path.outside-package' -Stage $Stage -Path $Field -Message "Resolved package path escapes the package root: $RelativePath"
            return $null
        }
        return $full
    } catch {
        Add-Issue -Code 'path.invalid' -Stage $Stage -Path $Field -Message "Could not resolve package path '$RelativePath': $($_.Exception.Message)"
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
        Add-Issue -Code 'file.missing' -Stage $Stage -Path (Get-RelativePackagePath $Path) -Message "$Label is missing."
        return $null
    }

    try {
        $raw = Get-Content -LiteralPath $Path -Raw -Encoding UTF8
        if ([string]::IsNullOrWhiteSpace($raw)) {
            Add-Issue -Code 'json.empty' -Stage $Stage -Path (Get-RelativePackagePath $Path) -Message "$Label is empty."
            return $null
        }
        return ($raw | ConvertFrom-Json)
    } catch {
        Add-Issue -Code 'json.invalid' -Stage $Stage -Path (Get-RelativePackagePath $Path) -Message "$Label is not valid JSON: $($_.Exception.Message)"
        return $null
    }
}

function Get-StageResult {
    param(
        [Parameter(Mandatory = $true)][bool]$Pass,
        [Parameter(Mandatory = $true)][int]$IssueCount,
        [object]$Details
    )

    $result = [ordered]@{
        pass = $Pass
        issueCount = $IssueCount
    }
    if ($null -ne $Details) {
        foreach ($key in $Details.Keys) { $result[$key] = $Details[$key] }
    }
    return $result
}

function Test-TextPathLeaks {
    param([Parameter(Mandatory = $true)][string]$Root)

    $textExtensions = @(
        '.json', '.jsonl', '.txt', '.md', '.xml', '.ini', '.cfg', '.config',
        '.manifest', '.props', '.targets', '.csproj', '.sln', '.cs', '.h',
        '.hpp', '.cpp', '.hlsl', '.glsl', '.vert', '.frag', '.comp', '.shader'
    )
    $leaks = New-Object System.Collections.ArrayList
    $absolutePatterns = @(
        '(?i)(?<![A-Za-z0-9])(?:[A-Z]:[\\/][^\r\n"''<>]+)',
        '(?i)(?<![A-Za-z0-9])(?:\\\\[A-Za-z0-9_.-]+[\\/][^\r\n"''<>]+)',
        '(?<![A-Za-z0-9])/(?:home|Users|root|workspace|workspaces|mnt|opt|private)/[^\r\n"''<>]+'
    )

    foreach ($file in @(Get-ChildItem -LiteralPath $Root -Recurse -File -ErrorAction SilentlyContinue)) {
        if ($textExtensions -notcontains $file.Extension.ToLowerInvariant()) { continue }
        if ($file.Length -gt 8MB) { continue }

        try { $content = Get-Content -LiteralPath $file.FullName -Raw -Encoding UTF8 } catch { continue }
        if ([string]::IsNullOrEmpty($content)) { continue }

        $lineNumber = 0
        foreach ($line in ($content -split "`r?`n")) {
            $lineNumber++
            foreach ($pattern in $absolutePatterns) {
                $match = [regex]::Match($line, $pattern)
                if (-not $match.Success) { continue }
                $value = $match.Value.Trim()
                # A URL is not a filesystem leak; the regex intentionally does
                # not match it, but this guard keeps the contract explicit.
                if ($value -match '^(?i)https?://') { continue }
                [void]$leaks.Add([ordered]@{
                    file = Get-RelativePackagePath $file.FullName
                    line = $lineNumber
                    value = if ($value.Length -gt 240) { $value.Substring(0, 240) + '…' } else { $value }
                })
                break
            }
        }
    }
    return @($leaks)
}

function Invoke-HeadlessPlayerProbe {
    param(
        [Parameter(Mandatory = $true)][string]$PlayerPath,
        [Parameter(Mandatory = $true)][string]$ProfilePath
    )

    $probe = [ordered]@{
        requested = $true
        pass = $false
        exitCode = $null
        timedOut = $false
        durationMs = $null
        outputLines = 0
        events = @()
    }

    $probeRoot = Join-Path ([IO.Path]::GetTempPath()) ('noemancer-release-closure-' + [Guid]::NewGuid().ToString('N'))
    $script:TemporaryProbeRoot = $probeRoot
    New-Item -ItemType Directory -Path $probeRoot -Force | Out-Null
    $stdoutPath = Join-Path $probeRoot 'stdout.jsonl'
    $stderrPath = Join-Path $probeRoot 'stderr.txt'
    $started = [DateTime]::UtcNow
    $process = $null
    try {
        $arguments = @(
            'player', '--profile', $ProfilePath, '--headless', '--frames', [string]$Frames,
            '--format', 'json'
        )
        $process = Start-Process -FilePath $PlayerPath -ArgumentList $arguments -WorkingDirectory $script:RootPath `
            -WindowStyle Hidden -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath -PassThru

        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            $probe.timedOut = $true
            try { $process.Kill($true) } catch { }
            $process.WaitForExit()
        }
        $probe.exitCode = $process.ExitCode
        $probe.durationMs = [int]([DateTime]::UtcNow - $started).TotalMilliseconds

        if (Test-Path -LiteralPath $stdoutPath -PathType Leaf) {
            $lines = @(Get-Content -LiteralPath $stdoutPath -Encoding UTF8)
            $probe.outputLines = $lines.Count
            $events = New-Object System.Collections.ArrayList
            foreach ($line in $lines) {
                if ([string]::IsNullOrWhiteSpace($line)) { continue }
                try {
                    $event = $line | ConvertFrom-Json
                    if ($null -ne (Get-PropertyValue $event 'event')) {
                        [void]$events.Add([string](Get-PropertyValue $event 'event'))
                    } elseif ($null -ne (Get-PropertyValue $event 'type')) {
                        [void]$events.Add([string](Get-PropertyValue $event 'type'))
                    }
                } catch { }
            }
            $probe.events = @($events | Select-Object -Unique)
        }

        $probe.pass = (-not $probe.timedOut) -and $probe.exitCode -eq 0 -and $probe.outputLines -gt 0
        if (-not $probe.pass) {
            $detail = "Player headless probe did not complete successfully (exit=$($probe.exitCode), timedOut=$($probe.timedOut), outputLines=$($probe.outputLines))."
            Add-Issue -Code 'player.headless-failed' -Stage 'headlessProbe' -Path (Get-RelativePackagePath $PlayerPath) -Message $detail
        }
    } catch {
        $probe.durationMs = [int]([DateTime]::UtcNow - $started).TotalMilliseconds
        Add-Issue -Code 'player.headless-error' -Stage 'headlessProbe' -Path (Get-RelativePackagePath $PlayerPath) -Message "Could not execute the headless Player probe: $($_.Exception.Message)"
    } finally {
        if ($null -ne $process) { $process.Dispose() }
    }
    return $probe
}

function Write-ReceiptAndExit {
    param(
        [Parameter(Mandatory = $true)][object]$Receipt,
        [Parameter(Mandatory = $true)][int]$ExitCode
    )

    $json = $Receipt | ConvertTo-Json -Depth 30
    if (-not [string]::IsNullOrWhiteSpace($ReceiptPath)) {
        try {
            $parent = Split-Path -Parent $ReceiptPath
            if (-not [string]::IsNullOrWhiteSpace($parent)) {
                New-Item -ItemType Directory -Path $parent -Force | Out-Null
            }
            Set-Content -LiteralPath $ReceiptPath -Value $json -Encoding UTF8
        } catch {
            [Console]::Error.WriteLine("Could not write release closure receipt '$ReceiptPath': $($_.Exception.Message)")
            exit 5
        }
    }
    [Console]::Out.WriteLine($json)
    exit $ExitCode
}

try {
    try {
        $resolvedRoot = (Resolve-Path -LiteralPath $PackageRoot -ErrorAction Stop).Path
        if (-not (Test-Path -LiteralPath $resolvedRoot -PathType Container)) {
            throw 'Package root is not a directory.'
        }
        $script:RootPath = [IO.Path]::GetFullPath($resolvedRoot).TrimEnd('\', '/')
    } catch {
        $earlyReceipt = [ordered]@{
            schema = 'noemancer.release-closure-receipt/0.1'
            success = $false
            packageRoot = $PackageRoot
            issues = @([ordered]@{ code = 'package-root-invalid'; stage = 'packageRoot'; message = $_.Exception.Message })
        }
        Write-ReceiptAndExit -Receipt $earlyReceipt -ExitCode 2
    }

    $profilePath = Join-Path $script:RootPath 'config\game-profile.json'
    $profile = Read-JsonFile -Path $profilePath -Stage 'gameProfile' -Label 'Game Profile'
    $profilePass = $null -ne $profile
    if ($null -ne $profile) {
        $schema = [string](Get-PropertyValue $profile 'schema')
        if ($schema -ne $script:ReleaseGameProfileSchema) {
            Add-Issue -Code 'game-profile.schema-invalid' -Stage 'gameProfile' -Path 'config/game-profile.json' -Message "Release closure requires $($script:ReleaseGameProfileSchema), got '$schema'."
            $profilePass = $false
        }
        foreach ($field in @('id', 'displayName', 'platform', 'configuration', 'executable', 'assetRegistry')) {
            if ([string]::IsNullOrWhiteSpace([string](Get-PropertyValue $profile $field))) {
                Add-Issue -Code 'game-profile.field-missing' -Stage 'gameProfile' -Path "config/game-profile.json/$field" -Message "Game Profile field '$field' is required."
                $profilePass = $false
            }
        }
        if ([string](Get-PropertyValue $profile 'platform') -ne 'windows' -or
            [string](Get-PropertyValue $profile 'configuration') -ne 'release') {
            Add-Issue -Code 'game-profile.release-required' -Stage 'gameProfile' -Path 'config/game-profile.json' -Message 'Release closure validation requires the Windows release Game Profile.'
            $profilePass = $false
        }
    }
    $profileIssueCount = $script:Issues.Count

    $playerPath = $null
    $registryPath = $null
    if ($null -ne $profile) {
        $playerRelative = Join-Path 'bin' ([string](Get-PropertyValue $profile 'executable'))
        $playerPath = Resolve-PackageRelativePath -RelativePath $playerRelative -Stage 'player' -Field '/executable'
        if ($null -eq $playerPath -or -not (Test-Path -LiteralPath $playerPath -PathType Leaf)) {
            Add-Issue -Code 'player.missing' -Stage 'player' -Path ($playerRelative -replace '\\', '/') -Message 'The Player executable declared by the Game Profile is missing from the package.'
        }

        $registryRelative = [string](Get-PropertyValue $profile 'assetRegistry')
        $registryPath = Resolve-PackageRelativePath -RelativePath $registryRelative -Stage 'assetRegistry' -Field '/assetRegistry'
    }
    $playerStageIssueCount = $script:Issues.Count
    $playerPass = $null -ne $playerPath -and (Test-Path -LiteralPath $playerPath -PathType Leaf)
    $playerStage = Get-StageResult -Pass $playerPass -IssueCount ($script:Issues.Count - $profileIssueCount) -Details ([ordered]@{
        declaredPath = if ($null -ne $profile) { 'bin/' + [string](Get-PropertyValue $profile 'executable') } else { $null }
        exists = $playerPass
    })

    $registry = if ($null -ne $registryPath) { Read-JsonFile -Path $registryPath -Stage 'assetRegistry' -Label 'Asset Registry' } else { $null }
    $assetRegistryPass = $null -ne $registry
    $assets = @()
    $assetById = @{}
    if ($null -ne $registry) {
        if ([string](Get-PropertyValue $registry 'schema') -ne 'noemancer.assets/0.1') {
            Add-Issue -Code 'asset-registry.schema-invalid' -Stage 'assetRegistry' -Path (Get-RelativePackagePath $registryPath) -Message 'Asset Registry schema must be noemancer.assets/0.1.'
            $assetRegistryPass = $false
        }
        $assetsProperty = Get-PropertyValue $registry 'assets'
        if ($null -eq $assetsProperty) {
            Add-Issue -Code 'asset-registry.assets-missing' -Stage 'assetRegistry' -Path (Get-RelativePackagePath $registryPath) -Message 'Asset Registry must contain an assets array.'
            $assetRegistryPass = $false
        } else {
            $assets = @($assetsProperty)
            foreach ($asset in $assets) {
                $id = [string](Get-PropertyValue $asset 'id')
                $relativeAssetPath = [string](Get-PropertyValue $asset 'path')
                $licenseId = [string](Get-PropertyValue $asset 'license')
                if ([string]::IsNullOrWhiteSpace($id)) {
                    Add-Issue -Code 'asset-registry.asset-id-missing' -Stage 'assetRegistry' -Message 'Every packaged asset requires a stable id.'
                    $assetRegistryPass = $false
                    continue
                }
                if ($assetById.ContainsKey($id)) {
                    Add-Issue -Code 'asset-registry.asset-id-duplicate' -Stage 'assetRegistry' -Path $id -Message "Asset id '$id' is duplicated."
                    $assetRegistryPass = $false
                } else { $assetById[$id] = $asset }
                if ([string]::IsNullOrWhiteSpace($relativeAssetPath)) {
                    Add-Issue -Code 'asset-registry.asset-path-missing' -Stage 'assetRegistry' -Path $id -Message "Asset '$id' is missing its package-relative path."
                    $assetRegistryPass = $false
                } elseif ([IO.Path]::IsPathRooted(($relativeAssetPath -replace '/', '\\'))) {
                    Add-Issue -Code 'asset-registry.asset-path-absolute' -Stage 'assetRegistry' -Path $id -Message "Asset '$id' must use a registry-relative path."
                    $assetRegistryPass = $false
                } else {
                    $registryDirectoryRelative = Split-Path (Get-RelativePackagePath $registryPath) -Parent
                    $assetPackageRelative = Join-Path $registryDirectoryRelative $relativeAssetPath
                    $resolvedAsset = Resolve-PackageRelativePath -RelativePath $assetPackageRelative -Stage 'assetRegistry' -Field $id
                    if ($null -eq $resolvedAsset -or -not (Test-Path -LiteralPath $resolvedAsset -PathType Leaf)) {
                        Add-Issue -Code 'asset-registry.asset-file-missing' -Stage 'assetRegistry' -Path $id -Message "Packaged asset '$relativeAssetPath' for '$id' is missing."
                        $assetRegistryPass = $false
                    }
                }
                if ([string]::IsNullOrWhiteSpace($licenseId)) {
                    Add-Issue -Code 'asset-registry.asset-license-missing' -Stage 'assetRegistry' -Path $id -Message "Packaged asset '$id' is missing its license reference."
                    $assetRegistryPass = $false
                }
            }
        }
    }
    $assetRegistryStage = Get-StageResult -Pass $assetRegistryPass -IssueCount ($script:Issues.Count - $playerStageIssueCount) -Details ([ordered]@{
        path = if ($null -ne $registryPath) { Get-RelativePackagePath $registryPath } else { $null }
        assetCount = $assets.Count
    })

    $licenseIssueStart = $script:Issues.Count
    $licensesPath = Join-Path $script:RootPath 'licenses\THIRD_PARTY.json'
    $noticePath = Join-Path $script:RootPath 'licenses\NOTICE.txt'
    $licenses = Read-JsonFile -Path $licensesPath -Stage 'licenseClosure' -Label 'THIRD_PARTY.json'
    $noticeText = $null
    if (-not (Test-Path -LiteralPath $noticePath -PathType Leaf)) {
        Add-Issue -Code 'file.missing' -Stage 'licenseClosure' -Path 'licenses/NOTICE.txt' -Message 'NOTICE.txt is missing.'
    } else {
        try { $noticeText = Get-Content -LiteralPath $noticePath -Raw -Encoding UTF8 } catch { Add-Issue -Code 'notice.read-failed' -Stage 'licenseClosure' -Path 'licenses/NOTICE.txt' -Message $_.Exception.Message }
        if ([string]::IsNullOrWhiteSpace($noticeText)) { Add-Issue -Code 'notice.empty' -Stage 'licenseClosure' -Path 'licenses/NOTICE.txt' -Message 'NOTICE.txt is empty.' }
    }
    $licenseMap = @{}
    $licensePass = $null -ne $licenses -and -not [string]::IsNullOrWhiteSpace($noticeText)
    if ($null -ne $licenses) {
        if ([string](Get-PropertyValue $licenses 'schema') -ne 'noemancer.third-party-licenses/0.1') {
            Add-Issue -Code 'license.schema-invalid' -Stage 'licenseClosure' -Path 'licenses/THIRD_PARTY.json' -Message 'THIRD_PARTY.json schema must be noemancer.third-party-licenses/0.1.'
            $licensePass = $false
        }
        $licenseEntries = Get-PropertyValue $licenses 'licenses'
        if ($null -eq $licenseEntries) {
            Add-Issue -Code 'license.entries-missing' -Stage 'licenseClosure' -Path 'licenses/THIRD_PARTY.json' -Message 'THIRD_PARTY.json must contain a licenses array.'
            $licensePass = $false
        } else {
            foreach ($license in @($licenseEntries)) {
                $id = [string](Get-PropertyValue $license 'id')
                $required = @('id', 'name', 'notice', 'redistributable', 'sourceUri', 'spdxId', 'thirdParty')
                foreach ($field in $required) {
                    if ($null -eq (Get-PropertyValue $license $field) -or
                        ($field -notin @('redistributable', 'thirdParty', 'sourceUri') -and [string]::IsNullOrWhiteSpace([string](Get-PropertyValue $license $field)))) {
                        Add-Issue -Code 'license.field-missing' -Stage 'licenseClosure' -Path "licenses/THIRD_PARTY.json/$field" -Message "License record '$id' is missing required field '$field'."
                        $licensePass = $false
                    }
                }
                if ([string]::IsNullOrWhiteSpace($id)) { continue }
                if ($licenseMap.ContainsKey($id)) {
                    Add-Issue -Code 'license.duplicate-id' -Stage 'licenseClosure' -Path $id -Message "License id '$id' is duplicated."
                    $licensePass = $false
                } else { $licenseMap[$id] = $license }
                if (-not [bool](Get-PropertyValue $license 'redistributable')) {
                    Add-Issue -Code 'license.not-redistributable' -Stage 'licenseClosure' -Path $id -Message "License '$id' is marked non-redistributable."
                    $licensePass = $false
                }
                if ([bool](Get-PropertyValue $license 'thirdParty') -and [string]::IsNullOrWhiteSpace([string](Get-PropertyValue $license 'notice'))) {
                    Add-Issue -Code 'license.notice-missing' -Stage 'licenseClosure' -Path $id -Message "Third-party license '$id' requires a non-empty notice."
                    $licensePass = $false
                }
                if ($null -ne $noticeText -and $noticeText.IndexOf("== $id |", [StringComparison]::Ordinal) -lt 0) {
                    Add-Issue -Code 'license.notice-not-closed' -Stage 'licenseClosure' -Path $id -Message "NOTICE.txt has no generated section for license '$id'."
                    $licensePass = $false
                }
            }
        }
    }
    foreach ($asset in $assets) {
        $id = [string](Get-PropertyValue $asset 'id')
        $licenseId = [string](Get-PropertyValue $asset 'license')
        if ([string]::IsNullOrWhiteSpace($licenseId)) { continue }
        if (-not $licenseMap.ContainsKey($licenseId)) {
            Add-Issue -Code 'license.reference-not-closed' -Stage 'licenseClosure' -Path $id -Message "Asset '$id' references license '$licenseId', which is absent from THIRD_PARTY.json."
            $licensePass = $false
        }
    }
    foreach ($packagedId in @((Get-PropertyValue $profile 'packagedAssets'))) {
        $packagedId = [string]$packagedId
        if ([string]::IsNullOrWhiteSpace($packagedId)) { continue }
        if (-not $assetById.ContainsKey($packagedId)) {
            Add-Issue -Code 'game-profile.asset-not-closed' -Stage 'licenseClosure' -Path '/packagedAssets' -Message "Game Profile packaged asset '$packagedId' is absent from Asset Registry."
            $licensePass = $false
        }
    }
    $licenseStage = Get-StageResult -Pass $licensePass -IssueCount ($script:Issues.Count - $licenseIssueStart) -Details ([ordered]@{
        manifest = 'licenses/THIRD_PARTY.json'
        notice = 'licenses/NOTICE.txt'
        licenseCount = $licenseMap.Count
    })

    $runtimeIssueStart = $script:Issues.Count
    $runtimePass = $true
    $runtimeRequirements = @((Get-PropertyValue $profile 'runtimeRequirements'))
    $runtimeMap = @{}
    foreach ($requirement in $runtimeRequirements) {
        $id = [string](Get-PropertyValue $requirement 'id')
        if ([string]::IsNullOrWhiteSpace($id)) {
            Add-Issue -Code 'runtime.requirement-id-missing' -Stage 'runtimeDependencies' -Message 'Runtime requirement id is missing.'
            $runtimePass = $false
            continue
        }
        $runtimeMap[$id] = $requirement
        if (-not [bool](Get-PropertyValue $requirement 'bundled')) {
            Add-Issue -Code 'runtime.requirement-not-bundled' -Stage 'runtimeDependencies' -Path $id -Message "Release runtime requirement '$id' is not marked bundled."
            $runtimePass = $false
        }
    }
    foreach ($requiredRuntimeId in @('microsoft-vc-runtime', 'microsoft-dotnet-runtime')) {
        if (-not $runtimeMap.ContainsKey($requiredRuntimeId)) {
            Add-Issue -Code 'runtime.requirement-missing' -Stage 'runtimeDependencies' -Path $requiredRuntimeId -Message "Game Profile must declare bundled runtime '$requiredRuntimeId'."
            $runtimePass = $false
        }
        if (-not $licenseMap.ContainsKey($requiredRuntimeId)) {
            Add-Issue -Code 'runtime.license-not-closed' -Stage 'runtimeDependencies' -Path $requiredRuntimeId -Message "Bundled runtime '$requiredRuntimeId' has no corresponding license record."
            $runtimePass = $false
        }
    }
    $dotnetRoot = Join-Path $script:RootPath 'runtime\dotnet'
    $dotnetHostFxr = @(Get-ChildItem -LiteralPath $dotnetRoot -Recurse -File -Filter 'hostfxr.dll' -ErrorAction SilentlyContinue)
    $dotnetCoreClr = @(Get-ChildItem -LiteralPath $dotnetRoot -Recurse -File -Filter 'coreclr.dll' -ErrorAction SilentlyContinue)
    if (-not (Test-Path -LiteralPath $dotnetRoot -PathType Container) -or $dotnetHostFxr.Count -eq 0 -or $dotnetCoreClr.Count -eq 0) {
        Add-Issue -Code 'runtime.dotnet-not-local' -Stage 'runtimeDependencies' -Path 'runtime/dotnet' -Message 'App-local .NET runtime is incomplete; hostfxr.dll and coreclr.dll must be inside the package.'
        $runtimePass = $false
    }
    $binRoot = Join-Path $script:RootPath 'bin'
    $vcFiles = @(Get-ChildItem -LiteralPath $binRoot -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^(?i)(msvcp140|vcruntime140|concrt140|vccorlib140).*\.dll$' })
    $hasMsvcp = @($vcFiles | Where-Object { $_.Name -match '^(?i)msvcp140' }).Count -gt 0
    $hasVcruntime = @($vcFiles | Where-Object { $_.Name -match '^(?i)vcruntime140' }).Count -gt 0
    if (-not $hasMsvcp -or -not $hasVcruntime) {
        Add-Issue -Code 'runtime.vc-not-local' -Stage 'runtimeDependencies' -Path 'bin' -Message 'App-local VC runtime is incomplete; at least one msvcp140*.dll and vcruntime140*.dll are required inside the package.'
        $runtimePass = $false
    }
    $runtimeStage = Get-StageResult -Pass $runtimePass -IssueCount ($script:Issues.Count - $runtimeIssueStart) -Details ([ordered]@{
        dotnetRoot = 'runtime/dotnet'
        dotnetHostFxrCount = $dotnetHostFxr.Count
        dotnetCoreClrCount = $dotnetCoreClr.Count
        vcFiles = @($vcFiles | ForEach-Object { Get-RelativePackagePath $_.FullName })
    })

    $pathLeakIssueStart = $script:Issues.Count
    $pathLeaks = @(Test-TextPathLeaks -Root $script:RootPath)
    if ($pathLeaks.Count -gt 0) {
        foreach ($leak in $pathLeaks) {
            Add-Issue -Code 'package.absolute-path-leak' -Stage 'pathLeaks' -Path $leak.file -Message "Absolute development-machine path at line $($leak.line): $($leak.value)"
        }
    }
    $pathLeakStage = Get-StageResult -Pass ($pathLeaks.Count -eq 0) -IssueCount ($script:Issues.Count - $pathLeakIssueStart) -Details ([ordered]@{
        scanExtensions = @('.json', '.jsonl', '.txt', '.md', '.xml', '.ini', '.cfg', '.config', '.manifest', '.props', '.targets', '.csproj', '.sln', '.cs', '.h', '.hpp', '.cpp', '.hlsl', '.glsl', '.vert', '.frag', '.comp', '.shader')
        leakCount = $pathLeaks.Count
        leaks = $pathLeaks
    })

    $headlessStage = [ordered]@{ requested = [bool]$HeadlessProbe; pass = $true; skipped = -not [bool]$HeadlessProbe }
    if ($HeadlessProbe) {
        if ($playerPass -and $profilePass) {
            $headlessStage = Invoke-HeadlessPlayerProbe -PlayerPath $playerPath -ProfilePath $profilePath
        } else {
            $headlessStage = [ordered]@{ requested = $true; pass = $false; skipped = $true; reason = 'Skipped because Game Profile or Player validation failed.' }
            Add-Issue -Code 'player.headless-skipped' -Stage 'headlessProbe' -Message 'Headless probe was requested but the package shape is invalid.'
        }
    }

    $structuralSuccess = $profilePass -and $playerPass -and $assetRegistryPass -and $licensePass -and $runtimePass -and ($pathLeaks.Count -eq 0)
    $success = $structuralSuccess -and ((-not $HeadlessProbe) -or [bool]$headlessStage.pass)
    $exitCode = if ($success) { 0 } elseif ($structuralSuccess -and $HeadlessProbe) { 4 } else { 3 }
    $receipt = [ordered]@{
        schema = 'noemancer.release-closure-receipt/0.1'
        success = $success
        packageRoot = $script:RootPath
        player = if ($null -ne $playerPath) { Get-RelativePackagePath $playerPath } else { $null }
        stages = [ordered]@{
            gameProfile = Get-StageResult -Pass $profilePass -IssueCount $profileIssueCount -Details ([ordered]@{ path = 'config/game-profile.json'; schema = if ($null -ne $profile) { [string](Get-PropertyValue $profile 'schema') } else { $null }; requiredSchema = $script:ReleaseGameProfileSchema })
            player = $playerStage
            assetRegistry = $assetRegistryStage
            licenseClosure = $licenseStage
            runtimeDependencies = $runtimeStage
            pathLeaks = $pathLeakStage
            headlessProbe = $headlessStage
        }
        issueCount = $script:Issues.Count
        issues = @($script:Issues)
    }
    Write-ReceiptAndExit -Receipt $receipt -ExitCode $exitCode
} catch {
    $receipt = [ordered]@{
        schema = 'noemancer.release-closure-receipt/0.1'
        success = $false
        packageRoot = $PackageRoot
        issueCount = 1
        issues = @([ordered]@{ code = 'script.unexpected-error'; stage = 'script'; message = $_.Exception.ToString() })
    }
    try { Write-ReceiptAndExit -Receipt $receipt -ExitCode 1 } catch { exit 1 }
} finally {
    if ($null -ne $script:TemporaryProbeRoot -and (Test-Path -LiteralPath $script:TemporaryProbeRoot)) {
        Remove-Item -LiteralPath $script:TemporaryProbeRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
