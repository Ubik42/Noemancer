# Noemancer 当前开发计划

> 状态：Current
> 更新日期：2026-08-25
> 权威范围：当前目标、执行顺序、退出条件和验证层级。
> 历史进展不保存在本页；旧版长日志可从 Git revision `3e15f66^` 查阅。
## 产品目标

先把 Noemancer 做成一个自洽、可独立创建和编辑项目的通用引擎，再用 `D:\3D\NoemancerProjects\NoemancerPlatformer` 平台跳跃工程验证 2D 游戏生产闭环，最后扩展 Hybrid Pixel / HD2D Profile。游戏规则应位于项目 C#，不能为了快速演示固化进引擎 C++。`game.lumen-run` 与 `lumen.*` 稳定 ID 暂作兼容身份；旧证据中的 “Lumen Run” 均指同一工程，不再作为产品或目录名称。

Noemancer 的差异化集中在：

- 权威状态天然具备稳定身份、Schema、来源、revision 和可查询语义；
- 人类 GUI、项目脚本、CLI、Agent 和测试共用领域命令与事务，不维护平行模型；
- 编译、运行、渲染和调试产生有界、可关联的结构化证据；
- 成熟中间件负责通用执行内核，引擎负责稳定领域模型、适配、编辑体验与 Agent 控制面。

## 当前执行前沿

### 当前主线：商业渲染强化与公开场景验证

第一阶段通用生产闭环、编辑器基础、Cook/Package/Player、Agent Authority 和 Hybrid Pixel 纵切已经成立，但这不等于引擎只剩边角料。渲染仍有商业化能力缺口，因此产品优先级从 UI 收尾切回 Rendering；高 DPI、可再分发多语字体和 cluster-aware 编辑保留在队列中，不删除也不与渲染批次混写。

独立验收工程 `D:\3D\NoemancerProjects\NoemancerRenderLab` 是渲染真实性客户，不属于引擎源码，也不建立第二套 Scene/Asset Schema。它由正式 Project Workspace Authority 创建，当前包含：

- 内置几何体 Raster 基线：PBR 金属度/粗糙度矩阵、方向/点/聚光、方向与局部阴影、HDR 自发光、Tone Mapping、深度与纹理采样；
- Khronos CC0 经典资产 Gallery：MetalRoughSpheresNoTextures、BoomBox、Lantern，模型、上游 Metadata、License 和 SHA-256 一并固定；
- 大型场景候选矩阵：Intel Sponza 与 Amazon Bistro 只按需下载；当前 Khronos/Crytek Sponza 因上游来源许可争议不得进入默认公开 Fixture。

渲染批次的顺序与退出条件：

1. `commercial-raster.render-lab-classic-scene-contract` 已退出：`generated/acceptance/render-lab-classic-scene-20260825-final/` 固定 1440×900 机位、三项 Khronos CC0 GLB、项目/场景/Registry/素材 SHA-256、质量 sidecar、Render Graph/Shader 指纹与 D3D12/Vulkan 双后端；Release Package Player 两后端 CPU Frame p95 为 1.56/1.41 ms，均为 3 Cooked Load、0 Source Decode、0 Offline Compile，包内源模型为 0。该历史证据生成时 SDL_GPU 尚无 timestamp API；后续 `performance.gpu-pass-timestamp-foundation` 已用固定补丁补齐真实 GPU Pass 计时，不能回写或伪装历史数据。
2. `asset.gltf-external-resource-jpeg-and-large-scene-readiness` 已退出：Adopt 官方 libjpeg-turbo 3.2.0；统一 PNG/JPEG RGBA8 Adapter、缩略图与 KTX2 Texture Cook；JSON `.gltf` 的文档、外部 buffer/PNG/JPEG 形成有界不可变依赖闭包、稳定 SHA-256 closure/recipe/cache identity、隔离 staging 和变更失效。真实三角形 JPEG Cook→`.meshbin`→cache-hit、128 外部依赖压力、旧计划拒绝、Release Package 复合许可证原文均通过。当前主机缺 NASM，JPEG 功能成立但 SIMD 明确未启用，不据此声称最佳解码性能。
3. `performance.editor-retained-ui-command-recording` 已退出：`noemancer.performance-evidence/0.1` 现在持久拆分 Thumbnail Sync、ImGui Build、四个 Retained Surface、ImGui GPU Record，以及 Refresh/Chrome/Scene/Animation/Outliner/Inspector/Assets/Console/Agent Context。分段证明 1 FPS 退化并非 RmlUi 或 Render Graph，而是大型 glTF 被三个不匹配的专用资产面板各自完整 Inspect/Decode；类型门禁移到昂贵检查之前后，同一 RenderLab、同一 1600×900 Release D3D12、未降低场景/画质/面板的 Frame p95 从 943.35 ms 降为 7.18 ms，Command Record p95 从 937.28 ms 降为 3.57 ms。修复后证据位于 `generated/acceptance/retained-ui-after-type-gate-final-20260825/`。
4. `performance.gpu-pass-timestamp-foundation` 已退出：固定 SDL 3.4.14 小补丁在后端内部拥有 D3D12/Vulkan query pool，runtime-private Adapter 提供稳定 Pass ID、三帧环、批量 resolve、fence 延迟读回、availability/overflow/null 合同；D3D12 与 Vulkan 各 60/60 采样帧均取得数值结果且零 skipped slot。普通帧不获取证据 fence，PresentMon 仍是独立的呈现遥测。
5. `render.dynamic-sky-atmosphere` 已退出：engine-owned `noemancer.sky-atmosphere/0.1` 与 `noemancer.sky-environment/0.1` 分别拥有物理大气和昼夜/气溶胶天气语义；Project `0.2`、Package Game Profile、Player 与 Editor/Agent 观察消费同一 plain data。太阳时钟只由显式 delta 推进，不读取系统时间；Runtime 以 10 Hz 累计更新太阳视图，介质 LUT 与太阳相关的 sky-view/camera-volume identity 分离，避免昼夜循环重算 transmittance 与 multi-scattering。Render Graph v13 运行 transmittance、multi-scattering、sky-view 与 32×32×32 camera volume；Perspective 和 Orthographic 分别使用透视射线与平行射线，Opaque 后的 Aerial Pass 均应用 `scene * transmittance + radiance`。Low 档使用无纹理、无 sampler、固定 O(1) 的 analytic 单次散射近似；高档 LUT 不可用时也显式退到该路径。Project Settings 的物理大气面板仍具备 validation、revision CAS、dry-run、原子持久化与 undo/redo；昼夜/天气当前通过严格 Project Manifest 与语义快照表达，尚无独立 GUI/MCP 事务，不据此宣称完整天气编辑器。D3D12/Vulkan 1920×1080 隐藏画面合同位于 `generated/acceptance/sky-atmosphere-d3d12-current/` 与 `sky-atmosphere-vulkan-current/`，两者画质、四 LUT、Camera Volume、Shader Manifest 均通过且稳定场景各只重建一次；Orthographic 与 analytic-low 另有隐藏探针。历史 960×540 GPU timestamp 数据仍只适用于原固定 workload；新验收脚本会把 Runtime 性能模式未真正报告 1080p 的情况判失败，避免伪称 1080p GPU 性能。
6. `render.screen-space-hiz-history-and-temporal-denoising` 已退出：Render Graph v14 在 opaque 后真实调度 RG32F linear view-depth min/max seed 与完整 ceil-half mip reduction；`noemancer.temporal-history/0.1` 以 TAA/SSR/SSGI/RTGI 独立 slot、revision、plan/begin/commit、稳定 identity 和 reset reason 取代 Scene Renderer 内的单布尔历史。共享 Temporal Resolve 同时写 resolved color、非 debug color history、depth history 与 normal history，并以 motion/depth/normal/reactive/disocclusion/neighborhood clamp 决定 history weight；Hybrid Pixel 仍明确禁用历史并走当前帧空间 resolve。Renderer Status v26 发布 `noemancer.screen-space-foundation/0.1`。Release 1920×1080 的 D3D12/Vulkan 隐藏画面与 60 帧 GPU timestamp 证据分别位于 `generated/acceptance/screen-space-foundation-direct3d12-20260825-144925/` 和 `screen-space-foundation-vulkan-20260825-144925/`：12 mip、21.095 MiB，Seed p95 0.0266/0.0226 ms、Reduce p95 0.0512/0.0647 ms、Temporal Resolve p95 0.0891/0.0932 ms。性能模式现在直接拥有请求的固定 1920×1080 Surface，不再把 Editor Dock 内的 1172×629 误报为 1080p。
7. `render.ssr-production-path` 已退出：Render Graph v15 在 Opaque 与共享 TAA 之间真实调度 hierarchical trace、独立 temporal resolve 和 IBL-specular-only composite；Mesh 与 lit Sprite 输出独立 specular-indirect 与 reflection-properties MRT，miss 保留原 split-sum IBL。Trace 使用共享 RG32F min/max HiZ、朝向相机的法线、表面自交剔除、可见面 crossing 与 binary refinement；Engine plain-data 合同提供 Off/Low/Medium/High、thickness、roughness cutoff、步数与 Hybrid Pixel 禁用策略，Runtime 只拥有 GPU Adapter。Renderer Status v27、`--ssr-debug final|confidence|hit-distance|roughness|miss|normal` 与独立 History slot 形成可观察边界。严格 Release 1920×1080 双后端证据位于 `generated/acceptance/ssr-v15-20260825-153029/`：D3D12/Vulkan Reflection ROI mean absolute delta 均约 0.00614、changed fraction 均约 0.581、控制区为 0；Trace p95 为 0.183/0.192 ms，Temporal p95 为 0.061/0.499 ms，Composite p95 为 0.030/0.033 ms。捕获使用显式关闭自动曝光的确定性开关，避免 A/B 被曝光历史污染。
8. `render.ssgi-production-path` 已退出：Render Graph v16 在 AO composite 与 SSR 之间真实调度 hierarchical hemisphere gather、cross-bilateral spatial resolve、独立 temporal resolve 与 diffuse-IBL-only composite。Gather 复用共享 RG32F min/max HiZ 粗筛并以全分辨率 scene depth 为命中 authority；Mesh/lit Sprite 共用 `normal.rgb+roughness.a+baseColor.rgb+metallic.a` 材质合同，miss/offscreen 保留原 IBL diffuse。Engine plain-data 提供 Off/Low/Medium/High、8-ray/8-step 高档有界预算、4 world-unit 局部半径、bent-normal/visibility、Hybrid Pixel 禁用和独立 History slot；公开样本/方向/步数上限与 Shader 的 16/16/32 编译边界一致，不允许静默截断。Runtime 暴露 `--ssgi-quality`、`--ssgi-debug final|confidence|visibility|bent-normal|miss`、Renderer Status v28 与四段 CPU/GPU 可观测时间。严格 Release 1920×1080 双后端证据位于 `generated/acceptance/ssgi-v16-20260825-161938/`：118 项检查全部通过；D3D12/Vulkan GI ROI mean absolute linear-luma delta 为 9.606e-5/9.613e-5、changed fraction 为 4.529%/4.530%，控制区均为 0；Gather p95 为 1.870/2.511 ms，Spatial 为 0.452/0.067 ms，Temporal 为 0.075/0.081 ms，Composite 为 0.038/0.039 ms。证据固定曝光并要求 60 帧真实 GPU timestamp，CPU 时间不能替代。
9. `render.bindless-gpu-scene-and-occlusion-decision` 已退出：Adopt 现有共享 RG32F HiZ 作为下一帧 GPU visibility 的保守遮挡输入；GPU Scene 继续复用稳定 Draw/Resource identity、dirty-range publication 和 indexed-indirect，不建立第二份 Scene；bindless 则因当前 stable batching 尚无 descriptor-bound 证据而延后。Render Graph v17、Renderer Status v29 和 `noemancer.gpu-visibility-readback/0.3` 已形成真实闭环。最终 Release 1920×1080 双后端证据位于 `generated/acceptance/gpu-occlusion-v17-20260825-final2/`：D3D12/Vulkan 均为 1,034 candidates、128 frustum culled、897 HiZ tested、896 HiZ culled、10 accepted/drawn，控制物存活且 GPU 集合是 CPU 视锥集合的保守子集，完整性错误为 0。修复了 Vulkan 统计缓冲跨帧累积并新增帧内闭合门禁。压力场景 CPU Frame p95 为 131.75/154.67 ms，明显未达 60Hz；该数值作为后续优化基线，不能被写成性能达标。
10. `render.shadow-scalability-vsm-decision` 已退出：新增 Engine-owned `noemancer.shadow-scalability-policy/0.1`、有界 `noemancer.shadow-page-contract/0.1` 实验合同和真实 `noemancer.shadow-scalability-stress/0.1` 场景。固定 1920×1080、1,024 caster、六个本地阴影请求的隐藏 D3D12/Vulkan 证据位于 `generated/acceptance/shadow-scalability-v30-20260825-stress2/`；两端均显示 1,024 caster、请求 6/选择 3/丢弃 3，GPU Shadow p95 为 0.003072/0.000891 ms，Engine 决策均为 `extend-atlas`。CPU Frame p95 为 42.16/41.57 ms，未达 60 Hz，证据据实失败而未缩负载。结论是保留 CSM/local atlas 生产 fallback，优先扩展/优化 atlas 与整体 CPU 帧；Virtual Page/VSM 仅保留 plain-data 原型边界并延后，不把合同冒充为渲染实现。
11. 当前位于 `render.native-rhi-raytracing-foundation`，随后是 `render.rtgi-production-path`。Engine 能力/所有权与统一执行回执合同已成立；Runtime 已在 RTX 4080 上分别通过真实 D3D12 DXR 1.1 与 Vulkan RT 创建硬件 Device、三角形 Vertex Buffer、BLAS scratch/result、单实例 TLAS scratch/result，并完成 Barrier、Queue Submit、Fence Wait 和资源释放。该纵切仍是短生命周期 Build Probe，没有持久 Native RHI、SBT、Trace Dispatch、Render Graph 光追节点或可见 RT 画面，因此公开状态继续保持 `nativeRhiReady=false`、`rtgiReady=false`。恢复开发后的下一批是把两端执行事实投影到统一 Receipt，建立持久设备/资源所有权、SBT、最小 Trace/Readback 与 Raster fallback，再进入 RTGI。每项高级效果必须同时给出真实节点、可观察参数、固定场景 A/B、GPU/显存预算、历史重置、跨后端结果和明确降级；只写 Shader、结构体或测试桩不计完成。
### 参考实现驱动的开发门禁

渲染、物理、动画、音频、资产和平台内核不再默认从零自写。每个高级批次先精读固定提交的成熟实现并形成 adoption record：精确仓库/提交/文件、根许可证和二级 Notice、`Adopt | Port | Adapt | Reject`、关键修改、Artifact ID 与验证证据。优先 Adopt 独立成熟中间件；对许可兼容、输入输出可切断的算法和 Shader 直接 Port；对绑定外部 RHI、全局状态或资源系统的实现 Adapt 其 pass 分解、数据布局、质量档和 failure mode；不能关闭许可、维护或目标 workload 的方案 Reject。

当前固定代码级地图见 [参考实现驱动的高性能渲染计划](research/2026-reference-driven-render-performance-plan.zh-CN.md)。Wicked Engine/Godot 允许按各自 MIT 与二级 Notice 合规移植；原则是“Port shader/math，Adapt orchestration/data ownership”。Unreal Engine 只用于 RDG、VSM、Lumen、TSR、GPU Scene 等生产边界研究，禁止复制受 EULA 管辖的源码、Shader、宏和类型。每项效果必须从公开 RenderLab Project 走到真实 Render Graph、固定 A/B、双后端证据和性能合同；不得靠降低对象、分辨率、灯光、采样或材质复杂度伪造收益。

### 已完成基底：生产后端去临时实现

miniaudio Resource Manager/Streaming、fastgltf/ufbx 离线语义适配、libjpeg-turbo、KTX2 BasisLZ/UASTC 与 meshoptimizer Mesh Cook 已接入。Asset Registry 会把 GLB、外部资源 JSON glTF 与 FBX 从不可变源闭包 Cook 为内容与配方共同寻址的 `noemancer/meshbin/0.2`，也会按 base-color/normal/data/emissive/UI 语义把 PNG/JPEG 编码为 KTX2；Player 直接校验并加载 Cooked Geometry，不带源模型解析或离线编译路径。Runtime 通过私有 libktx Adapter 校验并转码为可移植 RGBA8 上传。第三方类型仍封闭在 Adapter，Headless 参考 Mixer 与 Editor 源模型预览只作为明确边界内的路径，不进入发行包。

## Codex `/goal`（Ralph Loop）快速开发模式

当前 `/goal` 使用稳定目标，不在 Prompt 内复制会迅速过期的切片名或性能数字；瞬时队列只由 `docs/current-state.json.currentFrontier` 表达。可直接使用的目标文本如下：

> 持续把 `D:\3D\_tools\Noemancer` 推进为可由人类与 AI Agent 共同创建、编辑、调试、运行、打包和发布真实游戏的高性能通用引擎，并以 `D:\3D` 下的独立项目，尤其 `D:\3D\NoemancerProjects\NoemancerRenderLab`，作为公开产品路径的真实性客户。每次恢复必须先完整读取仓库 `AGENTS.md`、`docs/current-state.json`、`docs/architecture.md`、`docs/development-plan.zh-CN.md` 与 `docs/first-acceptance-status.zh-CN.md`，检查工作树并优先收口已经开始的连贯批次；不得把继承的修改丢下后另开同领域实现。瞬时队列只认 `currentFrontier`，旧对话、历史研究、已完成切片和旧暂停文字都不是当前指令。
>
> 从 `currentFrontier` 首个未阻塞项选择最大连贯、可审查的通用引擎批次。若队列为空，不得把 Goal 当作完成：依据本计划与能力状态复审真实产品缺口，按“通用生产闭环与性能/商业画质 → 编辑器作者体验 → 动画/物理 → Gameplay/VFX/音频/网络 → Agent 深度接入 → HD2D 专项”的依赖顺序，把下一组有明确退出条件的切片写回 `currentFrontier`。游戏与 Demo 只是通用能力的客户，不得把项目专用规则写进引擎，也不得为了长期停留在渲染而遗忘其余模块。各领域默认先做 build-vs-buy 与参考实现检索：优先 Adopt 成熟中间件；Port 许可兼容且可隔离的算法、Shader 和数据布局；Adapt 深度耦合实现的 pass 分解、资源策略、质量档和 failure mode；不能满足许可证、维护或目标 workload 时 Reject。每次高级渲染实现前记录精确上游提交/文件、许可证、修改和验证。Wicked/Godot 可合规移植并进入许可证台账；Unreal Engine 只研究生产约束，禁止复制其受 EULA 管辖的实现。第三方类型封闭在 plain-data Adapter 后，不得进入 Scene、Prefab、项目 C#、Semantic State Plane 或公共 RPC。
>
> Sol 主代理负责架构、共享接口、集成、权威文档、最终验证和 Git 边界；存在互不重叠且并行收益高的工作时，主动启用最多三个 `luna_worker` 实际开发 lane。优先多个 Writer 加至多一个解除关键不确定性的 Research lane，不用三个只读审计填满并发，不让主线程空等。共享 CMake、公共 Schema、World、Renderer/Render Graph/Editor 集中点和权威状态默认由 Sol 串行集成。
>
> 保持 `noemancer_engine <- noemancer_editor <- runtime`。GUI、CLI、Agent、脚本与测试共用领域 Authority；公共写操作逐步具备 Plan/Apply/Receipt、revision、dry-run、事务与 undo。游戏规则留在项目 C#。每项渲染能力必须进入真实 Render Graph 和公开项目路径，并提供稳定身份、参数/质量档、history reset、debug view、unsupported/fallback、固定机位 A/B、D3D12/Vulkan 证据、CPU 成本、可用时逐 Pass GPU 时间和显存语义。只有 Shader、结构体、测试桩或未启用分支不算完成；没有 GPU Timestamp 不得宣称 GPU 改善；不得缩减 workload 伪造优化。
>
> 每个 Ralph Loop 执行：读取真实执行路径与固定参考源码 → 划分独立 lane → 累积 coherent subsystem batch → Sol 审查 diff 与许可证/provenance → 用 `scripts/engine.ps1` 编译受影响目标 → 运行最小相关测试和直接探针 → GPU 变化时以隐藏进程捕获且不使用 Computer Use → 原地改写过时权威状态 → governance audit → 形成一个语义完整提交并在远端可用时推送 → 继续下一未阻塞项。不逐文件编译，不机械增加测试，不因单个慢测试重复全量套件；只有公共 Schema、共享 World/Runtime、构建依赖、跨 RHI 或里程碑才跑全量，Agent ABI 变化才增加 MCP smoke。测试和证据用于阻止高风险回归，不能取代实现本身或反复消耗整个批次；测试必须无交互、无 CRT 弹窗，证据进入 `generated/`。
>
> 保持 dirty worktree 安全，不覆盖、不 reset、不 checkout 用户或并行 Agent 修改。持续推进，仅在用户明确停止、形成值得用户实际打开编辑器/游戏验收的重大里程碑、确需新产品/架构授权，或同一外部阻塞连续重复且不存在安全替代路径时停止；单个子系统完成、普通实现选择、旧任务残留、暂时无 GPU telemetry、单个测试失败或子 Agent 等待都不是自动暂停理由。每次恢复都继续同一个 Goal，不创建第二个 Goal，不输出“保持暂停”式空转信息。

当前长期开发由 Codex `/goal` 持续恢复，并采用批量 Ralph Loop，而不是每改一个文件就停下来测试或汇报：

1. 从 `current-state.json.currentFrontier` 的首个未阻塞项开始，读取真实代码路径和相关 ADR，不重新翻阅全部历史研究。
2. 用户已对本仓库授予主动委派子 Agent 的持续明确授权。主代理划定一个可审阅的子系统批次并负责架构决策、跨模块接口、合并与状态写回；子 Agent 调度属于主代理的默认调度职责，不设置“等待用户逐轮明确授权”的门禁，也不要求用户在每轮重复授权。只要存在互不重叠、边界明确且并行收益高于协调成本的任务，就主动启用最多三个 `luna_worker`；不可拆分的关键路径仍由主代理直接实现。并行的目标是缩短墙上时间而非节省团队总 Token；worker 未在合理 checkpoint 内落盘时中断接管，不让主线程空等。
3. 同批次允许累计多个相关改动。只有 API/依赖不确定会让后续工作建立在猜测上时才提前编译；否则在集成边界统一编译受影响目标、运行最小相关测试集和一个必要探针。
4. 主代理审阅真实 diff，解决跨 Lane 接口问题；Worker 的完成报告不直接等于工程完成态。
5. 满足本项退出条件后，原地更新本页、`current-state.json` 与能力状态中的旧描述，并立即推进下一个未阻塞批次。Git 保存流水历史，权威文档只保存现在时。

互不依赖的后台资产 Job、Play World 差异、调试 Transport 和打包清单可以在写集隔离时并行；共享 CMake、公共 Schema、编辑器状态汇合点和最终 Runtime 接口由主代理串行集成。开发期 Sol/Luna 角色不属于引擎内 Runtime Agent、Semantic State Plane 或 MCP 公共 ABI。

### P1：已完成——引擎生产闭环收口

- 已完成的 P1 基础：Asset Browser 已接入稳定 ID、状态、进度、取消/重试和有界观察的后台 Job；后台 Import/Inspect 调用真实 Registry/Importer，结果以 live revision 校验回写；缩略图复用 lodepng 生成内容寻址 PNG 并由有界 SDL_GPU Cache 显示；缺源、缺依赖、循环与 Import/Cook 失败会形成确定性诊断和修复入口。Play World 具有独立的运行时 Outliner/声明式 Inspector、稳定实体/组件差异、逐项选择预览、基线冲突检查、dry-run、单次原子 Apply Back 与 Undo；托管调试已有隐藏进程、长寿命 DAP Session、请求关联、事件队列和断点/暂停/调用栈/终止协议；Package Pipeline 已具备 Game Profile、资源闭包、许可/NOTICE、确定性计划与原子提交契约。
- Asset/Cook 的 PNG/JPEG Registry/KTX2 生产接线已收口；Asset Browser 两种格式的真实缩略图与修复纵切已收口。WebP/EXR 等格式仍按真实项目需求接成熟解码器，不自研通用图片库。
- Play World：运行时查看和选择性 Apply Back 已收口；运行态语义投影有界发布，选择集合按稳定 change ID 重建候选 Scene，且只提交可持久化 Scene 差异，排除 subsystem 私有瞬态。
- Project Workspace：Editor 已可 New Project / Open Project；创建服务以 sibling staging + atomic rename 生成 Project、规范 Scene、引用内置起步资源的 Registry、可直接编译的 .NET 10 项目和 GameEntry。默认 `starter` 与 `hybrid-pixel` 是同一请求中的稳定 plain-data preset；CLI `project create --preset` 与 Project Hub 共用该 Authority，Hybrid preset 原子生成合法 Project `0.2` + Hybrid Pixel `0.1`，非法 preset 在 staging 前失败且不留目录。切换项目会重建 Scene、Registry、脚本上下文并重置 Editor 保存基线；退出脏 Scene 使用 Save / Discard / Cancel 协调器。
- C# 作者循环：Scripting Authority 发布有界的项目源码目录和编译指纹；Console 可打开项目、预览或交给外部代码编辑器打开项目内源码，Runtime 会规范化并拒绝越界路径。源码预览不再隐式改写断点。Editor 的 Debug 编译完成事件会在主线程安全点准备独立 Play World 热替换，下一次托管回调通过既有 collectible ALC 迁移状态；失败时保留旧程序集并保持 dirty，同一失败指纹不会触发自动重试风暴，手动重试仍可用。
- Cook/Package：Editor 已有 Game Profile、输出路径、Validate/Build 和异步状态；正式 Build 总是先编译所选 Debug/Release C# 配置，再为当前启动场景生成确定性 Cook Plan，允许内容缓存命中但不允许选择过期 Manifest。Windows Player 通过 sibling staging + atomic rename 分发；双击包内重命名 exe 会自动读取 Game Profile，只呈现游戏画面，并从包内加载预编译项目程序集、ManagedHost 与可选项目 HUD。Lumen Run 已完成真实原子分发、Headless 脚本运行以及隐藏 D3D12 Player 捕获；新建的零外部素材项目也已完成 Release 分发和项目代码执行探针。
- 真实 Lumen Run PNG 首次 KTX2 Cook 约 51 秒，完全缓存命中的 Debug 整包为 4.36 秒；这是两类独立预算。Runtime 按设备能力选择 BC7 或完整 RGBA8 回退；首帧前只物理分配并提交最小四级 tail，后续由屏幕占用、可见性老化、滞回、作者重要性/优先级、上传预算和驻留预算共同决定 replacement mip tier。资源替换与 GPU submit 事务化，失败可回滚；Sprite 以稳定 Asset ID 解析当前资源。
- 外部 Player 调试已收口：Editor 从 Debug Game Profile 分发目录启动独立 Player；ready/release 双事件把它停在 CoreCLR/项目程序集已加载但 Scene 脚本尚未创建的位置，netcoredbg 完成 attach、初始源码断点和 configurationDone 后才放行。Player 由 Runtime 进程控制器和 kill-on-close Job Object 显式拥有，DAP 只安全 disconnect；官方适配器已在 Lumen Run 包体命中 `CourierController.OnCreate` 并返回调用栈。
- 生产循环闭环：项目创建/打开、新建资产无关 Scene、项目内有界恢复候选、可发现的 C# 编辑入口、Play World 状态迁移热重载、独立 Player 调试、保存与 Windows 原子分发均已收口。Lumen Run 连续自动验收完成源项目 Headless、Release C# 编译/Cook/打包和包内 Player 运行；编辑器声明式可发现性与 Scene 保存/恢复由同批聚焦测试覆盖。P1 不再扩写功能。

退出条件：用户可像使用常规编辑器一样创建/打开项目、编辑场景与资产、编写并热重载 C#、运行和调试、保存并打包；无需修改引擎源码。

### P2：已完成——Editor Experience 基础

- 正式产品入口已从游戏验收快捷方式中分离：`Noemancer Editor.cmd` 从自身位置解析仓库、按需调用统一构建入口，并可无项目打开 Project Hub 或直接打开指定项目。Project Hub 使用自有平面矢量标识，提供 Open/New/Empty Workspace 与有界最近项目状态；同一状态以 `noemancer.startup-hub/0.1` 语义投影暴露，项目动作继续进入既有 Workspace Authority。无项目入口不再注入 VFX 调试粒子。Platformer 两个 `.cmd` 仅保留为验收项目入口。
- 保留 Dear ImGui 作为 Dockspace、Profiler、Render Debugger 和低层诊断壳；不把当前换色 ImGui 首版误认为最终产品 UI。
- 建立 Editor 专用视觉 token、字体/图标、表面层级、状态色、控件密度和一致的 hover/focus/disabled/error/success 反馈；中央 Scene View 保持最大工作面积，Outliner、Inspector、Asset Browser 和底部工具服务当前选择与制作循环。
- 首批收口应用顶栏、Edit/Play/Paused/Build 状态、场景工具栏、面板标题/搜索/空状态与 Inspector 信息层级；去除 `Bootstrap`、`GPU Scene` 等面向开发夹具的产品文案。
- 完成证据：现代壳层以及 Outliner/Inspector/Asset Browser 三大核心面板的首轮产品化已成立；搜索、authority、选择、组件/资产数量和后台 Job 进入同一有界语义投影。Edit Inspector 与 Outliner 已实际迁移为独立 Retained Surface：两者使用各自的 SDL_GPU 目标，尺寸随 Dock 内容区变化，窗口/Surface 指针、键盘和 IME 坐标显式路由。Inspector 控件仍从同一 Semantic UI binding/constraints 生成并进入 World plan/apply/receipt；Outliner 直接投影 Edit/Play World 的非持有 Authority View，以稳定实体 ID 构建父先于子的有界 Tree，点击与 Up/Down/Home/End/Enter 选择均回到 EditorUi 唯一 Selection Authority。局部焦点、选择和折叠可跨投影刷新保留，但不伪造 World transaction；4096 实体语义上限与 2048 DOM 节点上限被明确报告，当前不是大世界虚拟化实现。顶栏、Scene 工具栏和 Outliner 操作采用不依赖字体私有区的统一矢量图标，Dock 临时菜单三角已移除。隐藏 D3D12 整窗证据已在真实 Platformer 工程确认 Outliner 层级、选择高亮、Inspector 与 Scene 同帧合成。
- Asset Browser 已完成 retained collection 纵切：Model 按稳定 Asset ID 排序，查询和游标页以 256 项为硬上限；Editor 只持有临时 query/cursor/page-size，Registry 修订导致范围失效时归一到有效页但保留稳定选择。ImGui 混合壳显示 Previous/Next 与当前范围，600 资产压力测试覆盖第二页、末页、返回、过滤重置和 Registry 缩减。卡片的 `imageSource` 直接引用既有 Thumbnail Artifact；GPU Cache 的同一次 PNG 解码在提交成功后发布有界 CPU RGBA 快照，再注册到所有 Retained Surface 共用的纯数据图片表，不二次建立 Thumbnail Authority。图片表限制 256 项、64 MiB、单图 8 MiB 和 2048 边长，替换/移除带 revision；返回已淘汰页面时会确定性重建快照而不会永久空白。真实 Platformer 隐藏 D3D12 整窗已显示选择高亮和真实 PNG 纹理。Registry header、搜索/Rescan、Import/Inspect/Preview/Cook、后台 Job、诊断/修复与专用作者区仍使用原 Authority 和 ImGui 壳。

退出条件：编辑器在默认工作站尺寸下呈现统一、清晰、紧凑的现代专业工具界面；首屏可辨当前项目、场景、编辑/运行/编译状态和主操作；核心面板具备稳定语义投影，且不牺牲连续交互性能。

### P3：生产纵切已成立——Noemancer Platformer 第一阶段验收

- 只补阻塞真实游戏制作的通用能力，不抢先制作大量内容。
- 引擎根目录提供 `Open Platformer Editor.cmd` 与 `Play Platformer.cmd` 两个专用验收入口：前者增量构建后直接打开源项目编辑器，后者按 Runtime 与项目内容哈希创建或复用 Debug 原子包，并启动不含编辑器壳层的独立 Player。它们不替代官方 `Noemancer Editor.cmd`。首次游戏启动需要 Cook/Package，内容未变时后续启动复用同一包。
- 验证 Input、2D Character Motor、Camera、Tilemap/Sprite、Trigger/Tag、脚本、音频、UI、Save/Replay 和打包闭环。
- Input 已形成项目级可移植契约与改键作者闭环：Project manifest 严格校验 Action/Binding，SDL3 泛化键盘、鼠标和手柄 source，轴死区在 Action 层执行；可见 Project Settings、真实输入捕获、原子保存和热应用贯通 Edit World、Play World、Headless 与打包 Player，不复制第二份 Input 状态。Lumen Run 显式声明键盘、方向键、手柄摇杆/D-pad、跳跃、交互、暂停和重开映射，并已通过临时改绑验收。
- 项目 HUD 已形成首个生产纵切：Project manifest 引用严格校验的 Semantic UI 文档，Package closure 与 Game Profile 携带该资产；Runtime 把托管脚本 public state、Input Action 与 Gameplay Attribute 绑定为 revisioned UI 节点，同一物化文档交给 RmlUi 和 `ui.project.observe`。新建 Workspace 自动生成可运行 HUD，Lumen Run 的收集数、完成态和移动输入已在隐藏 Player 画面中成立。后续扩写组件库、交互 Action 和可视化作者工具，不回退到项目专用 C++ HUD。
- Sprite 状态驱动已使用既有 Sprite/Atlas 播放内核形成项目脚本纵切：C# World View 提供类型化速度、CharacterMotor2D 与当前 Sprite Playback；`sprite.playback.set` 原子校验实体、Clip、速度和翻转，只改变运行时游标并发布非持久化 revision/delta，不把每帧动画状态写回 Scene 或 Undo。Lumen Run 的项目 C# 已按 grounded/velocity 选择 idle/run/jump/fall，打包闭包携带对应 Sprite 与 Texture；当前源图没有真实 alpha，因此 Scene 保留隐藏 Sprite Renderer 与可见 graybox，待合格 RGBA 素材到位后只切换可见性，不改玩法代码。
- 项目音频已复用既有 miniaudio Resource Manager/Render Graph 完成生产纵切：Project `packagedAssets` 为脚本动态引用提供显式闭包根，Game Profile `0.3` 指向包本地 Asset Registry，Player 恢复稳定 ID/哈希/类型/依赖与包本地 source catalog；C# `PlayAudio` 只提交有界 plain-data 命令。Lumen Run 自有 SFX 已覆盖跳跃、收集、检查点与重生，不继续扩写音频功能。
- Save/Replay 生产纵切已经成立：Engine `0.2` 文档捕获 live durable Scene、仿真 Tick 与 `[ScriptState]`，Replay 携带初始 Save、输入 Tick 与固定步长并走正常仿真路径；C# 只提交 `SaveSlot`/`LoadSlot`/Replay 语义请求，World 做有界校验排队，Runtime Adapter 才把它们映射到平台用户数据目录和同目录临时文件原子替换。Lumen Run 启动/检查点写 `autosave`、R 键读取、路线完成写 `last-run`；Debug 包内 Player 已实际生成包含 7 个脚本实例的存档。完整路线与回放手感仍属于后续人工游戏验收，不阻塞生产硬化前沿。
- 把发现的问题回写为引擎通用契约和最小回归，不建立游戏专用 C++ 分支。

### P4：通用生产能力硬化

- 按真实项目阻塞补齐资产导入/Cook/依赖/流送、Prefab/Scene 作者循环、动画、物理、VFX、音频、UI、脚本调试、崩溃诊断、恢复和版本控制友好度。
- Release 分发与诊断闭包已收口：Noemancer 根源码采用 Apache-2.0；项目自有内容保持独立许可证身份。Package 对每个再分发条目和显式 Runtime/Cook root 生成确定性的 `licenseLedger`，校验 SPDX/LicenseRef、第三方 source URI、NOTICE、唯一 root 与可再分发状态；静态 Runtime 所含 SDL、Flecs、JSON、Dear ImGui、ImGuizmo、LodePNG、miniaudio、Jolt、FreeType、RmlUi、Lato、HarfBuzz、ICU、ozz、ufbx、fastgltf、meshoptimizer、KTX-Software 均有原文台账并随包复制。Release 包 app-local 携带固定 .NET 10.0.11 x64 与官方 VC143 Runtime，默认 UI 字体从包内相对路径解析；Shader 反射收据不再泄漏构建机绝对路径。Runtime 的 terminate/abort/SEH/Debug CRT fatal 路径保持无弹窗，stderr JSON 之外还在 `%LOCALAPPDATA%/Noemancer/Diagnostics` 写结构化 sidecar 与 Windows minidump，可由 `NOEMANCER_DIAGNOSTICS_DIR` 重定向。`scripts/verify-release-closure.ps1` 对包结构、许可证引用、VC/.NET、绝对路径和隐藏 Headless Player 生成机器收据；Lumen Run Release 验收 23 条许可证、0 路径泄漏、Player 3 帧退出码 0。独立物理机器、代码签名与安装器仍是公开发布门禁。
- 动画/物理作者能力第一轮已收口：物理 RigidBody、Box/Sphere/Capsule/Convex Collider 原本已由 Scene 属性、声明式 Inspector 和同一事务 Authority 完整编辑，因此不复制第二套工具；动画已从硬编码 locomotion 发展为 `noemancer.animation-state-machine/0.2` 与独立 `noemancer.animation-graph/0.1` 两层资产。Graph 组合 Clip/单一 State Machine/Blend 1D、override/additive Layer、骨骼 Mask 与 normalized-time Sync Group；Runtime 复用 ozz 的局部空间 Sampling/Blending/Additive，再执行一次 LocalToModel。Registry Inspect/Cook、Graph→StateMachine→Clip Package 闭包、Scene/Inspector 引用、World 实例、Editor Canvas 与四个 Agent 命令共享稳定 ID、严格文档、fingerprint 和源码事务；新建 Workspace 自动生成并连接模板。源码提交带单调 transaction ID，Runtime reload 失败只精确中止该事务。Canvas 现已支持有界节点创建/删除、Blend 1D Clip 连线/断线、端口与边命中、拖动布局和 Layer/Mask 调参；所有编辑共用 Graph patch、CAS fingerprint、事务、Undo/Redo 与 Registry 严格校验。Graph 0.1 当前仍限制为一个 State Machine、Blend 1D 叶子仅 Clip、override 必须位于 additive 前。动画压缩基线已证明 ozz runtime packing 很小；额外层级 key reduction 在真实 Mixamo Clip 上没有缩小最终 archive，因此不设为生产默认，也不为追求名义压缩率盲接 ACL。
- `production.cooked-animation-artifact` 已收口：版本控制中的 `noemancer.animation-clip/0.1` 只描述稳定 Clip 身份、Cook-only 源资产/索引与压缩模式；Cook 从同一不可变快照完成 hash 与 decode，把规范化 Skeleton、Inverse Bind、skin mapping 与 ozz Runtime Animation 写入内容/配方寻址的 `.animbin`，以固定小端 Header、严格 Manifest、分段及整包 SHA-256 校验。Player 只消费包内 `.animbin`，显式跳过 descriptor/offline builder，并发布 cooked load、source decode 与 offline compile 计数；真实 Debug 原子包已得到 `1/0/0`。Runtime 在进入 trusted-only ozz `IArchive` 前验证精确 archive 布局、分配计数、有限 duration 与统一 64-joint GPU 上限；坏类型、版本、计数、截断、篡改和 Registry hash 不匹配均在发布旧 Clip 前结构化失败。缓存损坏会确定性重建，Package Probe 不再覆盖计划身份，Cook-only 源许可作为显式 NOTICE root 保留，`local-only` build input 会被拒绝，复制身份在 atomic rename 前复验。证据位于 `generated/acceptance/cooked-animation-player-current/`。
- `production.cooked-geometry-runtime` 已收口：GLB/FBX 仅在 Cook 阶段由 fastgltf/ufbx 解码；`noemancer.mesh-runtime-artifact/0.2` 使用固定小端 `NMMESH02` Envelope、严格 Manifest、每 Primitive 独立 meshoptimizer `NMMSH001` 几何段、材质/透明/蒙皮信息和嵌入 KTX2 图像，所有分段及整包均带 SHA-256。配方身份包含 Artifact Schema、源哈希、Importer、目标 Profile 与实现策略，旧缓存不会跨版本复用。Package 要求 payload hash，完整执行 Runtime Loader 预检，拒绝旧 meshbin 及源 GLB/GLTF/FBX，并闭合 fastgltf、meshoptimizer、KTX-Software、ufbx 许可证；Player 只返回引擎自有 `GltfMeshData` 后上传 GPU。真实 Debug 原子包的 Headless、D3D12、Vulkan 均为 Cooked Load `1`、Source Decode `0`、Offline Compile `0`，包内源模型文件为 `0`。Codec Adapter 仍私有；未来几何压缩候选必须维持同一 Runtime/Package 契约。
- `commercial-raster.shader-artifact-contract` 已收口并持续扩展：版本控制中的 `noemancer.shader-artifact-source-contract/0.1` 当前是 43 个 Shader 的 stage/profile/entrypoint、SDL 资源数和 Compute thread group 权威输入；固定 DXC 构建同时生成 86 个 DXIL/SPIR-V 与确定性 `noemancer.shader-artifact-manifest/0.1`。构建门禁对 DXIL 容器/PSV 与 SPIR-V 指令、装饰、EntryPoint、ExecutionMode 做真实二进制反射，43/43 跨后端 ABI 一致且 0 issue。Scene/VFX/GPU visibility/Retained UI/天空/HiZ/Temporal/SSR 加载统一先校验 Schema、路径、stage、资源数量、bytes 与 SHA-256，再向 SDL_GPU 创建对象；错误 fail closed。Package Closure 从同一 Manifest 闭合实际引用的二进制和 Reflection Proof；旧 29/40-Shader 数字只描述当时历史包。
- `production.project-ui-authoring-and-input-remap` 已收口：可见 `Project Settings / Input Map` 面板与 Agent 语义快照共用稳定 action/binding 身份、冲突诊断和 revision-bound intent；SDL Runtime Adapter 只负责键盘、鼠标、手柄的规范化、设备观察和下一输入捕获。Engine `ProjectInputEditSession` 在候选副本上执行 CAS、校验和原子 Manifest 保存，成功后才发布 canonical actions；Runtime 同步热应用到 Edit World 与现有 Play World。通用 `--input-sample SOURCE VALUE` headless 探针把注入回执和完整 Input Action 状态作为结构化证据输出。Lumen Run 临时副本真实改绑 `gameplay.jump` 后，Project 与 Package Player 均证明 `keyboard.q => 1`、旧 `keyboard.space => 0`，源项目哈希保持不变。
- Windows Release 分发补净环境与跨机器验收；签名、安装器和平台支持按发布目标推进，不把当前开发机成功外推为商业发布证据。
- 继续优先采用成熟中间件作为执行内核；Noemancer 负责稳定领域模型、Adapter、编辑体验和语义控制面。

### P5：性能证据与优化

- 先固定 workload、硬件、驱动、编译器、分辨率和质量档，建立 Editor 响应、启动/编译、CPU/GPU frame、内存/显存、资产加载和子系统预算。
- `performance.baseline-contract` 已成立：Release Player 提供固定分辨率、预热/采样帧预算与不可覆盖的 `noemancer.performance-evidence/0.1`，记录端到端主线程帧墙钟、进程内存、Backend/Present Mode 和完整 Renderer Status；外部脚本固定官方 PresentMon 2.4.1 与 SHA-256，把有效 GPUTime/GPUBusy 合并为 `0.2`。`-RequireGpuTelemetry` 可用于 CI/验收机严格门禁；当前工作站 ETW 会话能启动/停止但没有目标 Present 行，因此证据明确标为 unavailable，绝不以 CPU 提交耗时冒充 GPU 时间。
- `performance.high-raster-workload` 已建立为 `noemancer.high-raster/0.1`：固定 1024 个全可见、材质参数不同且投射阴影的网格，连同稳定相机、1080p Release Profile 和分阶段 CPU 证据。首轮据此把材质参数纳入 per-instance ABI，并移除静态物体无意义的双份 64-joint palette/history，opaque draw 1029→69、shadow draw 1650→110，Frame p95 33.34→16.06 ms；隐藏 D3D12 捕获证明材质差异仍保留。
- `performance.representative-animation-physics-workload` 已建立为 `noemancer.animation-physics/0.1`：固定 64 个真实 Mixamo/ozz 蒙皮角色、256 个持续活动的 Jolt 动态球、普通 Scene/World/Render World 路径和 1920×1080 Player 风格表面。首轮同口径基线 Frame p95 4.34 ms、Simulation p95 2.54 ms；审计发现 Simulation View 重复求姿势且 `rootMotionMode=ignore` 仍执行根运动采样。Simulation View 去掉纯渲染 Palette、根运动按需计算后，最终 `generated/acceptance/animation-physics-20260822-072808/` 为 Frame mean/p95 3.27/3.60 ms、Simulation mean/p95 1.47/1.78 ms，Render Extract mean 0.67 ms。Renderer Status v12 明确报告 64 个 Skinned Instance/Draw Item 与 3328 个 Joint Matrix；隐藏画面已人工确认角色阵列和刚体区域均实际渲染。
- `production.animation-compression-evidence` 已成立：`noemancer.animation-compression/0.1` 同时记录 Raw/Runtime 的 resident estimate、little-endian archive bytes/FNV-1a hash、TRS key 数与 Skeleton archive；固定 257 点比较 local/model/10 cm probe/skinning/root motion，Release 64 pose A/B 把 FBX I/O、ufbx decode、ozz compile 与 JSON 排除在计时外。真实 `rumba-dancing-02.fbx` 的 312 个规范化 key 经候选层级 reduction 变为 85，但 baseline/candidate Runtime archive 都是 2,907 bytes；模型平移最大误差 0.451 mm、探针 0.659 mm、Root Motion 0，均在 1.1 mm 合同内。由于没有 Runtime 体积收益且单轮微秒级采样波动不足以证明性能改善，生产默认明确保留 `ozz_runtime_baseline`。证据由 `scripts/measure-animation-compression.ps1` 生成到 `generated/acceptance/animation-compression-20260822-current/`。
- `performance.frame-preparation-hotpath` 已收口：证据把准备阶段继续拆为 Event、Simulation 与 Post-Simulation，确认旧路径在一次固定帧内重复物化完整 World View，并为纯渲染实体重复维护 Transform 基线。改为每帧复用一次快照、只在会影响后续消费者的变更后刷新，并只跟踪可能移动的实体后，1024 实例场景 Frame mean 15.16→6.05 ms、p95 16.06→6.63 ms；Preparation mean 12.21→3.28 ms、Simulation mean 11.90→3.10 ms，49/49 Debug CTest 通过。证据位于 `generated/acceptance/performance-20260822-052732/`。
- `commercial-raster.reference-scene-contract` 已成立并升级为 `noemancer.commercial-raster-reference/1.8`：固定 1080p、Scene Camera、曝光补偿、Render Scale、预热/采样帧，包含 5×5 金属度/粗糙度矩阵、独立接触/凹角 AO、隔离 scene-linear 灰阶/RGB-CMY/HDR shoulder 色卡、CSM 阴影跨度、IBL、TAA、四级双滤波 Bloom、明暗深度样本、暖/冷 Point 和 Spot、方向光/局部光阴影缓存，以及普通 Asset Registry/fastgltf/SDL_GPU 路径的完整 glTF PBR 材质卡。`scripts/capture-render-reference.ps1` 在隐藏路径同时生成 Golden BMP、严格质量侧车、Bloom 与色彩响应质量收据、Release CPU evidence 和带 SHA-256 的 `noemancer.render-reference-evidence/0.1`，并强制检查五类纹理、MASK/BLEND、Double-sided、四级 CSM、七个局部阴影面、Renderer Status v22、Render Graph v11 与 ACES 矩阵输出合同。Scene-only capture 使用精确分辨率与 Scene Camera；内置球体为 32×16、1024 三角形的平滑 UV Sphere。
- `commercial-raster.clustered-local-lighting` 已成立：Scene `LocalLight` 以 point/spot、颜色、流明、米制范围、方向、锥角和光源半径形成稳定持久化契约，并贯通 World/Render World v13、声明式 Inspector、Schema/C# Binding 和 Agent 同源观察。Renderer 使用固定 16×9×24 对数深度 Cluster；CPU 以保守包围球确定性构建紧凑 light-index 列表，GPU Storage Buffer 在 Forward PBR 中消费。D3D12 参考证据 `generated/acceptance/raster-reference-20260822-060125/` 为 3 lights、4176 assignments、0 overflow，CPU Frame p95 4.55 ms；Vulkan 对照 `generated/acceptance/raster-reference-20260822-060359/` 为相同分配、p95 4.64 ms。构建迁移 Compute 只由测量触发；局部光阴影已由后续 1.3 合同补齐。
- `commercial-raster.textured-material-reference-coverage` 已成立：引擎自有 CC0 确定性 GLB 含 3 个材质、5 张嵌入 PNG 与 OPAQUE/MASK/BLEND，现由离线 `gltf.binary/0.1` 进入 `noemancer/meshbin/0.2`，Player 不再解析源 GLB；D3D12/Vulkan Golden 均实际上传 5 张纹理并报告 normal/metallic-roughness/occlusion/emissive 各 3 个 Primitive、MASK/BLEND 各 1 个、Double-sided 2 个。该覆盖现由 Reference 1.8 继续保留；GPU 时间仍等待可产生 Present 行的兼容验收机严格采集。
- `commercial-raster.local-light-shadows` 与 `commercial-raster.shadow-cache-and-scalability` 已成立：同一个 `LocalLight.castsShadows` 权威意图驱动 Point 六面与 Spot 单面 D32 Array/PCF，不引入第二种 Light 组件。`--shadow-quality low|medium|high` 在 GPU 分配前选择 512/768/1024 分辨率及有界灯光预算；每个已选灯面按稳定灯光身份、矩阵、可见投影物、Transform、Cutout、几何范围、GPU 资源身份和蒙皮状态生成指纹，未变化时保留原 Layer 并跳过整个 Render Pass。Renderer Status v11 发布面可用/重绘/复用、累计命中/失效、避免提交估算与失效策略。Reference 1.4 的 D3D12/Vulkan 稳态均为 7/7 面复用、首轮 7 Miss，CPU p95 分别 4.61/4.43 ms。
- `commercial-raster.directional-shadow-cache` 已成立：四个 CSM Layer 各自以稳定光照矩阵、可见投影物身份、Transform、Cutout、几何/GPU 资源和蒙皮姿势生成指纹；命中时保留已有 D32 Layer 并跳过完整 Cascade Render Pass。Renderer Status v14 发布每帧生成/复用、累计 Hit/Miss 与避免提交估算。Reference 1.5 的 D3D12/Vulkan 稳态均为 4/4 Cascade 复用、首轮 4 Miss、累计 252 Hit、避免 92 个实例提交；隐藏画面已审阅，D3D12 Golden SHA-256 与缓存前完全一致。证据分别位于 `generated/acceptance/raster-reference-20260822-081444/` 和 `generated/acceptance/raster-reference-20260822-081448/`。
- `commercial-raster.gpu-driven-submission` 与部分可见一致性证据均已成立：该历史证据对应 Renderer Status v22 / Render Graph v11，证明静态、不透明、未蒙皮且资源状态兼容的 Draw Item 可进入 16,384 实例/1,024 Batch 持久 GPU Scene Buffer；当前产品合同已由上方 v29/v17 与 readback `0.3` 取代。Compute 以六平面视锥压缩可见索引并写入 indexed-indirect instance count，图形 Vertex Shader 从 Storage Buffer 读取逐实例 Transform、上一帧 Transform、材质与对象 ID。全可见 High Raster 的 1025 个候选已从传统 65 个实例批次收敛为 1 个 indirect batch，连同 4 个小批/蒙皮回退共 5 个 opaque draw；同一 Release 二进制 5 组交替 A/B 的 Frame mean 中位数为开启 7.34 ms、关闭 7.52 ms，p95 中位数为 8.67/8.97 ms，Scene Record mean 中位数为 1.28/1.37 ms。早期 `noemancer.gpu-visibility-stress/0.1` 把 37% 压力实例确定性放到视锥外；readback `0.2` 在同一 command buffer 的 draw 后复制 indirect command 与 compact visible-index buffer，并用专属 fence 等待。D3D12/Vulkan 都得到 `1025` candidates、CPU/GPU `647/647` visible、`378` culled，逐批 `countMatch`、`exactSetMatch` 与总 `match` 全为 true；越界 Batch、数量不符、索引越界、错 Batch 和重复索引均为 0，CPU/GPU 集合 hash 相等。该一次性回读明确 `includedInPerformanceSample=false`，因此仍只宣称 CPU 提交改善，不在本机缺失 PresentMon 行时声称 GPU 时间改善。
- `commercial-raster.gpu-driven-stable-batching` 已把上述路径从逐帧重组推进到稳定资源身份/代际和 Draw Key：拓扑未变时复用线性批次，实例数据按精确 dirty range 局部上传，资源或拓扑变化才重建；Shader 对 Draw Metadata、Visible Index 与 Instance Buffer 的三重越界读取采用确定性裁剪保护。公共 GPU-driven ABI 为 `0.4`，Renderer Status 发布拓扑复用、脏区、批次/实例/命令上传量及累计稳定上传量。最终 Release D3D12/Vulkan 初始帧均上传 229,616 bytes，稳态拓扑复用且实例/批次脏区与上传为 0；部分可见工作负载仍精确得到 1025 candidates、647 visible、378 culled。最终源树对应证据位于 `generated/acceptance/gpu-stable-batching-latest-current/` 与 `generated/acceptance/gpu-frustum-stable-batching-latest-current/`。同一 Release High Raster 单轮 A/B 的开启/关闭 Frame p95 为 8.43/8.62 ms、mean 为 7.70/7.57 ms；只据此宣称未发现明确 CPU 帧回退，不宣称 GPU 时间改善。
- `commercial-raster.async-texture-streaming-pressure-workload` 已升级到物理 KTX2/Sprite 纵切：Registry/Package 保留 `mode/importance/priority`，纯 Engine Planner 计算屏幕需求和预算目标，Runtime 以可提交/回滚的 replacement tier 执行升降级。`noemancer.texture-residency/0.3` 区分 GPU allocation estimate 与不可用的原生物理遥测。统一的 Runtime-private Texture Resource Table 现已覆盖 Registry KTX2/PNG Sprite、Imported glTF PBR、RmlUi Game/Inspector、Inspector sampled target 和 Asset Browser Thumbnail；稳定 handle 的 identity generation 与物理 resource generation 分离，所有上传替换随 GPU submit 提交或回滚。最终 Lumen Run 双后端证据位于 `generated/acceptance/texture-streaming-20260822-104305/`：完整阶段 generation 为 `1,8`，不可见 Courier 正确保留 tail；压力阶段两后端均为 `22,15`、14 次 eviction、7 次 reupload、0 missing draw。当前 VFX 为无采样纹理的程序化 billboard，未来 flipbook/map 必须接入同一表，不以虚构资源阻塞本前沿退出。
- 性能证据区分一次性启动、资源 Cook、截图编码与稳态帧循环；接入可关联的 profiler/evidence 后，依据测量优化 jobs/ECS、加载/流送、渲染提交、Shader/PSO、动画、物理、VFX、音频和编辑器热路径。
- 每项结论记录场景、配置、revision 与设备；无代表性 workload 不宣称商业级或跨平台性能。

### P6：商业 Raster 画质

- 版本化参考场景、固定 Scene Camera/曝光、Golden Capture、结构化质量指标和 CPU 预算已成立；GPU 预算字段存在，但只有兼容验收机产生真实遥测后才可通过该门禁。功能名不等于画质证据。
- 对照 Filament、Wicked、Godot、Unity 与 UE 的可验证方案，保持部分可见 workload 与一次性 fenced readback 作为 GPU-driven 回归合同；现有 authored/editor-derived sampled consumer 已统一稳定 Texture Handle，后续只在新增纹理消费者时沿用该合同。标准 glTF PBR 五纹理通道、透明模式、Clustered Lighting、可缓存 Directional/Point/Spot 阴影、GPU static opaque indirect submission、IBL、TAAU、AO、色彩管线和后处理均需保持可复现合同。
- 高级 GI、光追、Slang 和 Native RHI 由 SDL_GPU 的实测限制或明确产品目标触发，不预先扩张维护面。
- Shader 源码、DXIL/SPIR-V、真实二进制反射、编译参数和哈希清单已形成构建/加载/Package 三重门禁。首个 Clean-room 2D 迁移与材质、AO/间接光、色彩输出参考画面均已收敛；Slang/native RHI 仍由证据触发，不随 Hybrid Pixel 默认扩张。
- Bloom 后处理保持 half/quarter/eighth/sixteenth 四级 Downsample 与三级 Upsample 合成；Reference 1.8 保留粗糙度感知 GGX 多重散射、独立间接光 MRT、八方向 horizon AO、两遍 bilateral 去噪和显式 HDR 合成，并把 `scene-linear Rec.709 → ACES working space → fitted RRT/ODT → bounded Rec.709 → explicit sRGB/UNORM` 固定为 Renderer Status v22 合同。隔离色卡证明六级中性灰阶单调且最大通道差 0.00765、RGB/CMY 六种签名完整、HDR 最后斜率低于首段且 0 clipping；D3D12/Vulkan Golden p95 为 7.60/7.82 ms。AO A/B 仍保持接触/凹角负向、控制区近零。`commercial-raster.material-postprocess-quality` 因此退出，当前前沿进入 Hybrid Pixel Core Profile。

### P7：Hybrid Pixel / HD2D Profile

- Core Profile 已收口：Project `0.2` 可选内嵌唯一 `noemancer.hybrid-pixel-profile/0.1`，Package 生成 Game Profile `0.4`；Editor/Agent 语义投影、源项目 Runtime 与包内 Player 读取同一值。Renderer 在 320×180 虚拟目标完成空间稳定渲染，关闭 TAA jitter/history，再以 nearest 整数倍复制到物理输出并确定性 letterbox；Camera/Sprite/Tile Cell snapping 只修改每帧新提取的 Render World，不回写 Scene。Picking 使用同一半开区间物理→虚拟映射，黑边点击不产生读回或 Fence。
- Core 验收位于 `generated/acceptance/hybrid-pixel-core-20260823-044007-3818ac58/`：Lumen Run 临时副本与 Release Package Player 在 D3D12/Vulkan、1440×900、1439×899、1920×1080 共 12 次隐藏捕获全部通过，跨后端对应 BMP SHA-256 完全一致；1440×900 为 4× 与 80/90 对称边框，1439×899 保持 4× 并以 79/80、89/90 分配奇数余量，1920×1080 为精确 6×。源项目完整树 hash 未改变，项目无 Native C++；Game Profile 0.4 和 28 DXIL + 28 SPIR-V 分发闭包成立。
- Sprite material 与混合光照已收口：`noemancer.sprite-asset/0.2` 在既有可选 material 上增加 lit/unlit、metallic/roughness 和 receive/cast shadow，normal/emissive/depth 依赖由 Sprite Codec 唯一投影并贯通 Registry→Cook→Package→Player。Render World v14、Renderer Status v24 和 224-byte Sprite GPU instance 复用同一 Directional、Clustered Point/Spot、CSM/Local Shadow、AO indirect 与 Texture Resource Table；旧 0.1 无 material Sprite 显式保持 unlit，不产生兼容性暗改。构建/分发闭包现为 29 个 HLSL、58 个 DXIL/SPIR-V artifact。
- 该切片的正式证据位于 `generated/acceptance/hybrid-pixel-mixed-lighting-20260823-133026-6eb55933/`：Lumen Run 临时副本与其 Release Package Player 在 D3D12/Vulkan 四路隐藏捕获全部通过；4 个 Sprite 的 lit/unlit、receive/cast、normal/emissive/depth 矩阵与 2 个 3D Mesh、Directional/Point/Spot 共享同一 Render World 指纹，源码/包体稳定 Renderer Status 指纹相等。阴影首帧实际提交、稳态缓存避免量均有机器证据；包体依赖闭包、16 帧 KTX2 清晰层级、源项目树未变和零项目 Native C++ 同时成立。Debug 全量 CTest 76/76 与 MCP smoke 通过。
- 像素对齐 VFX 与受控 Hybrid 后处理已收口：VFX Graph `render.sprite` 以向后兼容的 `pixelAlignment`、`sizeQuantization`、`sampling` plain-data 字段显式选择 Profile 或普通 Raster 行为；Render World v15 将策略带到既有 GPU lifecycle/group/sort/dual-indirect 路径，128-byte VFX Camera ABI 在虚拟目标中对齐当前/上一帧中心、量化尺寸并选择像素中心 coverage。`noemancer.hybrid-pixel-post-process/0.1` 纯策略在 Hybrid 下禁用 jitter/history、把曝光锁为 1，并把 Bloom/AO 固定在虚拟分辨率；Raster 保持 TAAU、自适应曝光和原参数。当前 VFX 仍是无纹理程序化 billboard，因此没有绕开 Texture Resource Table 的纹理消费者；未来 flipbook/sprite/map 一律先进入该表。
- 正式证据位于 `generated/acceptance/hybrid-pixel-vfx-post-20260823-140048-d6c27d21/`：临时 Lumen Run 与 Release Package Player 在 D3D12/Vulkan 四路各运行 720 个真实粒子，Render World v15 均报告唯一 `profile/profile/profile` 策略，Renderer Status v25 均报告 VFX 0.7、中心/尺寸对齐、锁定曝光、虚拟网格 Bloom/AO 和 nearest presentation；源码/包体指纹相等，项目树未变且无 Native C++。Debug 全量 CTest 77/77 与 MCP smoke 通过。
- Hybrid Profile 可视化作者 UI 已收口：`ProjectHybridPixelAuthoring` 是唯一项目事务 Authority，以 expected-revision CAS、dry-run、原子 manifest replacement 和可持久化 undo/redo 管理 optional Profile；只替换 Project `0.2` 的 `hybridPixelProfile`，保留 Input Actions 与未知字段。Headless-first Panel 提供启用/删除、虚拟尺寸、PPU、camera/sprite snapping、presentation filter、字段级诊断及实时整数缩放/letterbox 预览；Application 只在事务成功后热应用 Renderer。Editor 可见表单与 `projectSettingsHybridPixelProfile` 语义快照投影同一 snapshot/draft/validation/preview/request/history，没有第二份 Profile 数据库。
- 正式证据位于 `generated/acceptance/hybrid-pixel-profile-authoring-20260823-final/`：Lumen Run 临时副本执行真实 Apply→Undo→Redo，Panel 在无 ImGui Context、无窗口条件下输出稳定语义状态；`inputActions`、注入的未来字段及源项目 manifest/完整树 SHA-256 均保持。Debug 全量 CTest 79/79 与 MCP smoke 通过。
- Package→Player 生产验证已收口：`generated/acceptance/hybrid-pixel-production-20260823-final5/` 从未修改的 Lumen Run 复制项目，在同一启动 Scene 保留 7 个 C# 实例、5 个 Input Action 和项目 HUD，再组合 Hybrid Profile、4 个 Sprite material/shadow 矩阵、2D/3D 混合光照与像素 VFX/post。Release Cook/Package、D3D12/Vulkan 源码与 Player 隐藏捕获全部通过，Render World/Renderer Status 和统一 `runtime.production_state` 指纹一致；包内项目代码确实执行，源项目树未变且无 Native C++。60 帧隐藏 Player 采样的 D3D12/Vulkan CPU Frame p95 分别为 13.25/12.76 ms；Debug 全量 CTest 80/80 与 MCP smoke 通过。
- Semantic 2D Character Rig 可丢弃实验已按退出规则关闭，不提出生产 ADR。`generated/acceptance/semantic-2d-rig-prototype-current/` 使用版本控制友好的严格 JSON 定义两个角色、idle/run 与 north/east/south/west，确定性生成 24 个帧计划和 16 个既有 Sprite Clip；数组重排不改变计划指纹，单个 Pose 编辑只影响 1 帧且 12 个原帧 ID 保持。原型类型没有进入 Runtime、Scene、Registry kind 或 Package schema。
- 当前 workload 的规范 Rig 源为 16,976 bytes，直接逐帧作者描述估算为 9,834 bytes，输出 Sprite 元数据为 13,321 bytes；它证明了编辑隔离，却没有证明作者体积、构建或包体收益。因此 `semantic-2d-character-production` 继续由更大真实角色批量证据触发，不因“AI-native”标签固化第二套动画 Authority。
- `production.project-ui-authoring-and-interaction` 已退出：`ProjectUiAuthoringSession` 对项目 HUD 源文档执行 expected-revision CAS、dry-run、外部修改冲突检查、原子持久化和可持久化 Undo/Redo；RmlUi `button`、Retained UI、Agent `ui.project.action.invoke` 与项目 C# `OnUiAction` 共用规范 binding/action/revision。`components[]`/`componentRef` 复用既有 node/presentation 语言，支持声明继承、递归对象合并、数组替换和节点最终覆盖；Runtime 只物化有效节点并保留 `componentChain`，派生字段不能回写源码。
- Editor 的双栏 Project UI 作者界面现提供递归节点树、选中节点 Inspector、state/presentation/value、Design Token、组件声明的新增/更新/删除和高级 JSON，以及 valid/dirty/disabled/pending/conflict/error 字段状态；GUI 与 Agent 不持有第二份 UI 数据库。`serve --project PATH` 与 MCP 的 `NOEMANCER_PROJECT` 持有同一项目 World/Asset/UI Authority，`ui.project.source.observe/edit` 在提交后热应用。隐藏 D3D12 整窗证据 `generated/acceptance/project-ui-authoring-20260824-140441-a9ce5a2b/` 使用真实项目临时副本，质量合同、语义合同和源树未改变均通过，全程没有可见窗口或 Computer Use。它明确不是对另一个已打开 Editor 进程的远程控制，也不宣称跨进程共享 Undo journal。
- 新增可解析的 visual contract 回归，覆盖组件继承、`componentRef`、稳定字段身份、非法 JSON、dirty/pending/stale/conflict 与文档诊断；公共测试配置变化后的 Debug 全量 CTest 87/87 通过。
- `hybrid-pixel.large-sprite-tilemap-production-pressure` 已退出。可测生产负载覆盖长 Sprite 序列、Atlas 规划、Registry→Cook→Package 确定性闭包，以及大型稀疏 Tilemap 的 Chunk 可见集、稳定 GPU Range、碰撞烘焙、内存和增量更新成本。确定性多页计划已进入真实 Atlas 生成、逐页 KTX2/磁盘缓存、Package 闭包与 Runtime page overlay；没有证据要求立即引入 texture array，也不因单主机线程收益更改默认制品策略。
- 该切片首个生产批次已落盘：Sprite Codec 增加 source/frame/clip/引用硬上限和 Atlas union/overlap/free area、引用复用、稳定 layout fingerprint；`asset.sprite.pressure` 与 Registry `asset.inspect.renderPayload.production` 把同一统计投影给 CLI/Agent。Tilemap 增加统一层/Chunk/Cell/可见集/collider 预算、生产内存统计、确定性 packed range、局部可见查询，以及带 dry-run/commit 的增量 stroke 收据；收据量化 dirty/rebuilt/reused/created/removed Chunk、上传量与稳定 range 复用。`render.tilemap.pressure` 0.3 可模拟 64×64 Chunk 的稀疏占用而不实例化全部可寻址 Cell。
- 正式证据 `generated/acceptance/sprite-tilemap-production-pressure-evidence.json` 在真实项目临时副本上通过：真实 2048×2048 Atlas 含 2,048 帧/32 Clips，Registry→Inspect→Cook Apply→Package dry-run 闭合；Tilemap 为 4,096 Chunks、262,144 可寻址/65,536 占用 Cell，裁掉 3,469 Chunks，稳定 range move 为 0，源项目树未改变。KTX2 Adapter 的 Release 4096×4096 RGBA8 Basis-LZ 固定负载为 1 worker 首次 24.698 s、8 worker 20.853 s、同进程缓存约 66–99 ms；单主机数据不足以证明跨机器恒等，因此 Registry 默认仍为 1 worker。`generated/acceptance/sprite-atlas-page-cache-evidence.json` 用两个独立 Runtime 进程证明：修改单帧后受影响页 miss 且身份变化，其余 3 页保持内容/载荷/cache key 并全部命中。`generated/acceptance/sprite-atlas-package-player-current/evidence.json` 证明实际提交包只含规范 SpriteAtlas manifest 与有效分页 KTX2、不含源 PNG，Headless 与隐藏 D3D12 Player 均注册 256 个 binding；Renderer 的有界 `noemancer.sprite-atlas-runtime/0.1` 状态报告 1 个有效 manifest、1 个分页纹理已上传、0 缺失，源项目树未改变。

### P8：游戏迁移与制作验证（通用引擎完成后的下游验收）

游戏复刻、兼容引擎和开源游戏迁移是引擎的验收客户，不替代通用引擎主线。`game-migration.hybrid-pixel-hd2d-small-slice` 已退出：外部 `D:\3D\NoemancerProjects\NoemancerHd2dSlice` 只含项目 C#、Scene/Prefab、程序生成灰盒 Sprite/Tilemap、混合 2D/3D 光照、像素 VFX、项目 UI 和 Input，没有 Native C++ 或引擎分叉。`generated/acceptance/hd2d-small-slice-current-v3/evidence.json` 证明源码与原子 Release Package Player 的 Hybrid Profile、Input、UI 与脚本状态相等；两路隐藏 D3D12 均提交 50 个 lit Sprite/Tile cell、5 个 3D opaque instance 和真实 GPU VFX，960×540 画面可读，包内 60 帧 CPU p95 为 3.62 ms，源项目树未改变。Noemancer Platformer 继续作为基础回归客户；首个 Clean-room 纵切 `D:\3D\_tools\StarfallGauntlet` 已按 OpenTyrian 级纵向射击行为范围完成。

`development-loop.source-project-first-visual-latency` 已退出。旧 54.63 秒数字是 Debug 64 帧验收总时长，不是首帧；`noemancer.runtime-startup-telemetry/0.1` 现从进程入口有界记录真实 phase 与第 1/3/64 帧。固定 HD2D 1 帧隐藏 D3D12 对照为 Debug `firstFrameMs=16453.51`、Release `1690.07`；Debug 主要耗在 Retained UI 初始化 7796.92 ms 与首帧 loop 7116.90 ms，项目 C# 仅 12.65 ms、Asset Cook 0.01 ms。官方 `Noemancer Editor.cmd` 因此默认 Release，显式 `-Config Debug` 仍用于原生调试。项目 C# 的跨进程内容寻址缓存把真实冷编译约 1489 ms 降到磁盘命中约 12 ms，修改源码后约 1567 ms 正确失效；缓存原子写入、逐文件复验并受 32 条/256 MiB 全局预算约束。证据位于 `generated/acceptance/startup-telemetry-20260824-170450/` 与 `generated/acceptance/managed-compile-cache-evidence.json`。

`agent.open-editor-live-session-transport` 已退出。交互式 Editor 原子发布无 token 的 Session Descriptor，凭据独立保存在 sidecar；Windows named pipe 拒绝远程客户端、限制为同用户 DACL、首帧认证、有限连接/帧/队列/超时，IO 线程只排队，Editor 主线程才调用绑定现有 Edit World、Asset Registry 与 Project UI Session 的 Command Registry。MCP 自动选择唯一健康 Editor，多实例时拒绝猜测，无 descriptor 才回退独立 `serve`；项目切换轮换身份，heartbeat、stale cleanup 与退出撤销均有契约。Release 隐藏真实 Editor 验收从 revision 1 创建临时实体到 2，再由同一 journal Undo 到 3，临时实体消失且没有第二 World。证据位于 `generated/acceptance/live-editor-session-current/evidence.json`。

同一切片的可见上下文也已闭合：`noemancer.editor-context/0.1` 从真实 `EditorUi` 投影 Project/Scene、Edit/Play authority、Outliner/Inspector 选择、当前面板、活动 Dock/Tab 和最近事务，`editor.context.observe/intent` 通过 live Command Registry 使用同一 revision；人类操作会使旧意图失效，Play 选择不会污染 Edit World，面板意图在下一帧真正聚焦 Dock。没有第二套 Context 服务，也没有公开 ImGui/RmlUi 句柄。Release 隐藏 HD2D 项目中 MCP 将 Agent Context 切到 Inspector 后恢复，Context revision 9→11→14，World revision 始终为 1；证据位于 `generated/acceptance/live-editor-context-current/evidence.json`。

`production.virtual-filesystem-and-package-mount` 的首批基底已经退出：Engine VFS 以有界 plain-data 合同统一只读目录和当前原子 Package Directory，支持规范 URI、最长根/优先级/稳定挂载序、range/EOF、取消、SHA-256、revision 和有界 observation，并拒绝词法穿越与 symlink 越界。Runtime 的 Registry bridge 为源码与包体生成相同 `asset://roots/<slot>/...` 身份；miniaudio Resource Manager 已通过私有 `ma_vfs` Adapter 使用同一 URI，resident snapshot 与 bounded stream 不再接触绝对资源路径。其后的迁移批次增加了严格 UTF-8/JSON 文档读取、Asset ID→URI→完整 SHA-256 校验，以及只把显式 Game Profile 物理路径作为 Package trust root 的 Player bootstrap；Profile、Scene、HUD、Sprite/Atlas、PNG/KTX2、Tilemap、动画定义和 Cooked Animation/Geometry 已进入同一 VFS Authority。聚焦测试实际证明 source/package 同字节读取、并发句柄、真实 WAV 解码和正式 Game Profile 0.4 包启动。没有发明自有归档压缩格式，也没有把 miniaudio 或平台句柄泄露到 Scene/RPC。

`production.vfs-project-and-cooked-asset-migration` 已退出：Source Project 的 Manifest/Scene/HUD、多个 Registry manifest、Player Profile/Scene/HUD，以及主要 Runtime/Cooked 资源均已进入同一 VFS Authority；两个 `D:\3D` 真实项目与三类正式包均通过当前 Runtime 探针。Registry `refresh()` 会重新读取其 VFS URI，Animation Graph Agent inspect 通过 Engine plain callback 验证 Registry hash，不持有路径或 VFS 句柄。managed assembly 与离线 FBX/GLB 保持显式私有物理 Adapter，因为 HostFXR 与作者导入器当前需要文件路径；没有证据时不增加一次“VFS→临时文件”的复制层。长音频 range 当前每次重新 resolve/open/hash，先作为明确性能债记录；只有固定长流 workload 证明瓶颈后才在 VFS Authority 增加 opaque sequential reader，不允许 Adapter 绕开 VFS。

`editor.retained-outliner-and-asset-browser` 与 `editor.retained-authoring-actions` 已退出：Outliner 与 Asset Browser 都使用 renderer-neutral 有界文档、独立 RmlUi/SDL_GPU Surface、明确输入坐标、稳定选择和真实项目隐藏整窗证据；Asset Browser 还闭合了真实 Thumbnail Artifact 与 >256 资源导航。`presentation.inlineActionIds` 只是最多八项的可见动作白名单，按钮仍携带语义文档中的完整 binding；Outliner Create/Rename/Copy/Duplicate/Reparent/Delete/Paste 与 Asset Browser Import/Inspect/Build Preview/Cook 经统一 Runtime Adapter 核验 source revision、entity revision、选中实体/资产、Edit/Play 权限、确认状态和 Asset Job 忙碌状态，再调用既有 `EditorModel`，返回 `noemancer.retained-authoring-action-receipt/0.1`。通用 action `input`/`confirmation` 支持有界 text/combo/required checkbox；窄 Outliner 将含表单动作纵向布局，纯按钮 collection 仍横排。Reparent 候选排除自身与后代，领域 Model 仍最终拒绝不存在父级与循环；Delete 未显式确认绝不产生 mutation。没有复制 Scene、Registry、Selection、Thumbnail Cache、后台 Job 或 Undo。ImGui 继续承载 Dockspace、Profiler、Render Debugger、长任务和尚未迁移的低层工具。

`editor.project-hub-recent-workspaces` 与 `editor.dpi-localization-visual-matrix` 已退出。用户级最近工程 Store 使用 `noemancer.recent-workspaces/0.1`、有界严格 JSON、revision、写前重读与原子替换；损坏状态不阻断 Editor，下一次成功打开可恢复。UI 矩阵新增有界 `--ui-scale`、同源 locale 传播、ImGui/RmlUi 统一密度、全 `dp` Retained 样式、原始/可执行溢出诊断和 3×3 隐藏验收。固定 1440×900 下 9/9 运行通过，Inspector 组件高度随 1.0/1.5/2.0 从 32→49→64，项目指纹不变；证据位于 `generated/acceptance/ui-localization-matrix-current-v11/`。这只证明当前 Windows 主机的平台字体 fallback、Inspector RTL 与可滚动 Retained 面板；图像抽查显示 2.0 倍固定小画布的 ImGui 顶栏/工具栏仍会截断，且不是全编辑器 RTL。当前前沿因此是 `editor.high-dpi-responsive-chrome`，随后才是可再分发 CJK/Arabic fallback 与 cluster-aware 编辑；不得把平台字体存在等同于可发行包闭环。

中文产品入口已补上首批纵切：官方 Launcher 默认 `zh-CN` 并允许 `-Locale en-US`，Editor 菜单可即时切换简体中文/English；Project Hub、菜单、Command Bar、主面板标题和既有声明式 Inspector/HUD 词条使用同一当前 locale。Windows ImGui 使用宿主微软雅黑简体中文字形范围，解决当前开发机的 Chrome 缺字。语言偏好持久化、剩余专业面板词条、可再分发字体与跨平台 fallback 仍随 `editor.redistributable-cjk-arabic-fallback-and-cluster-editing` 收口；高 DPI 响应式 Chrome 保留为渲染强化批次之后的首个 Editor 前沿。

该最小纵切的退出不等于完整射击游戏生产栈：当前 Prefab 生成/销毁仍重建完整 Scene，项目碰撞使用有界实体观察上的半径判断；只有后续更大迁移或性能证据表明需要时，才增加池化生成和原生批量重叠查询。Hybrid Pixel/HD2D 项目仍在 Pixel Grid、Sprite normal/depth/material、混合光照和像素 VFX 的核心 Profile 成立后启动。任何迁移都不得因单个游戏的特殊需求向 Engine/Runtime C++ 写入专用规则；需求只有在能抽象为稳定通用能力，并通过独立 Fixture、Schema/命令、最小回归与性能证据后，才回写引擎。

迁移按梯度推进：引擎自有确定性 Fixture → Lumen Run 项目 C# 闭环 → OpenTyrian/OpenJazz 级小型 2D → OpenRA、Unciv、OpenTTD + 开放内容包级数据驱动项目 → OpenLara/Warzone 2100 级 3D 综合压力 → Hybrid Pixel/HD2D 项目。OpenMW、OpenRCT2、ScummVM、OpenGOAL 与静态重编译项目主要用于规模、兼容层和工具链研究，不作为近期默认移植目标。

`D:\3D\_tools\_reference\_game-remakes\README.md` 是研究与候选索引，不是构建依赖。默认采用“架构借鉴 + Clean-room 项目 C# 实现”；只有逐仓库、逐组件核验许可证并确认与 Noemancer 的发行方式兼容后，才允许复用代码。原版 ROM/MPQ/PAK、关卡、音乐、商标素材不得进入引擎仓库或默认分发包；优先使用明确开放的 OpenGFX/OpenSFX/OpenMSX 等内容，但仍需保留来源、许可证和 NOTICE。

Noemancer 根许可证与 Package 第三方许可证/NOTICE 台账现已成立。直接迁移仍须逐仓库、逐文件确认兼容性、保留作者与许可证，并优先采用“协议/行为研究 + 项目 C# clean-room 实现”；根许可证成立不等于自动允许复制任意候选源码或素材。

游戏角色/NPC 行为、关卡、任务、经济、武器、得分、剧情、敌人 AI、项目专用相机、HUD 文案、Input Action、Save/Replay 字段和具体动画/音频触发都留在项目层。Engine 只提供固定步长、输入、物理/导航/网络原语、Sprite/Tilemap/Animation/Rendering/Audio、UI Binding、Asset Cook/VFS/Package、Save/Replay 基元、Profiler、Semantic Observation 和稳定 Agent 命令。迁移验收必须证明：不修改引擎 C++ 即可实现规则，且项目可独立编译、运行、调试、保存与打包。

## 持续但不抢占主线的架构债务

- `world.cpp`、`scene_renderer.cpp`、`command_registry.cpp`、`editor_ui.cpp` 体积过大。功能迁移经过这些文件时顺手按领域拆分，不单独发动长期“重构一切”。
- `noemancer_engine` 当前仍是聚合领域库；新增代码不得让 Runtime/Editor 反向渗入 Engine。等模块边界稳定后再拆独立 targets，避免为目录美观制造构建开销。
- 原始网络 Transport、完整导航、ACL/其他动画 Codec、Slang/native RHI、Tracy/RenderDoc 深集成均由真实产品阻塞或测量证据触发，不与当前前沿同时横向铺开。
- Metal/Linux/macOS/移动真机优化在第一阶段效果闭环之后推进；公共数据和 Adapter 边界现在必须保持可移植。

## Agent 与语义层同步策略

Agent 能力不推迟到功能完成后，也不要求每个内部函数都包装成工具：

- 新系统先定义稳定领域对象、Schema、revision、来源与状态投影；
- 只为高价值复合操作提供命令，遵循 Observation → Plan → Apply → Receipt；
- GUI 和 Agent 调同一 Authority；MCP/CLI 只做协议适配；
- 大数据通过 Artifact/Evidence URI 披露，常用观察支持过滤、字段预算和 delta；
- 内部实现细节不为了“文本化”复制成第二份权威状态。

## 验证层级

| 层级 | 触发条件 | 默认动作 |
|---|---|---|
| Worker/内循环 | 边界明确的连续编辑 | 先完成相邻改动；仅在 API/依赖不确定时提前编译 |
| 集成批次 | 一个连贯子系统切片完成 | `engine.ps1 check -Target ... -TestRegex ...`；必要时增加一个直接相关工具或运行探针 |
| 全量 CTest | 公共 Schema、共享 World/Runtime、依赖、构建配置、里程碑或发布 | `engine.ps1 test` |
| MCP 全烟测 | 命令注册、Agent ABI、MCP Adapter 或里程碑 | `engine.ps1 test -WithMcp` |
| GPU 隐藏捕获 | Renderer、Shader、GPU 资源或视觉验收变化 | 只测相关后端与场景 |
| 双 GPU 后端 | 跨后端 Shader/RHI 风险或发布候选 | D3D12 + Vulkan 对照 |

测试按风险新增：崩溃修复、公共契约、持久化、解析器、并发/所有权、事务和曾发生的回归需要稳定测试；文档、重命名、简单接线或已有邻近覆盖不机械增加 case。

## 不做事项

- 不把所有历史研究中的候选方案同时排入开发队列。
- 不以模块数量、工具数量或测试数量衡量进度。
- 不因 AI-native 重写成熟物理、音频、格式、压缩、网络和导航内核。
- 不把 Semantic State Plane 做成第二个 ECS、第二份 Scene 或每帧全量 JSON dump。
- 不在无代表性 workload 时宣称商业级性能或跨平台性能。

机器可读的当前状态见 [current-state.json](current-state.json)，文档权威规则见 [README](README.md)。
