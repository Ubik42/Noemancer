[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [Alias('Project')]
    [string]$ProjectPath = '',

    [string]$Config = 'Release',

    [string]$Locale = 'zh-CN',

    [switch]$NoBuild,
    [switch]$VerifyOnly,
    [Alias('h')]
    [switch]$Help
)

$ErrorActionPreference = 'Stop'
$engineRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$engineScript = Join-Path $PSScriptRoot 'engine.ps1'
$runtime = Join-Path $engineRoot "build\windows-msvc-debug\src\runtime\$Config\noemancer.exe"

function Write-LaunchFailure {
    param(
        [Parameter(Mandatory)][string]$Code,
        [Parameter(Mandatory)][string]$Message,
        [hashtable]$Details = @{}
    )

    $payload = [ordered]@{
        schemaVersion = 'noemancer.editor-launch/0.1'
        success = $false
        code = $Code
        message = $Message
    }
    foreach ($key in $Details.Keys) { $payload[$key] = $Details[$key] }
    [Console]::Error.WriteLine(($payload | ConvertTo-Json -Compress -Depth 8))
    exit 30
}

function Write-LaunchHelp {
    @'
Noemancer Editor launcher

Usage:
  Noemancer Editor.cmd [PROJECT_PATH] [-Config Debug|Release] [-Locale zh-CN|en-US] [-NoBuild] [-VerifyOnly]
  powershell -File scripts/launch-editor.ps1 [-Project PROJECT_PATH] [-Config Debug|Release] [-Locale zh-CN|en-US]

PROJECT_PATH is optional. When omitted, the editor opens the Noemancer Project
Hub. -VerifyOnly performs a headless project/runtime check and never
opens a GUI window. The launcher resolves the repository from its own script
location, so the caller's current directory does not matter. Release is the
interactive product default; pass -Config Debug when debugging native engine
code.

The official launcher starts in Simplified Chinese by default. Choose Language
in the Editor menu to switch immediately, or pass -Locale en-US at launch.
'@ | Write-Output
}

if ($Help) {
    Write-LaunchHelp
    exit 0
}

if ($Config -notin @('Debug', 'Release')) {
    Write-LaunchFailure -Code 'config.invalid' `
        -Message 'The editor configuration must be Debug or Release.' `
        -Details @{ config = $Config }
}

if ($Locale -notmatch '^[A-Za-z0-9_-]{1,32}$') {
    Write-LaunchFailure -Code 'locale.invalid' `
        -Message 'The editor locale must contain only letters, digits, hyphen, or underscore.' `
        -Details @{ locale = $Locale }
}

if (-not (Test-Path -LiteralPath $engineScript -PathType Leaf)) {
    Write-LaunchFailure -Code 'launcher.script-missing' `
        -Message 'The Noemancer engine launcher script is missing.' `
        -Details @{ engineRoot = $engineRoot; script = $engineScript }
}

$resolvedProject = $null
if (-not [string]::IsNullOrWhiteSpace($ProjectPath)) {
    try {
        $resolvedProject = [IO.Path]::GetFullPath($ProjectPath)
    } catch {
        Write-LaunchFailure -Code 'project.path-invalid' `
            -Message 'The supplied project path cannot be resolved.' `
            -Details @{ projectPath = $ProjectPath; detail = $_.Exception.Message }
    }
    $manifest = Join-Path $resolvedProject 'noemancer.project.json'
    if (-not (Test-Path -LiteralPath $resolvedProject -PathType Container)) {
        Write-LaunchFailure -Code 'project.not-found' `
            -Message 'The supplied Noemancer project directory does not exist.' `
            -Details @{ projectPath = $resolvedProject; manifest = $manifest }
    }
    if (-not (Test-Path -LiteralPath $manifest -PathType Leaf)) {
        Write-LaunchFailure -Code 'project.manifest-missing' `
            -Message 'The supplied directory is not a Noemancer project.' `
            -Details @{ projectPath = $resolvedProject; manifest = $manifest }
    }
}

if (-not $NoBuild) {
    try {
        & $engineScript build -Config $Config -Target noemancer
    } catch {
        Write-LaunchFailure -Code 'engine.build-exception' `
            -Message 'The engine build command could not be started.' `
            -Details @{ config = $Config; detail = $_.Exception.Message }
    }
    if ($LASTEXITCODE -ne 0) {
        Write-LaunchFailure -Code 'engine.build-failed' `
            -Message 'The Noemancer runtime build failed.' `
            -Details @{ config = $Config; exitCode = $LASTEXITCODE; runtime = $runtime }
    }
}

if (-not (Test-Path -LiteralPath $runtime -PathType Leaf)) {
    Write-LaunchFailure -Code 'engine.runtime-missing' `
        -Message 'The requested Noemancer runtime executable is missing.' `
        -Details @{ config = $Config; runtime = $runtime; noBuild = [bool]$NoBuild }
}

$runtimeArguments = @('run', '--ui-locale', $Locale)
if ($null -ne $resolvedProject) { $runtimeArguments += @('--project', $resolvedProject) }

if ($VerifyOnly) {
    $verificationArguments = @($runtimeArguments + @('--headless', '--frames', '3', '--format', 'json'))
    $verificationOutput = @()
    try {
        $verificationOutput = @(& $runtime @verificationArguments 2>&1 | ForEach-Object { $_.ToString() })
    } catch {
        Write-LaunchFailure -Code 'editor.verify-exception' `
            -Message 'The headless Editor verification could not be started.' `
            -Details @{ runtime = $runtime; detail = $_.Exception.Message }
    }
    if ($LASTEXITCODE -ne 0) {
        Write-LaunchFailure -Code 'editor.verify-failed' `
            -Message 'The headless Editor/runtime verification failed.' `
            -Details @{ runtime = $runtime; projectPath = $resolvedProject; exitCode = $LASTEXITCODE; output = ($verificationOutput -join "`n") }
    }
    $verificationOutput | Write-Output
    exit 0
}

# Explorer and configured terminal associations can corrupt cmd /C quoting for
# a console-subsystem executable. Start the SDL process directly, preserving
# the engine root as its working directory and never invoking another shell.
try {
    if (-not ('NoemancerEditorDetachedProcess' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

public static class NoemancerEditorDetachedProcess {
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
        var startup = new StartupInfo();
        startup.cb = Marshal.SizeOf(typeof(StartupInfo));
        ProcessInformation process;
        var command = new StringBuilder("\"" + filePath + "\" " + arguments);
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
} catch {
    Write-LaunchFailure -Code 'editor.process-helper-failed' `
        -Message 'The Windows process launcher helper could not be prepared.' `
        -Details @{ detail = $_.Exception.Message }
}

$argumentText = 'run --ui-locale "' + $Locale + '"'
if ($null -ne $resolvedProject) { $argumentText += ' --project "' + $resolvedProject + '"' }
try {
    $processId = [NoemancerEditorDetachedProcess]::Start($runtime, $argumentText, $engineRoot)
} catch {
    Write-LaunchFailure -Code 'editor.start-failed' `
        -Message 'Windows could not start the Noemancer Editor runtime.' `
        -Details @{ runtime = $runtime; projectPath = $resolvedProject; detail = $_.Exception.Message }
}

[ordered]@{
    schemaVersion = 'noemancer.editor-launch/0.1'
    success = $true
    code = 'started'
    config = $Config
    locale = $Locale
    runtime = $runtime
    projectPath = $resolvedProject
    processId = $processId
} | ConvertTo-Json -Compress
exit 0
