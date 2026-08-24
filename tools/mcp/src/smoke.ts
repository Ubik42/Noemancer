import { resolve } from "node:path";

import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { StdioClientTransport } from "@modelcontextprotocol/sdk/client/stdio.js";

const serverPath = resolve(process.cwd(), "dist/index.js");
const transport = new StdioClientTransport({
  command: process.execPath,
  args: [serverPath],
});
const client = new Client({ name: "noemancer-smoke", version: "0.2.0" });

function textContent(result: unknown, tool: string): string {
  const content = (result as { content?: unknown }).content;
  if (!Array.isArray(content)) throw new Error(`${tool} did not return content`);
  const block = content.find((candidate): candidate is { type: "text"; text: string } => (
    typeof candidate === "object" && candidate !== null &&
    (candidate as { type?: unknown }).type === "text" &&
    typeof (candidate as { text?: unknown }).text === "string"
  ));
  if (block === undefined) throw new Error(`${tool} did not return text`);
  return block.text;
}

await client.connect(transport);
const tools = await client.listTools();
const names = tools.tools.map((tool) => tool.name).sort();
const expected = [
  "animation.graph.inspect",
  "animation.graph.instance.observe",
  "animation.graph.parameter.set",
  "animation.graph.patch",
  "animation.observe",
  "animation.skeleton.inspect",
  "animation.state-machine.inspect",
  "animation.state-machine.parameter.set",
  "asset.cook.apply",
  "asset.cook.plan",
  "asset.inspect",
  "asset.query",
  "asset.registry",
  "asset.source.redo",
  "asset.source.undo",
  "asset.sprite.pressure",
  "asset.tile-palette.autotile",
  "asset.tilemap.region",
  "asset.tilemap.stroke",
  "audio.bus.set",
  "audio.clip.load",
  "audio.listener.set",
  "audio.mixer.observe",
  "audio.voice.play",
  "audio.voice.spatial.set",
  "editor.inspector.describe",
  "engine.status",
  "gameplay.ability.activate",
  "gameplay.ability.activate-ray",
  "gameplay.ability.activate-sweep",
  "gameplay.ability.catalog",
  "gameplay.ability.grant",
  "gameplay.ability.observe",
  "gameplay.camera-follow-2d.observe",
  "gameplay.character-motor-2d.observe",
  "gameplay.effect.apply",
  "gameplay.effect.catalog",
  "gameplay.entity.despawn",
  "gameplay.events.observe",
  "gameplay.prefab.export",
  "gameplay.prefab.instantiate",
  "gameplay.prefab.spawn",
  "gameplay.replay.apply",
  "gameplay.replay.start",
  "gameplay.replay.stop",
  "gameplay.save.capture",
  "gameplay.save.restore",
  "input.actions.observe",
  "input.source.inject",
  "network.loopback.verify",
  "network.profile.describe",
  "network.snapshot.preview",
  "network.transport.verify",
  "physics.observe",
  "physics.ray-cast",
  "physics.sphere-sweep",
  "render.graph.inspect",
  "render.observe",
  "render.sprite.observe",
  "render.tilemap.pressure",
  "run.headless",
  "scene.entity.edit",
  "scene.export",
  "scene.open",
  "scene.save",
  "scene.transform.edit",
  "scene.validate",
  "schema.get",
  "scripting.abi.describe",
  "scripting.debug.attach-manifest",
  "scripting.debug.session.events",
  "scripting.debug.session.request",
  "scripting.debug.session.start",
  "scripting.debug.session.status",
  "scripting.debug.session.stop",
  "scripting.instance.attach",
  "scripting.instances.observe",
  "scripting.lifecycle.invoke",
  "scripting.project.compile",
  "scripting.project.observe",
  "semantic.conventions",
  "ui.delta",
  "ui.observe",
  "ui.project.action.invoke",
  "ui.project.observe",
  "ui.project.source.edit",
  "ui.project.source.observe",
  "ui.resources.inspect",
  "ui.retained.preview",
  "ui.text.inspect",
  "vfx.benchmark",
  "vfx.gpu-program.inspect",
  "vfx.graph.inspect",
  "vfx.graph.patch.apply",
  "vfx.graph.patch.plan",
  "vfx.graph.undo",
  "vfx.observe",
  "vfx.preview",
  "vfx.spawn",
  "world.change.apply",
  "world.delta",
  "world.property.plan",
  "world.query",
  "world.redo",
  "world.snapshot",
  "world.transform.plan",
  "world.undo",
];
if (JSON.stringify(names) !== JSON.stringify(expected)) {
  throw new Error(`Unexpected MCP tools: ${names.join(", ")}`);
}

const inputResult = await client.callTool({ name: "input.actions.observe", arguments: {} });
const projectUiResult = await client.callTool({ name: "ui.project.observe", arguments: {} });
const audioResult = await client.callTool({ name: "audio.mixer.observe", arguments: {} });
const gameplayResult = await client.callTool({ name: "gameplay.events.observe", arguments: { maxEvents: 8 } });
const characterMotorResult = await client.callTool({ name: "gameplay.character-motor-2d.observe", arguments: {} });
const cameraFollowResult = await client.callTool({ name: "gameplay.camera-follow-2d.observe", arguments: {} });
const animationMachine = await client.callTool({ name: "animation.state-machine.inspect", arguments: { entityId: "entity.demo-skeletal-cube" } });
const animationGraph = await client.callTool({ name: "animation.graph.instance.observe", arguments: { entityId: "entity.demo-skeletal-cube" } });
const sphereSweep = await client.callTool({ name: "physics.sphere-sweep", arguments: {
  origin: { x: 0, y: 3, z: 0 }, direction: { x: 0, y: -5, z: 0 }, radius: 0.2,
} });
const vfxGraph = await client.callTool({ name: "vfx.graph.inspect", arguments: {} });
const vfxBenchmark = await client.callTool({ name: "vfx.benchmark", arguments: { particleCount: 4096, steps: 8 } });
const vfxGpuProgram = await client.callTool({ name: "vfx.gpu-program.inspect", arguments: {} });
const vfxPreview = await client.callTool({ name: "vfx.preview", arguments: { seed: 42, steps: 20, maxParticles: 2 } });
const vfxSpawn = await client.callTool({ name: "vfx.spawn", arguments: { position: { x: 2.5, y: 2.2, z: 1 }, seed: 42 } });
const spritePressure = await client.callTool({ name: "asset.sprite.pressure", arguments: {
  frameCount: 1024, clipCount: 8, framesPerClip: 256, atlasColumns: 64, frameEdge: 16,
} });
const networkProfile = await client.callTool({ name: "network.profile.describe", arguments: {} });
const networkSnapshot = await client.callTool({ name: "network.snapshot.preview", arguments: { tick: 42, maxEntities: 2 } });
const networkLoopback = await client.callTool({ name: "network.loopback.verify", arguments: {} });
const networkTransport = await client.callTool({ name: "network.transport.verify", arguments: { payloadBytes: 384 } });
if (spritePressure.isError || !textContent(spritePressure, "asset.sprite.pressure").includes("noemancer.sprite-production-pressure/0.1")) {
  throw new Error("asset.sprite.pressure did not expose the bounded long-sequence production report");
}
if (inputResult.isError || !textContent(inputResult, "input.actions.observe").includes("gameplay.jump") ||
    projectUiResult.isError || !textContent(projectUiResult, "ui.project.observe").includes("noemancer.ui-observation/0.1") ||
    audioResult.isError || !textContent(audioResult, "audio.mixer.observe").includes("audio.master") ||
    gameplayResult.isError || !textContent(gameplayResult, "gameplay.events.observe").includes("noemancer.gameplay-events/0.1") ||
    characterMotorResult.isError || !textContent(characterMotorResult, "gameplay.character-motor-2d.observe").includes("noemancer.character-motor-2d-observation/0.1") ||
    cameraFollowResult.isError || !textContent(cameraFollowResult, "gameplay.camera-follow-2d.observe").includes("noemancer.camera-follow-2d-observation/0.1") ||
    animationMachine.isError || !textContent(animationMachine, "animation.state-machine.inspect").includes("animation.machine.basic-locomotion") ||
    animationGraph.isError || !textContent(animationGraph, "animation.graph.instance.observe").includes("noemancer.animation-graph-instance/0.1") ||
    sphereSweep.isError || !textContent(sphereSweep, "physics.sphere-sweep").includes("entity.demo-cube") ||
    vfxGraph.isError || !textContent(vfxGraph, "vfx.graph.inspect").includes("noemancer.vfx-graph/0.1") ||
    vfxBenchmark.isError || !textContent(vfxBenchmark, "vfx.benchmark").includes("structure-of-arrays/0.1") ||
    vfxGpuProgram.isError || !textContent(vfxGpuProgram, "vfx.gpu-program.inspect").includes("vfx_sim.comp") ||
    !textContent(vfxGpuProgram, "vfx.gpu-program.inspect").includes('"dispatchActive":false') ||
    vfxPreview.isError || !textContent(vfxPreview, "vfx.preview").includes("cpu-deterministic-reference") ||
    vfxSpawn.isError || !textContent(vfxSpawn, "vfx.spawn").includes('"spawned":48') ||
    networkProfile.isError || !textContent(networkProfile, "network.profile.describe").includes("network.optional-authoritative") ||
    networkSnapshot.isError || !textContent(networkSnapshot, "network.snapshot.preview").includes('"truncated":true') ||
    networkLoopback.isError || !textContent(networkLoopback, "network.loopback.verify").includes('"converged":true') ||
    networkTransport.isError || !textContent(networkTransport, "network.transport.verify").includes('"kernelSocket":true')) {
  throw new Error("Gameplay foundation observations are unavailable through MCP");
}

const vfxStateEnvelope = JSON.parse(textContent(await client.callTool({ name: "vfx.observe", arguments: { maxParticles: 0 } }), "vfx.observe")) as { result: { revision: number } };
const vfxPlanResult = await client.callTool({ name: "vfx.graph.patch.plan", arguments: {
  graphId: "vfx.debug-impact", patch: { capacity: 1024 }, baseRevision: vfxStateEnvelope.result.revision,
} });
const vfxPlanEnvelope = JSON.parse(textContent(vfxPlanResult, "vfx.graph.patch.plan")) as { result: object };
const vfxDryRun = await client.callTool({ name: "vfx.graph.patch.apply", arguments: { plan: vfxPlanEnvelope.result, dryRun: true } });
const vfxApply = await client.callTool({ name: "vfx.graph.patch.apply", arguments: { plan: vfxPlanEnvelope.result, dryRun: false } });
const vfxApplyEnvelope = JSON.parse(textContent(vfxApply, "vfx.graph.patch.apply")) as { result: { revisionAfter: number } };
const vfxUndo = await client.callTool({ name: "vfx.graph.undo", arguments: { expectedRevision: vfxApplyEnvelope.result.revisionAfter } });
if (vfxPlanResult.isError || !textContent(vfxPlanResult, "vfx.graph.patch.plan").includes('"changedPaths":["/capacity"]') ||
    vfxDryRun.isError || !textContent(vfxDryRun, "vfx.graph.patch.apply").includes("vfx.plan.valid") ||
    vfxApply.isError || vfxUndo.isError || !textContent(vfxUndo, "vfx.graph.undo").includes('"operation":"vfx.graph.undo"')) {
  throw new Error(`Revision-bound VFX Graph patch/dry-run/apply/rollback failed through MCP: ${[
    textContent(vfxPlanResult, "vfx.graph.patch.plan"), textContent(vfxDryRun, "vfx.graph.patch.apply"),
    textContent(vfxApply, "vfx.graph.patch.apply"), textContent(vfxUndo, "vfx.graph.undo"),
  ].join(" | ")}`);
}

const injectResult = await client.callTool({ name: "input.source.inject", arguments: { source: "keyboard.space", value: 1 } });
const gameplayTick = await client.callTool({ name: "run.headless", arguments: { frames: 1 } });
const gameplayAfterInput = await client.callTool({ name: "gameplay.events.observe", arguments: { maxEvents: 8 } });
const vfxAfterInput = await client.callTool({ name: "vfx.observe", arguments: { maxParticles: 2 } });
const abilityGrant = await client.callTool({ name: "gameplay.ability.grant", arguments: {
  entityId: "entity.demo-cube", abilityId: "ability.combat.impact",
} });
const abilityActivate = await client.callTool({ name: "gameplay.ability.activate", arguments: {
  entityId: "entity.demo-cube", abilityId: "ability.combat.impact", targetId: "entity.demo-sphere",
} });
const abilityObserve = await client.callTool({ name: "gameplay.ability.observe", arguments: { entityId: "entity.demo-cube" } });
const effectCatalog = await client.callTool({ name: "gameplay.effect.catalog", arguments: {} });
const effectApply = await client.callTool({ name: "gameplay.effect.apply", arguments: {
  sourceEntityId: "entity.demo-cube", targetEntityId: "entity.demo-sphere", effectId: "effect.recovery.minor",
} });
const rayAbilityGrant = await client.callTool({ name: "gameplay.ability.grant", arguments: {
  entityId: "entity.demo-sphere", abilityId: "ability.combat.impact",
} });
const rayAbilityActivate = await client.callTool({ name: "gameplay.ability.activate-ray", arguments: {
  entityId: "entity.demo-sphere", abilityId: "ability.combat.impact",
  origin: { x: 0, y: 3, z: 0 }, direction: { x: 0, y: -5, z: 0 },
} });
const audioBusResult = await client.callTool({ name: "audio.bus.set", arguments: { busId: "audio.sfx", gain: 0.75, muted: false } });
const audioPlayResult = await client.callTool({ name: "audio.voice.play", arguments: { assetId: "asset.audio.smoke", busId: "audio.sfx" } });
const audioListenerResult = await client.callTool({ name: "audio.listener.set", arguments: {
  position: { x: 0, y: 1.7, z: 0 }, forward: { x: 0, y: 0, z: -1 }, up: { x: 0, y: 1, z: 0 },
} });
const audioSpatialResult = await client.callTool({ name: "audio.voice.spatial.set", arguments: {
  voiceId: 1, position: { x: 4, y: 1.7, z: 0 }, minimumDistance: 1, maximumDistance: 50, rolloff: 1,
} });
const missingAudioClip = await client.callTool({ name: "audio.clip.load", arguments: { assetId: "asset.audio.missing" } });
if (injectResult.isError || gameplayTick.isError || gameplayAfterInput.isError ||
    !textContent(gameplayAfterInput, "gameplay.events.observe").includes("input.action.pressed") ||
    vfxAfterInput.isError || !textContent(vfxAfterInput, "vfx.observe").includes('"aliveCount":48') ||
    abilityGrant.isError || !textContent(abilityGrant, "gameplay.ability.grant").includes('"success":true') ||
    abilityActivate.isError || !textContent(abilityActivate, "gameplay.ability.activate").includes('"eventType":"combat.hit"') ||
    abilityObserve.isError || !textContent(abilityObserve, "gameplay.ability.observe").includes("ability.combat.impact") ||
    effectCatalog.isError || !textContent(effectCatalog, "gameplay.effect.catalog").includes("effect.damage.impact") ||
    effectApply.isError || !textContent(effectApply, "gameplay.effect.apply").includes('"after":100') ||
    rayAbilityGrant.isError || rayAbilityActivate.isError ||
    !textContent(rayAbilityActivate, "gameplay.ability.activate-ray").includes("entity.demo-cube") ||
    audioBusResult.isError || !textContent(audioBusResult, "audio.bus.set").includes('"success":true') ||
    audioPlayResult.isError || !textContent(audioPlayResult, "audio.voice.play").includes('"success":true') ||
    audioListenerResult.isError || !textContent(audioListenerResult, "audio.listener.set").includes('"success":true') ||
    audioSpatialResult.isError || !textContent(audioSpatialResult, "audio.voice.spatial.set").includes('"success":true') ||
    missingAudioClip.isError || !textContent(missingAudioClip, "audio.clip.load").includes("audio.asset-unavailable")) {
  throw new Error(`Persistent MCP gameplay mutation path did not produce action receipts and events: ${[
    textContent(vfxAfterInput, "vfx.observe"),
    textContent(abilityActivate, "gameplay.ability.activate"),
    textContent(effectApply, "gameplay.effect.apply"),
    textContent(rayAbilityActivate, "gameplay.ability.activate-ray"),
  ].join(" | ")}`);
}

const spawnResult = await client.callTool({ name: "gameplay.prefab.spawn", arguments: {
  sourceEntityId: "entity.demo-cube", newEntityId: "entity.mcp-spawn", displayName: "MCP Spawn", position: { x: 4, y: 2, z: 1 },
} });
const saveResult = await client.callTool({ name: "gameplay.save.capture", arguments: {} });
const saveEnvelope = JSON.parse(textContent(saveResult, "gameplay.save.capture")) as { result: { document: object } };
const despawnResult = await client.callTool({ name: "gameplay.entity.despawn", arguments: { entityId: "entity.mcp-spawn" } });
const restoreResult = await client.callTool({ name: "gameplay.save.restore", arguments: { document: saveEnvelope.result.document } });
const replayStart = await client.callTool({ name: "gameplay.replay.start", arguments: {} });
await client.callTool({ name: "input.source.inject", arguments: { source: "keyboard.e", value: 1 } });
await client.callTool({ name: "input.source.inject", arguments: { source: "keyboard.e", value: 0 } });
const replayStop = await client.callTool({ name: "gameplay.replay.stop", arguments: {} });
const replayEnvelope = JSON.parse(textContent(replayStop, "gameplay.replay.stop")) as { result: object };
const replayApply = await client.callTool({ name: "gameplay.replay.apply", arguments: { replay: replayEnvelope.result } });
if (spawnResult.isError || saveResult.isError || despawnResult.isError || restoreResult.isError || replayStart.isError || replayStop.isError || replayApply.isError ||
    !textContent(spawnResult, "gameplay.prefab.spawn").includes('"success":true') ||
    !textContent(restoreResult, "gameplay.save.restore").includes('"success":true') ||
    !textContent(replayApply, "gameplay.replay.apply").includes('"appliedSamples":2')) {
  throw new Error("Persistent MCP spawn, save/restore, and replay lifecycle failed");
}

const prefabExport = await client.callTool({ name: "gameplay.prefab.export", arguments: { entityId: "entity.demo-sphere" } });
const prefabEnvelope = JSON.parse(textContent(prefabExport, "gameplay.prefab.export")) as { result: object };
const prefabInstantiate = await client.callTool({ name: "gameplay.prefab.instantiate", arguments: {
  prefab: prefabEnvelope.result, newEntityId: "entity.mcp-prefab", displayName: "MCP Prefab", position: { x: -3, y: 2, z: 0 },
} });
const scriptAbi = await client.callTool({ name: "scripting.abi.describe", arguments: {} });
const scriptAttach = await client.callTool({ name: "scripting.instance.attach", arguments: {
  instanceId: "script.mcp", entityId: "entity.mcp-prefab", assemblyAsset: "asset.script.smoke", typeName: "Smoke.Controller",
} });
const scriptInvoke = await client.callTool({ name: "scripting.lifecycle.invoke", arguments: {
  instanceId: "script.mcp", callback: "OnCreate", arguments: { smoke: true },
} });
if(prefabExport.isError||prefabInstantiate.isError||scriptAbi.isError||scriptAttach.isError||scriptInvoke.isError||
    !textContent(prefabInstantiate,"gameplay.prefab.instantiate").includes('"success":true')||
    !textContent(scriptAbi,"scripting.abi.describe").includes('"requiredMajor":10')||
    !textContent(scriptAbi,"scripting.abi.describe").includes('"diagnostic"')||
    !textContent(scriptInvoke,"scripting.lifecycle.invoke").includes('"executedManagedCode":true'))
  throw new Error(`Prefab document or managed scripting ABI lifecycle failed: ${[
    textContent(prefabExport,"gameplay.prefab.export"),textContent(prefabInstantiate,"gameplay.prefab.instantiate"),
    textContent(scriptAbi,"scripting.abi.describe"),textContent(scriptAttach,"scripting.instance.attach"),
    textContent(scriptInvoke,"scripting.lifecycle.invoke"),
  ].join(" | ")}`);

const skeletonResult = await client.callTool({
  name: "animation.skeleton.inspect",
  arguments: { entityId: "entity.demo-skeletal-cube", maxJoints: 1 },
});
const skeletonEnvelope = JSON.parse(textContent(skeletonResult, "animation.skeleton.inspect")) as {
  result: { valid: boolean; returnedJointCount: number; truncated: boolean };
};
if (skeletonResult.isError || !skeletonEnvelope.result.valid ||
    skeletonEnvelope.result.returnedJointCount !== 1 || !skeletonEnvelope.result.truncated) {
  throw new Error("Bounded animation skeleton evidence is unavailable through MCP");
}

const inspectorResult = await client.callTool({
  name: "editor.inspector.describe",
  arguments: { entityId: "entity.demo-cube" },
});
if (inspectorResult.isError ||
    textContent(inspectorResult, "editor.inspector.describe").includes("engine.entity.material.roughness") === false ||
    textContent(inspectorResult, "editor.inspector.describe").includes("world.property.plan") === false) {
  throw new Error("Declarative Inspector document is unavailable through MCP");
}

const semanticUiResult = await client.callTool({
  name: "ui.observe",
  arguments: { entityId: "entity.demo-cube", roles: ["property"], depth: 0, includeValues: false },
});
const semanticUiDelta = await client.callTool({
  name: "ui.delta",
  arguments: { entityId: "entity.demo-cube", sinceRevision: 999, includeValues: false },
});
const uiResources = await client.callTool({ name: "ui.resources.inspect", arguments: { locale: "zh-CN" } });
const uiText = await client.callTool({
  name: "ui.text.inspect",
  arguments: { locale: "ar-SA", text: "\u0627\u0644\u0633\u0644\u0627\u0645", fontSize: 20 },
});
if (semanticUiResult.isError ||
    textContent(semanticUiResult, "ui.observe").includes("noemancer.ui-observation/0.1") === false ||
    textContent(semanticUiResult, "ui.observe").includes("engine.entity.material.roughness") === false ||
    textContent(semanticUiResult, "ui.observe").includes("world.property.plan") === false ||
    semanticUiDelta.isError ||
    textContent(semanticUiDelta, "ui.delta").includes("noemancer.ui-delta/0.1") === false ||
    textContent(semanticUiDelta, "ui.delta").includes("ui.resync-required") === false ||
    uiResources.isError || textContent(uiResources, "ui.resources.inspect").includes('"requestedMessagesLoaded":true') === false ||
    uiText.isError || textContent(uiText, "ui.text.inspect").includes('"requiredScript":"Arabic"') === false ||
    textContent(uiText, "ui.text.inspect").includes('"committedUtf8":true') === false ||
    textContent(uiText, "ui.text.inspect").includes('"harfBuzz":true') === false ||
    textContent(uiText, "ui.text.inspect").includes('"baseDirection":"rtl"') === false) {
  throw new Error("Generic bounded Semantic UI observation is unavailable through MCP");
}

const retainedUiResult = await client.callTool({
  name: "ui.retained.preview",
  arguments: { entityId: "entity.demo-cube", width: 800, height: 600, densityScale: 1.25 },
});
if (retainedUiResult.isError ||
    textContent(retainedUiResult, "ui.retained.preview").includes("noemancer.retained-ui-preview/0.1") === false ||
    textContent(retainedUiResult, "ui.retained.preview").includes("RmlUi") === false ||
    textContent(retainedUiResult, "ui.retained.preview").includes("engine.entity.material.roughness") === false) {
  throw new Error("RmlUi retained layout preview is unavailable through MCP");
}

const engineStatus = await client.callTool({
  name: "engine.status",
  arguments: {},
});
if (
  engineStatus.isError ||
  JSON.stringify(engineStatus.content).includes("render.graph") === false
) {
  throw new Error("engine.status did not expose the module graph");
}

const renderGraph = await client.callTool({ name: "render.graph.inspect", arguments: {} });
if (
  renderGraph.isError ||
  textContent(renderGraph, "render.graph.inspect").includes("render.pass.shadow-depth") === false ||
  textContent(renderGraph, "render.graph.inspect").includes("render.resource.object-id") === false ||
  textContent(renderGraph, "render.graph.inspect").includes("render.resource.world-normal") === false ||
  textContent(renderGraph, "render.graph.inspect").includes("render.resource.motion-vectors") === false ||
  textContent(renderGraph, "render.graph.inspect").includes("render.resource.temporal-history") === false ||
  textContent(renderGraph, "render.graph.inspect").includes("render.resource.temporal-depth-history") === false ||
  textContent(renderGraph, "render.graph.inspect").includes("render.resource.reactive-mask") === false ||
  textContent(renderGraph, "render.graph.inspect").includes("render.pipeline.aces-tone-map") === false ||
  textContent(renderGraph, "render.graph.inspect").includes("render.pipeline.taa") === false ||
  textContent(renderGraph, "render.graph.inspect").includes('"valid":true') === false
) {
  throw new Error("render.graph.inspect did not expose a valid deterministic graph");
}

const renderObservation = await client.callTool({ name: "render.observe", arguments: {} });
if (
  renderObservation.isError ||
  textContent(renderObservation, "render.observe").includes("entity.camera.editor") === false ||
  textContent(renderObservation, "render.observe").includes("entity.sun") === false ||
  textContent(renderObservation, "render.observe").includes("asset.texture.checker") === false
) {
  throw new Error("render.observe did not expose the ECS-derived render scene");
}

const assetRegistry = await client.callTool({ name: "asset.registry", arguments: {} });
const assetEnvelope = JSON.parse(textContent(assetRegistry, "asset.registry")) as {
  result: { assetCount: number; errorCount: number };
};
if (assetEnvelope.result.assetCount < 37 || assetEnvelope.result.errorCount !== 0) {
  throw new Error("asset.registry did not expose the canonical project assets");
}
const spawnShader = await client.callTool({
  name: "asset.inspect",
  arguments: { assetId: "asset.shader.vfx-spawn-compute" },
});
if (!textContent(spawnShader, "asset.inspect").includes("vfx_spawn.comp.hlsl")) {
  throw new Error("asset.inspect did not expose the GPU VFX spawn kernel");
}

const cookPlan = await client.callTool({
  name: "asset.cook.plan",
  arguments: {
    assetIds: ["asset.test.kenney.alien"],
    targetProfile: "windows-x64-debug",
  },
});
if (cookPlan.isError || textContent(cookPlan, "asset.cook.plan").includes("cache://sha256/") === false) {
  throw new Error("asset.cook.plan did not expose content-addressed cache intent");
}
const cookPlanEnvelope = JSON.parse(textContent(cookPlan, "asset.cook.plan")) as {
  result: Record<string, unknown>;
};
const cookDryRun = await client.callTool({
  name: "asset.cook.apply",
  arguments: { plan: cookPlanEnvelope.result, dryRun: true },
});
if (cookDryRun.isError || textContent(cookDryRun, "asset.cook.apply").includes("asset.cook-plan-validated") === false) {
  throw new Error("asset.cook.apply did not validate the immutable plan");
}

const assetInspection = await client.callTool({
  name: "asset.inspect",
  arguments: { assetId: "asset.test.kenney.alien" },
});
if (assetInspection.isError || textContent(assetInspection, "asset.inspect").includes('"format":"glb"') === false) {
  throw new Error("asset.inspect did not expose parsed GLB metadata");
}

const result = await client.callTool({
  name: "schema.get",
  arguments: {},
});
if (result.isError || JSON.stringify(result.content).includes("receipt") === false) {
  throw new Error("schema.get did not return the Agent ABI envelope");
}

const sceneValidation = await client.callTool({
  name: "scene.validate",
  arguments: {
    document: {
      schema: "noemancer.scene/0.1",
      sceneGuid: "scene.mcp-smoke",
      name: "MCP Smoke Scene",
      entities: [],
    },
  },
});
if (
  sceneValidation.isError ||
  JSON.stringify(sceneValidation.content).includes('\\"valid\\":true') === false
) {
  throw new Error("scene.validate did not validate a plain-data scene document");
}

const conventions = await client.callTool({
  name: "semantic.conventions",
  arguments: {},
});
if (
  conventions.isError ||
  JSON.stringify(conventions.content).includes("noemancer.semantic-conventions.core") === false
) {
  throw new Error("semantic.conventions did not preserve the structured registry");
}

const headless = await client.callTool({
  name: "run.headless",
  arguments: {},
});
if (headless.isError || JSON.stringify(headless.content).includes('\\"frames\\":3') === false) {
  throw new Error("run.headless did not apply the manifest default");
}

const snapshotResult = await client.callTool({ name: "world.snapshot", arguments: {} });
const snapshotEnvelope = JSON.parse(textContent(snapshotResult, "world.snapshot")) as {
  result: { revision: number };
};

const planResult = await client.callTool({
  name: "world.transform.plan",
  arguments: {
    entityId: "entity.demo-cube",
    baseRevision: snapshotEnvelope.result.revision,
    manager: "mcp.smoke",
    position: { x: 3, y: 4, z: 5 },
  },
});
const planEnvelope = JSON.parse(textContent(planResult, "world.transform.plan")) as {
  result: Record<string, unknown>;
};

const applyResult = await client.callTool({
  name: "world.change.apply",
  arguments: { plan: planEnvelope.result, dryRun: false },
});
const applyEnvelope = JSON.parse(textContent(applyResult, "world.change.apply")) as {
  result: { success: boolean; revisionAfter: number };
  receipt: { changedObjects: unknown[] };
};
if (!applyEnvelope.result.success || applyEnvelope.receipt.changedObjects.length !== 1) {
  throw new Error("Persistent MCP session did not apply a revision-bound change");
}

const undoResult = await client.callTool({
  name: "world.undo",
  arguments: { expectedRevision: applyEnvelope.result.revisionAfter, manager: "mcp.smoke" },
});
const undoEnvelope = JSON.parse(textContent(undoResult, "world.undo")) as {
  result: { success: boolean };
};
if (undoResult.isError || !undoEnvelope.result.success) {
  throw new Error("Persistent MCP session did not preserve undo history");
}

await client.close();
process.stdout.write(`MCP smoke passed: ${names.join(", ")}\n`);
