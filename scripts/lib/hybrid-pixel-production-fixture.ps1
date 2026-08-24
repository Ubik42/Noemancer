# Engine-owned Hybrid Pixel production fixture.
#
# This file is intentionally dot-sourceable.  It contains no top-level project
# mutation and does not invoke package/capture/runtime commands.  The public
# entry point operates on a caller-owned temporary project copy and keeps the
# source project outside of the operation's write set.

function Get-HybridPixelFixtureProperty {
    param(
        [AllowNull()]$Object,
        [Parameter(Mandatory = $true)][string]$Name
    )
    if ($null -eq $Object) { return $null }
    if ($Object -is [System.Collections.IDictionary]) {
        if ($Object.Contains($Name)) { return $Object[$Name] }
        return $null
    }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}

function Set-HybridPixelFixtureProperty {
    param(
        [Parameter(Mandatory = $true)]$Object,
        [Parameter(Mandatory = $true)][string]$Name,
        [AllowNull()]$Value
    )
    if ($Object -is [System.Collections.IDictionary]) {
        [void]($Object[$Name] = $Value)
        return
    }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) {
        [void](Add-Member -InputObject $Object -MemberType NoteProperty -Name $Name -Value $Value -PassThru)
    } else {
        [void]($Object.$Name = $Value)
    }
}

function Read-HybridPixelFixtureJson {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Label
    )
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Hybrid Pixel production fixture cannot find ${Label}: $Path"
    }
    try {
        $value = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json -Depth 100
    } catch {
        throw "Hybrid Pixel production fixture could not parse $Label '$Path': $($_.Exception.Message)"
    }
    if ($null -eq $value) { throw "Hybrid Pixel production fixture found an empty ${Label}: $Path" }
    return $value
}

function Write-HybridPixelFixtureJson {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)]$Value
    )
    $parent = Split-Path -Parent $Path
    if (-not [string]::IsNullOrWhiteSpace($parent)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    $json = $Value | ConvertTo-Json -Depth 100
    [IO.File]::WriteAllText($Path, $json + "`n", [Text.UTF8Encoding]::new($false))
}

function Get-HybridPixelFixtureFullPath {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$RelativePath,
        [Parameter(Mandatory = $true)][string]$Label
    )
    if ([string]::IsNullOrWhiteSpace($RelativePath)) {
        throw "Hybrid Pixel production fixture received an empty relative path for $Label."
    }
    if ([IO.Path]::IsPathRooted($RelativePath)) {
        throw "Hybrid Pixel production fixture rejected rooted $Label path '$RelativePath'."
    }
    $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
    $candidate = [IO.Path]::GetFullPath((Join-Path $rootFull $RelativePath.Replace('/', '\')))
    $prefix = $rootFull + [IO.Path]::DirectorySeparatorChar
    if (-not $candidate.Equals($rootFull, [StringComparison]::OrdinalIgnoreCase) -and
        -not $candidate.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Hybrid Pixel production fixture rejected escaping $Label path '$RelativePath'."
    }
    return $candidate
}

function ConvertTo-HybridPixelFixtureRelativePath {
    param([Parameter(Mandatory = $true)][string]$Path)
    return $Path.Replace('\', '/')
}

function Merge-HybridPixelFixtureIds {
    param(
        [AllowNull()][object[]]$Existing,
        [AllowNull()][string[]]$Required
    )
    $result = [System.Collections.Generic.List[string]]::new()
    $seen = @{}
    foreach ($value in @($Existing) + @($Required)) {
        if ($null -eq $value) { continue }
        $id = [string]$value
        if ([string]::IsNullOrWhiteSpace($id) -or $seen.ContainsKey($id)) { continue }
        $seen[$id] = $true
        [void]$result.Add($id)
    }
    return @($result.ToArray())
}

function Find-HybridPixelFixtureEntity {
    param(
        [Parameter(Mandatory = $true)][object[]]$Entities,
        [Parameter(Mandatory = $true)][string]$Guid
    )
    foreach ($entity in @($Entities)) {
        if ($null -ne $entity -and [string](Get-HybridPixelFixtureProperty $entity 'guid') -eq $Guid) {
            return $entity
        }
    }
    return $null
}

function Get-HybridPixelFixtureManagedScriptIds {
    param([Parameter(Mandatory = $true)][object[]]$Entities)
    $result = [System.Collections.Generic.List[string]]::new()
    foreach ($entity in @($Entities)) {
        $components = Get-HybridPixelFixtureProperty $entity 'components'
        $managed = Get-HybridPixelFixtureProperty $components 'ManagedScript'
        if ($null -eq $managed) { continue }
        $instanceId = [string](Get-HybridPixelFixtureProperty $managed 'instanceId')
        if ([string]::IsNullOrWhiteSpace($instanceId)) {
            $instanceId = [string](Get-HybridPixelFixtureProperty $entity 'guid')
        }
        if (-not [string]::IsNullOrWhiteSpace($instanceId)) { [void]$result.Add($instanceId) }
    }
    return @($result.ToArray())
}

function New-HybridPixelFixtureRegistryRecord {
    param(
        [Parameter(Mandatory = $true)][string]$Id,
        [Parameter(Mandatory = $true)][string]$DisplayName,
        [Parameter(Mandatory = $true)][string]$Kind,
        [Parameter(Mandatory = $true)][string]$RelativePath,
        [Parameter(Mandatory = $true)][string[]]$Tags,
        [AllowNull()][string[]]$Dependencies = @()
    )
    $normalizedPath = ConvertTo-HybridPixelFixtureRelativePath $RelativePath
    $record = [ordered]@{
        id = $Id
        displayName = $DisplayName
        kind = $Kind
        uri = 'asset://' + $normalizedPath
        path = $normalizedPath
        license = 'project-original'
        redistribution = 'project-only'
        tags = @($Tags)
    }
    if (@($Dependencies).Count -gt 0) { $record.dependencies = @($Dependencies) }
    return [pscustomobject]$record
}

function New-HybridPixelFixtureSpriteDocument {
    param(
        [Parameter(Mandatory = $true)][string]$AssetId,
        [Parameter(Mandatory = $true)][string]$ShadingModel,
        [Parameter(Mandatory = $true)][bool]$ReceivesShadows,
        [Parameter(Mandatory = $true)][bool]$CastsShadows,
        [Parameter(Mandatory = $true)][string]$SourceUri
    )
    return [ordered]@{
        schema = 'noemancer.sprite-asset/0.2'
        assetId = $AssetId
        textureAsset = 'texture.hybrid.pixel.base'
        textureSize = @(1448, 1086)
        pixelsPerUnit = 16
        sampling = 'nearest'
        alphaMode = 'cutout'
        material = [ordered]@{
            normalTextureAsset = 'texture.hybrid.pixel.normal'
            emissiveMaskTextureAsset = 'texture.hybrid.pixel.emissive'
            depthTextureAsset = 'texture.hybrid.pixel.depth'
            normalStrength = 0.85
            emissiveColor = @(1.0, 0.16, 0.04)
            emissiveIntensity = 0.35
            depthBias = 0.0005
            shadingModel = $ShadingModel
            metallic = 0.15
            roughness = 0.45
            receivesShadows = $ReceivesShadows
            castsShadows = $CastsShadows
        }
        frames = @([ordered]@{
            id = 'idle.0'
            rect = @(0, 0, 1448, 1086)
            trimOffset = @(0, 0)
            sourceSize = @(1448, 1086)
            pivot = @(0.5, 0.5)
            collisionProfile = ''
        })
        clips = @([ordered]@{
            id = 'idle'
            looping = $true
            frames = @([ordered]@{ frame = 'idle.0'; durationMs = 1000; event = '' })
        })
        provenance = [ordered]@{
            sourceUri = $SourceUri
            sourceSha256 = 'fixture'
            generator = 'hybrid-pixel-production-fixture'
            license = 'project-original'
        }
    }
}

function New-HybridPixelFixtureProfile {
    return [ordered]@{
        schema = 'noemancer.hybrid-pixel-profile/0.1'
        profileId = 'lumen-run-hybrid-pixel-mixed-lighting'
        enabled = $true
        virtualWidth = 320
        virtualHeight = 180
        pixelsPerUnit = 16
        integerScaling = $true
        snapCamera = $true
        snapSprites = $true
        presentationFilter = 'nearest'
    }
}

function New-HybridPixelFixtureSpriteEntity {
    param(
        [Parameter(Mandatory = $true)]$Row,
        [Parameter(Mandatory = $true)][string]$ParentGuid
    )
    return [ordered]@{
        guid = Get-HybridPixelFixtureSpriteEntityGuid -AssetId $Row.id
        name = $Row.name
        parent = $ParentGuid
        components = [ordered]@{
            Transform = [ordered]@{ position = $Row.position; scale = @(0.38, 0.38, 0.38) }
            SpriteRenderer = [ordered]@{
                spriteAsset = $Row.asset
                clip = 'idle'
                playbackSpeed = 1
                playing = $false
                flipX = $false
                flipY = $false
                sortingLayer = 'hybrid-pixel-production'
                sortingOrder = $Row.order
                visible = $true
            }
        }
    }
}

function Get-HybridPixelFixtureSpriteEntityGuid {
    param([Parameter(Mandatory = $true)][string]$AssetId)
    if ($AssetId.StartsWith('sprite.hybrid.pixel.', [StringComparison]::Ordinal)) {
        return $AssetId.Replace('sprite.hybrid.pixel.', 'entity.hybrid.sprite.')
    }
    return $AssetId.Replace('sprite.', 'entity.')
}

function New-HybridPixelFixturePointLightEntity {
    param([Parameter(Mandatory = $true)][string]$ParentGuid)
    return [ordered]@{
        guid = 'entity.hybrid.point-light'
        name = 'Point Hybrid Pixel Production Light'
        parent = $ParentGuid
        components = [ordered]@{
            Transform = [ordered]@{ position = @(0, 4, 4) }
            LocalLight = [ordered]@{
                kind = 'point'
                color = @(1.0, 0.55, 0.3)
                luminousPowerLumens = 1200
                rangeMeters = 12
                direction = @(0, -1, 0)
                innerConeDegrees = 25
                outerConeDegrees = 35
                sourceRadiusMeters = 0.1
                castsShadows = $true
            }
        }
    }
}

function New-HybridPixelFixtureSpotLightEntity {
    param([Parameter(Mandatory = $true)][string]$ParentGuid)
    return [ordered]@{
        guid = 'entity.hybrid.spot-light'
        name = 'Spot Hybrid Pixel Production Light'
        parent = $ParentGuid
        components = [ordered]@{
            Transform = [ordered]@{ position = @(0, 5, 3) }
            LocalLight = [ordered]@{
                kind = 'spot'
                color = @(0.3, 0.55, 1.0)
                luminousPowerLumens = 1400
                rangeMeters = 14
                direction = @(0, -1, -0.2)
                innerConeDegrees = 12
                outerConeDegrees = 28
                sourceRadiusMeters = 0.15
                castsShadows = $true
            }
        }
    }
}

function New-HybridPixelFixtureMeshEntity {
    param([Parameter(Mandatory = $true)][string]$ParentGuid)
    return [ordered]@{
        guid = 'entity.hybrid.mesh'
        name = 'Controlled Hybrid Pixel 3D Sphere'
        parent = $ParentGuid
        components = [ordered]@{
            Transform = [ordered]@{ position = @(0, -0.2, -0.8); scale = @(1.3, 1.3, 1.3) }
            MeshRenderer = [ordered]@{
                meshAsset = 'asset.primitive.sphere'
                visible = $true
                castsShadows = $true
                receivesShadows = $true
            }
            PbrMaterial = [ordered]@{
                baseColor = @(0.95, 0.2, 0.08)
                metallic = 0.06
                roughness = 0.36
                emissiveColor = @(0.04, 0.3, 0.8)
                emissiveIntensity = 0.35
            }
        }
    }
}

function New-HybridPixelProductionFixture {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true, Position = 0)]
        [Alias('ProjectRoot')]
        [string]$ProjectPath,
        [switch]$DryRun
    )

    if ([string]::IsNullOrWhiteSpace($ProjectPath)) {
        throw 'Hybrid Pixel production fixture requires -ProjectPath.'
    }
    if (-not (Test-Path -LiteralPath $ProjectPath -PathType Container)) {
        throw "Hybrid Pixel production fixture project directory does not exist: $ProjectPath"
    }
    $projectRoot = [IO.Path]::GetFullPath((Resolve-Path -LiteralPath $ProjectPath).Path)
    $assetRoot = Join-Path $projectRoot 'assets'
    $manifestPath = Join-Path $projectRoot 'noemancer.project.json'
    $registryPath = Join-Path $projectRoot 'assets\registry.json'
    $manifest = Read-HybridPixelFixtureJson -Path $manifestPath -Label 'project manifest'
    $registry = Read-HybridPixelFixtureJson -Path $registryPath -Label 'Asset Registry'

    $startupSceneRelative = [string](Get-HybridPixelFixtureProperty $manifest 'startupScene')
    if ([string]::IsNullOrWhiteSpace($startupSceneRelative)) {
        throw 'Hybrid Pixel production fixture requires a non-empty project startupScene.'
    }
    $startupSceneRelative = ConvertTo-HybridPixelFixtureRelativePath $startupSceneRelative
    $scenePath = Get-HybridPixelFixtureFullPath -Root $projectRoot -RelativePath $startupSceneRelative -Label 'startup Scene'
    $scene = Read-HybridPixelFixtureJson -Path $scenePath -Label 'startup Scene'
    $entitiesValue = Get-HybridPixelFixtureProperty $scene 'entities'
    if ($null -eq $entitiesValue) { throw "Hybrid Pixel production fixture startup Scene has no entities array: $scenePath" }
    $originalEntities = @($entitiesValue)
    $sceneEntities = [System.Collections.Generic.List[object]]::new()
    foreach ($entity in $originalEntities) { [void]$sceneEntities.Add($entity) }

    $originalManagedScriptIds = @(Get-HybridPixelFixtureManagedScriptIds -Entities $originalEntities)
    $originalEntityCount = $originalEntities.Count
    $originalManagedScriptCount = $originalManagedScriptIds.Count
    $rootGuid = $null
    foreach ($entity in $originalEntities) {
        $parent = Get-HybridPixelFixtureProperty $entity 'parent'
        if ($null -eq $parent -or [string]::IsNullOrWhiteSpace([string]$parent)) {
            $rootGuid = [string](Get-HybridPixelFixtureProperty $entity 'guid')
            if ($rootGuid -eq 'entity.lumen.root') { break }
        }
    }
    if ([string]::IsNullOrWhiteSpace($rootGuid)) { $rootGuid = 'entity.lumen.root' }

    $inputActionsValue = Get-HybridPixelFixtureProperty $manifest 'inputActions'
    $inputActions = if ($null -eq $inputActionsValue) { @() } else { @($inputActionsValue) }
    $hudDocument = Get-HybridPixelFixtureProperty $manifest 'hudDocument'
    $inputActionIds = @($inputActions | ForEach-Object { [string](Get-HybridPixelFixtureProperty $_ 'id') } | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })

    $recordsValue = Get-HybridPixelFixtureProperty $registry 'assets'
    $records = [System.Collections.Generic.List[object]]::new()
    if ($null -ne $recordsValue) {
        foreach ($record in @($recordsValue)) { [void]$records.Add($record) }
    }
    $recordById = @{}
    foreach ($record in $records) {
        $recordId = [string](Get-HybridPixelFixtureProperty $record 'id')
        if ([string]::IsNullOrWhiteSpace($recordId)) { throw 'Hybrid Pixel production fixture found a Registry record without an id.' }
        if ($recordById.ContainsKey($recordId)) { throw "Hybrid Pixel production fixture found duplicate Registry id '$recordId'." }
        $recordById[$recordId] = $record
    }

    $sourcePath = Join-Path $projectRoot 'assets\art\source\courier-action-source-v2.png'
    if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
        $sourcePath = $null
        foreach ($record in $records) {
            $kind = [string](Get-HybridPixelFixtureProperty $record 'kind')
            $recordRelative = [string](Get-HybridPixelFixtureProperty $record 'path')
            if ($kind -ne 'Texture' -or [string]::IsNullOrWhiteSpace($recordRelative)) { continue }
            try { $candidate = Get-HybridPixelFixtureFullPath -Root $assetRoot -RelativePath $recordRelative -Label 'Registry texture' }
            catch { continue }
            if (Test-Path -LiteralPath $candidate -PathType Leaf) { $sourcePath = $candidate; break }
        }
    }
    if ([string]::IsNullOrWhiteSpace($sourcePath) -or -not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
        throw 'Hybrid Pixel production fixture requires a source Texture file in the copied project.'
    }
    $sourceRelative = [string]$sourcePath.Substring($assetRoot.Length).TrimStart('\', '/')
    $sourceRelative = ConvertTo-HybridPixelFixtureRelativePath $sourceRelative

    $textureRows = @(
        [pscustomobject][ordered]@{ id = 'texture.hybrid.pixel.base'; displayName = 'Hybrid Pixel Base'; tags = @('sprite', 'base-color', 'srgb'); defaultPath = 'art/source/hybrid-pixel-production/base.png' }
        [pscustomobject][ordered]@{ id = 'texture.hybrid.pixel.normal'; displayName = 'Hybrid Pixel Normal'; tags = @('sprite', 'normal', 'linear'); defaultPath = 'art/source/hybrid-pixel-production/normal.png' }
        [pscustomobject][ordered]@{ id = 'texture.hybrid.pixel.emissive'; displayName = 'Hybrid Pixel Emissive Mask'; tags = @('sprite', 'emissive', 'mask', 'linear'); defaultPath = 'art/source/hybrid-pixel-production/emissive.png' }
        [pscustomobject][ordered]@{ id = 'texture.hybrid.pixel.depth'; displayName = 'Hybrid Pixel Height Depth'; tags = @('sprite', 'depth', 'linear'); defaultPath = 'art/source/hybrid-pixel-production/depth.png' }
    )
    $texturePaths = [ordered]@{}
    foreach ($row in $textureRows) {
        $relativePath = [string]$row.defaultPath
        if ($recordById.ContainsKey($row.id)) {
            $existing = $recordById[$row.id]
            if ([string](Get-HybridPixelFixtureProperty $existing 'kind') -ne 'Texture') {
                throw "Hybrid Pixel production fixture expected Registry id '$($row.id)' to be a Texture."
            }
            $existingPath = [string](Get-HybridPixelFixtureProperty $existing 'path')
            if (-not [string]::IsNullOrWhiteSpace($existingPath)) { $relativePath = ConvertTo-HybridPixelFixtureRelativePath $existingPath }
            else {
                Set-HybridPixelFixtureProperty -Object $existing -Name 'path' -Value (ConvertTo-HybridPixelFixtureRelativePath $relativePath)
                if ([string]::IsNullOrWhiteSpace([string](Get-HybridPixelFixtureProperty $existing 'uri'))) {
                    Set-HybridPixelFixtureProperty -Object $existing -Name 'uri' -Value ('asset://' + (ConvertTo-HybridPixelFixtureRelativePath $relativePath))
                }
            }
        } else {
            $record = New-HybridPixelFixtureRegistryRecord -Id $row.id -DisplayName $row.displayName -Kind 'Texture' -RelativePath $relativePath -Tags $row.tags
            [void]$records.Add($record)
            $recordById[$row.id] = $record
        }
        [void](Get-HybridPixelFixtureFullPath -Root $assetRoot -RelativePath $relativePath -Label "Texture '$($row.id)'")
        $texturePaths[$row.id] = ConvertTo-HybridPixelFixtureRelativePath $relativePath
    }

    $spriteMatrix = @(
        [pscustomobject][ordered]@{ id = 'sprite.hybrid.pixel.lit.cast.receive'; name = 'Hybrid Pixel Lit Cast Receive'; asset = 'sprite.hybrid.pixel.lit.cast.receive'; shading = 'lit'; receive = $true; cast = $true; position = @(-5.0, 1.0, 0); order = 0 }
        [pscustomobject][ordered]@{ id = 'sprite.hybrid.pixel.lit.no-shadow'; name = 'Hybrid Pixel Lit No Shadow'; asset = 'sprite.hybrid.pixel.lit.no-shadow'; shading = 'lit'; receive = $false; cast = $false; position = @(-1.7, 1.0, 0); order = 1 }
        [pscustomobject][ordered]@{ id = 'sprite.hybrid.pixel.unlit.cast.receive'; name = 'Hybrid Pixel Unlit Cast Receive'; asset = 'sprite.hybrid.pixel.unlit.cast.receive'; shading = 'unlit'; receive = $true; cast = $true; position = @(1.7, 1.0, 0); order = 2 }
        [pscustomobject][ordered]@{ id = 'sprite.hybrid.pixel.unlit.no-shadow'; name = 'Hybrid Pixel Unlit No Shadow'; asset = 'sprite.hybrid.pixel.unlit.no-shadow'; shading = 'unlit'; receive = $false; cast = $false; position = @(5.0, 1.0, 0); order = 3 }
    )
    $spriteRelativePaths = [ordered]@{}
    $textureDependencies = @($textureRows | ForEach-Object { $_.id })
    foreach ($row in $spriteMatrix) {
        $relativePath = 'art/hybrid-pixel-production/' + $row.id + '.sprite.json'
        if ($recordById.ContainsKey($row.id)) {
            $existing = $recordById[$row.id]
            if ([string](Get-HybridPixelFixtureProperty $existing 'kind') -ne 'Sprite') {
                throw "Hybrid Pixel production fixture expected Registry id '$($row.id)' to be a Sprite."
            }
            $existingPath = [string](Get-HybridPixelFixtureProperty $existing 'path')
            if (-not [string]::IsNullOrWhiteSpace($existingPath)) { $relativePath = ConvertTo-HybridPixelFixtureRelativePath $existingPath }
            else {
                Set-HybridPixelFixtureProperty -Object $existing -Name 'path' -Value $relativePath
                if ([string]::IsNullOrWhiteSpace([string](Get-HybridPixelFixtureProperty $existing 'uri'))) {
                    Set-HybridPixelFixtureProperty -Object $existing -Name 'uri' -Value ('asset://' + $relativePath)
                }
            }
            $dependencies = Merge-HybridPixelFixtureIds -Existing (Get-HybridPixelFixtureProperty $existing 'dependencies') -Required $textureDependencies
            Set-HybridPixelFixtureProperty -Object $existing -Name 'dependencies' -Value $dependencies
            $tags = Merge-HybridPixelFixtureIds -Existing (Get-HybridPixelFixtureProperty $existing 'tags') -Required @('sprite', 'hybrid-pixel', $row.shading)
            Set-HybridPixelFixtureProperty -Object $existing -Name 'tags' -Value $tags
        } else {
            $record = New-HybridPixelFixtureRegistryRecord -Id $row.id -DisplayName $row.name -Kind 'Sprite' -RelativePath $relativePath -Tags @('sprite', 'hybrid-pixel', $row.shading) -Dependencies $textureDependencies
            [void]$records.Add($record)
            $recordById[$row.id] = $record
        }
        [void](Get-HybridPixelFixtureFullPath -Root $assetRoot -RelativePath $relativePath -Label "Sprite '$($row.id)'")
        $spriteRelativePaths[$row.id] = ConvertTo-HybridPixelFixtureRelativePath $relativePath
    }

    $addedEntityIds = [System.Collections.Generic.List[string]]::new()
    $spriteEntityIds = [System.Collections.Generic.List[string]]::new()
    foreach ($row in $spriteMatrix) {
        $spriteEntityGuid = Get-HybridPixelFixtureSpriteEntityGuid -AssetId $row.id
        $entity = Find-HybridPixelFixtureEntity -Entities @($sceneEntities.ToArray()) -Guid $spriteEntityGuid
        if ($null -eq $entity) {
            $entity = New-HybridPixelFixtureSpriteEntity -Row $row -ParentGuid $rootGuid
            [void]$sceneEntities.Add($entity)
            [void]$addedEntityIds.Add($spriteEntityGuid)
        }
        [void]$spriteEntityIds.Add($spriteEntityGuid)
    }

    $selectedLocalLightEntityIds = [ordered]@{}
    foreach ($kind in @('point', 'spot')) {
        $found = $null
        foreach ($entity in @($sceneEntities.ToArray())) {
            $components = Get-HybridPixelFixtureProperty $entity 'components'
            $localLight = Get-HybridPixelFixtureProperty $components 'LocalLight'
            if ($null -ne $localLight -and [string](Get-HybridPixelFixtureProperty $localLight 'kind') -eq $kind) { $found = $entity; break }
        }
        if ($null -eq $found) {
            $stableGuid = if ($kind -eq 'point') { 'entity.hybrid.point-light' } else { 'entity.hybrid.spot-light' }
            $found = Find-HybridPixelFixtureEntity -Entities @($sceneEntities.ToArray()) -Guid $stableGuid
            if ($null -eq $found) {
                $found = if ($kind -eq 'point') { New-HybridPixelFixturePointLightEntity -ParentGuid $rootGuid } else { New-HybridPixelFixtureSpotLightEntity -ParentGuid $rootGuid }
                [void]$sceneEntities.Add($found)
                [void]$addedEntityIds.Add($stableGuid)
            }
        }
        $selectedLocalLightEntityIds[$kind] = [string](Get-HybridPixelFixtureProperty $found 'guid')
    }

    $meshEntity = Find-HybridPixelFixtureEntity -Entities @($sceneEntities.ToArray()) -Guid 'entity.hybrid.mesh'
    if ($null -eq $meshEntity) {
        $meshEntity = New-HybridPixelFixtureMeshEntity -ParentGuid $rootGuid
        [void]$sceneEntities.Add($meshEntity)
        [void]$addedEntityIds.Add('entity.hybrid.mesh')
    }

    $finalEntities = @($sceneEntities.ToArray())
    Set-HybridPixelFixtureProperty -Object $scene -Name 'entities' -Value $finalEntities
    $finalManagedScriptIds = @(Get-HybridPixelFixtureManagedScriptIds -Entities $finalEntities)
    $finalEntityCount = $finalEntities.Count
    $finalManagedScriptCount = $finalManagedScriptIds.Count

    $profile = New-HybridPixelFixtureProfile
    Set-HybridPixelFixtureProperty -Object $manifest -Name 'schema' -Value 'noemancer.project/0.2'
    Set-HybridPixelFixtureProperty -Object $manifest -Name 'hybridPixelProfile' -Value $profile
    $packagedAssets = Merge-HybridPixelFixtureIds -Existing (Get-HybridPixelFixtureProperty $manifest 'packagedAssets') -Required @($textureRows.id + $spriteMatrix.id)
    Set-HybridPixelFixtureProperty -Object $manifest -Name 'packagedAssets' -Value $packagedAssets
    Set-HybridPixelFixtureProperty -Object $registry -Name 'schema' -Value 'noemancer.assets/0.1'
    Set-HybridPixelFixtureProperty -Object $registry -Name 'assets' -Value @($records.ToArray())

    if (-not $DryRun) {
        foreach ($row in $textureRows) {
            $destination = Get-HybridPixelFixtureFullPath -Root $assetRoot -RelativePath $texturePaths[$row.id] -Label "Texture '$($row.id)'"
            if (-not ([IO.Path]::GetFullPath($sourcePath)).Equals([IO.Path]::GetFullPath($destination), [StringComparison]::OrdinalIgnoreCase)) {
                $destinationParent = Split-Path -Parent $destination
                New-Item -ItemType Directory -Path $destinationParent -Force | Out-Null
                Copy-Item -LiteralPath $sourcePath -Destination $destination -Force
            }
        }
        foreach ($row in $spriteMatrix) {
            $relative = $spriteRelativePaths[$row.id]
            $path = Get-HybridPixelFixtureFullPath -Root $assetRoot -RelativePath $relative -Label "Sprite '$($row.id)'"
            $document = New-HybridPixelFixtureSpriteDocument -AssetId $row.id -ShadingModel $row.shading -ReceivesShadows:$row.receive -CastsShadows:$row.cast -SourceUri $sourceRelative
            Write-HybridPixelFixtureJson -Path $path -Value $document
        }
        Write-HybridPixelFixtureJson -Path $scenePath -Value $scene
        Write-HybridPixelFixtureJson -Path $registryPath -Value $registry
        Write-HybridPixelFixtureJson -Path $manifestPath -Value $manifest
    }

    $localLightIds = @($selectedLocalLightEntityIds.point, $selectedLocalLightEntityIds.spot)
    $expectedAssetIds = @($textureRows.id + $spriteMatrix.id)
    $sceneSummary = [ordered]@{
        sceneGuid = [string](Get-HybridPixelFixtureProperty $scene 'sceneGuid')
        sceneRelative = $startupSceneRelative
        scenePath = $scenePath
        rootEntityId = $rootGuid
        originalEntityCount = $originalEntityCount
        finalEntityCount = $finalEntityCount
        addedEntityIds = @($addedEntityIds.ToArray())
        spriteEntityIds = @($spriteEntityIds.ToArray())
        localLightEntityIds = $localLightIds
        controlledMeshEntityId = [string](Get-HybridPixelFixtureProperty $meshEntity 'guid')
        controlledMeshAsset = 'asset.primitive.sphere'
    }
    return [ordered]@{
        schemaVersion = 'noemancer.hybrid-pixel-production-fixture/0.1'
        success = $true
        dryRun = [bool]$DryRun
        projectRoot = $projectRoot
        manifestPath = $manifestPath
        registryPath = $registryPath
        profile = $profile
        scene = $sceneSummary
        sceneRelative = $startupSceneRelative
        scenePath = $scenePath
        expectedAssetIds = $expectedAssetIds
        textureIds = @($textureRows.id)
        spriteIds = @($spriteMatrix.id)
        spriteMatrix = @($spriteMatrix)
        meshAssets = @('asset.primitive.cube', 'asset.primitive.sphere')
        controlledMeshAsset = 'asset.primitive.sphere'
        localLightKinds = @('point', 'spot')
        localLightEntityIds = $localLightIds
        originalEntityCount = $originalEntityCount
        finalEntityCount = $finalEntityCount
        originalManagedScriptCount = $originalManagedScriptCount
        finalManagedScriptCount = $finalManagedScriptCount
        managedScriptCount = $finalManagedScriptCount
        originalManagedScriptIds = $originalManagedScriptIds
        managedScriptIds = $finalManagedScriptIds
        inputActionCount = $inputActions.Count
        inputActionIds = $inputActionIds
        hudDocument = $hudDocument
        packagedAssetIds = $packagedAssets
        preserved = [ordered]@{
            startupScene = $true
            sceneEntities = $true
            managedScripts = ($originalManagedScriptIds -join "`n") -eq ($finalManagedScriptIds -join "`n")
            inputActions = $true
            hudDocument = $true
            registryRecords = $true
            scriptProject = $true
        }
    }
}
