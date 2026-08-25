# ADR 0008: libjpeg-turbo image adapter and external glTF source closure

## Status

Accepted on 2026-08-25.

## Context

Production JSON glTF commonly references external buffers and PNG/JPEG images. Treating only the root `.gltf` file as the source identity makes cache invalidation incorrect and lets a parser reread mutable authoring files after planning. Reimplementing JPEG entropy decode would add security, performance and maintenance risk without differentiating Noemancer.

## Decision

- **Adopt** official libjpeg-turbo 3.2.0 from its release archive, pinned by SHA-256 `6f30092cef9fb839779646608f4ee14ae3cbac989c47fa05e841b0841f09878e`.
- Keep TurboJPEG types private. The public `DecodedImage`/`decode_image_rgba8` adapter owns bounded RGBA8 output and stable errors for PNG/JPEG, thumbnails, standalone KTX2 Texture Cook and glTF images.
- **Adapt** fastgltf file import behind an engine-owned source closure: normalize relative URIs, reject URL/absolute/traversal/symlink escape, bound document/dependency/count/total bytes, hash every input, and materialize only the immutable snapshot into isolated staging.
- Cook recipe/cache identity includes the source-closure fingerprint. Apply regenerates the plan and captures the closure again before decode, so an external dependency mutation rejects a stale plan.
- Package libjpeg-turbo's official license text under the custom identity `LicenseRef-libjpeg-turbo-composite`, covering its BSD-3-Clause, IJG and Zlib notices.

## Consequences

JSON glTF, GLB and FBX converge on `noemancer/meshbin/0.2`; packaged Players never ship or decode source geometry. The current Windows host has no NASM, so libjpeg-turbo is functionally active but its x86 assembly SIMD backend is explicitly unavailable on that machine. A future toolchain capability check may enable it; no performance claim follows from adoption alone.

## Evidence

`noemancer.gltf_animation`, `noemancer.gltf_source_snapshot_pressure`, `noemancer.gltf_external_cook`, `noemancer.image_decoder`, `noemancer.asset_thumbnail` and `noemancer.asset_registry` cover real JPEG decode, 128 dependencies, budgets, mutation detection, Cook/cache invalidation and stable adapters. `generated/acceptance/libjpeg-package-closure/` is a real Release RenderLab package containing `THIRD_PARTY.json`, `NOTICE.txt` and `licenses/runtime/libjpeg-turbo-LICENSE.md`.
