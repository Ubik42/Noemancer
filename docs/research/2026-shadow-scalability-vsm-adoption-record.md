# Shadow Scalability / VSM Adoption Record

> 调研日期：2026-08-25
> 文档性质：Historical / 历史研究与实现准备记录，不是当前架构或路线图的权威来源。
> 当前切片：`render.shadow-scalability-vsm-decision`

## 结论先行

Noemancer 当前的 CSM + local shadow array 不是临时占位，而是应继续保留的生产回退路径。现有 `src/engine/cascaded_shadow.*` 已有 4 级 practical split、级联 texel snapping、有限输入校验和非有限结果拒绝；`assets/shaders/scene_lit.frag.hlsl` 已消费方向光级联与局部光 shadow array。没有证据表明本批次应立即把所有光源切换为 VSM。

建议采用“VSM 页抽象先行、方向光单 clipmap 最小纵切、GPU feedback 后置”的路线：

1. 先固定内部 plain-data 的页 key、页状态、失效原因、回退原因和统计合同，不把第三方类型或 UE 命名带入持久化 Scene、Agent 或公共 RPC。
2. 只为一个方向光实现一个有限页池和一个 clipmap/级联页层；页面缺失、投影异常、页池耗尽时回到 CSM，缺失页不得凭空把表面变暗。
3. 证实页表查找、tile render、generation/epoch 防陈旧、相机平移和 caster revision 失效后，再增加屏幕/GBuffer 请求、边缘膨胀和 receiver mask。
4. 全量 VSM、静态/动态双缓存、GPU invalidation、预过滤远景页、Nanite 类 page draw scheduling 都不是当前最小纵切的退出条件，必须由真实 RenderLab workload 的 page pressure 证据触发。

这样既能得到可测量的 VSM scalability 实验，又不会牺牲商业运行时必须具备的确定性 CSM fallback。

## 固定快照与许可证边界

| 参考仓 | 固定版本 | 许可证 | 本记录允许的使用方式 |
|---|---|---|---|
| `D:\\3D\\_tools\\_reference\\_game-engine\\WickedEngine` | `f4a0d2635d5224b4509da164fa75d90fbdaaea26`（2026-08-16，`combined shader for image and font renderer...`） | `LICENSE.txt`：MIT，Copyright 2026 Turánszki János | 可研究；若未来需要复制少量算法或 Shader，必须隔离、保留 MIT notice，并重新验证 ABI。默认优先独立实现/Adapt。 |
| `D:\\3D\\_tools\\_reference\\_game-engine\\godot` | `3000096f9aa6f46db98d3a6d2a9442d58cab96ac`（2026-08-17，Godot `4.8.0`） | `LICENSE.txt`：MIT，Godot contributors 与原作者 | 可研究；可在保留 notice 的前提下选择性 Port/Adapt 算法，但不能把 Godot 的第三方对象、RID 或渲染存储边界带入 Noemancer。 |
| `D:\\3D\\_tools\\_reference\\_game-engine\\UnrealEngine` | `71fe36aac5a8df5ccd66c763ffc902b29b6a9c43`（2026-07-28，`5.8.1 release`） | `LICENSE.md`：Unreal Engine EULA | 只作为生产约束和 failure-mode oracle。禁止复制 UE 源码、Shader、命名布局或通过改名规避许可。 |

本记录没有把任何参考仓文件复制到工程；下文路径和行号只用于可复核的研究证据。

## Noemancer 当前基线

### CSM / local shadow 的现状

- `src/engine/cascaded_shadow.hpp:12-36` 定义最多 4 级、默认 2048 分辨率、最大距离 80、split lambda 0.65、depth padding 10 的纯数据 CSM 计划。
- `src/engine/cascaded_shadow.cpp:44-96` 做 practical（线性/对数混合）分割，拟合每级 frustum 的 bounding sphere，以每纹素世界尺寸对中心做 snapping，生成正交投影，并拒绝非有限输入/输出。
- `assets/shaders/scene_lit.frag.hlsl:56-63` 的材质/场景 ABI 暴露 `cascadeSplits`、4 个 `cascadeViewProjections`、方向光 shadow 参数、8 个 local shadow projection；`shadow_layer_visibility` 和 `shadow_visibility` 在约 141-173 行完成方向光级联选择、PCF 与级联混合。
- 当前基线适合先做视觉和性能证据：稳定、容易 debug、跨 D3D12/Vulkan 绑定简单。VSM 不能在缺失页时直接替换成“shadow=0”，否则页池压力或反馈延迟会产生错误的黑色阴影。

### 本批次的非目标

- 不在此记录中改 `src/`、`assets/shaders/`、CMake、manifest 或权威文档。
- 不把 VSM 叫作“已完成的商业级功能”；在存在真实页 pressure、移动 caster、相机平移和 pool exhaustion 证据前，仍称为实验性可选路径。
- 不把 `Semantic State Plane` 变成第二个 shadow database。它只应观察页状态、请求、失效和回退的有界投影。

## WickedEngine：传统 CSM + 动态 atlas 的可复用经验

### 精确实现证据

1. 资源与 pass：`WickedEngine/WickedEngine/wiRenderer.h:32-34` 使用 `D16_UNORM` shadow depth（D32 注释备用），同时定义用于透明 shadow 的 `R16G16B16A16_FLOAT`；`wiRenderer.cpp:159-160` 持有 `shadowMapAtlas` 与透明 atlas。
2. 级联拟合：`WickedEngine/WickedEngine/wiRenderer.cpp:2924-3047` 的 `CreateDirLightShadowCams` 去除 camera jitter，反投影主相机 frustum，按 cascade split 在 light-view 中求边界球，随后在 `3003-3008` 按 shadow rect 的 texel size 对边界和中心做 snapping。`3013-3028` 对投影 Z 做紧分布并留出 cascade blending 空间。
3. atlas packing：`wiRenderer.cpp:3915-4060` 用 `wi::rectpacker::Rect` 为方向光各级联、spot/rectangle、point cube 和 rain blocker 打包；方向光的 rect 宽度按 cascade 数量展开（`3949-3960`），打包失败时通过 iterative scaling 重试（`3921-3923`、`3996-4000`），然后创建深度和透明 atlas（`4040-4059`）。
4. caster 选择与多 viewport：`wiRenderer.cpp:6671-6835` 对可见的动态灯建立 render pass；`6750-6754` 建立级联相机，`6768-6784` 逐对象做 cascade frustum mask，`6804-6835` 把各级联放入 atlas 的 viewport/scissor 并批量渲染。
5. 质量/性能旋钮：`wiRenderer.h:1297-1298` 暴露 shadow LOD override；这说明阴影性能不仅取决于纹理分辨率，也取决于 caster LOD 和每级联可见集合。

### 决策

- **Adopt（概念）**：去 jitter 的 light view、边界球拟合、cascade texel snapping、紧 Z + blending padding、多 viewport atlas、caster cascade mask。
- **Adapt（独立实现）**：Noemancer 继续用自己的 `ShadowVec3/ShadowMat4`、自己的 CSM contract 和 SDL_GPU 资源；不要把 Wicked 的 `wi::graphics`、`wi::rectpacker`、`Visibility` 或对象类型扩散到 Engine 公共层。
- **Port（仅在必要时）**：若未来需要少量 rect packing 或 shadow bias 数学，可在 MIT notice 可追踪的独立 adapter 中移植；当前没有必要复制代码。
- **Reject**：Wicked 没有 `VirtualShadowMap`、VSM page table 或 shadow physical page pool。其 `virtualTexture*` Shader 是材质纹理 residency，不是 shadow VSM，不能误判成现成 VSM。

### 其他可借鉴但不能混淆的系统

Wicked 的 `WickedEngine/WickedEngine/shaders/virtualTextureResidencyUpdateCS.hlsl`、`virtualTextureTileAllocateCS.hlsl`、`virtualTextureTileRequestsCS.hlsl` 以及 `wiScene.cpp` 的 texture-streaming feedback/readback 是“请求—分配—驻留”的类比。它们不是阴影实现，不能直接作为 VSM 方案；最多用于设计 Noemancer 的 bounded request telemetry。

Wicked 的 VXGI clipmap 位于 `wiScene.h:231-238`、`wiScene.cpp:687-732,823-836`、`wiRenderer.cpp:10082-10107`，服务于体素 GI 而非 shadow clipmap。将其作为阴影实现移植应 **Reject**。

## Godot 4.8：成熟的传统 shadow atlas / LRU 策略

### 精确实现证据

1. 存储布局：`servers/rendering/renderer_rd/storage_rd/light_storage.h:393-451` 定义 `ShadowAtlas`，四个 quadrant、每格 `owner/version/fog_version/alloc_tick`、`shadow_owners` 映射，以及独立的 `DirectionalShadow` depth/framebuffer。它没有 VSM 的 virtual page/physical page pool。
2. positional atlas 计算：`light_storage.h:684-738` 将 quadrant、subdivision 和 slot 转换为 atlas rect/texel size；`light_storage.cpp:2365-2398` 调整 atlas size 时释放深度资源、清空 quadrant owner 并从 light instances 移除引用，意味着资源规格改变会有意触发全量失效。
3. subdivision 与分配：`light_storage.cpp:2400-2459` 将 subdivision 规整为可平方的 power-of-two，并维护 `size_order`；`2462-2514` 优先找空槽，之后按 `last_scene_pass` 选择 least-recently-used 槽，同时以 `shadow_atlas_realloc_tolerance_msec=500` 避免刚分配的阴影过早被偷走。
4. 版本失效：`light_storage.cpp:2589-2711` 用 coverage 选择最佳 subdivision；已有 slot 对比 `p_light_version` 决定 `should_redraw`（`2642-2653`），重分配时清理旧 owner、调用 `_shadow_atlas_invalidate_shadow` 并返回 dirty（`2670-2708`）。`2714-2731` 处理被替换的 owner 和 omni 成对槽。
5. directional atlas：`light_storage.cpp:2751-2832` 创建独立方向光 atlas，`2787-2806` 按方向光数量排布矩形；`2821-2829` 对 2/4 split 再切分。
6. shadow render scheduling：`servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.cpp:1520-1613` 分离 directional、cube、positional 阴影，并把阴影绘制与 GI 安排到同一阶段。`2607-2795` 根据方向光 split 或 positional atlas key 计算 viewport/rect，`2808-2905` 收集、延迟并顺序提交 shadow passes。
7. filtering ABI：`servers/rendering/renderer_rd/shaders/scene_forward_lights_inc.glsl:312-455` 实现 directional/positional PCF 与 penumbra kernel；`410-450` 的方向光软阴影先查 blocker，再按 kernel 采样。资源绑定由 `render_forward_clustered.cpp:3382-3480` 以独立 positional atlas 与 directional atlas texture 绑定。

### 决策

- **Adopt（概念）**：shadow atlas 与方向光 atlas 分离；按 coverage/质量档选择槽位；版本化 dirty 判断；LRU 与短暂 reallocation tolerance；2/4 split 的显式排布；软阴影 kernel 与 shadow bias 的独立质量开关。
- **Adapt（独立实现）**：Noemancer 可以把 `owner/version/lastUsed/allocatedFrame` 的思想转为 plain-data `ShadowCacheEntry`，但不能携带 Godot RID、RendererRD、quadrant 容器或第三方 framebuffer。
- **Port（可选）**：只有在许可证 notice 和 Shader ABI 隔离可保证时，才选择性移植小段 PCF/penumbra 采样数学；现有 Noemancer Shader 已有自己的采样实现，不需要复制。
- **Reject**：本固定 Godot 快照中没有 VSM virtual page、page table、physical page pool、GPU page-request feedback 或 directional clipmap。不要把传统 atlas API 当成 VSM。

## Unreal Engine 5.8.1：VSM 的生产约束 oracle（禁止复制）

UE 证据用于回答“完整 VSM 要处理什么”，不是用于复制实现。相关文件受 Unreal EULA 约束。

### 页表、虚拟页和物理池

- `Engine/Shaders/Shared/VirtualShadowMapDefinitions.h:12-38` 固定了 128 texel page、128×128 level-0 virtual pages、最多 8 个 mip，并定义 page-table texture 的基础布局；`67-109` 定义 single-page map、invalid payload、statistics 和 overflow flag。
- `Engine/Source/Runtime/Renderer/Private/VirtualShadowMaps/VirtualShadowMapArray.h:67-89` 镜像 page/virtual address/HZB 常量；`157-190` 的 uniform fields 包括 full/single map 数量、最大物理页、pool/page-table 尺寸、场景帧号和 resolution bias。
- `VirtualShadowMapArray.h:212-240,497-539` 显示实际资源集合：`PageTable`、`PageFlags`、`PageReceiverMasks`、`PhysicalPagePool`、物理页 metadata、page request flags、uncached/allocated page bounds、dirty page flags、throttle/statistics buffers。这是系统级资源图，不是一个单独的 shadow texture。
- `VirtualShadowMapPageAccessCommon.ush:237-318` 定义 page-table entry 的物理地址、LOD offset、valid-for-rendering、secondary request 等编码；`356-368` 通过 virtual UV 查找最佳已映射页，允许向粗 mip 回退；`VirtualShadowMapProjectionCommon.ush:254-305` 对 directional clipmap 在更粗 level 继续查找并进行深度尺度转换。

### GPU 请求、粗页和页分配

- `VirtualShadowMapPageMarking.usf:67-143` 将 froxel/几何 footprint 投影到虚拟页矩形，并以 `VSM_FLAG_PRIMARY_REQUEST` 标记；`145-167` 可选写入 8×8 receiver mask。
- `VirtualShadowMapPageMarking.usf:314-347,470-545` 从 GBuffer/深度/法线像素生成请求，并对页边界做抖动膨胀，降低 disocclusion/边缘 miss；`499-527` 遍历方向光，`547-612` 处理 local lights。
- `VirtualShadowMapPageMarking.usf:796-873` 对 coarse clip level 显式请求粗页，方向光从 clipmap 中心附近标记少量页，避免远景所有页被请求。
- `VirtualShadowMapPhysicalPageManagement.usf:23-105` 用 LRU、available、empty、requested 四个 bounded list 管理物理页；`151-222` 将上帧 physical metadata 与本帧 VSM ID/page offset 对齐，失效标志从上一帧 request buffer 传递到物理页。
- `VirtualShadowMapPhysicalPageManagement.usf:236-473` 依据 request/age/invalidated 状态维护 static/dynamic cache，更新 `LastRequestedSceneFrameNumber`、requested/empty/LRU 列表以及 page flags。
- `VirtualShadowMapPhysicalPageManagement.usf:475-541` 为新 request 从 available list 取页、清理旧 page-table entry、写入物理页 metadata；没有可用页时保留未 backing 状态并把压力回传给 host，而不是无界分配。

### Clipmap、缓存失效和压力反馈

- `VirtualShadowMapClipmap.cpp:172-196` 显式配置 first/last level、coarse level、移动/静态 resolution bias、force invalidate、tight culling 和 receiver mask。
- `VirtualShadowMapClipmap.cpp:243-254,298-334` 根据相机投影/viewport 选择 level 与 resolution bias，并以 light/view 唯一键获取 per-light cache entry。
- `VirtualShadowMapClipmap.cpp:336-402` 将方向光 clipmap 原点和每个 level 的中心按 radius/grid snapping；`393-433` 把 page offset、Z 视域、WPO threshold 交给 cache entry。
- `VirtualShadowMapCacheManager.cpp:259-314` 对 clipmap 缓存设置 Z guardband；半径变化、`DeltaZ + LevelRadius > 0.9 * cachedRadius` 或 WPO threshold 变化时失效，否则通过 page offset 保留缓存页并平移映射。
- `VirtualShadowMapCacheManager.cpp:361-397` 在 light/cache key、force invalidate 或 receiver mask 模式改变时将条目标记为 uncached；连续移动对象优先走 uncached path，稳定后再建立 static cache。
- `VirtualShadowMapCacheManager.cpp:451-643` 收集 primitive add/update/remove，按 cast-shadow、dynamic/static policy、light radius 和 receiver-mask 策略过滤，再把 instance range 编码成每个 VSM page 的 GPU invalidation work。`Engine/Shaders/Private/VirtualShadowMaps/VirtualShadowMapCacheInvalidation.ush:43-165` 对已分配 page 的矩形执行 HZB 可见性测试并写 `OutPageRequestFlags`。
- `VirtualShadowMapCacheManager.cpp:676-749` 根据 free-page feedback 动态调整 resolution LOD bias；page pool 变成负数时明确报告 overflow 与 missing shadow artifact。`1089-1143` 显示 pool 尺寸、array size、flags 或 max physical pages 改变会重建 pool 并全量 `Invalidate`。

### 决策

- **Adopt（约束/观测模型）**：页表必须区分“有映射”“本级可渲染”“只找到粗级回退”“secondary request”；物理页必须记录 owner map/level/page address、generation/last-requested frame、dirty/uncached 状态；page pool 必须有硬上限；反馈必须有请求量、驻留量、失效量、miss、overflow 和 fallback 统计；moving caster 要进入 bounded uncached 或局部失效路径。
- **Adapt（独立实现）**：仅借鉴 page-table/physical-pool/cache-invalidation 的职责分层、clipmap 的整数 snapping、coarse fallback 和压力反馈。Noemancer 的页大小、资源格式、pass 顺序和字段名由自己的 Render Graph/SDL_GPU ABI 决定。
- **Reject（代码/Shader）**：`VirtualShadowMapArray.*`、`VirtualShadowMapCacheManager.*`、`VirtualShadowMapClipmap.*` 与 `Engine/Shaders/Private/VirtualShadowMaps/*` 均不得复制、改名或逐行翻写进 Noemancer。UE 的 Nanite/per-page dispatch、SMRT、reserved resource、GPU message 和 console-variable 体系不属于当前最小实现。

## CSM / local atlas 与 VSM 的边界决策

| 能力 | CSM + local atlas | VSM | Noemancer 决策 |
|---|---|---|---|
| 近景方向光稳定性 | 级联分辨率固定，配置/调试直接 | 只给被请求页高分辨率，需处理 request miss | CSM 保底；VSM 只做 opt-in experimental path |
| 远景覆盖 | 级联数量固定，远景浪费像素 | clipmap/mip 可用粗页覆盖 | 第一阶段只做单方向光 clipmap/页层；没有 coarse fallback 不可合入默认 |
| 多 local lights | atlas slot/array 简单可预测 | 每灯 virtual map + page pressure | 保持现有 local shadow array；VSM 初版只做 directional |
| 更新成本 | caster dirty 时重画整级联/slot | page request + physical tile render + invalidation | 先做 bounded CPU request/失效，再做 GPU feedback |
| 缺失/池耗尽 | atlas 分配失败可回到旧阴影或禁用 | miss/overflow 很容易出现黑斑 | missing/invalid 必须 CSM fallback 或 fully lit，不得错误加 shadow |
| 调试 | atlas/cascade visualization 直观 | page table、physical pool、age、dirty、overflow 多维 | Status/Observation 只暴露有界统计和 sample 页，不序列化整张页表 |
| 跨后端 | texture array + depth compare 简单 | uint page table、UAV/atomics/indirect dispatch 约束更强 | 先证明 DXIL/SPIR-V ABI，再扩展 GPU feedback |

## Adopt / Port / Adapt / Reject 总账

| 主题 | 决策 | 实施边界 |
|---|---|---|
| CSM practical split、light-space fitting、texel snapping | Adapt | 已有 Noemancer 实现继续作为 authority；仅用 Wicked 的对照证据做数值/画质验收。 |
| directional/local atlas rect packing | Adapt | 可吸收 Wicked 的多 viewport 与 Godot 的方向/位置 atlas 分离思想；使用 Noemancer own allocator。 |
| shadow version / LRU / allocation tolerance | Adapt | 以 plain-data `owner/version/lastUsed/allocatedFrame` 建模，不能引入 RID 或 `wi::rectpacker`。 |
| VSM page key / page table entry / physical tile metadata | Adapt | 仅实现最小 directional path；字段和位布局由 Noemancer contract 定义，不复制 UE constants。 |
| clipmap origin/level integer snapping | Adapt | 借鉴 UE 的几何约束；第一版单方向光、固定 bounded level，失败回 CSM。 |
| GBuffer/froxel request、page dilation、receiver mask | Adapt（后置） | 在 CPU page sampling 证明后再加，request overflow 必须可观测、可降级。 |
| GPU page pool/LRU/dirty invalidation | Adapt（后置） | 先用 CPU deterministic planner 测试，再实现 bounded compute passes；禁止无界请求。 |
| Wicked 的 CSM/atlas 小段数学/Shader | Port 可选 | MIT notice 可追踪且确有性能/正确性收益时才做；默认独立实现。 |
| Wicked VXGI/virtual texture 作为 shadow VSM | Reject | 不是 shadow page system，不能混用。 |
| Godot RD/RID/LightStorage/ShadowAtlas 容器 | Reject | 只借鉴策略，第三方类型封闭在参考仓，不进入 Noemancer。 |
| UE VSM 源码、Shader、Nanite/SMRT 绑定 | Reject | EULA；UE 只作生产约束 oracle。 |

## 下一批最小可实现纵切

以下是可直接交给实现批次的边界；本记录本身不执行这些代码改动。

### Slice A：`shadow.page-contract`

建立引擎内部、非持久化的 bounded plain-data contract：

- `ShadowPageKey`：`lightId`、`mapKind`（directional/local）、`level`、`pageX/pageY`、`epoch`。
- `ShadowPageState`：`requested`、`resident`、`renderableAtLevel`、`fallbackLevel`、`dirty`、`uncached`、`lastRequestedFrame`、`lastRenderedFrame`。
- `ShadowPageInvalidationReason`：light transform、camera/clipmap shift、caster revision、profile/resource resize、backend/device loss、manual/debug。
- `ShadowPageFallbackReason`：not requested、pool exhausted、non-finite projection、stale epoch、feedback overflow、unsupported backend。
- 统计最少包括：`poolCapacity`、`residentPages`、`requestedPages`、`newPages`、`evictedPages`、`invalidatedPages`、`missingSamples`、`fallbackSamples`、`poolExhaustions`、`feedbackOverflow`、`maxPagesRenderedThisFrame`。

退出证据：固定 JSON/结构化 status 的字段顺序和有限上限；同一输入 frame 序列得到相同 page keys/state；旧 epoch 的 sample 永不命中新页；NaN/负页坐标/越界页被拒绝并记录 fallback reason。

### Slice B：`shadow.directional-vsm-one-page`

只覆盖一个方向光和一个 clipmap/级联页层：

1. 复用现有 directional shadow camera/CSM fitting 生成一个 page-space projection；不要同时重写 local lights。
2. 建立有限物理 tile atlas 和页表。建议使用 profile 驱动的 page size/pool dimensions，不将 UE 的 128 页常量写死为兼容承诺。
3. 先由 CPU 从固定 receiver rectangle 生成有界 request，最多 `N` pages/frame；每个 page 以现有 shadow depth vertex/material path 渲染到 tile，保留至少一个 texel gutter，避免 atlas filter bleed。
4. scene-lit 采样先查本级 page，再查显式 coarse/CSM fallback。page absent、page not renderable、epoch mismatch 或投影非有限时必须走 CSM/fully-lit fallback，不写入“暗阴影”。
5. 光/相机/caster revision 改变时只使受影响 page dirty；尚未有可靠 bounds 时宁可 bounded 全量失效并测量，而不是使用可能漏失的局部失效。

退出证据：

- RenderLab 固定场景中，VSM off/on 的 CSM baseline 可对照；VSM on 的 page table、resident tile、rendered page count 和 fallback count 均出现在 status。
- 相机小幅平移只造成有限 page remap，陈旧 epoch 不可见；大幅跳转/光源旋转触发可解释的 full/region invalidation。
- 人为将 pool 限制到不足以覆盖请求时，画面仍是 CSM fallback/fully lit，不出现错误黑块；`poolExhaustions` 与 `fallbackSamples` 增加。
- Shader 在 DXIL 与 SPIR-V 上通过现有 manifest/reflect ABI；odd extent、页边界、非有限投影、空 scene 不崩溃。

### Slice C：`shadow.vsm-gpu-feedback`

只有 Slice B 证明了页查找和 fallback 后才进入：

- 从 depth/normal/GBuffer 或 froxel 生成方向光 page requests；request 数、边缘 dilation、receiver mask 都有质量档和硬上限。
- GPU compact/allocate 可替代 CPU request，但必须保留 deterministic debug/readback 采样和 overflow 统计。
- physical pool 使用 bounded available/LRU/requested lists；重新分配前清空旧 page-table entry；page metadata 保存 owner/level/address/epoch/lastRequestedFrame。
- dynamic caster 初期允许 uncached page；稳定后才尝试 static cache。不要在这一批引入 UE 的 Nanite/SMRT/预过滤远景页。

退出证据：GPU request 与 CPU reference planner 在固定小场景上 page-key 集合一致或差异有界；feedback overflow、pool exhaustion、cache invalidation storm 均能被结构化观察；D3D12/Vulkan hidden capture 证明没有 backend-specific stale page 或跨 tile bleed。

## 失败模式与明确护栏

| 失败模式 | 必须的行为 | 不能做的事 |
|---|---|---|
| physical page pool exhaustion | 增加 `poolExhaustions`，保留 coarse CSM/fully-lit fallback，按质量档降低 request | 不扩容到无界、不把未初始化 tile 当深阴影 |
| request/feedback overflow | 丢弃低优先级请求，保留粗级/CSM fallback，记录 overflow | 不静默截断、不把丢失 request 解释为“无阴影” |
| stale page / epoch mismatch | 拒绝 sample，走 fallback；必要时清理 page-table entry | 不沿用旧 light/camera/page 的深度 |
| clipmap 相机平移 | 使用整数 page offset；越出 guardband 时局部/全量失效并记录原因 | 不用浮点累积 offset 造成长期漂移 |
| light/caster revision 变化 | 只在有可靠 bounds 时局部失效；否则 bounded 全量失效 | 不声称局部失效但漏掉 caster |
| cache thrash（移动灯/动画物体） | 进入 uncached path 或 CSM；限制每帧重绘页数 | 不反复建立/销毁 static cache 造成尖峰 |
| atlas gutter/filter bleed | page/tile 包含显式 border/gutter，采样 clamp 在本页 | 不跨邻页线性过滤 |
| non-finite projection / invalid coordinates | page request/sample 失败，记录 `non-finite-projection`，保持可见/回退 | 不把 NaN 转成任意纹理坐标 |
| profile/backend/device reset | bump epoch，释放/重建私有页池，回 CSM 直到资源 ready | 不复用旧 GPU resource handle |
| missing page in shadow evaluation | 对阴影而言“未知”必须不额外变暗：优先 CSM，没 CSM 时 fully lit 并记录 miss | 不能用 occlusion 的“保守可见”语义直接误写成 shadow=0 |

## 观测与 AI/Agent 暴露边界

VSM 的页表和 physical pool 可能很大，不能把整个 GPU texture 每帧文本化。建议 Semantic Observation Graph 只暴露：

- 当前 renderer/shadow mode、profile、page pool capacity/usage/pressure；
- 选中的 light/page 的 stable identity、level、virtual address、physical tile、state、epoch、last requested/rendered frame；
- bounded sample（例如按 selector 取不超过 64 页）和按 reason 聚合的 counters；
- `fallbackActive`、`fallbackReason`、`pageMissRate`、`poolPressure`、`invalidationsLastFrame`；
- image/depth debug artifact URI，而不是把原始 GPU handle 或整张页表放入 RPC。

命令层仍然需要封装“重建 shadow pages”“冻结页池”“切换 CSM/VSM”“导出 page diagnostics”等高阶动作，但每个动作必须走现有 dry-run、revision/epoch、receipt 和 undo/rollback 约束。文本可读性不能替代资源边界与 mutation safety。

## 最终采用结论

- 当前默认：**Adopt CSM + local atlas baseline**，继续用真实 RenderLab 压力场景衡量稳定性、shadow ms、atlas occupancy 和画质。
- 下一步：**Adapt VSM data model and one directional page vertical slice**，仅在 bounded pool、CSM fallback、deterministic status、双后端 Shader ABI 证据齐全后开放实验开关。
- 后续按证据推进：**Adapt GPU feedback/receiver mask/cache**；如果 page pressure 不能证明比 CSM 带来可接受的远景覆盖或多灯成本收益，则 **Reject full VSM**，继续强化 CSM/local atlas、caster LOD、cascade fitting 和 temporal filtering。
- 许可证：Wicked/Godot 的 MIT 仅允许在 notice 可追踪的范围内选择性 Port；UE 仅为 EULA 约束 oracle，禁止复制实现。

本记录不改变 `docs/current-state.json`、`docs/architecture.md` 或 `docs/development-plan.zh-CN.md` 的权威状态；主 Agent 若采纳上述切片，应在权威计划中单独记录切片名、owner、代码边界和退出证据。
