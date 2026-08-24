# ADR 0001: bootstrap stack

## Status

Accepted on 2026-08-18.

## Decision

Use C++20, SDL 3.4.14 with SDL_GPU, Flecs 4.1.6, CMake, and Visual Studio 2022 for the first executable slice.

Keep the engine control protocol independent from MCP. A TypeScript sidecar will translate versioned engine JSON-RPC methods into MCP resources and tools.

## Consequences

- The first renderer targets SDL_GPU's portable feature set rather than ray tracing or mesh shaders.
- Flecs provides runtime reflection and world queries instead of building a custom ECS.
- Dependencies are pinned with CMake FetchContent until a project-local package-manager workflow is added.

