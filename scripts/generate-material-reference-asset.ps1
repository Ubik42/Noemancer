param(
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repositoryRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path $repositoryRoot 'assets/test/material-reference/material-reference.glb'
}
$OutputPath = [System.IO.Path]::GetFullPath($OutputPath)

Add-Type -AssemblyName System.Drawing.Common

$binary = [System.Collections.Generic.List[byte]]::new()

function Add-BinaryBlob {
    param(
        [Parameter(Mandatory)] [byte[]]$Bytes,
        [int]$Alignment = 4
    )
    while (($script:binary.Count % $Alignment) -ne 0) { $script:binary.Add(0) }
    $offset = $script:binary.Count
    $script:binary.AddRange($Bytes)
    return [pscustomobject]@{ Offset = $offset; Length = $Bytes.Length }
}

function ConvertTo-FloatBytes {
    param([Parameter(Mandatory)] [single[]]$Values)
    $bytes = [System.Collections.Generic.List[byte]]::new()
    foreach ($value in $Values) { $bytes.AddRange([System.BitConverter]::GetBytes($value)) }
    return ,$bytes.ToArray()
}

function ConvertTo-UInt16Bytes {
    param([Parameter(Mandatory)] [uint16[]]$Values)
    $bytes = [System.Collections.Generic.List[byte]]::new()
    foreach ($value in $Values) { $bytes.AddRange([System.BitConverter]::GetBytes($value)) }
    return ,$bytes.ToArray()
}

function Clamp-Byte {
    param([double]$Value)
    return [byte][Math]::Round([Math]::Max(0.0, [Math]::Min(255.0, $Value)))
}

function New-MaterialTexturePng {
    param(
        [Parameter(Mandatory)] [ValidateSet('base-color','normal','metallic-roughness','occlusion','emissive')] [string]$Kind,
        [int]$Size = 64
    )
    $bitmap = [System.Drawing.Bitmap]::new($Size, $Size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    try {
        for ($y = 0; $y -lt $Size; ++$y) {
            for ($x = 0; $x -lt $Size; ++$x) {
                $u = $x / [double]($Size - 1)
                $v = $y / [double]($Size - 1)
                switch ($Kind) {
                    'base-color' {
                        $cell = (([Math]::Floor($x / 8.0) + [Math]::Floor($y / 8.0)) % 2) -eq 0
                        $r = if ($cell) { 28 } else { 220 }
                        $g = if ($cell) { 176 } else { 78 }
                        $b = if ($cell) { 205 } else { 34 }
                        $alphaBand = [Math]::Floor($x / 8.0) % 3
                        $a = if ($alphaBand -eq 0) { 255 } elseif ($alphaBand -eq 1) { 112 } else { 0 }
                    }
                    'normal' {
                        $nx = [Math]::Sin($u * [Math]::PI * 8.0) * 0.32
                        $ny = [Math]::Cos($v * [Math]::PI * 8.0) * 0.32
                        $nz = [Math]::Sqrt([Math]::Max(0.0, 1.0 - $nx * $nx - $ny * $ny))
                        $r = ($nx * 0.5 + 0.5) * 255.0
                        $g = ($ny * 0.5 + 0.5) * 255.0
                        $b = ($nz * 0.5 + 0.5) * 255.0
                        $a = 255
                    }
                    'metallic-roughness' {
                        $r = 255
                        $g = (0.08 + 0.84 * $v) * 255.0
                        $b = (0.04 + 0.92 * $u) * 255.0
                        $a = 255
                    }
                    'occlusion' {
                        $dx = $u - 0.5
                        $dy = $v - 0.5
                        $distance = [Math]::Min(1.0, [Math]::Sqrt($dx * $dx + $dy * $dy) * 1.8)
                        $shade = (0.28 + 0.72 * $distance) * 255.0
                        $r = $shade; $g = $shade; $b = $shade; $a = 255
                    }
                    'emissive' {
                        $line = ([Math]::Abs($x - $y) -le 2) -or ([Math]::Abs(($Size - 1 - $x) - $y) -le 2)
                        $r = if ($line) { 255 } else { 4 }
                        $g = if ($line) { 116 } else { 8 }
                        $b = if ($line) { 24 } else { 18 }
                        $a = 255
                    }
                }
                $color = [System.Drawing.Color]::FromArgb((Clamp-Byte $a), (Clamp-Byte $r), (Clamp-Byte $g), (Clamp-Byte $b))
                $bitmap.SetPixel($x, $y, $color)
            }
        }
        $stream = [System.IO.MemoryStream]::new()
        try {
            $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
            return ,$stream.ToArray()
        } finally {
            $stream.Dispose()
        }
    } finally {
        $bitmap.Dispose()
    }
}

$positions = [System.Collections.Generic.List[single]]::new()
$normals = [System.Collections.Generic.List[single]]::new()
$texcoords = [System.Collections.Generic.List[single]]::new()
$tangents = [System.Collections.Generic.List[single]]::new()
foreach ($center in @(-2.4, 0.0, 2.4)) {
    foreach ($value in @(
        ($center - 0.8), 0.0, 0.0,
        ($center + 0.8), 0.0, 0.0,
        ($center + 0.8), 2.2, 0.0,
        ($center - 0.8), 2.2, 0.0)) {
        $positions.Add([single]$value)
    }
    for ($vertex = 0; $vertex -lt 4; ++$vertex) {
        foreach ($value in @(0.0, 0.0, 1.0)) { $normals.Add([single]$value) }
        foreach ($value in @(1.0, 0.0, 0.0, 1.0)) { $tangents.Add([single]$value) }
    }
    foreach ($value in @(0.0,1.0, 1.0,1.0, 1.0,0.0, 0.0,0.0)) { $texcoords.Add([single]$value) }
}
$indices = [uint16[]]@(0,1,2,0,2,3, 0,1,2,0,2,3, 0,1,2,0,2,3)

$positionBlob = Add-BinaryBlob (ConvertTo-FloatBytes $positions.ToArray())
$normalBlob = Add-BinaryBlob (ConvertTo-FloatBytes $normals.ToArray())
$texcoordBlob = Add-BinaryBlob (ConvertTo-FloatBytes $texcoords.ToArray())
$tangentBlob = Add-BinaryBlob (ConvertTo-FloatBytes $tangents.ToArray())
$indexBlob = Add-BinaryBlob (ConvertTo-UInt16Bytes $indices)

$textureKinds = @('base-color','normal','metallic-roughness','occlusion','emissive')
$textureBlobs = foreach ($kind in $textureKinds) { Add-BinaryBlob (New-MaterialTexturePng $kind) }

$bufferViews = [System.Collections.Generic.List[object]]::new()
$bufferViews.Add([ordered]@{ buffer=0; byteOffset=$positionBlob.Offset; byteLength=$positionBlob.Length; target=34962 })
$bufferViews.Add([ordered]@{ buffer=0; byteOffset=$normalBlob.Offset; byteLength=$normalBlob.Length; target=34962 })
$bufferViews.Add([ordered]@{ buffer=0; byteOffset=$texcoordBlob.Offset; byteLength=$texcoordBlob.Length; target=34962 })
$bufferViews.Add([ordered]@{ buffer=0; byteOffset=$tangentBlob.Offset; byteLength=$tangentBlob.Length; target=34962 })
$bufferViews.Add([ordered]@{ buffer=0; byteOffset=$indexBlob.Offset; byteLength=$indexBlob.Length; target=34963 })
foreach ($blob in $textureBlobs) {
    $bufferViews.Add([ordered]@{ buffer=0; byteOffset=$blob.Offset; byteLength=$blob.Length })
}

$accessors = [System.Collections.Generic.List[object]]::new()
for ($primitive = 0; $primitive -lt 3; ++$primitive) {
    $center = @(-2.4,0.0,2.4)[$primitive]
    $accessors.Add([ordered]@{ bufferView=0; byteOffset=$primitive*48; componentType=5126; count=4; type='VEC3'; min=@(($center-0.8),0.0,0.0); max=@(($center+0.8),2.2,0.0) })
}
for ($primitive = 0; $primitive -lt 3; ++$primitive) {
    $accessors.Add([ordered]@{ bufferView=1; byteOffset=$primitive*48; componentType=5126; count=4; type='VEC3' })
}
for ($primitive = 0; $primitive -lt 3; ++$primitive) {
    $accessors.Add([ordered]@{ bufferView=2; byteOffset=$primitive*32; componentType=5126; count=4; type='VEC2' })
}
for ($primitive = 0; $primitive -lt 3; ++$primitive) {
    $accessors.Add([ordered]@{ bufferView=3; byteOffset=$primitive*64; componentType=5126; count=4; type='VEC4' })
}
for ($primitive = 0; $primitive -lt 3; ++$primitive) {
    $accessors.Add([ordered]@{ bufferView=4; byteOffset=$primitive*12; componentType=5123; count=6; type='SCALAR' })
}

$materials = [System.Collections.Generic.List[object]]::new()
$materialNames = @('Opaque PBR Channels','Alpha Mask PBR Channels','Alpha Blend PBR Channels')
$alphaModes = @('OPAQUE','MASK','BLEND')
for ($material = 0; $material -lt 3; ++$material) {
    $entry = [ordered]@{
        name = $materialNames[$material]
        pbrMetallicRoughness = [ordered]@{
            baseColorFactor = @(1.0,1.0,1.0,1.0)
            baseColorTexture = [ordered]@{ index=0 }
            metallicFactor = 1.0
            roughnessFactor = 1.0
            metallicRoughnessTexture = [ordered]@{ index=2 }
        }
        normalTexture = [ordered]@{ index=1; scale=1.0 }
        occlusionTexture = [ordered]@{ index=3; strength=1.0 }
        emissiveTexture = [ordered]@{ index=4 }
        emissiveFactor = @(1.0,0.55,0.2)
        alphaMode = $alphaModes[$material]
        doubleSided = $material -ne 0
    }
    if ($material -eq 1) { $entry.alphaCutoff = 0.5 }
    $materials.Add($entry)
}

$primitives = [System.Collections.Generic.List[object]]::new()
for ($primitive = 0; $primitive -lt 3; ++$primitive) {
    $primitives.Add([ordered]@{
        attributes = [ordered]@{ POSITION=$primitive; NORMAL=3+$primitive; TEXCOORD_0=6+$primitive; TANGENT=9+$primitive }
        indices = 12+$primitive
        material = $primitive
        mode = 4
    })
}

$document = [ordered]@{
    asset = [ordered]@{ version='2.0'; generator='Noemancer deterministic material-reference generator/1.0'; copyright='Noemancer project, CC0-1.0' }
    scene = 0
    scenes = @([ordered]@{ name='Material Reference'; nodes=@(0) })
    nodes = @([ordered]@{ name='Material Cards'; mesh=0 })
    meshes = @([ordered]@{ name='Complete glTF PBR Material Coverage'; primitives=$primitives })
    materials = $materials
    samplers = @([ordered]@{ magFilter=9729; minFilter=9987; wrapS=10497; wrapT=10497 })
    textures = for ($index=0; $index -lt 5; ++$index) { [ordered]@{ sampler=0; source=$index } }
    images = for ($index=0; $index -lt 5; ++$index) { [ordered]@{ name=$textureKinds[$index]; bufferView=5+$index; mimeType='image/png' } }
    accessors = $accessors
    bufferViews = $bufferViews
    buffers = @([ordered]@{ byteLength=$binary.Count })
}

$jsonBytes = [System.Collections.Generic.List[byte]]::new()
$jsonBytes.AddRange([System.Text.Encoding]::UTF8.GetBytes(($document | ConvertTo-Json -Depth 30 -Compress)))
while (($jsonBytes.Count % 4) -ne 0) { $jsonBytes.Add([byte]0x20) }
while (($binary.Count % 4) -ne 0) { $binary.Add(0) }

$totalLength = 12 + 8 + $jsonBytes.Count + 8 + $binary.Count
$glb = [System.Collections.Generic.List[byte]]::new($totalLength)
$glb.AddRange([System.BitConverter]::GetBytes([uint32]0x46546C67))
$glb.AddRange([System.BitConverter]::GetBytes([uint32]2))
$glb.AddRange([System.BitConverter]::GetBytes([uint32]$totalLength))
$glb.AddRange([System.BitConverter]::GetBytes([uint32]$jsonBytes.Count))
$glb.AddRange([System.BitConverter]::GetBytes([uint32]0x4E4F534A))
$glb.AddRange($jsonBytes.ToArray())
$glb.AddRange([System.BitConverter]::GetBytes([uint32]$binary.Count))
$glb.AddRange([System.BitConverter]::GetBytes([uint32]0x004E4942))
$glb.AddRange($binary.ToArray())

$directory = Split-Path -Parent $OutputPath
[System.IO.Directory]::CreateDirectory($directory) | Out-Null
[System.IO.File]::WriteAllBytes($OutputPath, $glb.ToArray())

[pscustomobject]@{
    OutputPath = $OutputPath
    Bytes = $glb.Count
    Sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $OutputPath).Hash.ToLowerInvariant()
    Materials = 3
    EmbeddedTextures = 5
}
