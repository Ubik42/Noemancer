# ADR 0002: language and performance stack

## Status

Accepted baseline on 2026-08-21. C++20, C#/.NET 10, TypeScript control-plane, Flecs/Jolt/ozz and middleware-first boundaries are implemented. Future middleware additions and every performance-superiority claim remain benchmark-gated.

## Context

The engine needs fast native execution, short Agent iteration loops, structured editing of animation, physics and rendering, and a deployment path that does not make the authoring language the runtime bottleneck.

## Decision

- Keep the runtime kernel in C++20. Adopt C++23 features individually after compiler and build-time measurements.
- Use C# on .NET 10 as the first gameplay and developer scripting candidate. Host CoreCLR in development; treat NativeAOT as an optional shipping path, not the hot-reload path.
- Keep TypeScript in the tool and Agent control plane. Do not put TypeScript in per-frame engine hot loops.
- Generate C# bindings and tool contracts from the same versioned Engine Schema used by C++ reflection and the Agent ABI.
- Keep authoritative gameplay state in versioned ECS components so managed assemblies can be replaced without treating a managed object graph as engine state.
- Evaluate Jolt Physics for 3D physics, ozz-animation plus ACL for skeletal animation, and Slang 2026 for shaders.
- Keep SDL_GPU for the bootstrap renderer while introducing an engine-owned RHI and Render Graph. Add native D3D12/Vulkan backends only when a measured feature or diagnostic limit requires them.
- Keep Flecs behind the Engine World API until representative benchmarks justify replacing it.

## Consequences

- The engine has two development compilation loops: C++ for native kernels and C# for gameplay. A gameplay edit should not rebuild the engine.
- Managed/native calls must be batched; fine-grained per-entity P/Invoke is not an accepted gameplay API.
- Script hot replacement needs explicit safe points, state migration, validation and rollback.
- Physics, animation, Render Graph and Shader systems need versioned intermediate representations rather than editor-only object graphs.
- Using established middleware is not a project differentiator. The differentiator must be the Agent transaction and evidence model around those systems.
- No performance superiority claim is valid until the benchmark suite fixes workload, quality, hardware, compiler, driver and revision.

## Required spikes before acceptance

1. CoreCLR hosting, generated bindings, exception mapping, assembly replacement and state migration.
2. C# batch ECS throughput and GC behavior versus an equivalent native System.
3. Jolt deterministic replay and structured solver/contact traces.
4. Slang multi-target compilation, reflection, cache invalidation and pipeline rollback.
5. ozz plus ACL sampling, decompression and pose-error reporting.
6. SDL_GPU feature and diagnostic audit for mesh shaders, bindless resources, barriers, residency and GPU crash tooling.

The supporting research is in [2026 性能核心与 Agent 全面编辑技术选型](../research/2026-performance-and-agent-editable-stack.zh-CN.md).
