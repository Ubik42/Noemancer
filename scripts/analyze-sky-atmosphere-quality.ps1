[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ImagePath,
    [string]$OutputPath,
    [ValidateRange(1, 7680)]
    [int]$ExpectedWidth = 1920,
    [ValidateRange(1, 4320)]
    [int]$ExpectedHeight = 1080
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:Issues = [System.Collections.Generic.List[object]]::new()

function Add-Issue {
    param(
        [Parameter(Mandatory = $true)][string]$Code,
        [Parameter(Mandatory = $true)][string]$Message,
        [AllowNull()][object]$Observed,
        [AllowNull()][object]$Threshold
    )
    [void]$script:Issues.Add([ordered]@{
        code = $Code
        message = $Message
        observed = $Observed
        threshold = $Threshold
    })
}

function Write-JsonDocument {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)]$Value
    )
    $parent = Split-Path -Parent $Path
    if (-not [string]::IsNullOrWhiteSpace($parent)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    $json = $Value | ConvertTo-Json -Depth 32
    [IO.File]::WriteAllText($Path, $json + "`n", [Text.UTF8Encoding]::new($false))
}

function Get-BmpHeader {
    param([Parameter(Mandatory = $true)][byte[]]$Bytes)

    if ($Bytes.Length -lt 54 -or $Bytes[0] -ne 0x42 -or $Bytes[1] -ne 0x4d) {
        throw 'The image is not a supported BMP file.'
    }
    $pixelOffset = [BitConverter]::ToInt32($Bytes, 10)
    $dibSize = [BitConverter]::ToInt32($Bytes, 14)
    $width = [BitConverter]::ToInt32($Bytes, 18)
    $signedHeight = [BitConverter]::ToInt32($Bytes, 22)
    $planes = [BitConverter]::ToInt16($Bytes, 26)
    $bitsPerPixel = [BitConverter]::ToInt16($Bytes, 28)
    $compression = [BitConverter]::ToInt32($Bytes, 30)
    # SDL's BMP readback uses a 32-bit BGRA DIB with BI_BITFIELDS (3) on the
    # current Windows path. Plain BI_RGB (0) remains accepted for portable
    # fixtures; BI_BITFIELDS/BI_ALPHABITFIELDS are only accepted for 32-bit
    # pixels because 24-bit masks would make channel interpretation ambiguous.
    $compressionSupported = $compression -eq 0 -or ($bitsPerPixel -eq 32 -and ($compression -eq 3 -or $compression -eq 6))
    if ($dibSize -lt 40 -or $width -le 0 -or $signedHeight -eq 0 -or
        $planes -ne 1 -or ($bitsPerPixel -ne 24 -and $bitsPerPixel -ne 32) -or -not $compressionSupported) {
        throw "Unsupported BMP header (DIB=$dibSize width=$width height=$signedHeight bpp=$bitsPerPixel compression=$compression)."
    }
    $height = [Math]::Abs($signedHeight)
    $bytesPerPixel = [int]($bitsPerPixel / 8)
    $rowStride = [int]([Math]::Ceiling($width * $bytesPerPixel / 4.0) * 4)
    if ($pixelOffset -lt 54 -or $pixelOffset -ge $Bytes.Length -or
        $rowStride -le 0 -or $pixelOffset + ($rowStride * $height) -gt $Bytes.Length) {
        throw 'The BMP pixel payload is truncated.'
    }
    return [ordered]@{
        width = $width
        height = $height
        topDown = $signedHeight -lt 0
        bitsPerPixel = $bitsPerPixel
        bytesPerPixel = $bytesPerPixel
        rowStride = $rowStride
        pixelOffset = $pixelOffset
    }
}

if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = "$ImagePath.sky-quality.json"
}
$ImagePath = [IO.Path]::GetFullPath($ImagePath)
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
if (-not (Test-Path -LiteralPath $ImagePath -PathType Leaf)) {
    throw "Sky atmosphere image is missing: $ImagePath"
}

$bytes = [IO.File]::ReadAllBytes($ImagePath)
$header = Get-BmpHeader -Bytes $bytes
$width = [int]$header.width
$height = [int]$header.height
$sampleStep = 2
$rowSampleStep = 2

$globalCount = 0L
$globalLuma = 0.0
$globalLumaSquared = 0.0
$globalMin = 1.0
$globalMax = 0.0
$globalDark = 0L
$globalBright = 0L
$regions = [ordered]@{}
foreach ($name in @('top', 'horizon', 'lower')) {
    $regions[$name] = [ordered]@{ count = 0L; luma = 0.0; red = 0.0; green = 0.0; blue = 0.0 }
}

# A deliberately central, broad strip avoids the editor frame and makes the
# contract tolerant of scene geometry while still catching a flat/black sky.
$centralLeft = [int]($width * 0.15)
$centralRight = [int]($width * 0.85)
$rowMeans = [Collections.Generic.List[double]]::new()

for ($y = 0; $y -lt $height; $y += $sampleStep) {
    $sourceY = if ([bool]$header.topDown) { $y } else { $height - 1 - $y }
    $rowOffset = [int]$header.pixelOffset + ($sourceY * [int]$header.rowStride)
    $rowLuma = 0.0
    $rowCount = 0L
    for ($x = 0; $x -lt $width; $x += $sampleStep) {
        $offset = $rowOffset + ($x * [int]$header.bytesPerPixel)
        $red = $bytes[$offset + 2] / 255.0
        $green = $bytes[$offset + 1] / 255.0
        $blue = $bytes[$offset] / 255.0
        $luma = (0.2126 * $red) + (0.7152 * $green) + (0.0722 * $blue)
        $globalCount++
        $globalLuma += $luma
        $globalLumaSquared += $luma * $luma
        $globalMin = [Math]::Min($globalMin, $luma)
        $globalMax = [Math]::Max($globalMax, $luma)
        if ($luma -lt 0.02) { $globalDark++ }
        if ($luma -gt 0.98) { $globalBright++ }
        if ($x -ge $centralLeft -and $x -lt $centralRight) {
            $rowLuma += $luma
            $rowCount++
        }

        $normalizedY = $y / [double]$height
        $regionName = if ($normalizedY -lt 0.20) { 'top' }
            elseif ($normalizedY -ge 0.35 -and $normalizedY -lt 0.55) { 'horizon' }
            elseif ($normalizedY -ge 0.75) { 'lower' }
            else { $null }
        if ($null -ne $regionName) {
            $region = $regions[$regionName]
            $region['count']++
            $region['luma'] += $luma
            $region['red'] += $red
            $region['green'] += $green
            $region['blue'] += $blue
        }
    }
    if ($rowCount -gt 0 -and (($y % $rowSampleStep) -eq 0)) {
        [void]$rowMeans.Add($rowLuma / $rowCount)
    }
}

if ($globalCount -eq 0) { throw 'The BMP contains no sampled pixels.' }
$globalMean = $globalLuma / $globalCount
$variance = [Math]::Max(0.0, ($globalLumaSquared / $globalCount) - ($globalMean * $globalMean))
$globalStdDev = [Math]::Sqrt($variance)

$regionMetrics = [ordered]@{}
foreach ($name in $regions.Keys) {
    $region = $regions[$name]
    if ($region['count'] -gt 0) {
        $regionMeanLuma = $region['luma'] / $region['count']
        $regionMeanRed = $region['red'] / $region['count']
        $regionMeanGreen = $region['green'] / $region['count']
        $regionMeanBlue = $region['blue'] / $region['count']
        $regionMetrics[$name] = [ordered]@{
            sampleCount = $region['count']
            meanLuma = $regionMeanLuma
            meanRgb = @($regionMeanRed, $regionMeanGreen, $regionMeanBlue)
        }
    } else {
        $regionMetrics[$name] = [ordered]@{ sampleCount = 0; meanLuma = 0.0; meanRgb = @(0.0, 0.0, 0.0) }
    }
}

$rowDiffs = [Collections.Generic.List[double]]::new()
for ($index = 1; $index -lt $rowMeans.Count; $index++) {
    [void]$rowDiffs.Add([Math]::Abs($rowMeans[$index] - $rowMeans[$index - 1]))
}
$sortedDiffs = @($rowDiffs | Sort-Object)
$continuityP95 = if ($sortedDiffs.Count -gt 0) { $sortedDiffs[[Math]::Min($sortedDiffs.Count - 1, [int]($sortedDiffs.Count * 0.95))] } else { 1.0 }
$continuityP99 = if ($sortedDiffs.Count -gt 0) { $sortedDiffs[[Math]::Min($sortedDiffs.Count - 1, [int]($sortedDiffs.Count * 0.99))] } else { 1.0 }
$continuityMax = if ($sortedDiffs.Count -gt 0) { $sortedDiffs[$sortedDiffs.Count - 1] } else { 1.0 }

if ($width -ne $ExpectedWidth -or $height -ne $ExpectedHeight) {
    Add-Issue -Code 'dimensions.mismatch' -Message "Expected ${ExpectedWidth}x${ExpectedHeight}, got ${width}x${height}." `
        -Observed ([ordered]@{ width = $width; height = $height }) -Threshold ([ordered]@{ width = $ExpectedWidth; height = $ExpectedHeight })
}
if ($globalMean -le 0.03 -or $globalMax -le 0.12 -or ($globalDark / [double]$globalCount) -ge 0.95) {
    Add-Issue -Code 'image.black-or-empty' -Message 'The image is effectively black or empty.' `
        -Observed ([ordered]@{ meanLuma = $globalMean; maxLuma = $globalMax; darkFraction = $globalDark / [double]$globalCount }) `
        -Threshold ([ordered]@{ meanLumaGreaterThan = 0.03; maxLumaGreaterThan = 0.12; darkFractionLessThan = 0.95 })
}
if ($globalMean -ge 0.92 -or ($globalBright / [double]$globalCount) -ge 0.20) {
    Add-Issue -Code 'image.overexposed' -Message 'The image is effectively white or globally overexposed.' `
        -Observed ([ordered]@{ meanLuma = $globalMean; brightFraction = $globalBright / [double]$globalCount }) `
        -Threshold ([ordered]@{ meanLumaLessThan = 0.92; brightFractionLessThan = 0.20 })
}
$top = $regionMetrics.top
$horizon = $regionMetrics.horizon
$lumaDelta = [Math]::Abs([double]$top.meanLuma - [double]$horizon.meanLuma)
$rgbDelta = [Math]::Sqrt(
    [Math]::Pow([double]$top.meanRgb[0] - [double]$horizon.meanRgb[0], 2) +
    [Math]::Pow([double]$top.meanRgb[1] - [double]$horizon.meanRgb[1], 2) +
    [Math]::Pow([double]$top.meanRgb[2] - [double]$horizon.meanRgb[2], 2))
if ($top.sampleCount -lt 100 -or $horizon.sampleCount -lt 100 -or ($lumaDelta -lt 0.02 -and $rgbDelta -lt 0.03)) {
    Add-Issue -Code 'sky.flat-gradient' -Message 'Upper and horizon sky regions have no measurable luminance or colour separation.' `
        -Observed ([ordered]@{ top = $top; horizon = $horizon; lumaDelta = $lumaDelta; rgbDelta = $rgbDelta }) `
        -Threshold ([ordered]@{ lumaDeltaGreaterThan = 0.02; rgbDeltaGreaterThan = 0.03 })
}
if ($continuityP95 -gt 0.12 -or $continuityMax -gt 0.28) {
    Add-Issue -Code 'sky.horizon-discontinuity' -Message 'The central sky/horizon profile contains an abrupt discontinuity.' `
        -Observed ([ordered]@{ p95RowDelta = $continuityP95; p99RowDelta = $continuityP99; maxRowDelta = $continuityMax }) `
        -Threshold ([ordered]@{ p95RowDeltaAtMost = 0.12; maxRowDeltaAtMost = 0.28 })
}

$result = [ordered]@{
    schemaVersion = 'noemancer.sky-atmosphere-quality/0.1'
    analyzedAt = [DateTimeOffset]::UtcNow.ToString('o')
    image = [IO.Path]::GetFileName($ImagePath)
    imageSha256 = (Get-FileHash -LiteralPath $ImagePath -Algorithm SHA256).Hash.ToLowerInvariant()
    dimensions = [ordered]@{ width = $width; height = $height; expectedWidth = $ExpectedWidth; expectedHeight = $ExpectedHeight; match = ($width -eq $ExpectedWidth -and $height -eq $ExpectedHeight) }
    sampling = [ordered]@{ pixelStep = $sampleStep; rowStep = $rowSampleStep; colorSpace = 'sRGB-BMP/normalized' }
    metrics = [ordered]@{
        global = [ordered]@{ sampleCount = $globalCount; meanLuma = $globalMean; standardDeviation = $globalStdDev; minLuma = $globalMin; maxLuma = $globalMax; darkFraction = $globalDark / [double]$globalCount; brightFraction = $globalBright / [double]$globalCount }
        skyRegions = $regionMetrics
        upperToHorizon = [ordered]@{ lumaDelta = $lumaDelta; rgbEuclideanDelta = $rgbDelta }
        horizonContinuity = [ordered]@{ centralXRange = @(0.15, 0.85); sampledRows = $rowMeans.Count; p95RowDelta = $continuityP95; p99RowDelta = $continuityP99; maxRowDelta = $continuityMax }
    }
    thresholds = [ordered]@{ nonBlackMeanLumaMin = 0.03; nonBlackMaxLumaMin = 0.12; blackDarkFractionMax = 0.95; whiteMeanLumaMax = 0.92; whiteBrightFractionMax = 0.20; skyLumaDeltaMin = 0.02; skyRgbDeltaMin = 0.03; horizonP95RowDeltaMax = 0.12; horizonMaxRowDeltaMax = 0.28 }
    issues = @($script:Issues)
    pass = ($script:Issues.Count -eq 0)
}
Write-JsonDocument -Path $OutputPath -Value $result
$result | ConvertTo-Json -Depth 32 -Compress
if (-not [bool]$result.pass) { exit 5 }
