# Noemancer 渲染参考与技术取舍（2026-08-18）

> 文档类别：Historical rendering rationale。当前渲染边界与顺序以权威架构和计划为准。

## 结论

当前渲染不是从某一个参考引擎照搬出来的。实现由本工程独立编写，算法基线主要来自公开图形学模型和格式规范，系统边界则分别吸收 Filament、Godot、Bevy、Wicked Engine 与 Unreal Engine 已验证的约束。

我们现在只覆盖了“可验证 Forward PBR 纵切”，没有覆盖这些引擎的完整渲染能力。正确目标不是立刻做一个缩小版 UE，而是先把材质、色彩、阴影、时域、资产和证据接口做成可靠内核，再由 Hybrid Pixel 验证场景和性能数据选择主路径。

## 本地固定参考

| 参考 | 固定版本 | 主要学习内容 | 不照搬的部分 |
|---|---:|---|---|
| Filament | 官方在线 PBR/FrameGraph 文档 | GGX/Smith/Schlick、能量守恒、Split-Sum IBL、物理单位、色彩链、FrameGraph 资源生命周期 | 不直接嵌入 renderer；Noemancer 需要自己的语义和证据边界 |
| Godot | `3000096f9aa6` | Forward+/Mobile/Compatibility profile、clustered lighting、可切换后端、编辑器工作流 | 不使用全局 singleton/Variant 作为热路径和 Agent ABI |
| Bevy | `9f4ff89c1a6a` | Main World → Extract → Render World、phase、图式调度、可组合插件 | 不复制 Rust ECS/schedule API |
| Wicked Engine | `f4a0d2635d52` | 轻量 C++ 多后端、bindless、GPU profiler、DDGI/光追/粒子与高压场景 | 不采用全局静态子系统，也不整块嵌入黑盒 renderer |
| Unreal Engine 5.8.1 | `71fe36aac5a8` | Deferred/Mobile 双路径、RDG、GPU Scene、Instance Culling、Nanite、Lumen、VSM、TSR、Shader/PSO 和生产诊断 | 不复制源码、宏/UObject/RHI 体系；不默认承担高端桌面路径的复杂度 |

UE 官方源码位于 `D:\3D\_tools\_reference\_game-engine\UnrealEngine`。这是 `release` 分支的浅层、blob-filtered、sparse checkout，保留 Renderer、RenderCore、RHI、D3D12/Vulkan、Engine、ShaderCompiler、UnrealBuildTool、Shaders 与 Build 版本信息；没有下载完整历史、模板和二进制依赖。

## 不同引擎为什么选择不同路径

渲染架构不是单选题，答案由内容、平台和质量预算决定：

- UE 的高端桌面/主机默认场景需要大量动态灯光、复杂材质、Nanite、Lumen、VSM 和丰富后处理，Deferred 能提供宽 GBuffer 和统一屏幕空间数据；透明仍要走独立 forward/translucency 路径。
- Godot Forward+ 用 clustered light lists 保留 MSAA 和较直观的 forward 材质路径；Mobile 则减少 compute 和带宽成本；Compatibility 为旧设备/Web 保留另一套能力边界。
- Filament 优先移动带宽、包体、MSAA 和透明材质，因此用 clustered/forward 思路而非宽 GBuffer。
- Wicked 更像紧凑的高端技术 renderer，适合观察 bindless、GPU-driven、光追、DDGI 和粒子如何落到多后端，但编辑器/产品约束与 Noemancer 不同。

因此“社区主流”不是 Deferred 或 Forward+ 二选一，而是：明确 profile、让材质和资产独立于后端、让 Render Graph 管资源、根据真实灯光/过绘制/带宽数据切路径。

## Noemancer 当前明确取舍

| 决策 | 当前选择 | 原因 | 触发复审的证据 |
|---|---|---|---|
| 主路径 | Forward PBR，逐步升级 Clustered Forward+ | 快速形成跨平台正确基线；适合透明、MSAA 候选和 Hybrid Pixel；当前场景没有足够灯光证明需要 GBuffer | 典型场景灯数、材质复杂度、带宽和透明占比 |
| RHI | SDL_GPU 启动后端 + engine-owned IDs/Render Graph | 快速覆盖 D3D12/Vulkan/Metal；避免现在重写平台层 | bindless、timestamp、barrier、residency、crash diagnostics 的实测缺口 |
| PBR | glTF metallic-roughness + Filament 风格 GGX/Split-Sum 基线 | 资产互通、公开数学、可用材质球验证 | clearcoat/sheen/transmission/skin/hair 的验证游戏需求 |
| 阴影 | 先 texel-stable 4 级 CSM，再考虑 PCSS/contact；暂不做 VSM | 当前世界规模不值得承担 virtual page/cache invalidation 复杂度 | 大世界、Nanite-like 几何密度或阴影 atlas 预算失败 |
| AA | 当前 FXAA 仅兼容基线；目标 Motion Vector + TAA/TAAU | 商业画质需要时域稳定；必须包含 skinned previous pose 和响应式 mask | ghosting、透明/像素风稳定性和 GPU 预算 |
| GI | 当前 IBL；不立刻复制 Lumen/DDGI | 先保证材质、直接光、阴影和曝光正确 | Hybrid Pixel 场景对动态间接光的收益与预算 |
| GPU-driven | bounds/culling/batching → indirect → bindless/meshlet | 每一级都可独立测量；避免把 feature 名称当性能 | CPU draw submission、可见率、材质切换与 GPU occupancy |
| 透明 | sorted forward alpha 基线 | 简单、正确、可观察；适合近期资产 | 大量交叠粒子/毛发出现排序或过绘制瓶颈时评估 weighted OIT/PPLL |

## 能力覆盖矩阵

| 能力 | Noemancer | Godot Forward+ | Wicked | UE 5.8 高端路径 |
|---|---|---|---|---|
| 线性 HDR / PBR / IBL / tone map | 已有基础闭环 | 成熟 | 成熟 | 成熟、多材质模型 |
| Render Graph | 拓扑、读写验证、稳定 ID；缺 lifetime/barrier/alias | 内部渲染图/RenderingDevice | 以显式 renderer/RHI 调度为主 | RDG 完整编译、culling、barrier、transient/async |
| 多光源扩展 | 单方向光 | Clustered Forward+ | tiled/clustered 与高级路径 | Deferred + 多种 light pass |
| 阴影 | texel-stable 4 CSM、array D32、cascade blend、3×3 PCF 已完成 | CSM/PCSS 等 profile 化 | 多类阴影与光追 | VSM + 传统 shadow paths |
| 时域 | 无 Motion Vector/TAA | TAA/FSR 等 | TAA/FSR/DLSS 接口等 | TSR 与完整 history 管理 |
| GPU-driven | 尚未 | 部分可见性/实例路径 | bindless/indirect | GPU Scene、Instance Culling、Nanite |
| GI/反射 | Split-Sum IBL | SDFGI/SSIL/探针等 | DDGI/SSR/RT | Lumen/RT/reflection environment |
| 高级材质 | glTF standard + alpha | 标准/自定义 shader | 多模型 | layered materials、hair/skin/water/translucency |
| Agent 渲染证据 | 稳定 Pass/Resource ID、对象/深度/法线、隐藏截图 | 不是原生设计中心 | profiler 强但非统一 Agent ABI | 强工具链但不是模型原生统一图 |

所以目前的覆盖结论是：基础 raster correctness 正在成形，但离 Godot/Wicked/UE 的功能广度仍很远。Noemancer 的竞争点不是短期追平所有特性，而是每新增一项能力都同时提供可查询 Schema、源码/资产来源、性能预算、离屏证据和可回滚修改。

## 对接下来开发的直接约束

1. 先完成 IBL Cook 产物和材质 golden，避免运行时启动生成生产资源。
2. 阴影参考 Godot/Filament 的传统稳定 CSM，并核对 UE `ShadowSetup.cpp` 中 production failure modes；第一版不引入 VSM。
3. Render Graph 下一次结构升级直接增加 resource lifetime、load/store、barrier plan 与 evidence，不继续堆只有 pass 名称的假图。
4. Motion Vector/TAA 同时设计 history import/export，这一点参考 Filament FrameGraph temporal resource 与 UE TSR/RDG，而不是事后在 Shader 中补一张纹理。
5. bounds/culling/batching 先建立 CPU/GPU 预算，再决定是否进入 Wicked/UE 风格的 indirect/bindless/GPU Scene。

## 官方公开依据

- [Filament PBR](https://google.github.io/filament/Filament.md.html) 与 [FrameGraph](https://google.github.io/filament/notes/framegraph.html)
- [Godot internal rendering architecture](https://docs.godotengine.org/en/stable/engine_details/architecture/internal_rendering_architecture.html)
- [Godot renderer profiles](https://docs.godotengine.org/en/stable/tutorials/rendering/renderers.html)
- [Unreal Engine rendering introduction](https://dev.epicgames.com/documentation/en-us/unreal-engine/introduction-to-rendering-in-unreal-engine-for-unity-developers)
