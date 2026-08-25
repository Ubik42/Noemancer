[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(Position = 0)]
    [ValidateSet('configure', 'build', 'check', 'test', 'run')]
    [string]$Command = 'build',

    [string]$Config = 'Debug',

    [string]$Target,

    [string]$TestRegex,

    [switch]$WithMcp,

    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$EngineArguments
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$localCmake = Join-Path $projectRoot '_tools\cmake-4.4.2-windows-x86_64\bin\cmake.exe'
$cmake = $localCmake
if (-not (Test-Path -LiteralPath $cmake)) {
    $systemCmake = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($null -eq $systemCmake) { $systemCmake = Get-Command cmake -ErrorAction SilentlyContinue }
    if ($null -ne $systemCmake) { $cmake = $systemCmake.Source }
}
$preset = 'windows-msvc-debug'
$buildDirectory = Join-Path $projectRoot 'build\windows-msvc-debug'
$runtime = Join-Path $buildDirectory "src\runtime\$Config\noemancer.exe"
$mcpDirectory = Join-Path $projectRoot 'tools\mcp'
$ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
$operationMutex = $null
$operationMutexHeld = $false
$previousMsBuildDisableNodeReuse = $env:MSBUILDDISABLENODEREUSE

if (-not (Test-Path -LiteralPath $cmake)) {
    throw 'CMake 3.28 or newer is required. Install CMake or place the pinned distribution under _tools.'
}

function Invoke-NoemancerConfigure {
    $configureArguments = @('--preset', $preset)
    $cachePath = Join-Path $buildDirectory 'CMakeCache.txt'
    if (Test-Path -LiteralPath $cachePath) {
        $cachedSourceLine = Select-String -LiteralPath $cachePath `
            -Pattern '^CMAKE_HOME_DIRECTORY:INTERNAL=' | Select-Object -First 1
        if ($null -ne $cachedSourceLine) {
            $cachedSource = $cachedSourceLine.Line.Substring(
                'CMAKE_HOME_DIRECTORY:INTERNAL='.Length)
            $cachedSourceFull = [IO.Path]::GetFullPath($cachedSource).TrimEnd('\', '/')
            $projectRootFull = [IO.Path]::GetFullPath($projectRoot).TrimEnd('\', '/')
            if (-not $cachedSourceFull.Equals(
                $projectRootFull, [StringComparison]::OrdinalIgnoreCase)) {
                Write-Host "Build cache moved from '$cachedSourceFull'; configuring fresh at '$projectRootFull'."
                $configureArguments = @('--fresh') + $configureArguments
            }
        }
    }
    & $cmake @configureArguments
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $marker = Join-Path $buildDirectory '.noemancer-configured-source'
    [IO.File]::WriteAllText($marker, [IO.Path]::GetFullPath($projectRoot), [Text.UTF8Encoding]::new($false))
}

function Confirm-NoemancerConfigured {
    $marker = Join-Path $buildDirectory '.noemancer-configured-source'
    $expected = [IO.Path]::GetFullPath($projectRoot).TrimEnd('\', '/')
    $configured = if (Test-Path -LiteralPath $marker -PathType Leaf) {
        [IO.File]::ReadAllText($marker).Trim().TrimEnd('\', '/')
    } else { '' }
    if (-not $configured.Equals($expected, [StringComparison]::OrdinalIgnoreCase)) {
        Invoke-NoemancerConfigure
    }
}

if ($Command -in @('configure', 'build', 'check', 'test')) {
    # CMake/MSBuild and several integration tests share the same build tree and
    # deterministic scratch paths. Separate Codex workers may prepare source in
    # parallel, but overlapping build/test processes can observe half-relinked
    # binaries or delete each other's fixtures. Serialize only these operations;
    # `run` deliberately remains outside the lock so an open editor does not
    # block later development builds.
    $operationMutex = [System.Threading.Mutex]::new($false, 'Local\NoemancerEngineBuildAndTest')
    try {
        $operationMutexHeld = $operationMutex.WaitOne([TimeSpan]::FromMinutes(10))
    } catch [System.Threading.AbandonedMutexException] {
        $operationMutexHeld = $true
    }
    if (-not $operationMutexHeld) {
        throw 'Timed out waiting for another Noemancer build/test operation to finish.'
    }

    # Codex workers run many short-lived build processes. MSBuild's default
    # reusable worker nodes can outlive their CMake parent and later interfere
    # with a different serialized test batch. Keep every engine operation
    # process-scoped so completion really means no compiler workers remain.
    $env:MSBUILDDISABLENODEREUSE = '1'
}

try {
switch ($Command) {
    'configure' {
        Invoke-NoemancerConfigure
    }
    'build' {
        Confirm-NoemancerConfigured
        $buildArguments = @('--build', '--preset', $preset, '--config', $Config)
        if ($Target) { $buildArguments += @('--target', $Target) }
        & $cmake @buildArguments
    }
    'check' {
        Confirm-NoemancerConfigured
        if (-not $Target -and -not $TestRegex) {
            throw 'check requires -Target, -TestRegex, or both; use test for the full milestone gate.'
        }
        if ($Target) {
            & $cmake --build --preset $preset --config $Config --target $Target
            if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        }
        if ($TestRegex) {
            & $ctest --test-dir $buildDirectory -C $Config `
                -R $TestRegex --output-on-failure --interactive-debug-mode 0
            if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        }
    }
    'test' {
        Confirm-NoemancerConfigured
        & $cmake --build --preset $preset --config $Config
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        & $ctest --test-dir $buildDirectory -C $Config `
            --output-on-failure --interactive-debug-mode 0
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        if ($WithMcp) {
            if (-not (Test-Path -LiteralPath (Join-Path $mcpDirectory 'node_modules'))) {
                throw 'MCP gate requested but tools/mcp/node_modules is missing. Run npm ci in tools/mcp explicitly.'
            }
            Push-Location $mcpDirectory
            try {
                & npm.cmd run build
                if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
                & npm.cmd run smoke
            } finally {
                Pop-Location
            }
        }
    }
    'run' {
        if (-not (Test-Path -LiteralPath $runtime)) {
            & $cmake --build --preset $preset --config $Config --target noemancer
            if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        }
        & $runtime @EngineArguments
    }
}
} finally {
    if ($null -eq $previousMsBuildDisableNodeReuse) {
        Remove-Item Env:MSBUILDDISABLENODEREUSE -ErrorAction SilentlyContinue
    } else {
        $env:MSBUILDDISABLENODEREUSE = $previousMsBuildDisableNodeReuse
    }
    if ($operationMutexHeld) {
        $operationMutex.ReleaseMutex()
    }
    if ($null -ne $operationMutex) {
        $operationMutex.Dispose()
    }
}

exit $LASTEXITCODE
