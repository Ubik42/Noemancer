[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [Alias('ColorResponseImage', 'InputImage')]
    [string]$ImagePath,

    [Parameter(Mandatory = $true)]
    [string]$ContractPath,

    [string]$OutputPath = ''
)

# Offline verifier for the Commercial Raster 1.8 color-response fixture.
# The image is interpreted as sRGB output, converted to linear light for the
# quality checks, and never starts the renderer.  Contract ROI coordinates are
# normalized by default; a fixture may explicitly use coordinateSpace = pixel.
#
# Exit contract:
#   0 = the fixture and all response checks passed
#   2 = invalid invocation, contract, or unsupported/truncated BMP
#   3 = a readable fixture failed one or more quality checks
#   7 = the requested receipt could not be written
#   1 = unexpected verifier failure

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$script:Issues = [System.Collections.Generic.List[object]]::new()
$script:MaxDimension = 8192
$script:MaxPixels = 8192 * 8192
$script:ReceiptSchema = 'noemancer.color-response-quality-evidence/0.1'
$script:ReferenceId = 'noemancer.commercial-raster-reference/1.8'

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

function Get-PropertyOrNull {
    param(
        [AllowNull()]$Object,
        [Parameter(Mandatory = $true)][string]$Name
    )

    if ($null -eq $Object) { return $null }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}

function Get-FirstProperty {
    param(
        [AllowNull()]$Object,
        [Parameter(Mandatory = $true)][string[]]$Names
    )

    if ($null -eq $Object) { return $null }
    foreach ($name in $Names) {
        $property = $Object.PSObject.Properties[$name]
        if ($null -ne $property) { return $property.Value }
    }
    return $null
}

function Convert-ToFiniteDouble {
    param(
        [Parameter(Mandatory = $true)]$Value,
        [Parameter(Mandatory = $true)][string]$Label
    )

    try { $number = [double]$Value }
    catch { throw "Contract field '$Label' is not numeric." }
    if ([double]::IsNaN($number) -or [double]::IsInfinity($number)) {
        throw "Contract field '$Label' must be finite."
    }
    return $number
}

function Get-OptionalFiniteDouble {
    param(
        [AllowNull()]$Object,
        [Parameter(Mandatory = $true)][string[]]$Names,
        [Parameter(Mandatory = $true)][string]$Label
    )

    $value = Get-FirstProperty -Object $Object -Names $Names
    if ($null -eq $value) { return $null }
    return (Convert-ToFiniteDouble -Value $value -Label $Label)
}

function Get-StringOrEmpty {
    param(
        [AllowNull()]$Object,
        [Parameter(Mandatory = $true)][string[]]$Names
    )
    $value = Get-FirstProperty -Object $Object -Names $Names
    if ($null -eq $value) { return '' }
    return [string]$value
}

function Get-MaximumValue {
    param([Parameter(Mandatory = $true)][object[]]$Values)
    if ($Values.Count -eq 0) { throw 'Cannot compute a maximum over an empty sample set.' }
    return [double](($Values | Measure-Object -Maximum).Maximum)
}

function Get-MinimumValue {
    param([Parameter(Mandatory = $true)][object[]]$Values)
    if ($Values.Count -eq 0) { throw 'Cannot compute a minimum over an empty sample set.' }
    return [double](($Values | Measure-Object -Minimum).Minimum)
}

function Get-MaskMetadata {
    param([Parameter(Mandatory = $true)][uint32]$Mask)

    if ($Mask -eq 0) { throw 'BMP bitfield mask is empty.' }
    $shift = 0
    $remaining = $Mask
    while (($remaining -band [uint32]1) -eq 0) {
        $remaining = $remaining -shr 1
        $shift++
    }
    $maximum = [uint32]($Mask -shr $shift)
    while (($remaining -band [uint32]1) -eq 1) { $remaining = $remaining -shr 1 }
    if ($remaining -ne 0 -or $maximum -eq 0) {
        throw "BMP bitfield mask is not a non-empty contiguous mask: 0x$('{0:X8}' -f $Mask)."
    }
    return [ordered]@{
        shift = $shift
        maximum = $maximum
        scale = 255.0 / [double]$maximum
    }
}

function Convert-SrgbLut {
    $lut = [double[]]::new(256)
    for ($index = 0; $index -lt 256; $index++) {
        $srgb = $index / 255.0
        $lut[$index] = if ($srgb -le 0.04045) {
            $srgb / 12.92
        } else {
            [Math]::Pow(($srgb + 0.055) / 1.055, 2.4)
        }
    }
    return $lut
}

function Read-BmpChannels {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "BMP file does not exist: $Path"
    }
    $stream = [IO.File]::OpenRead($Path)
    $reader = [IO.BinaryReader]::new($stream)
    try {
        if ($stream.Length -lt 54) { throw 'BMP file is shorter than the minimum header.' }
        $signature = [Text.Encoding]::ASCII.GetString($reader.ReadBytes(2))
        if ($signature -ne 'BM') { throw 'Only Windows BMP files are supported.' }
        [void]$reader.ReadUInt32()
        [void]$reader.ReadUInt16()
        [void]$reader.ReadUInt16()
        $pixelOffset = [int64]$reader.ReadUInt32()
        $dibStart = $stream.Position
        $dibSizeRaw = $reader.ReadUInt32()
        if ($dibSizeRaw -lt 40 -or $dibSizeRaw -gt 4096) {
            throw "Unsupported BMP DIB header size: $dibSizeRaw"
        }
        $dibSize = [int]$dibSizeRaw
        if ($dibStart + [int64]$dibSize -gt $stream.Length) {
            throw 'BMP DIB header is truncated.'
        }
        $width = [int]$reader.ReadInt32()
        $heightSigned = [int]$reader.ReadInt32()
        $planes = [int]$reader.ReadUInt16()
        $bitsPerPixel = [int]$reader.ReadUInt16()
        $compression = [int]$reader.ReadUInt32()
        [void]$reader.ReadUInt32()
        [void]$reader.ReadInt32()
        [void]$reader.ReadInt32()
        [void]$reader.ReadUInt32()
        [void]$reader.ReadUInt32()

        if ($width -le 0 -or $heightSigned -eq 0 -or $heightSigned -eq [int]::MinValue) {
            throw 'BMP dimensions must be positive.'
        }
        $height = [Math]::Abs($heightSigned)
        if ($width -gt $script:MaxDimension -or $height -gt $script:MaxDimension -or
            ([int64]$width * [int64]$height) -gt $script:MaxPixels) {
            throw "BMP dimensions exceed the bounded analysis limit: ${width}x${height}."
        }
        if ($planes -ne 1 -or ($bitsPerPixel -ne 24 -and $bitsPerPixel -ne 32) -or
            ($compression -ne 0 -and -not ($compression -eq 3 -and $bitsPerPixel -eq 32))) {
            throw "Expected uncompressed 24/32-bit BMP (or 32-bit bitfields), got bpp=$bitsPerPixel compression=$compression."
        }

        $redMask = [uint32]0
        $greenMask = [uint32]0
        $blueMask = [uint32]0
        $redInfo = $null
        $greenInfo = $null
        $blueInfo = $null
        $minimumPixelOffset = $dibStart + [int64]$dibSize
        if ($compression -eq 3) {
            if ($dibSize -eq 40) { $minimumPixelOffset += 12L }
            if ($stream.Length -lt ($dibStart + 52L)) {
                throw '32-bit bitfield BMP is missing RGB masks.'
            }
            [void]$stream.Seek($dibStart + 40L, [IO.SeekOrigin]::Begin)
            $redMask = $reader.ReadUInt32()
            $greenMask = $reader.ReadUInt32()
            $blueMask = $reader.ReadUInt32()
            if (($redMask -band $greenMask) -ne 0 -or ($redMask -band $blueMask) -ne 0 -or
                ($greenMask -band $blueMask) -ne 0) {
                throw '32-bit bitfield BMP RGB masks overlap.'
            }
            $redInfo = Get-MaskMetadata -Mask $redMask
            $greenInfo = Get-MaskMetadata -Mask $greenMask
            $blueInfo = Get-MaskMetadata -Mask $blueMask
        }

        $bytesPerPixel = [int]($bitsPerPixel / 8)
        $stride = [int]([Math]::Ceiling(($width * $bytesPerPixel) / 4.0) * 4)
        if ($pixelOffset -lt $minimumPixelOffset -or $pixelOffset -ge $stream.Length) {
            throw 'BMP pixel offset is invalid.'
        }
        $required = [int64]$stride * [int64]$height
        if (($stream.Length - $pixelOffset) -lt $required) { throw 'BMP pixel payload is truncated.' }
        if ($required -gt [int]::MaxValue) { throw 'BMP pixel payload exceeds the bounded analysis limit.' }
        [void]$stream.Seek($pixelOffset, [IO.SeekOrigin]::Begin)
        $pixels = $reader.ReadBytes([int]$required)
        if ($pixels.Length -ne $required) { throw 'BMP pixel payload could not be read completely.' }

        $pixelCount = $width * $height
        $red = [byte[]]::new($pixelCount)
        $green = [byte[]]::new($pixelCount)
        $blue = [byte[]]::new($pixelCount)
        $valueIndex = 0
        for ($y = 0; $y -lt $height; $y++) {
            $sourceRow = if ($heightSigned -lt 0) { $y } else { $height - 1 - $y }
            $rowOffset = $sourceRow * $stride
            for ($x = 0; $x -lt $width; $x++) {
                $pixelOffsetInRow = $rowOffset + ($x * $bytesPerPixel)
                if ($compression -eq 3) {
                    $raw = [uint32]$pixels[$pixelOffsetInRow] -bor
                        ([uint32]$pixels[$pixelOffsetInRow + 1] -shl 8) -bor
                        ([uint32]$pixels[$pixelOffsetInRow + 2] -shl 16) -bor
                        ([uint32]$pixels[$pixelOffsetInRow + 3] -shl 24)
                    $redSample = [int][Math]::Round((($raw -band $redMask) -shr [int]$redInfo.shift) * [double]$redInfo.scale)
                    $greenSample = [int][Math]::Round((($raw -band $greenMask) -shr [int]$greenInfo.shift) * [double]$greenInfo.scale)
                    $blueSample = [int][Math]::Round((($raw -band $blueMask) -shr [int]$blueInfo.shift) * [double]$blueInfo.scale)
                } else {
                    $blueSample = [int]$pixels[$pixelOffsetInRow]
                    $greenSample = [int]$pixels[$pixelOffsetInRow + 1]
                    $redSample = [int]$pixels[$pixelOffsetInRow + 2]
                }
                $red[$valueIndex] = [byte][Math]::Min(255, [Math]::Max(0, $redSample))
                $green[$valueIndex] = [byte][Math]::Min(255, [Math]::Max(0, $greenSample))
                $blue[$valueIndex] = [byte][Math]::Min(255, [Math]::Max(0, $blueSample))
                $valueIndex++
            }
        }
        return [ordered]@{
            path = [IO.Path]::GetFullPath($Path)
            width = $width
            height = $height
            bitsPerPixel = $bitsPerPixel
            compression = $compression
            topDown = ($heightSigned -lt 0)
            red = $red
            green = $green
            blue = $blue
        }
    }
    finally {
        $reader.Dispose()
        $stream.Dispose()
    }
}

function Get-CoordinateSpace {
    param([Parameter(Mandatory = $true)]$Fixture)
    $space = Get-StringOrEmpty -Object $Fixture -Names @('coordinateSpace', 'analysisCoordinateSpace', 'coordinateSystem', 'roiCoordinateSpace')
    if ([string]::IsNullOrWhiteSpace($space)) { return 'normalized' }
    $normalized = $space.Trim().ToLowerInvariant()
    if ($normalized -in @('normalized', 'output-normalized', 'uv', 'unit')) { return 'normalized' }
    if ($normalized -in @('pixel', 'pixels', 'image-pixel')) { return 'pixel' }
    throw "Unsupported color-response coordinate space '$space'."
}

function Get-RoiBounds {
    param(
        [Parameter(Mandatory = $true)]$Spec,
        [Parameter(Mandatory = $true)][string]$Label,
        [Parameter(Mandatory = $true)][int]$ImageWidth,
        [Parameter(Mandatory = $true)][int]$ImageHeight,
        [Parameter(Mandatory = $true)][string]$CoordinateSpace
    )

    $roi = Get-FirstProperty -Object $Spec -Names @('roi', 'region', 'bounds', 'rectangle')
    if ($null -eq $roi) { $roi = $Spec }
    $arrayRect = Get-FirstProperty -Object $roi -Names @('rect', 'rectangle')
    if ($arrayRect -is [System.Array] -and @($arrayRect).Count -ge 4) {
        $x = Convert-ToFiniteDouble -Value $arrayRect[0] -Label "$Label.x"
        $y = Convert-ToFiniteDouble -Value $arrayRect[1] -Label "$Label.y"
        $w = Convert-ToFiniteDouble -Value $arrayRect[2] -Label "$Label.width"
        $h = Convert-ToFiniteDouble -Value $arrayRect[3] -Label "$Label.height"
        $x0 = $x; $y0 = $y; $x1 = $x + $w; $y1 = $y + $h
    } else {
        $x0 = Get-OptionalFiniteDouble -Object $roi -Names @('x0', 'left', 'minX') -Label "$Label.x0"
        $x1 = Get-OptionalFiniteDouble -Object $roi -Names @('x1', 'right', 'maxX') -Label "$Label.x1"
        $y0 = Get-OptionalFiniteDouble -Object $roi -Names @('y0', 'top', 'minY') -Label "$Label.y0"
        $y1 = Get-OptionalFiniteDouble -Object $roi -Names @('y1', 'bottom', 'maxY') -Label "$Label.y1"
        if ($null -eq $x0) { $x0 = Get-OptionalFiniteDouble -Object $roi -Names @('x', 'originX') -Label "$Label.x" }
        if ($null -eq $y0) { $y0 = Get-OptionalFiniteDouble -Object $roi -Names @('y', 'originY') -Label "$Label.y" }
        $w = Get-OptionalFiniteDouble -Object $roi -Names @('width', 'w', 'sizeX') -Label "$Label.width"
        $h = Get-OptionalFiniteDouble -Object $roi -Names @('height', 'h', 'sizeY') -Label "$Label.height"
        if ($null -eq $x1 -and $null -ne $x0 -and $null -ne $w) { $x1 = $x0 + $w }
        if ($null -eq $y1 -and $null -ne $y0 -and $null -ne $h) { $y1 = $y0 + $h }
    }
    if ($null -eq $x0 -or $null -eq $x1 -or $null -eq $y0 -or $null -eq $y1) {
        throw "ROI '$Label' must provide x0/x1/y0/y1 or x/y/width/height."
    }
    if ($CoordinateSpace -eq 'pixel') {
        $x0 = $x0 / [double]$ImageWidth
        $x1 = $x1 / [double]$ImageWidth
        $y0 = $y0 / [double]$ImageHeight
        $y1 = $y1 / [double]$ImageHeight
    }
    if ($x0 -lt 0 -or $x1 -gt 1 -or $y0 -lt 0 -or $y1 -gt 1 -or $x1 -le $x0 -or $y1 -le $y0) {
        throw "ROI '$Label' is outside the image or has non-positive extent."
    }
    return [ordered]@{ x0 = $x0; y0 = $y0; x1 = $x1; y1 = $y1 }
}

function Get-SampleSpecs {
    param(
        [Parameter(Mandatory = $true)]$Group,
        [Parameter(Mandatory = $true)][string]$Label,
        [Parameter(Mandatory = $true)][int]$ImageWidth,
        [Parameter(Mandatory = $true)][int]$ImageHeight,
        [Parameter(Mandatory = $true)][string]$CoordinateSpace
    )

    $raw = Get-FirstProperty -Object $Group -Names @('samples', 'rois', 'regions', 'entries', 'patches')
    if ($null -eq $raw -and $Group -is [System.Array]) { $raw = $Group }
    if ($null -ne $raw) {
        if ($raw -isnot [System.Array] -and $raw -is [pscustomobject] -and
            $null -eq (Get-FirstProperty -Object $raw -Names @('roi', 'region', 'bounds', 'x', 'x0'))) {
            $raw = @($raw.PSObject.Properties | ForEach-Object {
                [pscustomobject]@{ name = $_.Name; value = $_.Value }
            })
        }
        return @($raw)
    }

    $roi = Get-FirstProperty -Object $Group -Names @('roi', 'region', 'bounds', 'rectangle')
    if ($null -eq $roi -and $null -ne (Get-FirstProperty -Object $Group -Names @('x0', 'left', 'minX'))) {
        $roi = $Group
    }
    $countValue = Get-FirstProperty -Object $Group -Names @('sampleCount', 'columnCount', 'count', 'levels')
    if ($null -ne $roi -and $null -ne $countValue) {
        $count = [int](Convert-ToFiniteDouble -Value $countValue -Label "$Label.sampleCount")
        if ($count -lt 2 -or $count -gt 4096) { throw "$Label sampleCount must be between 2 and 4096." }
        $base = Get-RoiBounds -Spec $Group -Label "$Label.roi" -ImageWidth $ImageWidth -ImageHeight $ImageHeight -CoordinateSpace $CoordinateSpace
        $axis = (Get-StringOrEmpty -Object $Group -Names @('axis', 'sampleAxis')).ToLowerInvariant()
        if ([string]::IsNullOrWhiteSpace($axis)) { $axis = 'horizontal' }
        $centers = Get-FirstProperty -Object $Group -Names @('columnCenters', 'sampleCenters', 'centers')
        $halfWidth = Get-OptionalFiniteDouble -Object $Group -Names @('columnHalfWidth', 'sampleHalfWidth', 'halfWidth') -Label "$Label.halfWidth"
        if ($null -ne $centers -and @($centers).Count -ne $count) {
            throw "$Label center count does not match sampleCount."
        }
        $result = @()
        for ($index = 0; $index -lt $count; $index++) {
            if ($null -ne $centers -and $null -ne $halfWidth -and $axis -notin @('vertical', 'y')) {
                $center = Convert-ToFiniteDouble -Value $centers[$index] -Label "$Label.center[$index]"
                $sampleRoi = [ordered]@{
                    x0 = $center - $halfWidth; x1 = $center + $halfWidth
                    y0 = $base.y0; y1 = $base.y1
                }
            } elseif ($null -ne $centers -and $null -ne $halfWidth) {
                $center = Convert-ToFiniteDouble -Value $centers[$index] -Label "$Label.center[$index]"
                $sampleRoi = [ordered]@{
                    x0 = $base.x0; x1 = $base.x1
                    y0 = $center - $halfWidth; y1 = $center + $halfWidth
                }
            } else {
                $t0 = $index / [double]$count
                $t1 = ($index + 1) / [double]$count
                if ($axis -in @('vertical', 'y')) {
                    $sampleRoi = [ordered]@{
                        x0 = $base.x0; x1 = $base.x1
                        y0 = $base.y0 + (($base.y1 - $base.y0) * $t0)
                        y1 = $base.y0 + (($base.y1 - $base.y0) * $t1)
                    }
                } else {
                    $sampleRoi = [ordered]@{
                        x0 = $base.x0 + (($base.x1 - $base.x0) * $t0)
                        x1 = $base.x0 + (($base.x1 - $base.x0) * $t1)
                        y0 = $base.y0; y1 = $base.y1
                    }
                }
            }
            $result += [pscustomobject]@{
                index = $index
                order = $index
                roi = [pscustomobject]$sampleRoi
            }
        }
        return @($result)
    }
    return @($Group)
}

function Convert-ChannelName {
    param([AllowNull()][string]$Name)
    if ([string]::IsNullOrWhiteSpace($Name)) { return '' }
    switch ($Name.Trim().ToLowerInvariant()) {
        'r' { return 'red' }
        'red' { return 'red' }
        'g' { return 'green' }
        'green' { return 'green' }
        'b' { return 'blue' }
        'blue' { return 'blue' }
        default { return $Name.Trim().ToLowerInvariant() }
    }
}

function Get-SampleOrder {
    param(
        [Parameter(Mandatory = $true)]$Spec,
        [Parameter(Mandatory = $true)][int]$Index
    )
    $value = Get-FirstProperty -Object $Spec -Names @('order', 'input', 'inputValue', 'level', 'value', 'exposure')
    if ($null -eq $value) { return [double]$Index }
    return (Convert-ToFiniteDouble -Value $value -Label "sample[$Index].order")
}

function Measure-Region {
    param(
        [Parameter(Mandatory = $true)]$Image,
        [Parameter(Mandatory = $true)]$Bounds,
        [Parameter(Mandatory = $true)][double[]]$Lut
    )

    $x0 = [Math]::Max(0, [Math]::Min($Image.width - 1, [int][Math]::Floor($Bounds.x0 * $Image.width)))
    $x1 = [Math]::Max($x0 + 1, [Math]::Min($Image.width, [int][Math]::Ceiling($Bounds.x1 * $Image.width)))
    $y0 = [Math]::Max(0, [Math]::Min($Image.height - 1, [int][Math]::Floor($Bounds.y0 * $Image.height)))
    $y1 = [Math]::Max($y0 + 1, [Math]::Min($Image.height, [int][Math]::Ceiling($Bounds.y1 * $Image.height)))
    $count = 0
    $sumR = 0.0; $sumG = 0.0; $sumB = 0.0; $sumL = 0.0
    $clippedChannels = 0
    for ($y = $y0; $y -lt $y1; $y++) {
        $row = $y * $Image.width
        for ($x = $x0; $x -lt $x1; $x++) {
            $index = $row + $x
            $rByte = [int]$Image.red[$index]
            $gByte = [int]$Image.green[$index]
            $bByte = [int]$Image.blue[$index]
            $r = $Lut[$rByte]; $g = $Lut[$gByte]; $b = $Lut[$bByte]
            $sumR += $r; $sumG += $g; $sumB += $b
            $sumL += (0.2126 * $r) + (0.7152 * $g) + (0.0722 * $b)
            if ($rByte -ge 255) { $clippedChannels++ }
            if ($gByte -ge 255) { $clippedChannels++ }
            if ($bByte -ge 255) { $clippedChannels++ }
            $count++
        }
    }
    if ($count -le 0) { throw 'ROI resolved to zero pixels.' }
    $meanR = $sumR / $count; $meanG = $sumG / $count; $meanB = $sumB / $count; $meanL = $sumL / $count
    $maximum = [Math]::Max($meanR, [Math]::Max($meanG, $meanB))
    $minimum = [Math]::Min($meanR, [Math]::Min($meanG, $meanB))
    return [ordered]@{
        meanRed = $meanR
        meanGreen = $meanG
        meanBlue = $meanB
        linearLuma = $meanL
        neutralError = ($maximum - $minimum) / [Math]::Max($maximum, 0.02)
        clippedFraction = $clippedChannels / [double]($count * 3)
        pixelCount = $count
        bounds = $Bounds
    }
}

function Get-GlobalMetrics {
    param(
        [Parameter(Mandatory = $true)]$Image,
        [Parameter(Mandatory = $true)][double[]]$Lut
    )
    $count = $Image.width * $Image.height
    $sumL = 0.0; $black = 0; $maximum = 0.0
    for ($index = 0; $index -lt $count; $index++) {
        $r = $Lut[[int]$Image.red[$index]]
        $g = $Lut[[int]$Image.green[$index]]
        $b = $Lut[[int]$Image.blue[$index]]
        $luma = (0.2126 * $r) + (0.7152 * $g) + (0.0722 * $b)
        $sumL += $luma
        $maximum = [Math]::Max($maximum, [Math]::Max($r, [Math]::Max($g, $b)))
        if ($luma -le 0.001) { $black++ }
    }
    return [ordered]@{
        pixelCount = $count
        meanLinearLuma = $sumL / [double]$count
        blackPixelFraction = $black / [double]$count
        maxLinearChannel = $maximum
    }
}

function Write-ReceiptAndExit {
    param(
        [Parameter(Mandatory = $true)]$Receipt,
        [Parameter(Mandatory = $true)][int]$ExitCode
    )

    $json = $Receipt | ConvertTo-Json -Depth 40
    $writeFailed = $false
    if (-not [string]::IsNullOrWhiteSpace($OutputPath)) {
        try {
            $target = [IO.Path]::GetFullPath($OutputPath)
            $parent = [IO.Path]::GetDirectoryName($target)
            if (-not [string]::IsNullOrWhiteSpace($parent)) {
                New-Item -ItemType Directory -Path $parent -Force | Out-Null
            }
            Set-Content -LiteralPath $target -Value $json -Encoding UTF8
        } catch {
            $writeFailed = $true
            [Console]::Error.WriteLine("Could not write color-response quality receipt '$OutputPath': $($_.Exception.Message)")
        }
    }
    [Console]::Out.WriteLine($json)
    if ($writeFailed) { exit 7 }
    exit $ExitCode
}

$imageFullPath = $null
$contractFullPath = $null
$receipt = [ordered]@{
    schemaVersion = $script:ReceiptSchema
    success = $false
    generatedAt = [DateTimeOffset]::UtcNow.ToString('o')
    contract = [ordered]@{ path = $ContractPath }
    image = [ordered]@{ path = $ImagePath }
    thresholds = [ordered]@{}
    metrics = [ordered]@{}
    checks = [ordered]@{}
    issues = @()
}
$exitCode = 1

try {
    $imageFullPath = [IO.Path]::GetFullPath($ImagePath)
    $contractFullPath = [IO.Path]::GetFullPath($ContractPath)
    $receipt.image.path = $imageFullPath
    $receipt.contract.path = $contractFullPath
    $receipt.contract.sha256 = Get-FileSha256 -Path $contractFullPath
    $contract = Get-Content -LiteralPath $contractFullPath -Raw -ErrorAction Stop | ConvertFrom-Json -ErrorAction Stop
    $referenceId = [string](Get-FirstProperty -Object $contract -Names @('id', 'referenceId', 'contractId'))
    if ($referenceId -cne $script:ReferenceId) { throw "Unexpected reference contract id '$referenceId'." }
    $fixture = Get-FirstProperty -Object $contract -Names @('colorResponseFixture', 'colorResponse', 'colourResponseFixture')
    if ($null -eq $fixture) { throw 'Contract is missing colorResponseFixture.' }
    $fixtureSchema = [string](Get-FirstProperty -Object $fixture -Names @('schemaVersion', 'schema', 'id'))
    if ([string]::IsNullOrWhiteSpace($fixtureSchema) -or $fixtureSchema -notmatch '^noemancer\.color-response-fixture/[^\s]+$') {
        throw "Unsupported color-response fixture schema '$fixtureSchema'."
    }
    $receipt.contract.id = $referenceId
    $receipt.contract.fixtureSchema = $fixtureSchema

    $image = Read-BmpChannels -Path $imageFullPath
    $receipt.image.width = $image.width
    $receipt.image.height = $image.height
    $receipt.image.bitsPerPixel = $image.bitsPerPixel
    $receipt.image.compression = $image.compression
    $receipt.image.topDown = $image.topDown
    $receipt.image.sha256 = Get-FileSha256 -Path $imageFullPath

    $coordinateSpace = Get-CoordinateSpace -Fixture $fixture
    $defaults = [ordered]@{
        globalMeanLinearMin = 0.01
        globalBlackPixelFractionMax = 0.98
        grayMonotonicTolerance = 0.002
        grayAdjacentLumaMin = 0.0
        grayNeutralBiasMax = 0.08
        grayChannelSpreadMax = 0.12
        grayRangeMin = 0.02
        highlightMonotonicTolerance = 0.002
        highlightAdjacentLumaMin = 0.0
        highlightShoulderCompressionMax = 0.95
        highlightClippedFractionMax = 0.20
        highlightRangeMin = 0.05
        swatchDominanceRatioMin = 1.15
        swatchPrimaryLinearMin = 0.20
        swatchChannelMarginMin = 0.02
        swatchChromaMin = 0.05
        swatchDistinctChannelMin = 3
    }
    $override = Get-FirstProperty -Object $fixture -Names @('thresholds', 'qualityThresholds')
    if ($null -eq $override) { $override = Get-FirstProperty -Object $contract -Names @('colorResponseThresholds', 'qualityThresholds') }
    $thresholdAliases = @{
        grayAdjacentLumaMin = @('grayAdjacentLumaMin', 'neutralAdjacentLumaMin')
        grayChannelSpreadMax = @('grayChannelSpreadMax', 'neutralChannelSpreadMax')
        grayNeutralBiasMax = @('grayNeutralBiasMax', 'neutralRelativeBiasMax')
        highlightAdjacentLumaMin = @('highlightAdjacentLumaMin', 'hdrAdjacentLumaMin')
        highlightShoulderCompressionMax = @('highlightShoulderCompressionMax', 'hdrLateStepRatioMax')
        highlightClippedFractionMax = @('highlightClippedFractionMax', 'hdrClippedFractionMax')
        swatchPrimaryLinearMin = @('swatchPrimaryLinearMin', 'chromaticDominantChannelMin')
        swatchChannelMarginMin = @('swatchChannelMarginMin', 'chromaticDominantAdvantageMin')
    }
    foreach ($key in @($defaults.Keys)) {
        $names = if ($thresholdAliases.ContainsKey($key)) { $thresholdAliases[$key] } else { @($key) }
        $value = Get-FirstProperty -Object $override -Names $names
        if ($null -ne $value) { $defaults[$key] = Convert-ToFiniteDouble -Value $value -Label "thresholds.$key" }
    }
    $receipt.thresholds = $defaults
    $receipt.thresholdAssumptions = [ordered]@{
        colorSpace = 'sRGB encoded BMP converted with IEC 61966-2-1 transfer curve'
        globalBlackCutoffLinearLuma = 0.001
        highlightCompression = 'last adjacent ordered slope <= first adjacent slope * highlightShoulderCompressionMax'
        clipping = 'fraction of sampled RGB channels encoded at 255'
    }

    $global = Get-GlobalMetrics -Image $image -Lut (Convert-SrgbLut)
    $lut = Convert-SrgbLut
    $receipt.metrics.global = $global
    $receipt.checks.globalNonBlack = ($global.meanLinearLuma -ge $defaults.globalMeanLinearMin -and
        $global.blackPixelFraction -le $defaults.globalBlackPixelFractionMax -and $global.maxLinearChannel -gt 0)
    if (-not $receipt.checks.globalNonBlack) {
        Add-Issue -Code 'color-response.global-nonblack-failed' -Message 'The output is black or below the global linear-light non-black threshold.'
    }

    $roiGroups = Get-FirstProperty -Object $fixture -Names @('rois', 'regions', 'analysisRois')
    $grayGroup = Get-FirstProperty -Object $fixture -Names @('grayRamp', 'greyRamp', 'grayRampRoi', 'grayscaleRamp', 'grayscaleRampRoi', 'greyRampRoi')
    $highlightGroup = Get-FirstProperty -Object $fixture -Names @('hdrHighlights', 'hdrHighlight', 'highlightShoulder', 'highlightShoulderRoi', 'hdrHighlightRoi', 'highlightRoi')
    $swatchGroup = Get-FirstProperty -Object $fixture -Names @('colorSwatches', 'colourSwatches', 'representativeSwatches', 'swatches', 'swatchRois')
    if ($null -eq $grayGroup) { $grayGroup = Get-FirstProperty -Object $roiGroups -Names @('grayRamp', 'neutralRamp', 'greyRamp') }
    if ($null -eq $highlightGroup) { $highlightGroup = Get-FirstProperty -Object $roiGroups -Names @('hdrHighlights', 'hdrRollOff', 'highlightShoulder', 'highlights') }
    if ($null -eq $swatchGroup) { $swatchGroup = Get-FirstProperty -Object $roiGroups -Names @('colorSwatches', 'chromaticPatches', 'representativeSwatches', 'swatches') }
    if ($null -eq $grayGroup) { throw 'Contract is missing a gray ramp group.' }
    if ($null -eq $highlightGroup) { throw 'Contract is missing an HDR highlight group.' }
    if ($null -eq $swatchGroup) { throw 'Contract is missing representative color swatches.' }

    $graySpecs = @(Get-SampleSpecs -Group $grayGroup -Label 'grayRamp' -ImageWidth $image.width -ImageHeight $image.height -CoordinateSpace $coordinateSpace)
    $highlightSpecs = @(Get-SampleSpecs -Group $highlightGroup -Label 'highlightShoulder' -ImageWidth $image.width -ImageHeight $image.height -CoordinateSpace $coordinateSpace)
    $swatchSpecs = @(Get-SampleSpecs -Group $swatchGroup -Label 'colorSwatches' -ImageWidth $image.width -ImageHeight $image.height -CoordinateSpace $coordinateSpace)
    if ($graySpecs.Count -lt 3) { throw 'Gray ramp must provide at least three ordered samples.' }
    if ($highlightSpecs.Count -lt 3) { throw 'HDR highlight shoulder must provide at least three ordered samples.' }
    if ($swatchSpecs.Count -lt 3) { throw 'Representative swatches must provide at least three samples.' }
    $fixtureInput = Get-FirstProperty -Object $fixture -Names @('input', 'inputs', 'sourceValues')
    $swatchLabels = @(Get-FirstProperty -Object $fixtureInput -Names @('chromaticPatchLabels', 'swatchLabels', 'labels'))
    if ($swatchLabels.Count -ne 0 -and $swatchLabels.Count -ne $swatchSpecs.Count) {
        throw 'Chromatic patch label count does not match the swatch sample count.'
    }

    function Measure-Samples {
        param([Parameter(Mandatory = $true)]$Specs, [Parameter(Mandatory = $true)][string]$Label)
        $items = @()
        for ($index = 0; $index -lt $Specs.Count; $index++) {
            $spec = $Specs[$index]
            $bounds = Get-RoiBounds -Spec $spec -Label "$Label[$index]" -ImageWidth $image.width -ImageHeight $image.height -CoordinateSpace $coordinateSpace
            $measurement = Measure-Region -Image $image -Bounds $bounds -Lut $lut
            $order = Get-SampleOrder -Spec $spec -Index $index
            $items += [pscustomobject]@{
                index = $index
                order = $order
                measurement = $measurement
                spec = $spec
            }
        }
        return @($items | Sort-Object -Property order, index)
    }

    $graySamples = @(Measure-Samples -Specs $graySpecs -Label 'grayRamp')
    $highlightSamples = @(Measure-Samples -Specs $highlightSpecs -Label 'highlightShoulder')
    $swatchSamples = @(Measure-Samples -Specs $swatchSpecs -Label 'colorSwatches')

    $grayLuma = @($graySamples | ForEach-Object { [double]$_.measurement.linearLuma })
    $grayNeutral = @($graySamples | ForEach-Object { [double]$_.measurement.neutralError })
    $grayRange = (Get-MaximumValue $grayLuma) - (Get-MinimumValue $grayLuma)
    $grayMonotonic = $true
    $grayAdjacentMin = [double]::PositiveInfinity
    for ($index = 1; $index -lt $grayLuma.Count; $index++) {
        $delta = $grayLuma[$index] - $grayLuma[$index - 1]
        $grayAdjacentMin = [Math]::Min($grayAdjacentMin, $delta)
        if ($delta + $defaults.grayMonotonicTolerance -lt 0 -or $delta -lt $defaults.grayAdjacentLumaMin) { $grayMonotonic = $false }
    }
    $graySpread = Get-MaximumValue @($graySamples | ForEach-Object {
        $m = $_.measurement
        [Math]::Max([double]$m.meanRed, [Math]::Max([double]$m.meanGreen, [double]$m.meanBlue)) -
            [Math]::Min([double]$m.meanRed, [Math]::Min([double]$m.meanGreen, [double]$m.meanBlue))
    })
    $grayNeutralMaximum = Get-MaximumValue $grayNeutral
    $grayMetricSamples = @($graySamples | ForEach-Object {
        [ordered]@{ order = $_.order; linearLuma = $_.measurement.linearLuma; neutralError = $_.measurement.neutralError; bounds = $_.measurement.bounds }
    })
    $receipt.metrics.grayRamp = [ordered]@{
        samples = $grayMetricSamples
        range = $grayRange
        minimumAdjacentLumaDelta = $grayAdjacentMin
        maxChannelSpread = $graySpread
        maxNeutralError = $grayNeutralMaximum
        monotonic = $grayMonotonic
    }
    $receipt.checks.grayRampMonotonic = ($grayMonotonic -and $grayRange -ge $defaults.grayRangeMin)
    $receipt.checks.grayNeutrality = ($grayNeutralMaximum -le $defaults.grayNeutralBiasMax -and $graySpread -le $defaults.grayChannelSpreadMax)
    if (-not $receipt.checks.grayRampMonotonic) {
        Add-Issue -Code 'color-response.gray-ramp-failed' -Message 'The gray ramp is not sufficiently ordered or has insufficient response range.'
    }
    if (-not $receipt.checks.grayNeutrality) {
        Add-Issue -Code 'color-response.gray-neutrality-failed' -Message 'Gray ramp samples have excessive channel imbalance in linear light.'
    }

    $highlightLuma = @($highlightSamples | ForEach-Object { [double]$_.measurement.linearLuma })
    $highlightMonotonic = $true
    $highlightAdjacentMin = [double]::PositiveInfinity
    for ($index = 1; $index -lt $highlightLuma.Count; $index++) {
        $delta = $highlightLuma[$index] - $highlightLuma[$index - 1]
        $highlightAdjacentMin = [Math]::Min($highlightAdjacentMin, $delta)
        if ($delta + $defaults.highlightMonotonicTolerance -lt 0 -or $delta -lt $defaults.highlightAdjacentLumaMin) { $highlightMonotonic = $false }
    }
    $firstSlope = $highlightLuma[1] - $highlightLuma[0]
    $lastSlope = $highlightLuma[$highlightLuma.Count - 1] - $highlightLuma[$highlightLuma.Count - 2]
    $highlightRange = (Get-MaximumValue $highlightLuma) - (Get-MinimumValue $highlightLuma)
    $highlightClipping = Get-MaximumValue @($highlightSamples | ForEach-Object { [double]$_.measurement.clippedFraction })
    $shoulderCompressed = ($firstSlope -gt $defaults.highlightMonotonicTolerance -and
        $lastSlope -le ($firstSlope * $defaults.highlightShoulderCompressionMax))
    $highlightMetricSamples = @($highlightSamples | ForEach-Object {
        [ordered]@{ order = $_.order; linearLuma = $_.measurement.linearLuma; clippedFraction = $_.measurement.clippedFraction; bounds = $_.measurement.bounds }
    })
    $receipt.metrics.highlightShoulder = [ordered]@{
        samples = $highlightMetricSamples
        range = $highlightRange
        minimumAdjacentLumaDelta = $highlightAdjacentMin
        firstSlope = $firstSlope
        lastSlope = $lastSlope
        clippedFraction = $highlightClipping
        monotonic = $highlightMonotonic
        compressed = $shoulderCompressed
    }
    $receipt.checks.highlightMonotonic = ($highlightMonotonic -and $highlightRange -ge $defaults.highlightRangeMin)
    $receipt.checks.highlightShoulder = $shoulderCompressed
    $receipt.checks.highlightClipping = ($highlightClipping -le $defaults.highlightClippedFractionMax)
    if (-not $receipt.checks.highlightMonotonic) {
        Add-Issue -Code 'color-response.highlight-monotonic-failed' -Message 'HDR highlight samples are not sufficiently monotonic or have insufficient range.'
    }
    if (-not $receipt.checks.highlightShoulder) {
        Add-Issue -Code 'color-response.highlight-shoulder-failed' -Message 'HDR highlights do not show a measurable compressed shoulder.'
    }
    if (-not $receipt.checks.highlightClipping) {
        Add-Issue -Code 'color-response.highlight-clipping-failed' -Message 'HDR highlight clipping exceeds the contract threshold.'
    }

    $swatchMetrics = @()
    $primaryChannels = [System.Collections.Generic.List[string]]::new()
    $expectedSignatures = [System.Collections.Generic.List[string]]::new()
    $swatchPass = $true
    for ($index = 0; $index -lt $swatchSamples.Count; $index++) {
        $sample = $swatchSamples[$index]
        $measurement = $sample.measurement
        $channels = [ordered]@{ red = [double]$measurement.meanRed; green = [double]$measurement.meanGreen; blue = [double]$measurement.meanBlue }
        $orderedChannels = @($channels.GetEnumerator() | Sort-Object -Property Value -Descending)
        $primary = [string]$orderedChannels[0].Key
        $second = [double]$orderedChannels[1].Value
        $maximum = [double]$orderedChannels[0].Value
        $ratio = $maximum / [Math]::Max($second, 0.0001)
        $margin = $maximum - $second
        $chroma = ($maximum - [double]$orderedChannels[2].Value) / [Math]::Max($maximum, 0.02)
        $expected = if ($swatchLabels.Count -eq $swatchSamples.Count) {
            [string]$swatchLabels[$index]
        } else {
            Convert-ChannelName -Name (Get-StringOrEmpty -Object $sample.spec -Names @('primaryChannel', 'dominantChannel', 'expectedChannel', 'expectedPrimaryChannel'))
        }
        if ([string]::IsNullOrWhiteSpace($expected)) {
            $expectedObject = Get-FirstProperty -Object $sample.spec -Names @('expected', 'color', 'colour')
            if ($null -ne $expectedObject) {
                $expected = Convert-ChannelName -Name (Get-StringOrEmpty -Object $expectedObject -Names @('primaryChannel', 'dominantChannel', 'channel'))
            }
        }
        $signaturePass = $true
        switch ($expected.Trim().ToLowerInvariant()) {
            'red' { $signaturePass = $channels.red -ge $defaults.swatchPrimaryLinearMin -and
                    $channels.red - [Math]::Max($channels.green, $channels.blue) -ge $defaults.swatchChannelMarginMin }
            'green' { $signaturePass = $channels.green -ge $defaults.swatchPrimaryLinearMin -and
                      $channels.green - [Math]::Max($channels.red, $channels.blue) -ge $defaults.swatchChannelMarginMin }
            'blue' { $signaturePass = $channels.blue -ge $defaults.swatchPrimaryLinearMin -and
                     $channels.blue - [Math]::Max($channels.red, $channels.green) -ge $defaults.swatchChannelMarginMin }
            'cyan' { $signaturePass = [Math]::Min($channels.green, $channels.blue) -ge $defaults.swatchPrimaryLinearMin -and
                     [Math]::Min($channels.green, $channels.blue) - $channels.red -ge $defaults.swatchChannelMarginMin }
            'magenta' { $signaturePass = [Math]::Min($channels.red, $channels.blue) -ge $defaults.swatchPrimaryLinearMin -and
                        [Math]::Min($channels.red, $channels.blue) - $channels.green -ge $defaults.swatchChannelMarginMin }
            'yellow' { $signaturePass = [Math]::Min($channels.red, $channels.green) -ge $defaults.swatchPrimaryLinearMin -and
                       [Math]::Min($channels.red, $channels.green) - $channels.blue -ge $defaults.swatchChannelMarginMin }
            default { $signaturePass = ($ratio -ge $defaults.swatchDominanceRatioMin) -and
                        ($maximum -ge $defaults.swatchPrimaryLinearMin) -and
                        ($margin -ge $defaults.swatchChannelMarginMin) }
        }
        if (-not $signaturePass -or $chroma -lt $defaults.swatchChromaMin) {
            $swatchPass = $false
        }
        [void]$primaryChannels.Add($primary)
        [void]$expectedSignatures.Add($expected.Trim().ToLowerInvariant())
        $swatchMetrics += [ordered]@{
            name = (Get-StringOrEmpty -Object $sample.spec -Names @('name', 'id', 'label'))
            order = $sample.order
            meanRed = $measurement.meanRed
            meanGreen = $measurement.meanGreen
            meanBlue = $measurement.meanBlue
            primaryChannel = $primary
            expectedChannel = $expected
            signaturePass = $signaturePass
            dominanceRatio = $ratio
            channelMargin = $margin
            chroma = $chroma
            bounds = $measurement.bounds
        }
    }
    $distinctChannels = @($primaryChannels | Select-Object -Unique).Count
    $distinctSignatures = @($expectedSignatures | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
        Select-Object -Unique).Count
    if ($swatchLabels.Count -eq 0) {
        $swatchPass = $swatchPass -and ($distinctChannels -ge [int]$defaults.swatchDistinctChannelMin)
    } else {
        $swatchPass = $swatchPass -and ($distinctSignatures -eq $swatchSamples.Count)
    }
    $receipt.metrics.swatches = [ordered]@{
        samples = $swatchMetrics
        distinctPrimaryChannels = $distinctChannels
        distinctExpectedSignatures = $distinctSignatures
    }
    $receipt.checks.swatchSeparation = $swatchPass
    if (-not $swatchPass) {
        Add-Issue -Code 'color-response.swatch-separation-failed' -Message 'Representative color swatches lack stable primary-channel or hue separation.'
    }

    $receipt.success = (@($script:Issues).Count -eq 0)
    $exitCode = if ($receipt.success) { 0 } else { 3 }
}
catch {
    if (@($script:Issues).Count -eq 0) {
        Add-Issue -Code 'analysis.invalid-input' -Message (Limit-Text -Text $_.Exception.Message) -Stage 'validation'
    } else {
        Add-Issue -Code 'analysis.unexpected' -Message (Limit-Text -Text $_.Exception.Message) -Stage 'validation'
    }
    $receipt.success = $false
    $exitCode = 2
}

$receipt.issues = @($script:Issues)
Write-ReceiptAndExit -Receipt $receipt -ExitCode $exitCode
