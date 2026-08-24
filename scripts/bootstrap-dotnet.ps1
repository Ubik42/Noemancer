param(
    [string]$InstallDirectory = (Join-Path $PSScriptRoot "..\_tools\dotnet"),
    [string]$Version = "10.0.400"
)

$ErrorActionPreference = "Stop"
$installer = Join-Path ([System.IO.Path]::GetTempPath()) "noemancer-dotnet-install.ps1"
Invoke-WebRequest "https://dot.net/v1/dotnet-install.ps1" -OutFile $installer
& $installer -Version $Version -InstallDir $InstallDirectory -Architecture x64
& (Join-Path $InstallDirectory "dotnet.exe") --info
