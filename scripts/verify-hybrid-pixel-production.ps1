[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$ProjectRoot = $(if ($env:NOEMANCER_PLATFORMER_PROJECT) { $env:NOEMANCER_PLATFORMER_PROJECT } else { Join-Path ([IO.Path]::GetPathRoot($PSScriptRoot)) '3D\NoemancerPlatformer' }),
    [string]$RuntimePath = '',
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Release',
    [ValidateRange(1, 60)]
    [int]$Frames = 16,
    [ValidateRange(10, 600)]
    [int]$TimeoutSeconds = 180,
    [ValidateRange(640, 7680)]
    [int]$WindowWidth = 1280,
    [ValidateRange(360, 4320)]
    [int]$WindowHeight = 720,
    [string]$OutputRoot = ''
)

# Stable public entry point for the current production-composition frontier.
# The implementation remains shared with the preceding VFX/post acceptance so
# source/package capture, bounded process handling and package closure cannot
# silently diverge between two near-identical harnesses.
$implementation = Join-Path $PSScriptRoot 'verify-hybrid-pixel-mixed-lighting.ps1'
& $implementation -ProjectRoot $ProjectRoot -RuntimePath $RuntimePath -Config $Config `
    -Frames $Frames -TimeoutSeconds $TimeoutSeconds -WindowWidth $WindowWidth `
    -WindowHeight $WindowHeight -OutputRoot $OutputRoot -Contract production
exit $LASTEXITCODE
