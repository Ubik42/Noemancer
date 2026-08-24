[CmdletBinding()]
param(
    [string]$InstallDirectory = (Join-Path $PSScriptRoot '..\_tools\netcoredbg')
)

$ErrorActionPreference = 'Stop'
$version = '3.1.2-1054'
$archiveName = 'netcoredbg-win64.zip'
$archiveUrl = "https://github.com/Samsung/netcoredbg/releases/download/$version/$archiveName"
$expectedSha256 = '09B4385FD556014A8A96DF7A368D4937F4FC9E06F031365D41A288AABD2D78F9'
$resolvedInstallDirectory = [System.IO.Path]::GetFullPath($InstallDirectory)
$expectedExecutable = Join-Path $resolvedInstallDirectory 'netcoredbg.exe'

if (Test-Path -LiteralPath $expectedExecutable) {
    Write-Host "NetCoreDbg $version is already installed at $expectedExecutable"
    exit 0
}

$temporaryRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("noemancer-netcoredbg-" + [guid]::NewGuid().ToString('N'))
$archivePath = Join-Path $temporaryRoot $archiveName
$extractRoot = Join-Path $temporaryRoot 'extract'
New-Item -ItemType Directory -Path $temporaryRoot -Force | Out-Null

try {
    Invoke-WebRequest -UseBasicParsing -Uri $archiveUrl -OutFile $archivePath
    $actualSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $archivePath).Hash
    if ($actualSha256 -ne $expectedSha256) {
        throw "NetCoreDbg archive checksum mismatch. Expected $expectedSha256, received $actualSha256."
    }

    Expand-Archive -LiteralPath $archivePath -DestinationPath $extractRoot
    $payloadRoot = Join-Path $extractRoot 'netcoredbg'
    if (-not (Test-Path -LiteralPath (Join-Path $payloadRoot 'netcoredbg.exe'))) {
        throw 'The verified NetCoreDbg archive did not contain netcoredbg/netcoredbg.exe.'
    }

    $installParent = Split-Path -Parent $resolvedInstallDirectory
    New-Item -ItemType Directory -Path $installParent -Force | Out-Null
    if (Test-Path -LiteralPath $resolvedInstallDirectory) {
        $existingItems = @(Get-ChildItem -LiteralPath $resolvedInstallDirectory -Force)
        if ($existingItems.Count -ne 0) {
            throw "Refusing to overwrite non-empty NetCoreDbg directory: $resolvedInstallDirectory"
        }
        Remove-Item -LiteralPath $resolvedInstallDirectory
    }
    Move-Item -LiteralPath $payloadRoot -Destination $resolvedInstallDirectory
    Write-Host "Installed NetCoreDbg $version at $expectedExecutable"
} finally {
    if (Test-Path -LiteralPath $temporaryRoot) {
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
    }
}
