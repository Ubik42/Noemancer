[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(Mandatory = $true)]
    [string]$AoEnabledImage,

    [Parameter(Mandatory = $true)]
    [string]$AoDisabledImage,

    [Parameter(Mandatory = $true)]
    [string]$ContractPath,

    [string]$OutputPath = ''
)

# Offline A/B verifier for the versioned Commercial Raster material/AO fixture.
# It never starts the Runtime and never opens a window. The two BMP captures are
# sampled in linear luma so exposure drift, localized AO darkening and a non-AO
# control region can be compared without depending on renderer internals.
#
# Exit contract:
#   0 = dimensions, non-black images, exposure, AO and control checks passed
#   2 = invalid invocation, contract or unsupported/truncated BMP
#   3 = images were readable but the quality contract failed
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
    if (-not [string]::IsNullOrWhiteSpace($OutputPath)) {
        try {
            $target = [System.IO.Path]::GetFullPath($OutputPath)
            $parent = [System.IO.Path]::GetDirectoryName($target)
            if (-not [string]::IsNullOrWhiteSpace($parent)) {
                New-Item -ItemType Directory -Path $parent -Force | Out-Null
            }
            Set-Content -LiteralPath $target -Value $json -Encoding UTF8
        }
        catch {
            $writeFailed = $true
            [Console]::Error.WriteLine("Could not write material/AO quality receipt '$OutputPath': $($_.Exception.Message)")
        }
    }
    [Console]::Out.WriteLine($json)
    if ($writeFailed) { exit 7 }
    exit $ExitCode
}

function Get-MaskMetadata {
    param([Parameter(Mandatory = $true)][uint32]$Mask)

    if ($Mask -eq 0) {
        return [ordered]@{ shift = 0; maximum = 0; scale = 1.0 }
    }
    $shift = 0
    $remaining = $Mask
    while (($remaining -band [uint32]1) -eq 0) {
        $remaining = $remaining -shr 1
        $shift++
    }
    $maximum = [uint32]($Mask -shr $shift)
    while (($remaining -band [uint32]1) -eq 1) { $remaining = $remaining -shr 1 }
    if ($remaining -ne 0) {
        throw "BMP bitfield mask is not contiguous: 0x$('{0:X8}' -f $Mask)."
    }
    if ($maximum -eq 0) {
        throw "BMP bitfield mask is empty: 0x$('{0:X8}' -f $Mask)."
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

function Read-BmpLuma {
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
        [void]$reader.ReadUInt32() # file size
        [void]$reader.ReadUInt16() # reserved 1
        [void]$reader.ReadUInt16() # reserved 2
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
        [void]$reader.ReadUInt32() # image size; may be zero for BI_RGB
        [void]$reader.ReadInt32() # x pixels per metre
        [void]$reader.ReadInt32() # y pixels per metre
        [void]$reader.ReadUInt32() # palette colours
        [void]$reader.ReadUInt32() # important colours

        if ($width -le 0 -or $heightSigned -eq 0 -or $heightSigned -eq [int]::MinValue) {
            throw 'BMP dimensions must be positive.'
        }
        $height = [Math]::Abs($heightSigned)
        if ($width -gt 8192 -or $height -gt 8192 -or
            ([int64]$width * [int64]$height) -gt $script:MaxPixels) {
            throw "BMP dimensions exceed the bounded analysis limit: ${width}x${height}."
        }
        if ($planes -ne 1 -or ($bitsPerPixel -ne 24 -and $bitsPerPixel -ne 32) -or
            ($compression -ne 0 -and -not ($compression -eq 3 -and $bitsPerPixel -eq 32))) {
            throw "Expected uncompressed 24/32-bit BMP (planes=1, compression=0 or 32-bit bitfields=3), got bpp=$bitsPerPixel compression=$compression."
        }

        $redMask = [uint32]0
        $greenMask = [uint32]0
        $blueMask = [uint32]0
        if ($compression -eq 3) {
            # For both a 40-byte header with trailing masks and a 52/56-byte
            # header with in-header masks, RGB masks begin at DIB+40.
            [void]$stream.Seek($dibStart + 40, [IO.SeekOrigin]::Begin)
            $redMask = $reader.ReadUInt32()
            $greenMask = $reader.ReadUInt32()
            $blueMask = $reader.ReadUInt32()
            if ($redMask -eq 0 -or $greenMask -eq 0 -or $blueMask -eq 0) {
                throw '32-bit bitfield BMP must provide non-zero RGB masks.'
            }
            if (($redMask -band $greenMask) -ne 0 -or
                ($redMask -band $blueMask) -ne 0 -or
                ($greenMask -band $blueMask) -ne 0) {
                throw '32-bit bitfield BMP RGB masks overlap.'
            }
        }
        $redInfo = Get-MaskMetadata -Mask $redMask
        $greenInfo = Get-MaskMetadata -Mask $greenMask
        $blueInfo = Get-MaskMetadata -Mask $blueMask

        $bytesPerPixel = [int]($bitsPerPixel / 8)
        $stride = [int]([Math]::Ceiling(($width * $bytesPerPixel) / 4.0) * 4)
        $minimumPixelOffset = $dibStart + [int64]$dibSize
        if ($compression -eq 3 -and $dibSize -eq 40) { $minimumPixelOffset += 12L }
        if ($pixelOffset -lt $minimumPixelOffset -or $pixelOffset -ge $stream.Length) {
            throw 'BMP pixel offset is invalid.'
        }
        $required = [int64]$stride * [int64]$height
        if (($stream.Length - $pixelOffset) -lt $required) { throw 'BMP pixel payload is truncated.' }
        if ($required -gt [int]::MaxValue) { throw 'BMP pixel payload exceeds the bounded analysis limit.' }
        [void]$stream.Seek($pixelOffset, [IO.SeekOrigin]::Begin)
        $pixels = $reader.ReadBytes([int]$required)
        if ($pixels.Length -ne $required) { throw 'BMP pixel payload could not be read completely.' }

        $lut = Convert-SrgbLut
        $values = [double[]]::new($width * $height)
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
                    $red = [int][Math]::Round((($raw -band $redMask) -shr [int]$redInfo.shift) * [double]$redInfo.scale)
                    $green = [int][Math]::Round((($raw -band $greenMask) -shr [int]$greenInfo.shift) * [double]$greenInfo.scale)
                    $blue = [int][Math]::Round((($raw -band $blueMask) -shr [int]$blueInfo.shift) * [double]$blueInfo.scale)
                } else {
                    $blue = [int]$pixels[$pixelOffsetInRow]
                    $green = [int]$pixels[$pixelOffsetInRow + 1]
                    $red = [int]$pixels[$pixelOffsetInRow + 2]
                }
                $values[$valueIndex] = (0.2126 * $lut[$red]) + (0.7152 * $lut[$green]) + (0.0722 * $lut[$blue])
                $valueIndex++
            }
        }
        return [ordered]@{
            path = $Path
            width = $width
            height = $height
            bitsPerPixel = $bitsPerPixel
            compression = $compression
            topDown = $heightSigned -lt 0
            values = $values
        }
    }
    finally {
        $reader.Dispose()
        $stream.Dispose()
    }
}

function Get-RoiBounds {
    param(
        [Parameter(Mandatory = $true)]$Roi,
        [Parameter(Mandatory = $true)][int]$Width,
        [Parameter(Mandatory = $true)][int]$Height
    )

    if ($null -eq $Roi -or $Roi.PSObject.Properties.Name -notcontains 'shape' -or
        [string]$Roi.shape -ne 'rectangle') {
        throw 'Material/AO fixture ROI must be a rectangle.'
    }
    foreach ($field in @('x0', 'x1', 'y0', 'y1')) {
        if ($Roi.PSObject.Properties.Name -notcontains $field) { throw "ROI is missing '$field'." }
    }
    $x0 = [double]$Roi.x0
    $x1 = [double]$Roi.x1
    $y0 = [double]$Roi.y0
    $y1 = [double]$Roi.y1
    if ([double]::IsNaN($x0) -or [double]::IsNaN($x1) -or [double]::IsNaN($y0) -or [double]::IsNaN($y1) -or
        [double]::IsInfinity($x0) -or [double]::IsInfinity($x1) -or
        [double]::IsInfinity($y0) -or [double]::IsInfinity($y1) -or
        $x0 -lt 0.0 -or $x1 -gt 1.0 -or $x0 -ge $x1 -or
        $y0 -lt 0.0 -or $y1 -gt 1.0 -or $y0 -ge $y1) {
        throw "ROI bounds must be finite normalized coordinates with x0<x1 and y0<y1: [$x0,$x1,$y0,$y1]."
    }
    $left = [Math]::Max(0, [Math]::Min($Width - 1, [int][Math]::Floor($x0 * $Width)))
    $right = [Math]::Max($left + 1, [Math]::Min($Width, [int][Math]::Ceiling($x1 * $Width)))
    $top = [Math]::Max(0, [Math]::Min($Height - 1, [int][Math]::Floor($y0 * $Height)))
    $bottom = [Math]::Max($top + 1, [Math]::Min($Height, [int][Math]::Ceiling($y1 * $Height)))
    return [ordered]@{
        normalized = [ordered]@{ x0 = $x0; x1 = $x1; y0 = $y0; y1 = $y1 }
        left = $left
        right = $right
        top = $top
        bottom = $bottom
    }
}

function Get-RegionStats {
    param(
        [Parameter(Mandatory = $true)][double[]]$Enabled,
        [Parameter(Mandatory = $true)][double[]]$Disabled,
        [Parameter(Mandatory = $true)][int]$Width,
        [Parameter(Mandatory = $true)][int]$Height,
        [Parameter(Mandatory = $true)]$Roi
    )

    $bounds = Get-RoiBounds -Roi $Roi -Width $Width -Height $Height
    $enabledSum = 0.0
    $disabledSum = 0.0
    $deltaSum = 0.0
    $absoluteDeltaSum = 0.0
    $negativeCount = [int64]0
    $blackEnabledCount = [int64]0
    $blackDisabledCount = [int64]0
    $absoluteDeltas = [System.Collections.Generic.List[double]]::new()
    $count = [int64]0
    for ($y = $bounds.top; $y -lt $bounds.bottom; $y++) {
        $offset = $y * $Width
        for ($x = $bounds.left; $x -lt $bounds.right; $x++) {
            $index = $offset + $x
            $enabledValue = [double]$Enabled[$index]
            $disabledValue = [double]$Disabled[$index]
            $delta = $enabledValue - $disabledValue
            $enabledSum += $enabledValue
            $disabledSum += $disabledValue
            $deltaSum += $delta
            $absoluteDelta = [Math]::Abs($delta)
            $absoluteDeltaSum += $absoluteDelta
            [void]$absoluteDeltas.Add($absoluteDelta)
            if ($delta -lt 0.0) { $negativeCount++ }
            if ($enabledValue -le 0.005) { $blackEnabledCount++ }
            if ($disabledValue -le 0.005) { $blackDisabledCount++ }
            $count++
        }
    }
    $absoluteDeltas.Sort()
    $p95Index = [Math]::Max(0, [int][Math]::Ceiling($count * 0.95) - 1)
    $enabledMean = if ($count -gt 0) { $enabledSum / [double]$count } else { 0.0 }
    $disabledMean = if ($count -gt 0) { $disabledSum / [double]$count } else { 0.0 }
    return [ordered]@{
        normalized = $bounds.normalized
        pixels = $count
        enabledMeanLinear = $enabledMean
        disabledMeanLinear = $disabledMean
        deltaMeanLinear = if ($count -gt 0) { $deltaSum / [double]$count } else { 0.0 }
        absoluteDeltaMeanLinear = if ($count -gt 0) { $absoluteDeltaSum / [double]$count } else { 0.0 }
        absoluteDeltaP95Linear = if ($count -gt 0) { $absoluteDeltas[$p95Index] } else { 0.0 }
        negativeFraction = if ($count -gt 0) { $negativeCount / [double]$count } else { 0.0 }
        enabledBlackPixelFraction = if ($count -gt 0) { $blackEnabledCount / [double]$count } else { 1.0 }
        disabledBlackPixelFraction = if ($count -gt 0) { $blackDisabledCount / [double]$count } else { 1.0 }
    }
}

function Get-ConfiguredThreshold {
    param(
        [Parameter(Mandatory = $true)]$Thresholds,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][double]$Default
    )
    if ($null -ne $Thresholds -and $Thresholds.PSObject.Properties.Name -contains $Name) {
        $value = [double]$Thresholds.$Name
        if ([double]::IsNaN($value) -or [double]::IsInfinity($value)) {
            throw "Contract threshold '$Name' must be finite."
        }
        return $value
    }
    return $Default
}

function Get-RequiredContractProperty {
    param(
        [Parameter(Mandatory = $true)]$Object,
        [Parameter(Mandatory = $true)][string]$Path
    )

    $current = $Object
    foreach ($part in $Path.Split('.')) {
        if ($null -eq $current) { throw "Contract field '$Path' is missing." }
        $property = $current.PSObject.Properties[$part]
        if ($null -eq $property) { throw "Contract field '$Path' is missing." }
        $current = $property.Value
    }
    return $current
}

function Require-ContractString {
    param(
        [Parameter(Mandatory = $true)]$Object,
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Expected
    )

    $value = Get-RequiredContractProperty -Object $Object -Path $Path
    if ($value -isnot [string] -or -not [string]::Equals($value, $Expected, [StringComparison]::Ordinal)) {
        throw "Contract field '$Path' must equal '$Expected'."
    }
    return $value
}

function Get-OptionalFiniteContractNumber {
    param(
        [Parameter(Mandatory = $true)]$Object,
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][int]$Default
    )

    $current = $Object
    foreach ($part in $Path.Split('.')) {
        if ($null -eq $current) { return [double]$Default }
        $property = $current.PSObject.Properties[$part]
        if ($null -eq $property) { return [double]$Default }
        $current = $property.Value
    }
    $value = [double]$current
    if ([double]::IsNaN($value) -or [double]::IsInfinity($value)) {
        throw "Contract field '$Path' must be finite."
    }
    return $value
}

try {
    if (-not (Test-Path -LiteralPath $ContractPath -PathType Leaf)) {
        throw "Contract file does not exist: $ContractPath"
    }
    $contract = Get-Content -LiteralPath $ContractPath -Raw | ConvertFrom-Json
    if ($null -eq $contract -or $contract.PSObject.Properties.Name -notcontains 'materialAoFixture') {
        throw 'Contract does not contain materialAoFixture.'
    }
    $contractId = Require-ContractString -Object $contract -Path 'id' -Expected 'noemancer.commercial-raster-reference/1.8'
    $fixture = Get-RequiredContractProperty -Object $contract -Path 'materialAoFixture'
    if ($null -eq $fixture -or $fixture.PSObject.Properties.Name -notcontains 'rois') {
        throw 'materialAoFixture does not contain normalized rois.'
    }
    $fixtureSchema = Require-ContractString -Object $fixture -Path 'schemaVersion' -Expected 'noemancer.material-ao-fixture/0.1'
    [void](Require-ContractString -Object $fixture -Path 'analysisCoordinateSpace' -Expected 'output-normalized')
    [void](Require-ContractString -Object $fixture -Path 'abIntent.enabledImage' -Expected 'ao-enabled')
    [void](Require-ContractString -Object $fixture -Path 'abIntent.disabledImage' -Expected 'ao-disabled')
    [void](Require-ContractString -Object $fixture -Path 'abIntent.delta' -Expected 'enabled-minus-disabled-linear-luma')
    $rois = $fixture.rois
    foreach ($roiName in @('materialGradient', 'ao', 'contact', 'concave', 'control')) {
        if ($rois.PSObject.Properties.Name -notcontains $roiName) {
            throw "materialAoFixture.rois is missing '$roiName'."
        }
    }
    $thresholds = if ($fixture.PSObject.Properties.Name -contains 'thresholds') { $fixture.thresholds } else { $null }
    $expectedWidth = 1920
    $expectedHeight = 1080
    if ($contract.PSObject.Properties.Name -contains 'capture' -and $null -ne $contract.capture) {
        $expectedWidth = [int](Get-OptionalFiniteContractNumber -Object $contract -Path 'capture.width' -Default $expectedWidth)
        $expectedHeight = [int](Get-OptionalFiniteContractNumber -Object $contract -Path 'capture.height' -Default $expectedHeight)
        if ($expectedWidth -lt 1 -or $expectedHeight -lt 1 -or $expectedWidth -gt 8192 -or $expectedHeight -gt 8192) {
            throw "Contract capture dimensions are outside the bounded range: ${expectedWidth}x${expectedHeight}."
        }
    }

    $enabled = Read-BmpLuma -Path (Resolve-Path -LiteralPath $AoEnabledImage -ErrorAction Stop).Path
    $disabled = Read-BmpLuma -Path (Resolve-Path -LiteralPath $AoDisabledImage -ErrorAction Stop).Path
    $enabledHash = Get-FileSha256 -Path $enabled.path
    $disabledHash = Get-FileSha256 -Path $disabled.path
    $dimensionsMatch = $enabled.width -eq $expectedWidth -and $enabled.height -eq $expectedHeight -and
        $disabled.width -eq $expectedWidth -and $disabled.height -eq $expectedHeight -and
        $enabled.width -eq $disabled.width -and $enabled.height -eq $disabled.height
    if (-not $dimensionsMatch) {
        Add-Issue -Code 'dimensions.mismatch' -Stage 'dimensions' `
            -Message "Expected ${expectedWidth}x${expectedHeight}, got enabled=$($enabled.width)x$($enabled.height), disabled=$($disabled.width)x$($disabled.height)."
    }

    $width = [int]$enabled.width
    $height = [int]$enabled.height
    if ($width -ne $disabled.width -or $height -ne $disabled.height) {
        throw 'Enabled and disabled BMP dimensions differ; cannot compare A/B pixels.'
    }
    $enabledValues = [double[]]$enabled.values
    $disabledValues = [double[]]$disabled.values
    $wholeImage = [pscustomobject][ordered]@{ shape = 'rectangle'; x0 = 0.0; x1 = 1.0; y0 = 0.0; y1 = 1.0 }
    $global = Get-RegionStats -Enabled $enabledValues -Disabled $disabledValues -Width $width -Height $height -Roi $wholeImage
    $materialGradient = Get-RegionStats -Enabled $enabledValues -Disabled $disabledValues -Width $width -Height $height -Roi $rois.materialGradient
    $ao = Get-RegionStats -Enabled $enabledValues -Disabled $disabledValues -Width $width -Height $height -Roi $rois.ao
    $contact = Get-RegionStats -Enabled $enabledValues -Disabled $disabledValues -Width $width -Height $height -Roi $rois.contact
    $concave = Get-RegionStats -Enabled $enabledValues -Disabled $disabledValues -Width $width -Height $height -Roi $rois.concave
    $control = Get-RegionStats -Enabled $enabledValues -Disabled $disabledValues -Width $width -Height $height -Roi $rois.control

    $globalMeanMin = Get-ConfiguredThreshold -Thresholds $thresholds -Name 'globalMeanLinearMin' -Default 0.02
    $globalMeanMax = Get-ConfiguredThreshold -Thresholds $thresholds -Name 'globalMeanLinearMax' -Default 0.90
    $globalBlackMax = Get-ConfiguredThreshold -Thresholds $thresholds -Name 'globalBlackPixelFractionMax' -Default 0.98
    $globalDeltaMax = Get-ConfiguredThreshold -Thresholds $thresholds -Name 'globalMeanDeltaAbsMax' -Default 0.025
    $globalRatioMax = Get-ConfiguredThreshold -Thresholds $thresholds -Name 'globalMeanRatioDeltaMax' -Default 0.08
    $aoDeltaMin = Get-ConfiguredThreshold -Thresholds $thresholds -Name 'aoMeanDeltaMin' -Default -0.20
    $aoDeltaMax = Get-ConfiguredThreshold -Thresholds $thresholds -Name 'aoMeanDeltaMax' -Default -0.001
    $aoNegativeMin = Get-ConfiguredThreshold -Thresholds $thresholds -Name 'aoNegativeFractionMin' -Default 0.05
    $aoMeanMin = Get-ConfiguredThreshold -Thresholds $thresholds -Name 'aoEnabledMeanLinearMin' -Default 0.01
    $aoBlackMax = Get-ConfiguredThreshold -Thresholds $thresholds -Name 'aoBlackPixelFractionMax' -Default 0.75
    $controlDeltaMax = Get-ConfiguredThreshold -Thresholds $thresholds -Name 'controlMeanDeltaAbsMax' -Default 0.01
    $controlP95Max = Get-ConfiguredThreshold -Thresholds $thresholds -Name 'controlP95AbsDeltaMax' -Default 0.025

    $globalMeanRatioDelta = [Math]::Abs(
        ($global.enabledMeanLinear / [Math]::Max($global.disabledMeanLinear, 0.000001)) - 1.0)
    $blackScreen = $global.enabledMeanLinear -le 0.000001 -and
        $global.disabledMeanLinear -le 0.000001 -and
        $global.enabledBlackPixelFraction -ge 0.999 -and
        $global.disabledBlackPixelFraction -ge 0.999
    $finite = @($global, $materialGradient, $ao, $contact, $concave, $control) | ForEach-Object {
        [double]::IsFinite([double]$_.enabledMeanLinear) -and
            [double]::IsFinite([double]$_.disabledMeanLinear) -and
            [double]::IsFinite([double]$_.deltaMeanLinear)
    }
    $checks = [ordered]@{
        dimensions = $dimensionsMatch
        finite = -not ($finite -contains $false)
        blackScreen = -not $blackScreen
        nonBlack = $global.enabledMeanLinear -ge $globalMeanMin -and
            $global.disabledMeanLinear -ge $globalMeanMin -and
            $global.enabledMeanLinear -le $globalMeanMax -and
            $global.disabledMeanLinear -le $globalMeanMax -and
            $global.enabledBlackPixelFraction -le $globalBlackMax -and
            $global.disabledBlackPixelFraction -le $globalBlackMax
        globalExposureDrift = [Math]::Abs([double]$global.deltaMeanLinear) -le $globalDeltaMax -and
            $globalMeanRatioDelta -le $globalRatioMax
        aoNegativeFinite = $ao.deltaMeanLinear -ge $aoDeltaMin -and
            $ao.deltaMeanLinear -le $aoDeltaMax -and $ao.negativeFraction -ge $aoNegativeMin
        aoNotOverBlack = $ao.enabledMeanLinear -ge $aoMeanMin -and
            $ao.enabledBlackPixelFraction -le $aoBlackMax
        controlStable = [Math]::Abs([double]$control.deltaMeanLinear) -le $controlDeltaMax -and
            $control.absoluteDeltaP95Linear -le $controlP95Max
    }
    foreach ($check in $checks.Keys) {
        if (-not [bool]$checks[$check]) {
            Add-Issue -Code "material-ao.$check-failed" -Stage 'quality' `
                -Message "Material/AO quality check '$check' did not meet its fixed threshold."
        }
    }
    if ($blackScreen) {
        Add-Issue -Code 'material-ao.black-screen' -Stage 'quality' `
            -Message 'Both AO A/B captures are effectively black screens.'
    }

    $receipt = [ordered]@{
        schemaVersion = 'noemancer.material-ao-quality-evidence/0.1'
        success = ($script:Issues.Count -eq 0)
        contract = [ordered]@{
            id = if ($contract.PSObject.Properties.Name -contains 'id') { $contract.id } else { $null }
            sceneGuid = if ($contract.PSObject.Properties.Name -contains 'sceneGuid') { $contract.sceneGuid } else { $null }
            fixtureSchema = if ($fixture.PSObject.Properties.Name -contains 'schemaVersion') { $fixture.schemaVersion } else { $null }
        }
        images = [ordered]@{
            enabled = [ordered]@{ path = $enabled.path; sha256 = $enabledHash; width = $enabled.width; height = $enabled.height; bitsPerPixel = $enabled.bitsPerPixel; compression = $enabled.compression; topDown = $enabled.topDown }
            disabled = [ordered]@{ path = $disabled.path; sha256 = $disabledHash; width = $disabled.width; height = $disabled.height; bitsPerPixel = $disabled.bitsPerPixel; compression = $disabled.compression; topDown = $disabled.topDown }
            expectedWidth = $expectedWidth
            expectedHeight = $expectedHeight
        }
        thresholds = [ordered]@{
            globalMeanLinearMin = $globalMeanMin
            globalMeanLinearMax = $globalMeanMax
            globalBlackPixelFractionMax = $globalBlackMax
            globalMeanDeltaAbsMax = $globalDeltaMax
            globalMeanRatioDeltaMax = $globalRatioMax
            aoMeanDeltaMin = $aoDeltaMin
            aoMeanDeltaMax = $aoDeltaMax
            aoNegativeFractionMin = $aoNegativeMin
            aoEnabledMeanLinearMin = $aoMeanMin
            aoBlackPixelFractionMax = $aoBlackMax
            controlMeanDeltaAbsMax = $controlDeltaMax
            controlP95AbsDeltaMax = $controlP95Max
        }
        metrics = [ordered]@{
            global = [ordered]@{
                enabledMeanLinear = $global.enabledMeanLinear
                disabledMeanLinear = $global.disabledMeanLinear
                deltaMeanLinear = $global.deltaMeanLinear
                absoluteDeltaMeanLinear = $global.absoluteDeltaMeanLinear
                meanRatioDelta = $globalMeanRatioDelta
                blackScreen = $blackScreen
                enabledBlackPixelFraction = $global.enabledBlackPixelFraction
                disabledBlackPixelFraction = $global.disabledBlackPixelFraction
            }
            rois = [ordered]@{
                materialGradient = $materialGradient
                ao = $ao
                contact = $contact
                concave = $concave
                control = $control
            }
        }
        checks = $checks
        issues = @($script:Issues.ToArray())
    }
    $analysisExitCode = if ($receipt.success) { 0 } else { 3 }
    Write-ReceiptAndExit -Receipt $receipt -ExitCode $analysisExitCode
}
catch {
    $receipt = [ordered]@{
        schemaVersion = 'noemancer.material-ao-quality-evidence/0.1'
        success = $false
        checks = [ordered]@{}
        issues = @([ordered]@{
            code = 'analysis.unexpected'
            stage = 'analysis'
            message = Limit-Text "$($_.Exception.GetType().Name): $($_.Exception.Message) [$($_.InvocationInfo.PositionMessage)]"
        })
    }
    Write-ReceiptAndExit -Receipt $receipt -ExitCode 2
}
