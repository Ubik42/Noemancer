[CmdletBinding(PositionalBinding=$false)]
param(
    [ValidateSet('Debug','Release')][string]$Config='Release',
    [string]$ProjectRoot='D:\3D\NoemancerProjects\NoemancerRenderLab',
    [string]$OutputRoot='',
    [ValidateRange(640,7680)][int]$Width=1440,
    [ValidateRange(360,4320)][int]$Height=900,
    [ValidateRange(1,10000)][int]$WarmupFrames=32,
    [ValidateRange(60,10000)][int]$SampleFrames=120,
    [ValidateRange(1.0,1000.0)][double]$CpuFrameP95MillisecondsMax=16.67,
    [ValidateRange(30,3600)][int]$TimeoutSeconds=900,
    [ValidateSet('direct3d12','vulkan')][string[]]$GpuBackends=@('direct3d12','vulkan'),
    [switch]$RequireGpuTelemetry
)

$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
$repo=[IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$runtime=Join-Path $repo "build\windows-msvc-debug\src\runtime\$Config\noemancer.exe"
$engineScript=Join-Path $repo 'scripts\engine.ps1'
$script:EvidenceRoot=$null
$script:Issues=[Collections.Generic.List[object]]::new()

function Get-Sha([string]$Path) {
    'sha256:'+(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Get-TextSha([string]$Text) {
    $bytes=[Text.Encoding]::UTF8.GetBytes($Text)
    'sha256:'+([Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes))).ToLowerInvariant()
}
function Get-Rel([string]$Path) {
    [IO.Path]::GetRelativePath($script:EvidenceRoot,[IO.Path]::GetFullPath($Path)).Replace('\','/')
}
function Read-Json([string]$Path) {
    if(-not(Test-Path -LiteralPath $Path -PathType Leaf)){throw "Missing JSON: $Path"}
    Get-Content -LiteralPath $Path -Raw -Encoding utf8|ConvertFrom-Json -Depth 100
}
function Add-Issue([string]$Code,[string]$Stage,[string]$Message) {
    $script:Issues.Add([ordered]@{code=$Code;stage=$Stage;message=$Message})
}
function Require([bool]$Condition,[string]$Code,[string]$Stage,[string]$Message) {
    if(-not $Condition){Add-Issue $Code $Stage $Message;throw $Message}
}

function Invoke-Hidden {
    param([string]$FilePath,[string[]]$Arguments,[string]$WorkingDirectory,
          [string]$StdoutPath,[string]$StderrPath,[string]$InputText='')
    $info=[Diagnostics.ProcessStartInfo]::new()
    $info.FileName=$FilePath;$info.WorkingDirectory=$WorkingDirectory
    $info.UseShellExecute=$false;$info.CreateNoWindow=$true
    $info.WindowStyle=[Diagnostics.ProcessWindowStyle]::Hidden
    $info.RedirectStandardInput=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    foreach($arg in $Arguments){[void]$info.ArgumentList.Add([string]$arg)}
    $process=[Diagnostics.Process]::new();$process.StartInfo=$info
    $watch=[Diagnostics.Stopwatch]::StartNew()
    if(-not $process.Start()){throw "Could not start $FilePath"}
    $stdoutTask=$process.StandardOutput.ReadToEndAsync();$stderrTask=$process.StandardError.ReadToEndAsync()
    if($InputText){$process.StandardInput.Write($InputText)}
    $process.StandardInput.Close()
    $timedOut=-not $process.WaitForExit($TimeoutSeconds*1000)
    if($timedOut){try{$process.Kill($true)}catch{};[void]$process.WaitForExit(10000)}
    $stdout=$stdoutTask.GetAwaiter().GetResult();$stderr=$stderrTask.GetAwaiter().GetResult()
    [IO.File]::WriteAllText($StdoutPath,$stdout,[Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText($StderrPath,$stderr,[Text.UTF8Encoding]::new($false))
    $result=[ordered]@{exitCode=if($timedOut){124}else{$process.ExitCode};timedOut=$timedOut
        durationMilliseconds=[math]::Round($watch.Elapsed.TotalMilliseconds,3)
        stdout=Get-Rel $StdoutPath;stderr=Get-Rel $StderrPath;stdoutText=$stdout;stderrText=$stderr}
    $process.Dispose();$result
}

function Invoke-ProjectTools([string]$Root,$Requests,[string]$Label) {
    $input=(($Requests|ForEach-Object{$_|ConvertTo-Json -Depth 100 -Compress})-join "`n")+"`n"
    $out=Join-Path $script:EvidenceRoot "$Label.stdout.jsonl";$err=Join-Path $script:EvidenceRoot "$Label.stderr.log"
    $raw=Invoke-Hidden $runtime @('serve','--project',$Root,'--format','jsonl') $Root $out $err $input
    $responses=@()
    foreach($line in ($raw.stdoutText -split "`r?`n"|Where-Object{$_.Trim()})){
        try{$responses+=($line|ConvertFrom-Json -Depth 100)}catch{Add-Issue 'serve.invalid-jsonl' $Label $_.Exception.Message}
    }
    [ordered]@{process=$raw;responses=$responses}
}
function Get-Response($Responses,[string]$Id) {
    $Responses|Where-Object{$_.id -eq $Id}|Select-Object -First 1
}

function Invoke-Cook([string]$Root,[string[]]$AssetIds) {
    $planSession=Invoke-ProjectTools $Root @(
        [ordered]@{id='registry';name='asset.registry';arguments=[ordered]@{refresh=$false}},
        [ordered]@{id='plan';name='asset.cook.plan';arguments=[ordered]@{assetIds=$AssetIds;targetProfile='windows-x64-release'}}
    ) 'cook-plan'
    Require ($planSession.process.exitCode -eq 0 -and -not $planSession.process.timedOut) 'cook.plan-session-failed' 'cook' 'Cook plan session failed.'
    $planResponse=Get-Response $planSession.responses 'plan';$plan=$planResponse.response.result
    Require ($null-ne $planResponse -and $planResponse.exitCode -eq 0 -and $plan.valid -eq $true -and $plan.code -eq 'ok') 'cook.plan-failed' 'cook' 'Three-model Cook plan was not valid.'
    foreach($id in $AssetIds){Require (@($plan.inputs|Where-Object{$_.assetId -eq $id}).Count -eq 1) 'cook.input-missing' 'cook' "Cook plan omitted $id."}
    $applySession=Invoke-ProjectTools $Root @(
        [ordered]@{id='registry';name='asset.registry';arguments=[ordered]@{refresh=$false}},
        [ordered]@{id='apply';name='asset.cook.apply';arguments=[ordered]@{plan=$plan;dryRun=$false}}
    ) 'cook-apply'
    Require ($applySession.process.exitCode -eq 0 -and -not $applySession.process.timedOut) 'cook.apply-session-failed' 'cook' 'Cook apply session failed.'
    $applyResponse=Get-Response $applySession.responses 'apply';$apply=$applyResponse.response.result
    Require ($null-ne $applyResponse -and $applyResponse.exitCode -eq 0 -and $apply.success -eq $true -and $apply.dryRun -eq $false -and $apply.code -eq 'ok') 'cook.apply-failed' 'cook' 'Cook apply did not commit the Cook manifest.'
    [ordered]@{planFingerprint=Get-TextSha($plan|ConvertTo-Json -Depth 100 -Compress);inputCount=@($plan.inputs).Count
        planProcess=$planSession.process|Select-Object exitCode,timedOut,durationMilliseconds,stdout,stderr
        applyProcess=$applySession.process|Select-Object exitCode,timedOut,durationMilliseconds,stdout,stderr
        receipt=$apply}
}

function Invoke-RenderEvidence {
    param([string]$Mode,[string]$Backend,[string]$Executable,[string[]]$PrefixArguments,[string]$WorkingDirectory)
    $stage="$Mode-$Backend";$image=Join-Path $script:EvidenceRoot "$stage.bmp"
    $perf=Join-Path $script:EvidenceRoot "$stage.performance.json"
    $captureOut=Join-Path $script:EvidenceRoot "$stage.capture.stdout.jsonl";$captureErr=Join-Path $script:EvidenceRoot "$stage.capture.stderr.log"
    $performanceOut=Join-Path $script:EvidenceRoot "$stage.performance.stdout.jsonl";$performanceErr=Join-Path $script:EvidenceRoot "$stage.performance.stderr.log"
    # Capture and performance are deliberately separate processes: the runtime
    # gives the performance recorder sole ownership of its warm-up/sample frame
    # budget, so combining both modes would make the evidence ambiguous.
    $common=@('--format','json','--gpu-backend',$Backend,'--window-width',[string]$Width,'--window-height',[string]$Height,'--exposure','1.0','--render-scale','1.0')
    $captureArgs=@($PrefixArguments)+$common+@('--capture-frame',$image)
    $captureProcess=Invoke-Hidden $Executable $captureArgs $WorkingDirectory $captureOut $captureErr
    Require ($captureProcess.exitCode -eq 0 -and -not $captureProcess.timedOut) 'render.capture-process-failed' $stage "$stage capture failed."
    $performanceArgs=@($PrefixArguments)+$common+@('--performance-evidence',$perf,'--performance-hidden','--performance-workload',"noemancer.render-lab.classic/$Mode/0.1",
        '--performance-warmup-frames',[string]$WarmupFrames,'--performance-sample-frames',[string]$SampleFrames)
    $performanceProcess=Invoke-Hidden $Executable $performanceArgs $WorkingDirectory $performanceOut $performanceErr
    Require ($performanceProcess.exitCode -eq 0 -and -not $performanceProcess.timedOut) 'render.performance-process-failed' $stage "$stage performance run failed."
    $qualityPath="$image.quality.json"
    Require (Test-Path -LiteralPath $image -PathType Leaf) 'render.image-missing' $stage 'Capture image missing.'
    Require (Test-Path -LiteralPath $qualityPath -PathType Leaf) 'render.quality-missing' $stage 'Quality sidecar missing.'
    Require (Test-Path -LiteralPath $perf -PathType Leaf) 'render.performance-missing' $stage 'Performance evidence missing.'
    $quality=Read-Json $qualityPath;$performance=Read-Json $perf;$renderer=$quality.renderer
    Require ($quality.pass -eq $true -and $quality.dimensionsMatch -eq $true -and [int]$quality.width -eq $Width -and [int]$quality.height -eq $Height) 'render.quality-failed' $stage 'Image quality or fixed dimensions failed.'
    Require ([string]$renderer.schemaVersion -eq 'noemancer.renderer-status.v27') 'render.status-schema' $stage 'Renderer Status v26 missing.'
    Require ([string]$renderer.device.backend -eq $Backend) 'render.backend-mismatch' $stage 'Requested/reported backend mismatch.'
    Require ([string]$renderer.activeCameraId -eq 'entity.classic.camera') 'render.camera-mismatch' $stage 'Fixed classic camera was not active.'
    Require (@($renderer.graph.errors).Count -eq 0 -and @($renderer.graph.executionOrder).Count -ge 18) 'render.graph-invalid' $stage 'Render Graph is incomplete or invalid.'
    Require ([string]$renderer.device.artifactStatus -eq 'manifest-and-artifact-verified') 'render.shader-invalid' $stage 'Shader artifact contract was not verified.'
    Require ([int]$renderer.importedGpuMeshes -ge 3) 'render.models-missing' $stage 'Three real models were not uploaded.'
    $cpuFrameP95=[double]$performance.cpu.frameTime.p95
    # Source mode is the full editor and is recorded for diagnosis; the
    # commercial runtime budget belongs to the packaged Player product path.
    if($Mode -ne 'source'){
        Require ($cpuFrameP95 -le $CpuFrameP95MillisecondsMax) 'render.cpu-budget' $stage 'Packaged Player CPU frame p95 exceeded budget.'
    }
    $loads=$renderer.geometryLoading
    if($Mode -eq 'source'){Require ([int]$loads.sourceAssetDecodes -ge 3) 'render.source-decodes' $stage 'Source run did not decode three GLBs.'}
    else{Require ([int]$loads.cookedArtifactLoads -ge 3 -and [int]$loads.sourceAssetDecodes -eq 0 -and [int]$loads.offlineCompiles -eq 0) 'render.player-not-cooked-only' $stage 'Player was not cooked-only.'}
    $gpuAvailable=$performance.gpu.available -eq $true
    if($RequireGpuTelemetry){Require $gpuAvailable 'render.gpu-required' $stage 'Real GPU telemetry is required but unavailable.'}
    $gpu=if($gpuAvailable){[ordered]@{available=$true;source=$performance.gpu.source;metrics=$performance.gpu.metrics}}
        else{[ordered]@{available=$false;source=if($performance.gpu.requiredSource){$performance.gpu.requiredSource}else{$performance.gpu.source}
            reason=if($performance.gpu.reason){$performance.gpu.reason}else{$performance.gpu.diagnostic};metrics=$null}}
    $graphJson=$renderer.graph|ConvertTo-Json -Depth 100 -Compress;$statusJson=$renderer|ConvertTo-Json -Depth 100 -Compress
    [ordered]@{pass=$true;mode=$Mode;backend=$Backend;process=[ordered]@{
            capture=$captureProcess|Select-Object exitCode,timedOut,durationMilliseconds,stdout,stderr
            performance=$performanceProcess|Select-Object exitCode,timedOut,durationMilliseconds,stdout,stderr}
        image=Get-Rel $image;imageSha256=Get-Sha $image;qualitySidecar=Get-Rel $qualityPath;qualitySidecarSha256=Get-Sha $qualityPath
        performanceEvidence=Get-Rel $perf;performanceEvidenceSha256=Get-Sha $perf
        quality=[ordered]@{meanLuma=[double]$quality.metrics.meanLuma;darkPixelFraction=[double]$quality.metrics.darkPixelFraction;brightPixelFraction=[double]$quality.metrics.brightPixelFraction}
        cpu=[ordered]@{frameP95Milliseconds=$cpuFrameP95;budgetMilliseconds=$CpuFrameP95MillisecondsMax;budgetApplied=$Mode-ne'source';withinBudget=$cpuFrameP95-le$CpuFrameP95MillisecondsMax;sceneRecordP95Milliseconds=[double]$performance.cpu.sceneRenderRecord.p95}
        gpu=$gpu;renderer=[ordered]@{schemaVersion=$renderer.schemaVersion;statusFingerprint=Get-TextSha $statusJson
            graphId=$renderer.graph.graphId;graphSchemaVersion=$renderer.graph.schemaVersion;graphFingerprint=Get-TextSha $graphJson;passCount=@($renderer.graph.executionOrder).Count
            shaderArtifact=$renderer.device.shaderArtifact;shaderManifestHash=$renderer.device.artifactContract.manifestHash;shaderSourceContractHash=$renderer.device.artifactContract.sourceContractHash
            importedGpuMeshes=[int]$renderer.importedGpuMeshes;importedPrimitives=[int]$renderer.importedPrimitives;importedTextures=[int]$renderer.importedTextures
            geometryLoading=[ordered]@{cookedArtifactLoads=[int]$loads.cookedArtifactLoads;sourceAssetDecodes=[int]$loads.sourceAssetDecodes;offlineCompiles=[int]$loads.offlineCompiles}
            gpuTimestampQueries=[bool]$renderer.framePipeline.gpuTimestampQueries;gpuTimestampReason=$renderer.framePipeline.gpuTimestampReason;
            gpuTimestamp=$renderer.framePipeline.gpuTimestamp}}
}

$manifestPath=$null
try{
    $project=[IO.Path]::GetFullPath($ProjectRoot)
    if([string]::IsNullOrWhiteSpace($OutputRoot)){$OutputRoot=Join-Path $repo ('generated\acceptance\render-lab-classic-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))}
    $OutputRoot=[IO.Path]::GetFullPath($OutputRoot)
    if(Test-Path -LiteralPath $OutputRoot){throw "Immutable evidence output already exists: $OutputRoot"}
    [void](New-Item -ItemType Directory -Path $OutputRoot);$script:EvidenceRoot=$OutputRoot
    $manifestPath=Join-Path $OutputRoot 'render-lab-evidence.json'
    Require (Test-Path -LiteralPath $project -PathType Container) 'static.project-missing' 'static' "Project missing: $project"
    if(-not(Test-Path -LiteralPath $runtime -PathType Leaf)){
        $pwsh=(Get-Command pwsh -ErrorAction Stop).Source
        $build=Invoke-Hidden $pwsh @('-NoLogo','-NoProfile','-File',$engineScript,'build','-Config',$Config,'-Target','noemancer') $repo (Join-Path $OutputRoot 'build.stdout.log') (Join-Path $OutputRoot 'build.stderr.log')
        Require ($build.exitCode -eq 0 -and -not $build.timedOut) 'build.failed' 'build' 'Runtime build failed.'
    }
    Require (Test-Path -LiteralPath $runtime -PathType Leaf) 'build.runtime-missing' 'build' "Runtime missing: $runtime"

    $contractFile=Join-Path $project 'render-lab.contract.json';$contract=Read-Json $contractFile
    Require ([string]$contract.schemaVersion -eq 'noemancer.render-lab-contract/0.1' -and [string]$contract.contractId -eq 'commercial-raster.render-lab-classic-scene-contract') 'static.contract-schema' 'static' 'RenderLab acceptance contract is missing or incompatible.'
    Require ([int]$contract.surface.expectedWidth -eq $Width -and [int]$contract.surface.expectedHeight -eq $Height) 'static.surface-contract' 'static' 'Requested capture dimensions do not match the RenderLab contract.'
    $requestedBackends=@($GpuBackends|Sort-Object);$contractBackends=@($contract.backends.backend|Sort-Object)
    foreach($backend in $requestedBackends){Require ($contractBackends -contains $backend) 'static.backend-contract' 'static' "Backend is not admitted by the RenderLab contract: $backend"}

    foreach($entry in $contract.integrity.files){
        $integrityPath=[IO.Path]::GetFullPath((Join-Path $project ([string]$entry.path)))
        Require ($integrityPath.StartsWith($project+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) 'static.integrity-path' 'static' "Integrity path escapes the project: $($entry.path)"
        Require (Test-Path -LiteralPath $integrityPath -PathType Leaf) 'static.integrity-missing' 'static' "Integrity input is missing: $($entry.path)"
        Require ((Get-Sha $integrityPath) -eq [string]$entry.hash) 'static.integrity-hash' 'static' "Immutable project input changed: $($entry.path)"
    }

    $projectFile=Join-Path $project 'noemancer.project.json';$sceneFile=Join-Path $project 'scenes\classic-assets.scene.json';$registryFile=Join-Path $project 'assets\registry.json'
    $projectDoc=Read-Json $projectFile;$scene=Read-Json $sceneFile;$registry=Read-Json $registryFile
    Require ([string]$projectDoc.projectId -eq 'game.noemancer-render-lab' -and [string]$projectDoc.startupScene -eq 'scenes/classic-assets.scene.json') 'static.project-contract' 'static' 'Project identity/startup Scene changed.'
    Require ([string]$scene.sceneGuid -eq 'scene.noemancer-render-lab.classic-assets') 'static.scene-contract' 'static' 'Scene identity changed.'
    $camera=@($scene.entities|Where-Object{$_.guid -eq 'entity.classic.camera'})|Select-Object -First 1
    Require ($null-ne $camera -and $camera.components.Camera.primary -eq $true -and (@($camera.components.Transform.position)-join ',') -eq '9.5,6.2,13.8' -and (@($camera.components.Camera.target)-join ',') -eq '0,1.5,0') 'static.camera-contract' 'static' 'Fixed camera changed.'

    $assetIds=@('asset.render-lab.khronos.metal-rough-spheres','asset.render-lab.khronos.boombox','asset.render-lab.khronos.lantern')
    $assets=@()
    foreach($id in $assetIds){
        $asset=@($registry.assets|Where-Object{$_.id -eq $id})|Select-Object -First 1
        Require ($null-ne $asset -and [string]$asset.license -eq 'CC0-1.0' -and [string]$asset.redistribution -eq 'public') 'static.asset-contract' 'static' "Missing/non-CC0 asset: $id"
        $model=Join-Path (Join-Path $project 'assets') ([string]$asset.path);Require (Test-Path -LiteralPath $model -PathType Leaf) 'static.glb-missing' 'static' "GLB missing: $id"
        $hash=Get-Sha $model;Require ([string]$asset.contentHash -eq $hash) 'static.glb-hash' 'static' "GLB hash mismatch: $id"
        $folder=Split-Path -Parent $model;$license=Join-Path $folder 'LICENSE.md';$metadata=Join-Path $folder 'metadata.json'
        Require (Test-Path -LiteralPath $license -PathType Leaf) 'static.license-missing' 'static' "License missing: $id"
        Require (Test-Path -LiteralPath $metadata -PathType Leaf) 'static.metadata-missing' 'static' "Metadata missing: $id"
        $assets+=[ordered]@{id=$id;uri=$asset.uri;license=$asset.license;redistribution=$asset.redistribution;contentHash=$hash;bytes=(Get-Item -LiteralPath $model).Length
            licensePath=[IO.Path]::GetRelativePath($project,$license).Replace('\','/');licenseSha256=Get-Sha $license;metadataPath=[IO.Path]::GetRelativePath($project,$metadata).Replace('\','/');metadataSha256=Get-Sha $metadata}
    }
    $assetFingerprint=Get-TextSha($assets|ConvertTo-Json -Depth 20 -Compress)

    $sourceRuns=@();foreach($backend in $GpuBackends){$sourceRuns+=Invoke-RenderEvidence 'source' $backend $runtime @('run','--project',$project) $project}
    $cook=Invoke-Cook $project $assetIds
    $packageRoot=Join-Path $OutputRoot 'package';$packageProcess=Invoke-Hidden $runtime @('package','--project',$project,'--output',$packageRoot,'--target-profile','windows-x64-release','--format','json') $project (Join-Path $OutputRoot 'package.stdout.json') (Join-Path $OutputRoot 'package.stderr.log')
    Require ($packageProcess.exitCode -eq 0 -and -not $packageProcess.timedOut) 'package.failed' 'package' 'Release Package failed after committed Cook.'
    $profilePath=Join-Path $packageRoot 'config\game-profile.json';$profile=Read-Json $profilePath;$player=Join-Path $packageRoot ('bin\'+[string]$profile.executable)
    Require (Test-Path -LiteralPath $player -PathType Leaf) 'package.player-missing' 'package' 'Packaged Player missing.'
    $leaked=@(Get-ChildItem -LiteralPath $packageRoot -Recurse -File|Where-Object{$_.Extension -in @('.glb','.gltf','.fbx')})
    Require ($leaked.Count -eq 0) 'package.source-leak' 'package' 'Source model leaked into package.'
    $playerRuns=@();foreach($backend in $GpuBackends){$playerRuns+=Invoke-RenderEvidence 'package' $backend $player @('player','--profile',$profilePath) $packageRoot}

    $allRuns=@($sourceRuns)+@($playerRuns)
    foreach($run in $allRuns){
        $timestampReported = if($run.renderer.gpuTimestampQueries){
            $null -ne $run.renderer.gpuTimestamp -and [bool]$run.renderer.gpuTimestamp.supported
        }else{
            -not[string]::IsNullOrWhiteSpace($run.renderer.gpuTimestampReason)
        }
        Require $timestampReported 'gpu.timestamp-report' "$($run.mode)-$($run.backend)" 'GPU timestamp capability or explicit unsupported reason was not reported.'
    }
    $manifest=[ordered]@{schemaVersion='noemancer.render-lab-evidence/0.1';capturedAt=[DateTimeOffset]::UtcNow.ToString('o');pass=$true;configuration=$Config
        runtime=[ordered]@{path=$runtime.Replace('\','/');sha256=Get-Sha $runtime}
        contract=[ordered]@{id='commercial-raster.render-lab-classic-scene-contract';schemaVersion=$contract.schemaVersion;source='project://render-lab.contract.json';sha256=Get-Sha $contractFile;projectId=$projectDoc.projectId;sceneGuid=$scene.sceneGuid;startupScene=$projectDoc.startupScene
            camera=[ordered]@{entityId=$camera.guid;position=@($camera.components.Transform.position);target=@($camera.components.Camera.target);verticalFovDegrees=$camera.components.Camera.verticalFovDegrees;nearClip=$camera.components.Camera.nearClip;farClip=$camera.components.Camera.farClip}
            width=$Width;height=$Height;exposure=1.0;renderScale=1.0;warmupFrames=$WarmupFrames;sampleFrames=$SampleFrames;cpuFrameP95MillisecondsMax=$CpuFrameP95MillisecondsMax;gpuTelemetryRequired=[bool]$RequireGpuTelemetry}
        project=[ordered]@{root=$project.Replace('\','/');manifestSha256=Get-Sha $projectFile;sceneSha256=Get-Sha $sceneFile;registrySha256=Get-Sha $registryFile;assetFingerprint=$assetFingerprint;assets=$assets}
        sourceBackends=$sourceRuns;cook=$cook
        package=[ordered]@{process=$packageProcess|Select-Object exitCode,timedOut,durationMilliseconds,stdout,stderr;root=Get-Rel $packageRoot;profile=Get-Rel $profilePath;profileSha256=Get-Sha $profilePath;player=Get-Rel $player;playerSha256=Get-Sha $player;sourceModelFiles=0;backends=$playerRuns}
        gpuTelemetry=[ordered]@{availableRuns=@($allRuns|Where-Object{$_.gpu.available}).Count;unavailableRuns=@($allRuns|Where-Object{-not$_.gpu.available}|ForEach-Object{"$($_.mode)/$($_.backend)"});timestampQueriesSupported=$false;policy='Unavailable is explicit; CPU submission or numeric zero is never substituted for GPU execution time.'}
        issues=@()}
    $manifest|ConvertTo-Json -Depth 100|Set-Content -LiteralPath $manifestPath -Encoding utf8NoBOM
    [pscustomobject]@{Success=$true;Evidence=$manifestPath;SourceRuns=$sourceRuns.Count;PlayerRuns=$playerRuns.Count}
}catch{
    if($null-eq $script:EvidenceRoot){throw}
    if($script:Issues.Count-eq 0){Add-Issue 'unhandled.failure' 'orchestration' $_.Exception.Message}
    [ordered]@{schemaVersion='noemancer.render-lab-evidence/0.1';capturedAt=[DateTimeOffset]::UtcNow.ToString('o');pass=$false;error=$_.Exception.Message;issues=@($script:Issues)}|
        ConvertTo-Json -Depth 100|Set-Content -LiteralPath $manifestPath -Encoding utf8NoBOM
    Write-Error "RenderLab evidence failed: $($_.Exception.Message)";exit 1
}
