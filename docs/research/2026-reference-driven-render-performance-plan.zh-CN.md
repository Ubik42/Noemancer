# Noemancer 参考实现驱动的高性能渲染计划（2026-08-25）

> Status: Historical research input; non-authoritative after capture.
>
> 目标落点：`D:\cs\Noemancer\docs\research\2026-reference-driven-render-performance-plan.zh-CN.md`
>
> 文档性质：当前渲染强化批次的研究输入。执行顺序仍由 `docs/current-state.json.currentFrontier` 负责，不能让本报告独立成为第二份路线图。

## 结论

Noemancer 已经采用现代高性能方向，但还没有证据证明“极致性能”或相对 Godot、Wicked Engine、Unreal Engine 的性能优势。当前成立的是 Forward PBR、Clustered local lights、GPU static opaque frustum cull、compact visible index、indexed indirect、稳定 batching、细粒度 dirty upload、KTX2 streaming、meshoptimizer Cook、ozz 与 Jolt 等生产纵切；现有 A/B 主要证明 CPU Frame/Scene Record 没有明显回退，以及 GPU 可见集合和提交结构正确。当前主机没有稳定的逐 Pass GPU Timestamp，PresentMon 也没有产生有效行，因此不能把 CPU Frame 改善写成 GPU 性能改善。

接下来的默认开发方式改为“参考实现驱动”：每项渲染能力先精读固定版本的成熟实现，明确 `Adopt / Port / Adapt / Reject`，再决定 Noemancer 的最小移植边界。宽松许可证代码允许直接移植，但必须保留来源、提交、许可证、修改说明和独立验证；Unreal Engine 源码只作生产约束与对照，不复制进 Apache-2.0 工程。

## 当前性能事实

### 已证明

- GPU static opaque compute cull 能产生与 CPU 一致的 exact visible set；1025 candidates 中 647 visible、378 culled，D3D12/Vulkan 结果一致。
- 兼容压力场景能把大量对象压缩为一个 indexed-indirect batch，并显著降低 CPU draw submission 数。
- 稳定 batching 的稳态帧可复用 topology，未变化帧的 dirty range 和稳定上传可归零。
- Render World 与 Gameplay World 分离；Simulation View 不再重复计算 render-only skeletal palette，root motion ignore 不再隐式采样。
- 纹理、几何、动画使用成熟中间件和 Cooked-only Player 路径，减少运行时源格式解析和离线编译成本。
- 64 个蒙皮角色与 256 个持续活动 Jolt 刚体的 Release CPU workload 已有固定证据。

### 未证明

- 没有可信的逐 Render Pass GPU 时间、queue overlap、pipeline bubble、bandwidth、occupancy 或物理显存遥测。
- 现有 GPU-driven A/B 只能证明 CPU Frame/Scene Record 没有明显回退，不能宣称 GPU 更快。
- Cluster assignment 目前是 CPU conservative sphere；尚未证明 compute clustering 更划算。
- 没有生产 HiZ occlusion、bindless resource model、async compute、meshlet/mesh shader、PSO cache 或 native residency telemetry。
- SDL_GPU 足以建立跨后端产品纵切，但无法承载当前规划的完整 DXR/Vulkan RT、深度 GPU 诊断和高端资源控制。
- 没有 AMD/NVIDIA/Intel、多机器、跨平台和大型公开场景的质量/性能矩阵。

因此，所有“极致”“领先”“优于参考引擎”的表述必须保持禁止，直到同一公开 workload、同一画质合同与可复现 GPU 数据支持该结论。

## 固定参考与许可证边界

| 参考 | 本地固定位置 | 许可/使用边界 | 主要用途 |
|---|---|---|---|
| Wicked Engine `f4a0d2635d5224b4509da164fa75d90fbdaaea26` | `D:\cs\_reference\github\_game-engine\WickedEngine` | MIT；允许 Port。AMD FidelityFX DNSR/FSR/ParallelSort 等另有 Notice，不能只保留 Wicked 根许可 | C++/HLSL 高端路径、SSR、SSGI、TAA、天空大气、DDGI、GPU profiler、bindless/RT |
| Godot `3000096f9aa6f46db98d3a6d2a9442d58cab96ac` | `D:\cs\_reference\github\_game-engine\godot` | MIT；允许 Port。Intel SSIL 与 Spartan/Panos Karabelas TAA 需分别保留二级 Copyright/permission notice | Forward+、SSIL、TAA、Sky、环境参数、质量档与编辑器暴露 |
| Unreal Engine 5.8.1 | `D:\cs\_reference\github\_game-engine\UnrealEngine` | 受 Epic EULA 约束；只研究，不复制源码、Shader、宏或类型 | RDG、VSM、Lumen、TSR、GPU Scene、Instance Culling 的生产边界与 failure mode |
| Filament | 官方 PBR/FrameGraph 文档 | 公开文档/许可逐源确认 | PBR 数值、物理单位、曝光、色彩链和资源 lifetime oracle |
| Bevy | `D:\cs\_reference\github\_game-engine\bevy` | Rust 实现，主要 Adapt | Main World → Extract → Render World、phase 与图式调度 |

每个 Port 必须新增 `sourceProvenance` 记录：上游仓库、提交、源文件、许可证、移植日期、关键修改、Noemancer Shader Artifact ID 和验证证据。第三方类型不得进入 Scene、Prefab、项目 C#、Semantic State Plane 或公共 RPC。

## 代码级移植地图

### Wicked Engine

#### 天空大气：Adapt，首选实现参考

- 调度入口：`WickedEngine/wiRenderer.cpp` 的 `ComputeSkyAtmosphereTextures`、`ComputeSkyAtmosphereSkyViewLut`、`ComputeSkyAtmosphereCameraVolumeLut`。
- Shader 编目：`WickedEngine/offlineshadercompiler.cpp` 中的 transmittance、multi-scattering、sky-view、sky-luminance、camera-volume LUT。
- Shader 核心：`WickedEngine/shaders/skyAtmosphere*.hlsl`、`skyAtmosphere.hlsli`、`aerialPerspectiveCS.hlsl`。
- 可复用：大气参数、LUT 分解、采样数学、质量档、相机进入/离开大气层的边界处理。
- 必须替换：`GetWeather/GetFrame`、bindless descriptor、Wicked Texture/CommandList、全局 renderer 状态。

#### SSR：Adapt Shader/Pass 分解，不复制 Renderer

- 调度入口：`WickedEngine/wiRenderer.cpp` 的 `Postprocess_SSR`。
- Pass：tile max roughness horizontal/vertical、depth hierarchy、ray trace、resolve、temporal、upsample。
- Shader：`WickedEngine/shaders/ssr_*.hlsl`。
- Noemancer 依赖：depth pyramid、scene color history、normal/roughness、motion vectors、reactive/disocclusion、history reset。
- 决策：先建立共享 HiZ/history/denoiser，再 Port 算法；禁止 SSR 自己建立第二套 temporal authority。

#### SSGI：Adapt

- 调度入口：`WickedEngine/wiRenderer.cpp` 的 `Postprocess_SSGI`。
- Shader：`ssgi_deinterleaveCS.hlsl`、`ssgiCS.hlsl`、`ssgi_upsampleCS.hlsl` 及共享 include。
- 可复用：deinterleave、wide/normal 质量档、ray sampling、upsample。
- 必须重新设计：与 Noemancer AO/IBL MRT 的合成关系、history、漏光诊断、Hybrid Pixel 禁用/降级策略。

#### TAA/Temporal Denoising：Adapt 为共享基础

- 调度入口：`WickedEngine/wiRenderer.cpp` 的 `Postprocess_TemporalAA`。
- Shader：`temporalaaCS.hlsl` 及 RT shadow/RTAO/SSR temporal/filter Shader。
- 可复用：重投影、邻域 clamp、history confidence、tile classification、spatial filter 组合。
- Noemancer 目标：一个 versioned history service，统一 current/previous matrices、motion、depth、normal、reactive、disocclusion 和 reset reason；TAA、SSR、SSGI、未来 RTGI 只消费该服务。

#### DDGI/RT：延后 Adapt

- 调度入口：`WickedEngine/wiRenderer.cpp` 的 `DDGI`。
- Shader：`ddgi_rayallocationCS`、`ddgi_indirectprepareCS`、`ddgi_raytraceCS[_rtapi]`、`ddgi_updateCS[_depth]`。
- 依赖：bindless、indirect dispatch、BVH/BLAS/TLAS 或软件 BVH、SM 6.5/native RT API、稳定 GPU timing。
- 决策：在 native RHI/RT foundation 之前只用作接口与测试设计参考，不把软件路径或空结构体冒充 RTGI。

### Godot

#### Forward+ 与质量档：Adapt

- `servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.cpp/.h`
- `servers/rendering/renderer_rd/shaders/forward_clustered/scene_forward_clustered*.glsl`
- `servers/rendering/renderer_rd/shaders/scene_forward_lights_inc.glsl`
- 学习重点：cluster/grid 参数、profile 分层、灯光/阴影资源布局和 Editor Environment 参数；不复制 RID/global storage 模型。

#### SSIL：Port 数学，Adapt 调度

- `servers/rendering/renderer_rd/effects/ss_effects.cpp/.h`
- `servers/rendering/renderer_rd/shaders/effects/ssil.glsl`
- `ssil_importance_map.glsl`、`ssil_blur.glsl`、`ssil_interleave.glsl`
- 学习重点：importance/adaptive 质量、interleave、normal rejection、blur/upsample 和可调 fade。
- 二级许可：相关 Shader 含 Intel Corporation 2016 notice，移植时必须原样保留。

#### TAA：Port 数学，Adapt history

- `servers/rendering/renderer_rd/effects/taa.cpp/.h`
- `servers/rendering/renderer_rd/shaders/effects/taa_resolve.glsl`
- `servers/rendering/renderer_scene_cull.cpp` 的 camera jitter phase。
- 不复制 Godot history ownership；Noemancer 使用共享 temporal authority。
- 二级许可：`taa_resolve.glsl` 含 Panos Karabelas/Spartan Engine notice，不能只记录 Godot MIT。

#### SSR：Port 完整 Shader 链，Adapt 资源编排

- `servers/rendering/renderer_rd/shaders/effects/screen_space_reflection_downsample.glsl`
- `screen_space_reflection_hiz.glsl`
- `screen_space_reflection.glsl`
- `screen_space_reflection_resolve.glsl`
- `screen_space_reflection_filter.glsl`
- `specular_merge.glsl`
- 调度入口：`servers/rendering/renderer_rd/effects/ss_effects.cpp` 的 `screen_space_reflection` 与 `forward_clustered/render_forward_clustered.cpp` 的 `_process_ssr`。
- 注意 Godot 读取 previous final frame，且与 SSIL 共用 last-frame scope；normal/roughness packing、projection/Y flip、depth convention、odd dimension 和 transparent viewport 行为都必须显式对齐。

#### Sky/Environment：Adapt 产品模型

- `servers/rendering/renderer_rd/environment/sky.cpp`
- `servers/rendering/renderer_scene_render.cpp` 与 `rendering_method.h` 的 Sky/Fog/SSR/SSIL Environment API。
- 学习重点：Realtime/Incremental/Quality 模式、环境参数分组、变更失效和编辑器暴露；物理大气 Shader 首选 Wicked/公开模型。
- Godot `PhysicalSkyMaterial` 是 Preetham analytic daylight，适合作为低成本档；不是多 LUT、行星尺度、多重散射终局方案。

#### GPU Timestamp：Adopt API 形状，Adapt Native 后端

- 抽象：`servers/rendering/rendering_device*.{h,cpp}` 与 `rendering_device_graph.cpp`。
- D3D12/Vulkan：`drivers/d3d12/rendering_device_driver_d3d12.cpp`、`drivers/vulkan/rendering_device_driver_vulkan.cpp` 的 query pool、timestamp write、延迟 result readback 和频率/period 换算。
- 采用点：固定容量 frame-ring、named marker、CPU/GPU paired result、frame age/availability、query overflow；marker 必须成为图命令而不是只包 CPU record 函数。
- 多 queue 时间只能在同一 timeline 内比较；async compute 后必须分 queue 校准，不能随意相减。

## 不应现在“全抄”的系统

- Wicked `globals.hlsli`：把 Frame、Camera、Scene、Bindless、RT、材质、流送与 GI 绑成单一 ABI。
- Wicked 近两万行 `wiRenderer.cpp`：只作为调度 oracle，不整体嵌入。
- Wicked SSGI、Surfel、VXGI、DDGI 四套 GI：近期只做 SSGI/SSIL 基线，Native RT 后以 DDGI 为主候选。
- Wicked visibility-buffer compute shading 与 software BVH fallback：会把现有 Forward 主线同时改成另一种 Renderer，当前 Reject。
- Godot `SkyRD`、`SSEffects`、`ClusterBuilderRD`、Shadow Atlas/Culling、RenderingDevice：均深度绑定 RID、全局 storage 与 Shader generator，应 Adapt 而非整类复制。
- Godot/Wicked 的传统 query occlusion：不作为 Noemancer HiZ GPU-driven 终局方案。
- UE VSM/Lumen/Nanite/TSR 源码：研究生产边界，禁止复制。

## 采用规则

1. **Adopt**：成熟独立库能直接负责完整执行内核，例如 Jolt、ozz、meshoptimizer；只写 plain-data Adapter。
2. **Port**：宽松许可证实现具有清晰输入输出且依赖可切断；尽量保留经过验证的算法和 Shader，不凭记忆重写。
3. **Adapt**：实现与上游 RHI、全局状态、资源系统或 Shader ABI 深度耦合；保留 pass decomposition、数据布局、质量策略和 failure mode，使用 Noemancer Render Graph/Authority 重接。
4. **Reject**：许可证不兼容、必须引入半个外部引擎、目标 workload 没有收益，或无法形成可验证维护边界。

“自己写”不再是默认；没有记录参考检索与 Adopt/Port/Adapt/Reject 的高级渲染 PR 不进入实现阶段。

## 当前渲染执行顺序

1. **RenderLab Classic Scene Contract**：把外部公开工程变成真实 Golden 客户；固定 camera、asset hash、quality sidecar、D3D12/Vulkan 图像和 workload 身份。
2. **glTF External Resource/JPEG/Large Scene Readiness**：保证后续经典大场景可以进入 Cooked-only Player；外部 buffer/image URI、data URI、JPEG、不可变依赖快照、路径越界/符号链接拒绝和依赖变化检测必须一起完成。
3. **GPU Pass Telemetry Foundation**：在不伪造数据的前提下建立逐 Pass timestamp/correlation；SDL_GPU 当前明确不提供 query API，先产出结构化 unsupported/capability，再以 Native D3D12/Vulkan 最小诊断 Adapter ADR 建立 frame-ring、延迟读回、marker availability 和 query overflow。退出条件是至少一条可复现的逐 Pass GPU 时间路径，未完成结果不能输出 0 冒充有效值。
4. **Dynamic Sky Atmosphere**：参考 Wicked LUT/raymarch + Godot Environment/质量档；先做 transmittance/multi-scatter/sky-view，后做 aerial perspective/camera volume。
5. **Shared HiZ + Temporal History/Denoiser**：建立 depth pyramid、history import/export、motion/depth/normal rejection、reactive/disocclusion、reset 和 debug views。
6. **SSR**：按 Wicked pass 分解移植，并用 RenderLab roughness/edge/disocclusion/运动机位验证。
7. **SSGI/SSIL**：Wicked SSGI 与 Godot SSIL 双参考；与 AO/IBL 正确合成，验证漏光、薄墙、运动和半分辨率恢复。
8. **Bindless/GPU Scene/Occlusion 复审**：以 CPU submission、descriptor churn、visible ratio 和 GPU 数据决定；不为了 feature 名称提前承担复杂度。
9. **Shadow Scalability/VSM ADR**：先扩展传统 CSM/local shadow workload，再决定 virtual pages；UE 只作生产约束参考。
10. **Native RHI/RT Foundation → DDGI/RTGI**：先做 capability、BLAS/TLAS、同步、Shader、crash/timing 和 raster fallback，再 Port/Adapt DDGI。

## 每个渲染批次的强制退出合同

- 一份上游 adoption record，含精确文件、提交、许可证和决策。
- 一个真实 Render Graph 节点集合，不接受只有 Shader 文件或结构体。
- 参数、质量档、history reset、unsupported/fallback 的 plain-data Schema。
- RenderLab 固定机位的 on/off A/B 与至少一个 failure-mode 场景。
- D3D12/Vulkan 结果；平台不支持必须明确报告。
- CPU Frame/Record 与可用时的逐 Pass GPU 时间；没有 GPU 数据就禁止 GPU 性能结论。
- 显存/working-set 估计需区分逻辑字节、已提交资源和物理 residency。
- 不通过减少对象、灯光、分辨率、采样或材质复杂度伪造性能提升。
- 只运行受影响测试与一个直接探针；到跨 RHI/公共 Schema/里程碑边界再跑全量测试。

## 近期可并行批次

- Writer A：RenderLab Golden/evidence script 与公开项目 workload；不改 Renderer。
- Writer B：GPU timing capability/probe、结构化 unsupported receipt 与 Native Adapter ADR；不改 Scene/Asset。
- Writer C：glTF external/JPEG Adapter 与安全路径/Cook 测试；不改 Renderer。
- Sol：统一 Render Graph/Renderer evidence schema、adoption record、集成与权威状态。

完成前三项后再并行 Sky Shader、Environment Authority 和 RenderLab atmosphere fixture。SSR、SSGI 不与共享 history 基础并行抢写同一 Render Graph 集中点。
