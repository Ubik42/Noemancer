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
$runtime = Join-Path $projectRoot "build\windows-msvc-debug\src\runtime\$Config\noemancer.exe"
$mcpDirectory = Join-Path $projectRoot 'tools\mcp'
$ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
$operationMutex = $null
$operationMutexHeld = $false
$previousMsBuildDisableNodeReuse = $env:MSBUILDDISABLENODEREUSE

if (-not (Test-Path -LiteralPath $cmake)) {
    throw 'CMake 3.28 or newer is required. Install CMake or place the pinned distribution under _tools.'
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
        & $cmake --preset $preset
    }
    'build' {
        $buildArguments = @('--build', '--preset', $preset, '--config', $Config)
        if ($Target) { $buildArguments += @('--target', $Target) }
        & $cmake @buildArguments
    }
    'check' {
        if (-not $Target -and -not $TestRegex) {
            throw 'check requires -Target, -TestRegex, or both; use test for the full milestone gate.'
        }
        if ($Target) {
            & $cmake --build --preset $preset --config $Config --target $Target
            if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        }
        if ($TestRegex) {
            & $ctest --test-dir (Join-Path $projectRoot 'build\windows-msvc-debug') -C $Config `
                -R $TestRegex --output-on-failure --interactive-debug-mode 0
            if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        }
    }
    'test' {
        & $cmake --build --preset $preset --config $Config
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        & $ctest --test-dir (Join-Path $projectRoot 'build\windows-msvc-debug') -C $Config `
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
