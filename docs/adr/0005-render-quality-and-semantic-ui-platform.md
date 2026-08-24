# ADR 0005: render quality contract and semantic UI platform

## Status

Accepted baseline on 2026-08-21. The engine-owned Render Graph/quality contract, Semantic UI Core, RmlUi adapter, HarfBuzz/ICU text boundary and separate Editor/Game component libraries are current architecture. Slang, native graphics backends and advanced rendering tiers remain evidence-gated follow-ups.

## Context

Noemancer needs a visually competitive renderer and a distinctive, styleable UI without turning either system into an opaque editor-only implementation. Editor UI and game UI have different component sets, but both need declarative documents, themes, localization, accessibility, deterministic tests, and stable semantic identities that Agents can query and edit.

SDL_GPU and Dear ImGui made the first executable editor slice small and portable. They do not by themselves provide a production renderer, a retained UI document model, high-end GPU features, or international text layout.

## Decision

### Rendering

- Treat SDL_GPU as the portable bootstrap and compatibility backend, not as the ceiling of the renderer.
- Put an engine-owned RHI and Render Graph above all graphics backends. Persist only engine resource/pass IDs; never SDL, D3D12, Vulkan, or Metal handles.
- Build the renderer in quality tiers:
  1. reference-quality raster baseline: linear HDR, color management, physically based material/light/camera units, IBL, clustered lighting, stable shadows, anti-aliasing, exposure, tone mapping, and post-processing;
  2. scalable production path: GPU-driven culling, indirect submission, texture streaming, temporal upscaling, virtualized resources, and platform-specific optimization;
  3. optional advanced path: native D3D12/Vulkan features, hardware ray tracing, mesh shaders, and experimental GI only when a profile needs them.
- Use Slang as the shader-language candidate and require reflection, source maps, cache keys, diagnostics, and rollback around shader compilation.
- Define visual quality with versioned reference scenes, fixed cameras/exposure, golden images, artifact metrics, performance budgets, and captured render evidence. Feature names such as PBR, GI, or ray tracing are not quality evidence.
- Use Filament's documented PBR model and selected Wicked Engine scenes/features as comparison oracles. Do not embed a complete reference renderer before the engine-owned render/evidence boundaries exist.

### UI

- Keep Dear ImGui as the bootstrap editor shell and low-level diagnostics UI. It is not the long-term application/game UI document format.
- Build one retained **Semantic UI Core** shared by editor and game runtime: node tree, stable IDs, component properties, state, events/actions, data binding, focus/navigation, layout, animation timeline, accessibility, and localization keys.
- Adopt the useful ideas behind React and shadcn rather than embedding a browser by default:
  - declarative component trees;
  - open, local component source;
  - composable headless behavior;
  - design tokens and stylesheet variables;
  - predictable schemas that humans and Agents can edit;
  - strong visual defaults with full project ownership.
- Use different component libraries over the same core:
  - `Editor Components`: docking, tree/table, inspector, graph, timeline, viewport overlay, profiler;
  - `Game Components`: HUD, menu, dialogue, inventory, subtitles, controller navigation and safe zones.
- Evaluate RmlUi as the first retained document/style implementation spike. It already provides a C++ DOM, HTML/CSS-like documents, Flexbox, animation, data binding, localization hooks, SDL3 integration, and an SDL_GPU backend. Noemancer should own the Render Graph backend and semantic bridge rather than expose RmlUi objects in public schemas.
- Do not ship Chromium/CEF as the default game UI. A WebView may remain an optional editor extension host after memory, startup, sandboxing, input, accessibility, and packaging costs are measured.

### Internationalization

- Store user-visible text as message keys plus typed arguments, never as concatenated fragments.
- Use HarfBuzz for shaping and ICU (or a measured equivalent) for locale rules, BiDi, segmentation, plural/select formatting, numbers, dates, and fallback.
- Make pseudo-localization, text expansion, RTL mirroring, font fallback, missing-glyph detection, IME, controller navigation, and locale-specific assets automated UI tests.
- Every semantic UI node exposes its resolved locale, message key, bounds, overflow/clipping state, font/fallback chain, and source location to the observation graph.

## Consequences

- Editor chrome and game UI can reuse the hard parts without pretending they are the same product surface.
- CSS-like styling becomes data that can be diffed, themed, hot-reloaded, tested, and safely patched by an Agent.
- RmlUi is a candidate implementation detail, not the engine's public UI identity.
- The current ImGui editor remains useful while the retained UI core matures panel by panel.
- SDL_GPU can deliver the first high-quality raster renderer, but advanced feature parity requires native backend spikes; this cost is explicit rather than discovered late.

## Required spikes before acceptance

1. RmlUi document, stylesheet, data-binding, Chinese/Arabic text, DPI, and SDL_GPU Render Graph integration.
2. Semantic UI node mirroring from both ImGui bootstrap panels and retained documents.
3. HarfBuzz + ICU shaping, BiDi, line breaking, fallback and pseudo-locale screenshot tests.
4. Renderer quality scene with HDR/PBR/IBL/shadows/tone mapping and deterministic image comparison.
5. SDL_GPU audit followed by a minimal native D3D12 backend experiment for bindless, mesh shader, ray tracing, crash diagnostics, and resource residency.
