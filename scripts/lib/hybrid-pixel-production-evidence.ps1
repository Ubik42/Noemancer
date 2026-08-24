# Semantic production-state comparison for the Hybrid Pixel production slice.
# Rendering and package closure stay with the owning acceptance harness. This
# module compares only runtime.production_state so machine paths cannot become
# false product drift.

function Get-HybridProductionProperty {
    param([AllowNull()]$Object, [Parameter(Mandatory = $true)][string]$Name)
    if ($null -eq $Object) { return $null }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}

function Get-HybridProductionState {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    $state = $null
    foreach ($line in [IO.File]::ReadLines((Resolve-Path -LiteralPath $Path).Path)) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        try { $event = ConvertFrom-Json -InputObject $line -Depth 100 -ErrorAction Stop } catch { continue }
        if ([string](Get-HybridProductionProperty $event 'event') -ne 'runtime.production_state') { continue }
        try { $state = ConvertFrom-Json -InputObject ([string]$event.message) -Depth 100 -ErrorAction Stop } catch { $state = $null }
    }
    return $state
}

function Get-HybridProductionInputProjection {
    param([AllowNull()]$InputState)
    $actions = @()
    foreach ($action in @((Get-HybridProductionProperty $InputState 'actions'))) {
        $bindings = @((Get-HybridProductionProperty $action 'bindings') | Sort-Object source, scale, deadZone | ForEach-Object {
            [ordered]@{ source = [string]$_.source; scale = [double]$_.scale; deadZone = [double]$_.deadZone }
        })
        $actions += [ordered]@{ id = [string]$action.id; kind = [string]$action.kind; bindings = $bindings }
    }
    return @($actions | Sort-Object id)
}

function Get-HybridProductionUiProjection {
    param([AllowNull()]$Ui)
    $nodes = @()
    foreach ($node in @((Get-HybridProductionProperty $Ui 'nodes'))) {
        $nodes += [ordered]@{
            id = [string](Get-HybridProductionProperty $node 'id')
            parentId = Get-HybridProductionProperty $node 'parentId'
            role = [string](Get-HybridProductionProperty $node 'role')
            label = [string](Get-HybridProductionProperty $node 'label')
            binding = Get-HybridProductionProperty $node 'binding'
            state = Get-HybridProductionProperty $node 'state'
            value = Get-HybridProductionProperty $node 'value'
            actions = @((Get-HybridProductionProperty $node 'actions'))
        }
    }
    return [ordered]@{
        schemaVersion = [string](Get-HybridProductionProperty $Ui 'schemaVersion')
        documentId = [string](Get-HybridProductionProperty $Ui 'documentId')
        kind = [string](Get-HybridProductionProperty $Ui 'kind')
        surface = [string](Get-HybridProductionProperty $Ui 'surface')
        locale = [string](Get-HybridProductionProperty $Ui 'locale')
        themeId = [string](Get-HybridProductionProperty $Ui 'themeId')
        textDirection = [string](Get-HybridProductionProperty $Ui 'textDirection')
        valid = [bool](Get-HybridProductionProperty $Ui 'valid')
        roots = @((Get-HybridProductionProperty $Ui 'roots'))
        designTokens = Get-HybridProductionProperty $Ui 'designTokens'
        nodes = @($nodes | Sort-Object id)
    }
}

function Get-HybridProductionScriptingProjection {
    param([AllowNull()]$Scripting)
    $instances = @()
    foreach ($instance in @((Get-HybridProductionProperty $Scripting 'instances'))) {
        $instances += [ordered]@{
            id = [string]$instance.id; entityId = [string]$instance.entityId
            typeName = [string]$instance.typeName; assemblyAsset = [string]$instance.assemblyAsset
            state = [string]$instance.state; sceneOwned = [bool]$instance.sceneOwned
            properties = $instance.properties; publicState = $instance.publicState
            lastCallback = [string]$instance.lastCallback; callbackCount = [int]$instance.callbackCount
        }
    }
    $lastResult = Get-HybridProductionProperty $Scripting 'lastManagedResult'
    return [ordered]@{
        schemaVersion = [string](Get-HybridProductionProperty $Scripting 'schemaVersion')
        backend = [string](Get-HybridProductionProperty $Scripting 'backend')
        status = [string](Get-HybridProductionProperty $Scripting 'status')
        ready = [bool](Get-HybridProductionProperty $Scripting 'ready')
        instances = @($instances | Sort-Object id)
        execution = [ordered]@{
            success = [bool](Get-HybridProductionProperty $lastResult 'success')
            projectCodeExecuted = [bool](Get-HybridProductionProperty $lastResult 'projectCodeExecuted')
            callback = [string](Get-HybridProductionProperty $lastResult 'callback')
            instanceId = [string](Get-HybridProductionProperty $lastResult 'instanceId')
            typeName = [string](Get-HybridProductionProperty $lastResult 'typeName')
        }
    }
}

function Get-HybridProductionProjection {
    param([AllowNull()]$State)
    if ($null -eq $State) { return $null }
    return [ordered]@{
        schemaVersion = [string](Get-HybridProductionProperty $State 'schemaVersion')
        entityCount = [int](Get-HybridProductionProperty $State 'entityCount')
        hybridPixelProfile = Get-HybridProductionProperty $State 'hybridPixelProfile'
        input = Get-HybridProductionInputProjection (Get-HybridProductionProperty $State 'input')
        projectUi = Get-HybridProductionUiProjection (Get-HybridProductionProperty $State 'projectUi')
        scripting = Get-HybridProductionScriptingProjection (Get-HybridProductionProperty $State 'scripting')
    }
}

function Get-HybridProductionSha256 {
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Text)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([Convert]::ToHexString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($Text)))).ToLowerInvariant() }
    finally { $sha.Dispose() }
}

function Test-HybridPixelProductionEvidence {
    [CmdletBinding(PositionalBinding = $false)]
    param(
        [Parameter(Mandatory = $true)][string]$SourceHeadlessPath,
        [Parameter(Mandatory = $true)][string]$PackageHeadlessPath,
        [string[]]$SourceCapturePaths = @(),
        [string[]]$PackageCapturePaths = @(),
        [Parameter(Mandatory = $true)][string]$PackageRoot,
        [Parameter(Mandatory = $true)][string]$CopiedProjectRoot,
        [Parameter(Mandatory = $true)]$Fixture
    )
    $issues = [System.Collections.Generic.List[object]]::new()
    $addIssue = {
        param([string]$Code, [string]$Stage, [string]$Path, [string]$Message)
        [void]$issues.Add([pscustomobject][ordered]@{ code = $Code; stage = $Stage; path = $Path; message = $Message })
    }
    $source = Get-HybridProductionState $SourceHeadlessPath
    $package = Get-HybridProductionState $PackageHeadlessPath
    if ($null -eq $source) { & $addIssue 'production-state.source-missing' 'source-headless' '/runtime.production_state' 'Source runtime production state is missing.' }
    if ($null -eq $package) { & $addIssue 'production-state.package-missing' 'package-headless' '/runtime.production_state' 'Packaged Player production state is missing.' }
    if ($null -ne $source -and [string]$source.mode -ne 'source-project') { & $addIssue 'production-state.source-mode' 'source-headless' '/mode' "Expected source-project, got '$($source.mode)'." }
    if ($null -ne $package -and [string]$package.mode -ne 'packaged-player') { & $addIssue 'production-state.package-mode' 'package-headless' '/mode' "Expected packaged-player, got '$($package.mode)'." }

    $sourceProjection = Get-HybridProductionProjection $source
    $packageProjection = Get-HybridProductionProjection $package
    $sourceJson = if ($null -eq $sourceProjection) { '' } else { $sourceProjection | ConvertTo-Json -Depth 100 -Compress }
    $packageJson = if ($null -eq $packageProjection) { '' } else { $packageProjection | ConvertTo-Json -Depth 100 -Compress }
    $sourceHash = Get-HybridProductionSha256 $sourceJson
    $packageHash = Get-HybridProductionSha256 $packageJson
    if ($sourceHash -ne $packageHash) { & $addIssue 'production-state.fingerprint-mismatch' 'comparison' '/' 'Source and packaged Player semantic production states differ.' }

    if ($null -ne $sourceProjection) {
        if ($sourceProjection.entityCount -ne [int]$Fixture.finalEntityCount) { & $addIssue 'production-state.entity-count' 'source-headless' '/entityCount' 'Runtime entity count differs from the augmented fixture.' }
        if (@($sourceProjection.scripting.instances).Count -ne [int]$Fixture.managedScriptCount) { & $addIssue 'production-state.script-count' 'source-headless' '/scripting/instances' 'Runtime managed instance count differs from the fixture.' }
        if (-not $sourceProjection.scripting.execution.success -or -not $sourceProjection.scripting.execution.projectCodeExecuted) { & $addIssue 'production-state.project-code-not-executed' 'source-headless' '/scripting/execution' 'Managed project code did not produce a successful execution proof.' }
        if (@($sourceProjection.input).Count -ne [int]$Fixture.inputActionCount) { & $addIssue 'production-state.input-count' 'source-headless' '/input' 'Runtime Input Action count differs from the fixture.' }
        if (-not $sourceProjection.projectUi.valid -or @($sourceProjection.projectUi.nodes).Count -eq 0) { & $addIssue 'production-state.ui-invalid' 'source-headless' '/projectUi' 'Project UI is missing or invalid.' }
    }
    foreach ($path in @($SourceCapturePaths) + @($PackageCapturePaths)) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { & $addIssue 'production-state.capture-missing' 'capture' $path 'Expected hidden capture evidence is missing.' }
    }
    foreach ($required in @('config\game-profile.json', 'content\assets\registry.json')) {
        if (-not (Test-Path -LiteralPath (Join-Path $PackageRoot $required) -PathType Leaf)) { & $addIssue 'production-state.package-authority-missing' 'package' $required 'Packaged authority file is missing.' }
    }
    if (-not (Test-Path -LiteralPath (Join-Path $CopiedProjectRoot 'noemancer.project.json') -PathType Leaf)) { & $addIssue 'production-state.project-copy-missing' 'fixture' '/noemancer.project.json' 'Copied project authority is missing.' }

    return [ordered]@{
        pass = ($issues.Count -eq 0)
        fingerprints = [ordered]@{ schemaVersion = 'noemancer.hybrid-pixel-production-fingerprints/0.1'; source = $sourceHash; package = $packageHash; equal = ($sourceHash -eq $packageHash) }
        comparisons = [ordered]@{ productionStateEqual = ($sourceHash -eq $packageHash); sourceMode = $source.mode; packageMode = $package.mode }
        issues = @($issues.ToArray())
        snapshot = [ordered]@{ schemaVersion = 'noemancer.hybrid-pixel-production-evidence/0.1'; entityCount = $sourceProjection.entityCount; inputActionCount = @($sourceProjection.input).Count; managedInstanceCount = @($sourceProjection.scripting.instances).Count; uiNodeCount = @($sourceProjection.projectUi.nodes).Count; projectCodeExecuted = $sourceProjection.scripting.execution.projectCodeExecuted }
    }
}
