# ADR 0009: GPU Pass Timestamp Adapter

- Status: implemented and dual-backend verified
- Date: 2026-08-25
- Scope: runtime diagnostics and performance evidence

## Decision

Noemancer will keep SDL_GPU 3.4.14 as the window/device/render portability layer, but maintain a small, deterministic patch that adds timestamp-query pools to its existing backend function table. The patch exposes opaque SDL-owned pools rather than native D3D12/Vulkan handles. `SceneRenderer` sees only a runtime-private `GpuPassTimestampAdapter`; Render Graph, Scene, C#, Semantic State Plane and public RPC continue to use stable plain data.

The first implementation profiles the graphics queue only. Each frame-in-flight owns one query pool and one submission fence. Every Render Graph pass receives a begin/end timestamp pair at its existing debug-group boundary; frame end resolves the used query range once. A slot is read only after its fence signals. Capacity is checked before recording and reports `overflowed` plus `droppedPasses`. Unsupported, pending, invalid-order and unavailable results remain `null`; a numeric zero is never synthesized as a failure value. Presentation timing remains a separate PresentMon source.

## Reference decisions

Both fixed reference repositories are MIT licensed.

### Godot

- Repository: `D:/3D/_tools/_reference/_game-engine/godot`
- Commit: `3000096f9aa6f46db98d3a6d2a9442d58cab96ac`
- High-level ring: `servers/rendering/rendering_device.h:1835-1850`, `servers/rendering/rendering_device.cpp:8071-8121`, `8240-8244`, `8497-8504`, `8722-8734`.
- Render Graph marker command: `servers/rendering/rendering_device_graph.h:955`, `servers/rendering/rendering_device_graph.cpp:384`, `1243-1246`, `2584-2590`.
- D3D12 pool/readback: `drivers/d3d12/rendering_device_driver_d3d12.cpp:5576-5638`.
- Vulkan pool/conversion: `drivers/vulkan/rendering_device_driver_vulkan.cpp:6780-6838`.

**Adopt** per-frame pool ownership, fence-gated slot reuse and result frame identity. **Port** the Vulkan fixed-point conversion principle where the backend needs it. **Adapt** marker recording to stable Noemancer Render Graph pass IDs and structured availability/overflow. **Reject** out-of-range results returned as zero, ignored Vulkan result codes and per-marker D3D12 resolve.

### Wicked Engine

- Repository: `D:/3D/_tools/_reference/_game-engine/WickedEngine`
- Commit: `f4a0d2635d5224b4509da164fa75d90fbdaaea26`
- RHI contract: `WickedEngine/wiGraphics.h:301,665,1157`, `WickedEngine/wiGraphicsDevice.h:76,97,156,244-247`.
- Frame ring/batch resolve: `WickedEngine/wiProfiler.cpp:34-36`, `84-99`, `108-190`, `225-279`.
- D3D12: `WickedEngine/wiGraphicsDevice_DX12.cpp:2978`, `4037`, `7054-7084`.
- Vulkan: `WickedEngine/wiGraphicsDevice_Vulkan.cpp:2388`, `2914`, `4813`, `8257-8304`.

**Adopt** frame-end batch resolve. **Port** D3D12 timestamp `EndQuery`/`ResolveQueryData` and Vulkan timestamp write concepts inside the SDL backend patch. **Adapt** the profiler into a bounded plain-data Adapter. **Reject** unbounded atomic query allocation, `abs(end-begin)`, Vulkan `WAIT_BIT`, capability assertions and conversion of invalid/large values to zero.

## Why a pinned SDL patch

SDL 3.4.14 exposes neither timestamp queries nor native GPU device/queue/command-buffer handles. Its backend structs live in private `.c` files, so casting opaque public pointers to copied private layouts would be ABI-undefined and fragile. The local patch leaves native ownership where it already exists, adds explicit unsupported hooks for other backends, is version-pinned and is reapplied deterministically during dependency population. This is narrower and safer than building a second native renderer beside SDL_GPU.

## Consequences and limits

- Performance evidence mode acquires a nonblocking fence for each profiled frame; ordinary product frames keep the existing fence-free submit path.
- Timestamp duration is queue execution time, not input latency, display latency or full-frame present time.
- Retained UI, ImGui, uploads and capture are not silently included in the Scene Render Graph pass sum. They require distinct markers if later promoted to GPU evidence.
- Async compute/copy must use separate pools, timestamp periods and queue domains; cross-queue subtraction is forbidden until an explicit calibration contract exists.
- Updating SDL requires rebasing the patch, rerunning both backend probes and reviewing upstream timestamp APIs before retaining the fork.

## Verification

- D3D12: `generated/acceptance/gpu-pass-timestamp-d3d12-20260825-121931/` captured all 60 sampled frames with numeric Render Graph pass durations, zero skipped pending slots and a 1 ns timestamp period.
- Vulkan: `generated/acceptance/gpu-pass-timestamp-vulkan-20260825-122002/` captured all 60 sampled frames with the same bounded contract and zero skipped pending slots.
- The first clean migrated build exposed and fixed two patch defects rather than hiding them: the D3D12 query-heap IID and non-debug command-buffer validation policy.
