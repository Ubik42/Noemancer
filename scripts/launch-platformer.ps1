[CmdletBinding()]
param(
    [ValidateSet('Editor', 'Game')]
    [string]$Mode = 'Editor',
    [string]$ProjectRoot = '',
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Debug',
    [switch]$VerifyOnly
)

$ErrorActionPreference = 'Stop'
$engineRoot = Split-Path -Parent $PSScriptRoot
$workspaceRoot = Split-Path -Parent $engineRoot
if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $ProjectRoot = Join-Path $workspaceRoot '_games\lumen-run'
}
$ProjectRoot = [IO.Path]::GetFullPath($ProjectRoot)
$engineScript = Join-Path $PSScriptRoot 'engine.ps1'
$runtime = Join-Path $engineRoot "build\windows-msvc-debug\src\runtime\$Config\noemancer.exe"
$projectManifest = Join-Path $ProjectRoot 'noemancer.project.json'

if (-not (Test-Path -LiteralPath $projectManifest -PathType Leaf)) {
    throw "Platformer project manifest is missing: $projectManifest"
}

function Start-NoemancerProcess {
    param(
        [Parameter(Mandatory)][string]$FilePath,
        [Parameter(Mandatory)][string]$WorkingDirectory,
        [Parameter(Mandatory)][string]$Arguments
    )
    # Runtime and Player retain the console subsystem for CLI diagnostics.
    # Explorer may route them through a configured default terminal such as
    # Cmder/ConEmu, corrupting its cmd /C quoting. DETACHED_PROCESS bypasses
    # that terminal without applying SW_HIDE to the SDL window.
    if (-not ('NoemancerDetachedProcess' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

public static class NoemancerDetachedProcess {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct StartupInfo {
        public int cb; public string reserved; public string desktop; public string title;
        public int x; public int y; public int xSize; public int ySize;
        public int xCountChars; public int yCountChars; public int fillAttribute; public int flags;
        public short showWindow; public short reserved2; public IntPtr reserved2Pointer;
        public IntPtr standardInput; public IntPtr standardOutput; public IntPtr standardError;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct ProcessInformation {
        public IntPtr process; public IntPtr thread; public int processId; public int threadId;
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool CreateProcessW(
        string applicationName, StringBuilder commandLine,
        IntPtr processAttributes, IntPtr threadAttributes, bool inheritHandles,
        uint creationFlags, IntPtr environment, string currentDirectory,
        ref StartupInfo startupInfo, out ProcessInformation processInformation);

    [DllImport("kernel32.dll")]
    private static extern bool CloseHandle(IntPtr handle);

    public static int Start(string filePath, string arguments, string workingDirectory) {
        StartupInfo startup = new StartupInfo();
        startup.cb = Marshal.SizeOf(typeof(StartupInfo));
        ProcessInformation process;
        StringBuilder command = new StringBuilder("\"" + filePath + "\" " + arguments);
        const uint DetachedProcess = 0x00000008;
        if (!CreateProcessW(filePath, command, IntPtr.Zero, IntPtr.Zero, false,
                            DetachedProcess, IntPtr.Zero, workingDirectory,
                            ref startup, out process)) {
            throw new Win32Exception(Marshal.GetLastWin32Error());
        }
        CloseHandle(process.thread);
        CloseHandle(process.process);
        return process.processId;
    }
}
'@
    }
    try {
        [void][NoemancerDetachedProcess]::Start($FilePath, $Arguments, $WorkingDirectory)
    } catch {
        throw "Windows could not start $FilePath directly: $($_.Exception.Message)"
    }
}

& $engineScript build -Config $Config -Target noemancer
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $runtime -PathType Leaf)) {
    throw 'Noemancer could not build the selected runtime configuration.'
}

if ($Mode -eq 'Editor') {
    if ($VerifyOnly) {
        & $runtime run --project $ProjectRoot --headless --frames 3 --format json
        if ($LASTEXITCODE -ne 0) { throw 'The Editor project-load verification failed.' }
        return
    }
    Start-NoemancerProcess -FilePath $runtime -WorkingDirectory $engineRoot `
        -Arguments ('run --project "' + $ProjectRoot + '"')
    return
}

function Get-TextSha256 {
    param([Parameter(Mandatory)][string]$Text)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [Text.Encoding]::UTF8.GetBytes($Text)
        return ([BitConverter]::ToString($algorithm.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant()
    } finally {
        $algorithm.Dispose()
    }
}

function Get-FileSha256 {
    param([Parameter(Mandatory)][string]$Path)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($Path)
    try {
        return ([BitConverter]::ToString($algorithm.ComputeHash($stream))).Replace('-', '').ToLowerInvariant()
    } finally {
        $stream.Dispose()
        $algorithm.Dispose()
    }
}

function Get-ProjectFingerprint {
    param([Parameter(Mandatory)][string]$Root)
    $normalizedRoot = [IO.Path]::GetFullPath($Root).TrimEnd([IO.Path]::DirectorySeparatorChar)
    $records = foreach ($file in Get-ChildItem -LiteralPath $normalizedRoot -Recurse -File) {
        $rootUri = New-Object Uri(($normalizedRoot + [IO.Path]::DirectorySeparatorChar))
        $fileUri = New-Object Uri($file.FullName)
        $relative = [Uri]::UnescapeDataString($rootUri.MakeRelativeUri($fileUri).ToString())
        if ($relative -match '^(?:\.git|generated|build|cache|\.vs|[^/]+/(?:bin|obj))(?:/|$)' -or
            $relative -match '/(?:bin|obj)/') { continue }
        $hash = Get-FileSha256 -Path $file.FullName
        "$relative|$hash"
    }
    return (Get-TextSha256 -Text (($records | Sort-Object) -join "`n"))
}

$runtimeHash = Get-FileSha256 -Path $runtime
$projectHash = Get-ProjectFingerprint -Root $ProjectRoot
$packageKey = (Get-TextSha256 -Text "$runtimeHash`n$projectHash").Substring(0, 16)
$oneClickRoot = Join-Path $ProjectRoot 'generated\one-click'
$packageRoot = Join-Path $oneClickRoot ("$($Config.ToLowerInvariant())-$packageKey")
$profilePath = Join-Path $packageRoot 'config\game-profile.json'

if (-not (Test-Path -LiteralPath $profilePath -PathType Leaf)) {
    New-Item -ItemType Directory -Force -Path $oneClickRoot | Out-Null
    $targetProfile = if ($Config -eq 'Release') { 'windows-x64-release' } else { 'windows-x64-debug' }
    $packageLog = Join-Path $oneClickRoot ("package-$packageKey.log")
    $packageOutput = @(& $runtime package --project $ProjectRoot --output $packageRoot `
        --target-profile $targetProfile --format json 2>&1)
    $packageOutput | Set-Content -LiteralPath $packageLog -Encoding UTF8
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $profilePath -PathType Leaf)) {
        throw "Playable package creation failed. See $packageLog"
    }
}

$profile = Get-Content -LiteralPath $profilePath -Raw | ConvertFrom-Json
$executableName = [string]$profile.executable
if ([string]::IsNullOrWhiteSpace($executableName) -or [IO.Path]::IsPathRooted($executableName) -or
    $executableName -match '[\\/]' -or $executableName -notmatch '\.exe$') {
    throw 'The generated Game Profile contains an invalid executable name.'
}
$player = Join-Path $packageRoot (Join-Path 'bin' $executableName)
if (-not (Test-Path -LiteralPath $player -PathType Leaf)) {
    throw "Packaged Player is missing: $player"
}

if ($VerifyOnly) {
    & $player player --profile $profilePath --headless --frames 3 --format json
    if ($LASTEXITCODE -ne 0) { throw 'The packaged game verification failed.' }
    return
}

Start-NoemancerProcess -FilePath $player -WorkingDirectory $packageRoot `
    -Arguments ('player --profile "' + $profilePath + '"')
