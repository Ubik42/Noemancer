# Native RHI / Ray Tracing Foundation Adoption Record — Historical
> Historical: 本文是 2026-08-25 的研究、许可证核对与采用决策记录，不是当前架构或路线图的权威替代品。
> Historical: 结论只对下方固定 commit、路径和许可证快照负责；上游更新后必须重新核对。
> Historical: 本记录没有把任何第三方源文件复制进 Noemancer，也不声称 Noemancer 已经具备生产级硬件光追。
> Historical: 任何未来实现都必须以真实 D3D12/Vulkan GPU 证据、fallback 和许可证审计为退出条件。

> 调研范围：D3D12 DXR 与 Vulkan KHR acceleration structure / ray-tracing pipeline 的 BLAS、TLAS、scratch、compaction、barrier、SBT 和 dispatch 生产边界。
>
> 当前切片：`render.native-rhi-raytracing-foundation`。本记录服务于该切片，不授予修改 Runtime、Render Graph、Shader 或第三方代码的权限。

## 结论先行

Noemancer 应该把硬件光追做成一个**显式 Native RHI 能力层**，而不是把 SDL_GPU 的可移植句柄或私有布局强行解释成 DXR/Vulkan RT。SDL_GPU 继续负责启动、窗口、常规 raster 和跨后端基础资源；只有当设备能力、资源地址、加速结构、shader table、barrier 和 GPU 时间戳都能被原生后端证明时，才允许进入 RT path。

建议采用以下边界：

1. **Adopt API 语义，不复制 SDK 或引擎整体。** 以 Microsoft DXR / Khronos Vulkan 规范作为资源状态、地址、构建、SBT、dispatch 和同步的硬合同。
2. **Port/Adapt 有许可证兼容且可隔离的局部模式。** Wicked Engine、Godot、DiligentCore 和 Bevy Solari 都可作为算法/生命周期/验证的研究来源；第三方类型不得越过 Noemancer plain-data adapter。
3. **Reject 整体嵌入与不可复制来源。** Unreal Engine 仅作生产边界和 failure-mode oracle，受 EULA 约束，禁止复制代码、shader、命名布局或通过改名规避许可；Filament 的强项是 raster/PBR/FrameGraph，不是当前 RT foundation。
4. **最小纵切必须双后端、可回退、可验收。** 1 个三角形 BLAS + 1 个 TLAS instance + raygen/miss/closest-hit + 64/256 输出先证明真实 build、barrier、SBT、`TraceRays`，再加入 compaction/update，最后才让 RTGI 消费它。
5. **缺能力或证据就明确回退。** 设备不支持、shader artifact 缺失、验证层错误、预算超限或 readback 失败时回到 raster SSR/SSGI/CSM，并在状态中记录原因；不允许用黑图、零时间戳或“模拟 RT”报告成功。

当前状态：Noemancer 仍只有 SDL_GPU raster 基线和受限的原生时间戳/诊断适配；在完成下方纵切并取得 D3D12 + Vulkan 的真实证据之前，`native-rhi-raytracing-foundation` 应保持 `planned / not accepted`。

## 固定快照、路径与许可证

路径均是本机参考仓的绝对位置，代码引用使用相对路径；`commit` 是本记录的可复核输入，不代表上游最新版本。

| 参考 | 固定版本 | 许可证 | 本记录核查路径 / 证据 | 使用边界 |
|---|---|---|---|---|
| Wicked Engine | `f4a0d2635d5224b4509da164fa75d90fbdaaea26`；`D:\3D\_tools\_reference\_game-engine\WickedEngine` | `LICENSE.txt`：MIT，Copyright Turánszki János | `WickedEngine/wiGraphics.h:1187-1368`；`WickedEngine/wiGraphicsDevice_DX12.cpp:4305-4443,7296-7432`；`WickedEngine/wiGraphicsDevice_Vulkan.cpp:5273-5451,8512+` | 可研究；需要移植时保留 MIT notice，优先隔离算法/数据布局，不能引入 `wi::graphics` 类型。 |
| Godot | `3000096f9aa6f46db98d3a6d2a9442d58cab96ac`；`D:\3D\_tools\_reference\_game-engine\godot` | `LICENSE.txt`：MIT | `drivers/vulkan/rendering_device_driver_vulkan.h/.cpp`：`RaytracingCapabilities`、BLAS/TLAS、pipeline、SBT、`command_trace_rays`、extension/feature probe | 可研究或选择性 Port；保留 Godot notice；不得把 RID、RenderingDevice 私有对象或 Godot storage 作为 Noemancer 公共 ABI。此快照的 RT 实现证据集中在 Vulkan driver。 |
| DiligentCore | DiligentEngine `aca22851ae2b369b770d112bc27c63cb60ce963f`；其 submodule DiligentCore `bb821b78a49b0bf8b4665ece82cacbb2efc1ba5c` | `License.txt`：Apache-2.0 | `Graphics/GraphicsEngine/interface/BottomLevelAS.h`、`TopLevelAS.h`、`DeviceContext.h`、`ShaderBindingTable.h`；`Graphics/GraphicsEngineD3D12/src/BottomLevelASD3D12Impl.cpp`、`DeviceContextD3D12Impl.cpp`；`Graphics/GraphicsEngineVulkan/src/BottomLevelASVkImpl.cpp`、`DeviceContextVkImpl.cpp`；`Tests/DiligentCoreAPITest/src/Vulkan/RayTracingReferenceVk.cpp` | 最接近可借鉴的跨后端接口合同；可 Port/Adapt 局部验证和生命周期语义，保留 Apache notice，不嵌入 Diligent device/context。 |
| Bevy | `9f4ff89c1a6aa49efe0ade126ed67c948121a30b`；`D:\3D\_tools\_reference\_game-engine\bevy` | `LICENSE-MIT` + `LICENSE-APACHE`（双许可） | `crates/bevy_solari/src/scene/blas.rs`：scratch、BLAS、compaction；`scene/binder.rs`：TLAS、上一帧 TLAS、instance binding；`scene/bindings.wesl`；`pathtracer/`、`realtime/` | 可研究/Adapt staged compaction、previous-TLAS 和 temporal 生命周期；不引入 Bevy ECS、wgpu 句柄或 WESL 作为 Noemancer RHI。 |
| Filament | v1.55.0 tag `e4b1f0413b83aa8c3b4dda1da914c71323a16780`；`https://github.com/google/filament` | `LICENSE`：Apache-2.0 | tag 下 `filament/`、`libs/filament/` 的 raster/PBR/FrameGraph 资源；官方 [FrameGraph notes](https://google.github.io/filament/notes/framegraph.html) | Adapt resource lifetime、pass declaration、PBR 质量策略；Reject 为硬件 RT foundation，不能把其 raster FrameGraph 当作 BLAS/TLAS/SBT 实现。 |
| Unreal Engine | `71fe36aac5a8df5ccd66c763ffc902b29b6a9c43`；`D:\3D\_tools\_reference\_game-engine\UnrealEngine` | `LICENSE.md`：Unreal Engine EULA | `Engine/Source/Runtime/Renderer/Private/RayTracing/`、`Renderer/Private/PathTracing.cpp`、`D3D12RHI/`、`VulkanRHI/` | 仅研究生产分层、缓存、fallback 和故障模式；禁止复制源码、shader、命名布局或二进制。 |

官方规范是硬事实来源：

- [Microsoft DXR functional specification](https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html)；[`D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC`](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_build_raytracing_acceleration_structure_desc)。
- [Vulkan Ray Tracing specification](https://docs.vulkan.org/spec/latest/chapters/raytracing.html)、[acceleration structures](https://docs.vulkan.org/spec/latest/chapters/accelstructures.html) 和 [official guide](https://docs.vulkan.org/guide/latest/extensions/ray_tracing.html)。
- [Wicked Engine repository](https://github.com/turanszky/WickedEngine/tree/f4a0d2635d5224b4509da164fa75d90fbdaaea26)、[Godot repository](https://github.com/godotengine/godot/tree/3000096f9aa6f46db98d3a6d2a9442d58cab96ac)、[DiligentCore snapshot](https://github.com/DiligentGraphics/DiligentCore/tree/bb821b78a49b0bf8b4665ece82cacbb2efc1ba5c)、[Bevy snapshot](https://github.com/bevyengine/bevy/tree/9f4ff89c1a6aa49efe0ade126ed67c948121a30b)、[Filament tag](https://github.com/google/filament/tree/e4b1f0413b83aa8c3b4dda1da914c71323a16780)。

## 硬件 RT 的共同生产边界

### BLAS / TLAS 与 scratch

两套 API 的实际生命周期都不是“给 mesh 一个 ray-tracing flag”这么简单：

1. 先从几何/实例描述和设备属性查询 prebuild size；结果至少包含结果 AS 大小、build scratch 大小，通常还包括 update scratch 大小和对齐要求。
2. BLAS 输入三角形/AABB 的 vertex/index/transform buffer 必须有设备可访问地址；TLAS 输入 instance buffer 必须编码 transform、mask、custom index 和 SBT record offset 等字段。
3. 为结果 AS 和 scratch 分别分配生命周期可证明的 GPU 资源。scratch 不能与仍在执行的 build/update 重叠使用；结果 AS 在 build 后才可被 RT shader 读取。
4. 允许 update/refit 时，初始 build flags、几何数量/拓扑与后续 update 约束必须持久化；不可把“任意 mesh 改动”误报成 refit。
5. 每一帧 TLAS 变换更新、对象删除、材质 hit-group 变化和相机历史都要有 revision/retirement 规则。跨帧资源不能在 GPU 尚未完成时回收。

Wicked 的 DX12 路径在 `CreateRaytracingAccelerationStructure` 中把 `ALLOW_UPDATE`、`ALLOW_COMPACTION`、`PREFER_FAST_TRACE/BUILD` 转成 DXR flags，调用 prebuild info，并计算 scratch；Vulkan 路径同样把 flags 转成 KHR build flags，以 device-address + alignment 切出 scratch。Diligent 将这类信息提升为 `BottomLevelAS`、`TopLevelAS` 和 `GetScratchBufferSizes` plain interface；Bevy 则将 scratch/compaction 放入 staged scene binder。三者说明 Noemancer 应把 size/flag/alignment 当作可观察合同，而不是 backend 私有日志。

### Compaction

compaction 不是必然的同步“缩小 buffer”：

- DXR 使用 post-build info/query 取得 compacted size，待 query 可读后，分配新结果 AS，再执行 `CopyRaytracingAccelerationStructure(..., COMPACT)`；源/目标和 query 的状态转换、fence、延迟释放必须可审计。
- Vulkan 使用 acceleration-structure property/query pool 获取 compacted size，再用 `vkCmdCopyAccelerationStructureKHR` 的 compact mode；build、query、copy、后续 trace 间需要显式 stage/access 同步。
- Diligent 的 `CompactedSize`、`WriteBLASCompactedSize`/`WriteTLASCompactedSize` 和 copy-AS API 是很清晰的跨 API 形状；Bevy `allocate_blas` → `compact_raytracing_blas` 展示了“先可用、后压缩”的异步 staging 思路。
- 第一个 Noemancer vertical slice 可以先证明 build/trace，再把 compaction 作为紧随其后的退出门槛；但正式 RTGI 不能永远只使用未压缩 AS，必须报告 compaction requested/available/committed/failed 以及保留的 fallback 原因。

### Barrier / resource state

资源状态是结果正确性的组成部分，而不是可选优化：

- DXR 结果 AS 需处于 `D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE`；build 输入需要符合 DXR 对 vertex/index/instance/transform/scratch 的读写状态；SBT/其他 buffer 需要进入 non-pixel shader resource 等可读状态。DXR 规范还要求 AS 写入在后续 RT read 前有正确的 UAV/barrier 语义。
- Vulkan build 使用 `VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR`（或等价 stage）及 acceleration-structure read/write access；trace/query 使用 `VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR`，SBT 使用 ray-tracing shader stage + shader read。scratch 在相邻 build/update 间也必须同步。
- Barrier 计划必须以 plain-data 记录“资源、旧状态、新状态、src/dst stage/access、queue ownership、reason、frame/revision”，Native adapter 再把它编码成 D3D12/Vulkan 命令。禁止从 Shader 或 Agent 侧直接拼 native barrier。

### Pipeline / SBT / dispatch

光追 pipeline 与 raster pipeline 的失败边界不同：

- DXR state object 需要 ray-generation、miss、hit-group（及可选 callable）导出；SBT 需要从 state object properties 取 shader identifier，按设备对齐规则布置 raygen/miss/hit/callable records，再填 `D3D12_DISPATCH_RAYS_DESC`。
- Vulkan 需要 `VK_KHR_RAY_TRACING_PIPELINE`、shader group handle、SBT buffer device address/stride/size，并按 `shaderGroupHandleAlignment`、`shaderGroupBaseAlignment` 和 `maxShaderGroupStride` 验证；`vkCmdTraceRaysKHR` 的四个 region（raygen、miss、hit、callable）必须有可审计布局。
- SBT record 中的 material/object index 不应暴露为裸 GPU 地址；Noemancer 只允许 stable slot + revision，Native adapter 维护实际 descriptor/device address。材质变更应使 hit-group/SBT revision 失效，而不是静默复用旧表。
- `TraceRays` 的 width/height/depth、pipeline recursion、SBT bytes、descriptor set/heaps 和 output UAV 必须进入状态 receipt；CPU wall-clock 不能冒充 GPU pass timestamp。

## 参考实现逐项比较与采用决策

| 来源/能力 | 观察到的生产做法 | Noemancer 决策 | 原因和边界 |
|---|---|---|---|
| D3D12 DXR / Vulkan KHR 官方 API | prebuild size → aligned AS/scratch → BLAS/TLAS build/update → barrier → SBT → trace；compaction 由 query + copy 完成 | **Adopt** 语义与验证项；**Adapt** 成 plain-data contract | 这是设备/规范硬合同，不是可替换的“引擎风格”；只在 D3D12/Vulkan Native adapter 编码句柄和命令。 |
| Wicked Engine `wiGraphicsDevice_*` | 一个统一接口覆盖 BLAS/TLAS、flags、scratch、native AS、pipeline、shader identifier、dispatch，DX12/Vulkan 各自编码 | **Port** 少量数学/布局（若确有需要）；**Adapt** 生命周期 | MIT 允许选择性移植，但其资源分配、bindless 和 `wi::` ABI 不符合 Noemancer；不能复制整套 graphics device。 |
| Godot Vulkan driver | `RaytracingCapabilities`、extension/feature/property probe、显式 `blas_create/tlas_create/command_build_* / command_trace_rays` 和 SBT 对齐检查 | **Adopt** capability gating/失败码；**Adapt** driver contract；**Port** 仅独立验证代码 | Godot 的 plain-ish driver 边界和“能力不完整即禁用”很适合 Agent 可读状态；不要引入 RID/storage。当前快照证据集中在 Vulkan，不能当作 D3D12 实现。 |
| DiligentCore | API 直接提供 BLAS/TLAS desc、scratch size、build/update、compacted size query/copy、SBT、`TraceRays`/indirect，并有 D3D12/Vulkan 实现与 API tests | **Adopt** 接口形状和验证矩阵；**Adapt** backend orchestration；必要时 **Port** 独立 helper | Apache-2.0 兼容且跨后端经验最完整；Diligent context/device lifetime 不应成为 Noemancer 的公共对象。 |
| Bevy Solari | wgpu abstraction 下的 BLAS allocation/compaction、TLAS binder、上一帧 TLAS、temporal/path-tracing 消费 | **Adapt** staged compaction、previous-TLAS、temporal reset 语义；**Reject** wgpu/ECS types | MIT/Apache 兼容，适合观察现代实时 RT 的资源时序；其实现不能证明 SDL_GPU 能提供 native RT，也不能直接承担 Noemancer 的 backend。 |
| Filament FrameGraph/PBR | 资源声明、pass lifetime、PBR/raster 质量和移动平台预算；未作为本记录的 RT BLAS/TLAS/SBT 实现 | **Adapt** FrameGraph/resource lifetime/PBR 组织；**Reject** RT foundation | Apache-2.0 友好，但研究快照的强项不在硬件 RT；不能用“有 FrameGraph”替代真实 DXR/Vulkan evidence。 |
| Unreal Engine | 完整商业生产栈的 RHI/Render Dependency Graph、ray tracing scene、shader binding/缓存、fallback 与预算分层 | **Reject** 代码/Shader/布局；**Adopt** 公开可观察的生产问题清单 | EULA 禁止将 UE 源码变成 Noemancer 实现；只记录“哪些 failure mode 必须存在”，不做语义克隆。 |

### Adopt / Port / Adapt / Reject 的可执行定义

- **Adopt**：只表示将规范语义、公开数据关系和验收要求写进 Noemancer 合同；不自动等于复制代码。
- **Port**：仅对 MIT/Apache 兼容来源的、边界清晰且可保留 notice 的独立算法/数据布局；每个 Port 必须有来源 commit、文件、许可证、差异说明和独立测试。
- **Adapt**：以 Noemancer plain-data、Render Graph revision/history、Agent receipt 和 fallback 重新表达概念；默认优先 Adapt。
- **Reject**：禁止复制、整体嵌入、暴露第三方类型、依赖未验证后端，或把只支持 raster/单一 API 的实现宣传为生产 RT foundation。

## Noemancer SDL_GPU 与 Native adapter 边界

### SDL_GPU 保留的职责

SDL_GPU 可以继续承担窗口/交换链、设备启动、常规 buffer/texture/sampler、raster pipeline、render pass、跨后端基础提交和没有硬件 RT 要求的项目。它是跨平台 bootstrap，不是 RT 能力天花板。

SDL_GPU 的 opaque handle、private struct、内部 backend cast 和“某平台恰好可取到 native pointer”不能进入 Scene、Asset、Agent、脚本或持久化项目格式。现有原生 GPU timestamp 适配是针对真实 query 的狭窄诊断边界，不应外推成 acceleration structure 入口。

### Native adapter 的职责

建议建立独立的 `NativeRtAdapter`（名称仅为设计占位，不在本记录中改代码），对外只接受/返回 plain data：

```text
RtDeviceCapabilities
RtGeometryBuild
RtInstanceBuild
RtBuildPlan
RtScratchPlan
RtCompactionRecord
RtBarrierPlan
RtShaderBindingTable
RtDispatchRays
RtFallbackReason
```

其中至少要能表达：

- backend/API、adapter name/driver/build、RT pipeline/ray query/AS feature flags、alignment 和 recursion limits；
- BLAS/TLAS kind、geometry/instance counts、allow-update/allow-compaction/build-vs-trace preference、prebuild result size；
- scratch build/update size、peak bytes、device-address/offset 是否满足对齐、资源 revision 和 retirement fence；
- barrier 的资源逻辑 ID、旧/新状态、stage/access、queue ownership 和 reason；
- SBT 的 shader group count、record bytes/stride/region alignment、pipeline revision、material/object slot revision；
- dispatch dimensions、output logical ID、GPU query/timestamp IDs 与 completion fence；
- fallback code（例如 `rt.unsupported`, `rt.shader-artifact-missing`, `rt.validation-failed`, `rt.budget-exceeded`, `rt.readback-timeout`）和不丢失原因链。

Native D3D12 adapter 负责 `ID3D12Device5`/state object/BLAS-TLAS resources/GPU VA/query/descriptor heap/barrier/`DispatchRays`；Vulkan adapter 负责 KHR function pointers/feature chain/AS/query pool/device address/pipeline/SBT/barrier/`vkCmdTraceRaysKHR`。这些类型和句柄只能留在 adapter/backend translation unit；Render Graph 只看到逻辑资源、pass reads/writes、revision 和 barrier plan。

### Agent / Semantic State Plane 的可读投影

AI 需要读到的是稳定事实而不是 native pointer。每一帧（或变更时）输出有限 receipt：

```json
{
  "feature": "native-raytracing",
  "backend": "d3d12|vulkan|raster-fallback",
  "status": "disabled|building|ready|fallback|error",
  "capabilities": {"blas": true, "tlas": true, "trace_rays": true, "compaction": false},
  "scene_revision": 12,
  "blas": {"count": 1, "built": 1, "updated": 0, "compacted": 0, "scratch_peak_bytes": 0},
  "tlas": {"count": 1, "instance_count": 1, "revision": 12},
  "sbt": {"raygen_bytes": 64, "miss_bytes": 64, "hit_bytes": 64, "alignment_ok": true},
  "dispatch": {"width": 64, "height": 64, "depth": 1},
  "fallback_reason": null,
  "evidence": {"gpu_timestamps": true, "image_hash": "...", "validation_errors": 0}
}
```

这只是示例字段形状，不是当前 schema 的声明；实现时要注册版本、字段 presence、稳定排序和 fingerprint。receipt 不得写出地址、descriptor heap index、pointer 或未受控第三方枚举。复杂操作仍可由 Agent tool 封装，但工具返回的内容必须能追溯到同一份事实 receipt。

## 最小生产纵切

### Slice A：真实 BLAS/TLAS/TraceRays

固定确定性场景：一个三角形 mesh（3 vertices + 3 indices）、一个 TLAS instance、一个相机和一个 64×64（随后 256×256）输出 UAV。场景必须有 raster fallback 同样的几何、材质和相机。

退出条件：

1. D3D12 与 Vulkan 都通过 capability probe；不支持的设备明确进入 fallback，不伪造 ready。
2. 计算并记录 BLAS/TLAS prebuild sizes、scratch/update scratch、对齐、GPU address/resource revision。
3. 在 Render Graph 中提交 AS build pass；记录输入资源和 barrier plan，build 完成后才能让 trace pass 读取。
4. 构造最小 RT pipeline：raygen、miss、closest-hit；生成并验证 SBT 四 region（当前可以没有 callable，但 receipt 必须明确 `callable=none`）。
5. 绑定 TLAS、输出 UAV 和固定常量，执行真实 `DispatchRays`/`vkCmdTraceRaysKHR`，以 fence + GPU timestamp 完成；输出有稳定 hash，且与 raster fallback 的 A/B 不是同一张伪造黑图。
6. 场景 transform 修改一次，走明确的 TLAS update/refit 或重新 build；若几何拓扑修改，必须拒绝 refit 并说明原因。

Slice A 只证明“硬件 RT 通了”，不宣称 RTGI、反射、透明、multi-bounce 或商业画质完成。

### Slice B：compaction、SBT revision 与 history

在 Slice A 后紧接完成：

- BLAS/TLAS post-build compacted-size query、fence/readback、aligned compact target、copy-AS、旧资源延迟回收；receipt 分别记录 requested/queried/committed/failed。
- 材质/hit-group revision 变化使 SBT 重新生成；仅 instance transform 变化不得无故重编 shader table。
- 相机切换、scene revision 变化、resize、backend/device lost 时 reset RT temporal/history；不能把旧 TLAS/SBT 当作新场景继续 trace。
- D3D12/Vulkan 各自验证 barrier，而不是用一个“generic barrier succeeded”字段掩盖 backend 差异。

### Slice C：Render Graph 与 RTGI 消费

只有 A/B 已有真实双后端证据后，才允许 RTGI pass 消费 TLAS：先做单 bounce、固定 max distance/roughness、低分辨率输出和 raster fallback；再做 temporal denoise、screen-space fallback 和预算策略。RTGI 不得绕过 Render Graph 直接创建/释放 AS，也不得在缺少 Native RT 时改变场景语义。

## Fallback、预算与失败语义

| 失败点 | 必须动作 | 禁止动作 |
|---|---|---|
| 无 AS / RT pipeline feature，或扩展/driver 不完整 | `rt.unsupported`，使用 raster SSR/SSGI/CSM，receipt 保留 capability matrix | 把 ray query、软件 BVH 或普通 compute 道具标成硬件 RT ready |
| prebuild size、alignment、GPU address 或 scratch 不满足 | `rt.resource-contract-failed`，不提交 build/trace | 截断地址、偷偷改 stride、复用未对齐 scratch |
| shader/DXIL/SPIR-V 编译或 SBT handle/layout 失败 | `rt.shader-artifact-missing` 或 `rt.sbt-invalid`，回退 | 使用 stale SBT、全零 shader identifier 或静默命中 miss |
| barrier、validation layer、device lost、fence/readback timeout | `rt.validation-failed` / `rt.device-lost` / `rt.readback-timeout`，销毁/retire 资源后回退 | 以 CPU wall-clock 或“命令已录制”充当 GPU 完成 |
| scratch/AS/dispatch 预算超限 | `rt.budget-exceeded`，按质量级别降采样或回 raster | 通过减少 workload、隐藏几何或跳过 pass 伪造性能达标 |
| compaction query 尚未完成 | 保留可读的未压缩 AS，状态为 `compaction=pending`，下一安全点再压缩 | 立即释放源 AS 或把 pending 当 committed |

质量旋钮（off/low/medium/high）只能改变采样、分辨率、更新频率和预算，不改变 receipt 语义。所有降级都要记录“请求值、实际值、降级原因”。

## 证据合同（未来 RenderLab / acceptance）

本节是下一批实现的验收合同草案，不表示现有脚本或 Runtime 已支持这些字段。

### 固定运行

- 隐藏窗口、Release、1920×1080 主运行；D3D12 和 Vulkan 分开进程/receipt；另允许 64/256 的 RT micro-slice 作为快速 bring-up，但不能用它冒充主画质证据。
- 固定 shader/artifact、相机、mesh、材质、seed、frames-in-flight、present/resize 状态；关闭自动曝光和随机 jitter，避免 A/B 被曝光或噪声混淆。
- capture 与 performance 分离：capture 保存图片、receipt、日志和 hash；performance 只在 warm-up 后采 GPU pass timestamp 和 VRAM/working-set。CPU frame time 单独标 `cpu_measurement`，不得填入 GPU 字段。

### 结构化 receipt 必须能回答

1. 设备/驱动/backend/RT feature matrix 是否真实；
2. BLAS/TLAS 的数量、geometry/instance 数、build/update 次数、revision、result bytes、scratch build/update bytes、峰值和 compaction 状态；
3. 每个 AS build/update/copy/query/trace pass 的 Render Graph ID、资源读写、barrier、GPU timestamp；
4. SBT 每个 region 的 address/offset（对外仅逻辑 region）、bytes/stride/alignment、shader-group revision、material/instance slot revision；
5. trace dimensions、recursion、output format、descriptor binding 状态和 validation error 数；
6. A/B 图像 hash、线性空间 RGB/亮度统计、RT hit/ miss 或 debug view、fallback 画面和控制区指标；
7. reset 原因（camera/scene/resize/device/compaction/SBT），以及 fallback reason chain；
8. 产物路径、SHA-256、schema version、commit/config fingerprint。

### 视觉和性能判定

- RT on/off 必须在固定反射/命中 ROI 有正向、可重复的线性空间差异；控制 ROI 变化应在合同阈值内。差异不能只来自曝光、clear color 或随机噪声。
- 必须提供 `rt-hit-debug`、`rt-miss-debug`、`blas-instance-debug` 或等价视图，能在文字 receipt 中说明命中/漏命中计数。没有真实 hit/debug 数据时，不得宣称 trace 有效。
- GPU timestamp 必须来自 D3D12 timestamp query 或 Vulkan calibrated/timestamp query 的真实结果；每个 pass 的 start/end/valid/frequency/backend 都要有。没有 GPU query 视为性能证据缺失，不以 CPU 时间替代。
- D3D12 与 Vulkan 的 A/B 结果分别记录；跨后端只要求语义/阈值一致，不要求 bit-identical。任一后端失败应是该 backend 的 fail/fallback，不得被另一后端的成功覆盖。
- 任何缺字段、hash 不匹配、图片无法读取、GPU timestamp invalid、预期 pass 缺失或启动进程未隐藏，都应 fail closed（非零退出码）；“脚本执行完”不是成功。

建议的未来 pass/状态 ID（实现前仍属提案）：

```text
render.pass.rt-blas-build
render.pass.rt-tlas-build
render.pass.rt-as-compaction-query
render.pass.rt-as-compaction-copy
render.pass.rt-sbt-upload
render.pass.rt-trace-rays
render.pass.rt-fallback-raster
```

脚本应验证真实 pass presence，而不是只匹配命令行开关或字符串日志；如果 backend 不支持，则只允许出现 `render.pass.rt-fallback-raster` 和明确 fallback reason。

## 风险与待决策

1. **SDL_GPU 版本边界**：在 Native adapter 接入前，不要赌 SDL_GPU 暴露未来 RT API；先保持 adapter 可编译隔离，避免把 D3D12/Vulkan 依赖污染普通构建。
2. **shader toolchain**：DXIL 与 SPIR-V 的编译、反射、shader group export 命名和 debug symbols 需要单独锁版本；跨后端同一 HLSL 不等于同一 SBT layout。
3. **分配和 compaction**：AS 结果、scratch、query、SBT 和 descriptor 的 GPU lifetime 是多帧问题；必须用 fence/retirement ledger，不能让 allocator 的“引用计数”隐藏 GPU 未完成。
4. **barrier/queue**：graphics/compute/transfer queue、queue ownership、UAV 规则在两个 API 不对称；只写抽象 stage 名称不足以证明正确。
5. **性能**：TLAS 每帧全量重建、SBT 全量上传、过高 recursion 或每像素多 bounce 可能使 RTGI 失去商业可用性；先以真实 GPU pass timestamp 和 scratch/VRAM 预算决定优化顺序。
6. **许可证**：MIT/Apache 的选择性 Port 仍需保留 notice 和来源；UE EULA 不能通过“只参考实现”变成代码复制。每次 Port 都应形成单独 provenance 记录。
7. **Agent 可读性**：receipt 是可观察投影，不是第二个资源数据库；字段应稳定、有限、有 revision，native handle 永远不进入 Semantic State Plane。

## 后续顺序（研究记录，不改权威路线图）

| 顺序 | 工作项 | 退出证据 |
|---|---|---|
| S0 | 锁定本记录、provenance 和 plain-data schema 草案 | commit/path/license 可复核，未复制 UE/第三方整体 |
| S1 | Native D3D12/Vulkan capability probe 与资源合同 | 双 backend feature matrix、alignment、失败码 receipt |
| S2 | Slice A：1 BLAS + 1 TLAS + SBT + trace | 隐藏 Release 双 backend 真实 GPU pass/image/fence |
| S3 | barriers、update/refit、compaction query/copy | revision、history reset、query/retire、compaction bytes 全部有证据 |
| S4 | Render Graph pass、fallback/debug views、Agent receipt | pass presence、fallback 不丢语义、schema/fingerprint stable |
| S5 | 低分辨率 RTGI/反射消费与 denoise | ROI A/B、GPU timestamps、预算与跨后端结果 |
| S6 | 更高质量、异步调度、VRAM/working-set 和 device matrix | 不以隐藏 workload 达标；商业画质前先过真实性和稳定性门槛 |

### 本记录的明确非目标

- 不把 SDL_GPU private handle cast、软件 BVH、screen-space reflection 或普通 compute pass 叫作 DXR/Vulkan hardware RT。
- 不因为某个参考引擎有 RT 就复制其完整 renderer、ECS、asset system、shader ABI 或 allocator。
- 不在本研究记录中修改 `docs/development-plan.zh-CN.md`、代码、CMake、Shader、测试注册或外部 RenderLab；主 Agent 集成时应引用本记录，而不是把历史记录当作已完成状态。
