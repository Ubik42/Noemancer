[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$ImagePath,

    [string]$ReceiptPath = '',

    [ValidateRange(1, 8192)]
    [int]$ExpectedWidth = 1920,

    [ValidateRange(1, 8192)]
    [int]$ExpectedHeight = 1080
)

# This is intentionally an offline image contract. It does not start the
# Runtime, invoke a GPU capture, or inspect renderer internals. The capture
# remains the input; this script only measures the versioned Bloom fixture.
#
# Exit contract:
#   0 = dimensions, source core, near/far halo and energy limits passed
#   2 = invalid invocation or unsupported BMP
#   3 = image was readable but the Bloom quality contract failed
#   7 = the requested receipt could not be written
#   1 = unexpected verifier failure

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$script:Issues = [System.Collections.Generic.List[object]]::new()
$script:MaxPixels = 8192 * 8192

function Add-Issue {
    param(
        [Parameter(Mandatory = $true)][string]$Code,
        [Parameter(Mandatory = $true)][string]$Message,
        [string]$Stage = 'analysis'
    )

    [void]$script:Issues.Add([pscustomobject][ordered]@{
        code = $Code
        stage = $Stage
        message = $Message
    })
}

function Limit-Text {
    param([AllowNull()][string]$Text)
    if ($null -eq $Text) { return '' }
    if ($Text.Length -le 512) { return $Text }
    return $Text.Substring(0, 512) + '…'
}

function Get-FileSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Evidence file does not exist: $Path"
    }
    $hash = [string](Get-FileHash -LiteralPath $Path -Algorithm SHA256 -ErrorAction Stop).Hash
    if ($hash -notmatch '^[0-9A-Fa-f]{64}$') {
        throw "SHA-256 provider returned an invalid digest for '$Path'."
    }
    return $hash.ToLowerInvariant()
}

function Write-ReceiptAndExit {
    param(
        [Parameter(Mandatory = $true)]$Receipt,
        [Parameter(Mandatory = $true)][int]$ExitCode
    )

    $json = $Receipt | ConvertTo-Json -Depth 30
    $writeFailed = $false
    if (-not [string]::IsNullOrWhiteSpace($ReceiptPath)) {
        try {
            $target = [System.IO.Path]::GetFullPath($ReceiptPath)
            $parent = [System.IO.Path]::GetDirectoryName($target)
            if (-not [string]::IsNullOrWhiteSpace($parent)) {
                New-Item -ItemType Directory -Path $parent -Force | Out-Null
            }
            Set-Content -LiteralPath $target -Value $json -Encoding UTF8
        }
        catch {
            $writeFailed = $true
            [Console]::Error.WriteLine("Could not write Bloom quality receipt '$ReceiptPath': $($_.Exception.Message)")
        }
    }
    [Console]::Out.WriteLine($json)
    if ($writeFailed) { exit 7 }
    exit $ExitCode
}

function Get-MaskMetadata {
    param([Parameter(Mandatory = $true)][uint32]$Mask)

    if ($Mask -eq 0) { return [ordered]@{ shift = 0; maximum = 0; scale = 1.0 } }
    $shift = 0
    $remaining = $Mask
    while (($remaining -band [uint32]1) -eq 0) {
        $remaining = $remaining -shr 1
        $shift++
    }
    $maximum = [uint32]($Mask -shr $shift)
    while (($remaining -band [uint32]1) -eq 1) { $remaining = $remaining -shr 1 }
    if ($remaining -ne 0) { throw "BMP bitfield mask is not contiguous: 0x$('{0:X8}' -f $Mask)." }
    if ($maximum -eq 0) { throw "BMP bitfield mask is empty: 0x$('{0:X8}' -f $Mask)." }
    return [ordered]@{
        shift = $shift
        maximum = $maximum
        scale = 255.0 / [double]$maximum
    }
}

function Read-Bmp24Or32 {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "BMP file does not exist: $Path"
    }
    $stream = [IO.File]::OpenRead($Path)
    $reader = [IO.BinaryReader]::new($stream)
    try {
        $signature = [Text.Encoding]::ASCII.GetString($reader.ReadBytes(2))
        if ($signature -ne 'BM') { throw 'Only Windows BMP files are supported.' }
        [void]$reader.ReadUInt32() # file size
        [void]$reader.ReadUInt16() # reserved 1
        [void]$reader.ReadUInt16() # reserved 2
        $pixelOffset = [int64]$reader.ReadUInt32()
        $dibSizeRaw = $reader.ReadUInt32()
        if ($dibSizeRaw -lt 40 -or $dibSizeRaw -gt 4096) { throw "Unsupported BMP DIB header size: $dibSizeRaw" }
        $dibSize = [int]$dibSizeRaw
        if ((14L + [int64]$dibSize) -gt $stream.Length) { throw 'BMP DIB header is truncated.' }
        $width = [int]$reader.ReadInt32()
        $heightSigned = [int]$reader.ReadInt32()
        $planes = [int]$reader.ReadUInt16()
        $bitsPerPixel = [int]$reader.ReadUInt16()
        $compression = [int]$reader.ReadUInt32()
        [void]$reader.ReadUInt32() # image size; may be zero for BI_RGB
        [void]$reader.ReadInt32() # x pixels per metre
        [void]$reader.ReadInt32() # y pixels per metre
        [void]$reader.ReadUInt32() # palette colours
        [void]$reader.ReadUInt32() # important colours

        $redMask = [uint32]0
        $greenMask = [uint32]0
        $blueMask = [uint32]0
        $alphaMask = [uint32]0
        if ($compression -eq 3) {
            # BI_BITFIELDS stores the channel masks immediately after the
            # common DIB fields. The captures produced by SDL use the normal
            # BGRA32 masks, but decoding the masks keeps this contract honest.
            $redMask = $reader.ReadUInt32()
            $greenMask = $reader.ReadUInt32()
            $blueMask = $reader.ReadUInt32()
            if ($dibSize -ge 56) { $alphaMask = $reader.ReadUInt32() }
        }

        $redInfo = Get-MaskMetadata -Mask $redMask
        $greenInfo = Get-MaskMetadata -Mask $greenMask
        $blueInfo = Get-MaskMetadata -Mask $blueMask

        if ($width -le 0 -or $heightSigned -eq 0 -or $heightSigned -eq [int]::MinValue) { throw 'BMP dimensions must be positive.' }
        $height = [Math]::Abs($heightSigned)
        if ($width -gt 8192 -or $height -gt 8192 -or ([int64]$width * [int64]$height) -gt $script:MaxPixels) {
            throw "BMP dimensions exceed the bounded analysis limit: ${width}x${height}."
        }
        if ($planes -ne 1 -or ($bitsPerPixel -ne 24 -and $bitsPerPixel -ne 32) -or
            ($compression -ne 0 -and -not ($compression -eq 3 -and $bitsPerPixel -eq 32))) {
            throw "Expected uncompressed 24/32-bit BMP (planes=1, compression=0 or 32-bit bitfields=3), got bpp=$bitsPerPixel compression=$compression."
        }
        if ($compression -eq 3) {
            if ($redMask -eq 0 -or $greenMask -eq 0 -or $blueMask -eq 0) { throw 'BMP bitfield masks must define red, green, and blue channels.' }
            if (($redMask -band $greenMask) -ne 0 -or ($redMask -band $blueMask) -ne 0 -or ($greenMask -band $blueMask) -ne 0) {
                throw 'BMP bitfield masks overlap.'
            }
        }
        $minimumPixelOffset = 14L + [int64]$dibSize
        if ($compression -eq 3 -and $dibSize -eq 40) { $minimumPixelOffset += 12L }
        if ($pixelOffset -lt $minimumPixelOffset -or $pixelOffset -ge $stream.Length) { throw 'BMP pixel offset is invalid.' }

        $bytesPerPixel = [int]($bitsPerPixel / 8)
        $stride = [int]([Math]::Ceiling(($width * $bytesPerPixel) / 4.0) * 4)
        [void]$stream.Seek($pixelOffset, [IO.SeekOrigin]::Begin)
        $remaining = $stream.Length - $pixelOffset
        $required = [int64]$stride * [int64]$height
        if ($remaining -lt $required) { throw 'BMP pixel payload is truncated.' }
        if ($required -gt [int]::MaxValue) { throw 'BMP pixel payload exceeds the bounded analysis limit.' }
        $pixels = $reader.ReadBytes([int]$required)
        if ($pixels.Length -ne $required) { throw 'BMP pixel payload is truncated.' }
        return [ordered]@{
            width = $width
            height = $height
            heightSigned = $heightSigned
            bitsPerPixel = $bitsPerPixel
            stride = $stride
            bytesPerPixel = $bytesPerPixel
            compression = $compression
            redMask = $redMask
            greenMask = $greenMask
            blueMask = $blueMask
            alphaMask = $alphaMask
            redShift = $redInfo.shift
            greenShift = $greenInfo.shift
            blueShift = $blueInfo.shift
            redScale = $redInfo.scale
            greenScale = $greenInfo.scale
            blueScale = $blueInfo.scale
            topDown = $heightSigned -lt 0
            pixels = $pixels
        }
    }
    finally {
        $reader.Dispose()
        $stream.Dispose()
    }
}

function Convert-SrgbLut {
    $lut = [double[]]::new(256)
    for ($i = 0; $i -lt 256; $i++) {
        $s = $i / 255.0
        $lut[$i] = if ($s -le 0.04045) { $s / 12.92 } else { [Math]::Pow(($s + 0.055) / 1.055, 2.4) }
    }
    return $lut
}

function Build-LumaBuffer {
    param([Parameter(Mandatory = $true)]$Bmp)

    $width = [int]$Bmp.width
    $height = [int]$Bmp.height
    $stride = [int]$Bmp.stride
    $bytesPerPixel = [int]$Bmp.bytesPerPixel
    $pixels = [byte[]]$Bmp.pixels
    $lut = Convert-SrgbLut
    $luma = [double[]]::new($width * $height)
    [long[]]$histogram = [long[]]::new(256)
    $sum = 0.0
    $brightCount = [long]0
    $saturatedCount = [long]0
    $nonZeroCount = [long]0
    $maxLinear = 0.0
    $index = 0

    for ($y = 0; $y -lt $height; $y++) {
        $sourceRow = if ([bool]$Bmp.topDown) { $y } else { $height - 1 - $y }
        $rowOffset = $sourceRow * $stride
        for ($x = 0; $x -lt $width; $x++) {
            $pixelOffset = $rowOffset + ($x * $bytesPerPixel)
            if ([int]$Bmp.compression -eq 3) {
                $raw = [uint32]$pixels[$pixelOffset] -bor
                    ([uint32]$pixels[$pixelOffset + 1] -shl 8) -bor
                    ([uint32]$pixels[$pixelOffset + 2] -shl 16) -bor
                    ([uint32]$pixels[$pixelOffset + 3] -shl 24)
                $red = [int][Math]::Round((($raw -band [uint32]$Bmp.redMask) -shr [int]$Bmp.redShift) * [double]$Bmp.redScale)
                $green = [int][Math]::Round((($raw -band [uint32]$Bmp.greenMask) -shr [int]$Bmp.greenShift) * [double]$Bmp.greenScale)
                $blue = [int][Math]::Round((($raw -band [uint32]$Bmp.blueMask) -shr [int]$Bmp.blueShift) * [double]$Bmp.blueScale)
            }
            else {
                $blue = [int]$pixels[$pixelOffset]
                $green = [int]$pixels[$pixelOffset + 1]
                $red = [int]$pixels[$pixelOffset + 2]
            }
            $linear = (0.2126 * $lut[$red]) + (0.7152 * $lut[$green]) + (0.0722 * $lut[$blue])
            $luma[$index] = $linear
            $sum += $linear
            if ($linear -gt $maxLinear) { $maxLinear = $linear }
            if ($linear -gt 0.000001) { $nonZeroCount++ }
            $histogram[[Math]::Min(255, [Math]::Max(0, [int][Math]::Floor($linear * 255.0)))]++
            if ($linear -ge 0.75) { $brightCount++ }
            if ($red -ge 250 -and $green -ge 250 -and $blue -ge 250) { $saturatedCount++ }
            $index++
        }
    }

    $pixelCount = [int64]$width * [int64]$height
    $p99Target = [int64][Math]::Ceiling($pixelCount * 0.99)
    $running = [int64]0
    $p99Bin = 255
    for ($bin = 0; $bin -lt 256; $bin++) {
        $running += $histogram[$bin]
        if ($running -ge $p99Target) { $p99Bin = $bin; break }
    }
    return [ordered]@{
        values = $luma
        pixelCount = $pixelCount
        meanLinear = $sum / [double]$pixelCount
        maxLinear = $maxLinear
        nonZeroPixelFraction = $nonZeroCount / [double]$pixelCount
        blackScreen = $maxLinear -le 0.000001
        brightPixelFraction = $brightCount / [double]$pixelCount
        saturatedPixelFraction = $saturatedCount / [double]$pixelCount
        p99Linear = ($p99Bin + 1) / 255.0
    }
}

function Convert-NormalizedBounds {
    param(
        [Parameter(Mandatory = $true)][double]$X0,
        [Parameter(Mandatory = $true)][double]$X1,
        [Parameter(Mandatory = $true)][double]$Y0,
        [Parameter(Mandatory = $true)][double]$Y1,
        [Parameter(Mandatory = $true)][int]$Width,
        [Parameter(Mandatory = $true)][int]$Height
    )

    $left = [Math]::Max(0, [Math]::Min($Width - 1, [int][Math]::Floor($X0 * $Width)))
    $right = [Math]::Max($left + 1, [Math]::Min($Width, [int][Math]::Ceiling($X1 * $Width)))
    $top = [Math]::Max(0, [Math]::Min($Height - 1, [int][Math]::Floor($Y0 * $Height)))
    $bottom = [Math]::Max($top + 1, [Math]::Min($Height, [int][Math]::Ceiling($Y1 * $Height)))
    return [ordered]@{ left = $left; right = $right; top = $top; bottom = $bottom }
}

function Get-RegionStats {
    param(
        [Parameter(Mandatory = $true)][double[]]$Values,
        [Parameter(Mandatory = $true)][int]$Width,
        [Parameter(Mandatory = $true)][int]$Height,
        [Parameter(Mandatory = $true)][double]$X0,
        [Parameter(Mandatory = $true)][double]$X1,
        [Parameter(Mandatory = $true)][double]$Y0,
        [Parameter(Mandatory = $true)][double]$Y1
    )

    $bounds = Convert-NormalizedBounds -X0 $X0 -X1 $X1 -Y0 $Y0 -Y1 $Y1 -Width $Width -Height $Height
    $sum = 0.0
    $count = [int64]0
    for ($y = $bounds.top; $y -lt $bounds.bottom; $y++) {
        $offset = $y * $Width
        for ($x = $bounds.left; $x -lt $bounds.right; $x++) {
            $sum += $Values[$offset + $x]
            $count++
        }
    }
    return [ordered]@{
        normalized = [ordered]@{ x0 = $X0; x1 = $X1; y0 = $Y0; y1 = $Y1 }
        pixels = $count
        meanLinear = if ($count -gt 0) { $sum / [double]$count } else { 0.0 }
        sumLinear = $sum
    }
}

function Find-BrightestPixel {
    param(
        [Parameter(Mandatory = $true)][double[]]$Values,
        [Parameter(Mandatory = $true)][int]$Width,
        [Parameter(Mandatory = $true)][int]$Height,
        [Parameter(Mandatory = $true)][double]$X0,
        [Parameter(Mandatory = $true)][double]$X1,
        [Parameter(Mandatory = $true)][double]$Y0,
        [Parameter(Mandatory = $true)][double]$Y1
    )

    $bounds = Convert-NormalizedBounds -X0 $X0 -X1 $X1 -Y0 $Y0 -Y1 $Y1 -Width $Width -Height $Height
    $best = -1.0
    $bestX = -1
    $bestY = -1
    for ($y = $bounds.top; $y -lt $bounds.bottom; $y++) {
        $offset = $y * $Width
        for ($x = $bounds.left; $x -lt $bounds.right; $x++) {
            $value = $Values[$offset + $x]
            if ($value -gt $best) {
                $best = $value
                $bestX = $x
                $bestY = $y
            }
        }
    }
    return [ordered]@{
        found = $bestX -ge 0 -and $best -gt 0.000001
        x = if ($bestX -ge 0 -and $best -gt 0.000001) { $bestX } else { -1 }
        y = if ($bestY -ge 0 -and $best -gt 0.000001) { $bestY } else { -1 }
        xNormalized = if ($bestX -ge 0 -and $best -gt 0.000001) { $bestX / [double]$Width } else { $null }
        yNormalized = if ($bestY -ge 0 -and $best -gt 0.000001) { $bestY / [double]$Height } else { $null }
        peakLinear = [Math]::Max(0.0, $best)
        searchRect = [ordered]@{ x0 = $X0; x1 = $X1; y0 = $Y0; y1 = $Y1 }
    }
}

function Get-RingStats {
    param(
        [Parameter(Mandatory = $true)][double[]]$Values,
        [Parameter(Mandatory = $true)][int]$Width,
        [Parameter(Mandatory = $true)][int]$Height,
        [Parameter(Mandatory = $true)][int]$CenterX,
        [Parameter(Mandatory = $true)][int]$CenterY,
        [Parameter(Mandatory = $true)][double]$InnerRadius,
        [Parameter(Mandatory = $true)][double]$OuterRadius
    )

    $left = [Math]::Max(0, [int][Math]::Floor($CenterX - $OuterRadius))
    $right = [Math]::Min($Width, [int][Math]::Ceiling($CenterX + $OuterRadius + 1))
    $top = [Math]::Max(0, [int][Math]::Floor($CenterY - $OuterRadius))
    $bottom = [Math]::Min($Height, [int][Math]::Ceiling($CenterY + $OuterRadius + 1))
    $innerSquared = $InnerRadius * $InnerRadius
    $outerSquared = $OuterRadius * $OuterRadius
    $sum = 0.0
    $count = [int64]0
    for ($y = $top; $y -lt $bottom; $y++) {
        $dy = $y - $CenterY
        $offset = $y * $Width
        for ($x = $left; $x -lt $right; $x++) {
            $dx = $x - $CenterX
            $distanceSquared = ($dx * $dx) + ($dy * $dy)
            if ($distanceSquared -ge $innerSquared -and $distanceSquared -lt $outerSquared) {
                $sum += $Values[$offset + $x]
                $count++
            }
        }
    }
    return [ordered]@{
        innerRadiusPixels = $InnerRadius
        outerRadiusPixels = $OuterRadius
        pixels = $count
        meanLinear = if ($count -gt 0) { $sum / [double]$count } else { 0.0 }
        sumLinear = $sum
    }
}

try {
    $resolvedImage = (Resolve-Path -LiteralPath $ImagePath -ErrorAction Stop).Path
    $bmp = Read-Bmp24Or32 -Path $resolvedImage
    $imageSha256 = Get-FileSha256 -Path $resolvedImage
    $width = [int]$bmp.width
    $height = [int]$bmp.height
    if ($width -ne $ExpectedWidth -or $height -ne $ExpectedHeight) {
        Add-Issue -Code 'dimensions.mismatch' -Stage 'dimensions' -Message "Expected ${ExpectedWidth}x${ExpectedHeight}, got ${width}x${height}."
    }

    $buffer = Build-LumaBuffer -Bmp $bmp
    $values = [double[]]$buffer.values

    # The pin is authored at (-4.15, 5.75, 1.1) with the fixed reference camera.
    # This broad normalized search window leaves margin for backend rasterization
    # and resolution changes while excluding the textured cards and white gate.
    $source = Find-BrightestPixel -Values $values -Width $width -Height $height `
        -X0 0.24 -X1 0.42 -Y0 0.24 -Y1 0.53

    $smallCoreRadius = [Math]::Max(8.0, [Math]::Round([Math]::Min($width, $height) * 0.012, 2))
    $rings = [ordered]@{
        core = $null
        near = $null
        far = $null
        baseline = $null
    }
    if ($source.found) {
        $rings.core = Get-RingStats -Values $values -Width $width -Height $height -CenterX $source.x -CenterY $source.y -InnerRadius 0 -OuterRadius $smallCoreRadius
        $rings.near = Get-RingStats -Values $values -Width $width -Height $height -CenterX $source.x -CenterY $source.y -InnerRadius ($smallCoreRadius * 1.5) -OuterRadius ($smallCoreRadius * 3.5)
        $rings.far = Get-RingStats -Values $values -Width $width -Height $height -CenterX $source.x -CenterY $source.y -InnerRadius ($smallCoreRadius * 3.5) -OuterRadius ($smallCoreRadius * 6.5)
        $rings.baseline = Get-RingStats -Values $values -Width $width -Height $height -CenterX $source.x -CenterY $source.y -InnerRadius ($smallCoreRadius * 7.5) -OuterRadius ($smallCoreRadius * 11.0)
    }

    $coreMean = if ($null -ne $rings.core) { [double]$rings.core.meanLinear } else { 0.0 }
    $nearMean = if ($null -ne $rings.near) { [double]$rings.near.meanLinear } else { 0.0 }
    $farMean = if ($null -ne $rings.far) { [double]$rings.far.meanLinear } else { 0.0 }
    $baselineMean = if ($null -ne $rings.baseline) { [double]$rings.baseline.meanLinear } else { 0.0 }
    $nearLift = [Math]::Max(0.0, $nearMean - $baselineMean)
    $farLift = [Math]::Max(0.0, $farMean - $baselineMean)
    $haloEnergy = if ($null -ne $rings.near -and $null -ne $rings.far) {
        ($nearLift * [double]$rings.near.pixels) + ($farLift * [double]$rings.far.pixels)
    } else { 0.0 }
    $corePixels = 0
    if ($null -ne $rings.core) { $corePixels = [int64]$rings.core.pixels }
    $coreEnergy = [Math]::Max(0.000001, $coreMean * [double]$corePixels)
    $haloEnergyRatio = $haloEnergy / $coreEnergy

    # This fixed broad ROI is the large weak panel. It is reported separately
    # from the halo gate so a backend's exact perspective edge cannot hide a
    # valid high-frequency Bloom result.
    $largeArea = Get-RegionStats -Values $values -Width $width -Height $height -X0 0.36 -X1 0.64 -Y0 0.13 -Y1 0.31
    $largeAreaAbove = Get-RegionStats -Values $values -Width $width -Height $height -X0 0.36 -X1 0.64 -Y0 0.06 -Y1 0.12
    $largeAreaLift = [double]$largeArea.meanLinear - [double]$largeAreaAbove.meanLinear

    $thresholds = [ordered]@{
        sourcePeakLinearMin = 0.55
        sourceCoreMeanLinearMin = 0.16
        nearLiftLinearMin = 0.0020
        farLiftLinearMin = 0.00045
        # This is total baseline-subtracted halo energy divided by total core
        # energy, not a per-pixel luminance ratio. The two annuli contain far
        # more pixels than the core by design, so a healthy multi-scale halo
        # may carry several times the core's integrated energy. The bounded
        # interval still rejects an absent pyramid and an energy runaway.
        haloEnergyRatioMin = 0.5
        haloEnergyRatioMax = 12.0
        globalMeanLinearMax = 0.42
        brightPixelFractionMax = 0.20
        saturatedPixelFractionMax = 0.12
        largeAreaLiftLinearMin = -0.02
    }

    $checks = [ordered]@{
        dimensions = ($width -eq $ExpectedWidth -and $height -eq $ExpectedHeight)
        blackScreen = -not [bool]$buffer.blackScreen
        sourceCore = [bool]$source.found -and [double]$source.peakLinear -ge $thresholds.sourcePeakLinearMin -and $coreMean -ge $thresholds.sourceCoreMeanLinearMin
        nearHalo = [bool]$source.found -and $nearLift -ge $thresholds.nearLiftLinearMin
        farHalo = [bool]$source.found -and $farLift -ge $thresholds.farLiftLinearMin
        haloEnergy = [bool]$source.found -and $haloEnergyRatio -ge $thresholds.haloEnergyRatioMin -and $haloEnergyRatio -le $thresholds.haloEnergyRatioMax
        globalEnergy = [double]$buffer.meanLinear -le $thresholds.globalMeanLinearMax -and
            [double]$buffer.brightPixelFraction -le $thresholds.brightPixelFractionMax -and
            [double]$buffer.saturatedPixelFraction -le $thresholds.saturatedPixelFractionMax
        largeWeakArea = $largeAreaLift -ge $thresholds.largeAreaLiftLinearMin
    }

    foreach ($check in $checks.Keys) {
        if (-not [bool]$checks[$check]) {
            Add-Issue -Code "bloom.$check-failed" -Stage 'quality' -Message "Bloom quality check '$check' did not meet its fixed threshold."
        }
    }
    if ([bool]$buffer.blackScreen) {
        Add-Issue -Code 'bloom.black-screen' -Stage 'quality' -Message "The captured BMP is a black screen (maximum linear luminance $($buffer.maxLinear))."
    }

    $receipt = [ordered]@{
        schemaVersion = 'noemancer.bloom-quality-evidence/0.1'
        success = ($script:Issues.Count -eq 0)
        image = [ordered]@{
            path = $resolvedImage
            sha256 = $imageSha256
            width = $width
            height = $height
            bitsPerPixel = $bmp.bitsPerPixel
            topDown = $bmp.topDown
        }
        fixture = [ordered]@{
            schema = 'noemancer.bloom-quality-fixture/0.1'
            smallHighIntensitySourceId = 'entity.reference.emissive-cool'
            largeWeakAreaSourceId = 'entity.reference.emissive-warm'
            darkNeighbor = 'upper-background-and-shadow-column'
            coordinateSpace = 'output-normalized'
            sourceSearch = $source.searchRect
        }
        thresholds = $thresholds
        metrics = [ordered]@{
            global = [ordered]@{
                meanLinear = $buffer.meanLinear
                maxLinear = $buffer.maxLinear
                nonZeroPixelFraction = $buffer.nonZeroPixelFraction
                blackScreen = $buffer.blackScreen
                p99Linear = $buffer.p99Linear
                brightPixelFraction = $buffer.brightPixelFraction
                saturatedPixelFraction = $buffer.saturatedPixelFraction
            }
            source = $source
            rings = $rings
            halo = [ordered]@{
                baselineMeanLinear = $baselineMean
                nearLiftLinear = $nearLift
                farLiftLinear = $farLift
                energyLinear = $haloEnergy
                coreEnergyLinear = $coreEnergy
                energyRatio = $haloEnergyRatio
                radii = [ordered]@{
                    core = $smallCoreRadius
                    near = @(([double]$smallCoreRadius * 1.5), ([double]$smallCoreRadius * 3.5))
                    far = @(([double]$smallCoreRadius * 3.5), ([double]$smallCoreRadius * 6.5))
                    baseline = @(([double]$smallCoreRadius * 7.5), ([double]$smallCoreRadius * 11.0))
                }
            }
            largeWeakArea = [ordered]@{
                region = $largeArea
                adjacentAbove = $largeAreaAbove
                liftLinear = $largeAreaLift
            }
        }
        checks = $checks
        issues = @($script:Issues.ToArray())
    }
    $analysisExitCode = 3
    if ($receipt.success) { $analysisExitCode = 0 }
    Write-ReceiptAndExit -Receipt $receipt -ExitCode $analysisExitCode
}
catch {
    $receipt = [ordered]@{
        schemaVersion = 'noemancer.bloom-quality-evidence/0.1'
        success = $false
        image = [ordered]@{
            path = $ImagePath
            sha256 = $null
        }
        checks = [ordered]@{}
        issues = @([ordered]@{
            code = 'analysis.unexpected'
            stage = 'analysis'
            message = Limit-Text "$($_.Exception.GetType().Name): $($_.Exception.Message) [$($_.InvocationInfo.PositionMessage)]"
        })
    }
    try {
        $resolvedForError = (Resolve-Path -LiteralPath $ImagePath -ErrorAction Stop).Path
        $receipt.image.path = $resolvedForError
        $receipt.image.sha256 = Get-FileSha256 -Path $resolvedForError
    }
    catch {
        # Keep the structured failure receipt writable even when the input is missing or unreadable.
    }
    $unexpectedExitCode = 2
    if (Test-Path -LiteralPath $ImagePath -PathType Leaf) { $unexpectedExitCode = 3 }
    Write-ReceiptAndExit -Receipt $receipt -ExitCode $unexpectedExitCode
}
