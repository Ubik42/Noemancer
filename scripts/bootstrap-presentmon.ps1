[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$version = '2.4.1'
$expectedSha256 = 'D74183E7AE630F72CD3690BE0373ECBFDC6CBB86578148AAB8FA2A7166068F34'
$toolRoot = Join-Path $PSScriptRoot "..\_tools\presentmon\$version"
$executable = Join-Path $toolRoot 'PresentMon.exe'
$download = "$executable.download"

New-Item -ItemType Directory -Force -Path $toolRoot | Out-Null
if (Test-Path -LiteralPath $executable) {
    $actual = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash
    if ($actual -eq $expectedSha256) {
        [pscustomobject]@{ Version=$version; Path=$executable; Sha256=$actual; CacheHit=$true }
        exit 0
    }
    throw "PresentMon exists but its SHA256 does not match the pinned release: $executable"
}

Invoke-WebRequest -UseBasicParsing `
    -Uri "https://github.com/GameTechDev/PresentMon/releases/download/v$version/PresentMon-$version-x64.exe" `
    -OutFile $download
$actual = (Get-FileHash -LiteralPath $download -Algorithm SHA256).Hash
if ($actual -ne $expectedSha256) {
    Remove-Item -LiteralPath $download -Force
    throw "Downloaded PresentMon SHA256 mismatch. Expected $expectedSha256, received $actual."
}
Move-Item -LiteralPath $download -Destination $executable
[pscustomobject]@{ Version=$version; Path=$executable; Sha256=$actual; CacheHit=$false }
