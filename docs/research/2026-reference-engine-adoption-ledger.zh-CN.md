# 参考引擎架构采用账本（2026-08-18）

> 文档类别：Historical adoption ledger。当前采用状态以代码、ADR 与能力矩阵为准。

> 目标不是把开源引擎拼装成 Noemancer，而是把成熟实现已经验证过的约束内化成自己的接口、测试和取舍。除第三方依赖外，本次 C++ 实现为重新设计和编写，没有复制参考仓源码。

## 固定参考快照

| 引擎 | 本地提交 | 本次精读入口 |
|---|---|---|
| Bevy | `9f4ff89c1a6a` | `crates/bevy_app/src/plugin.rs`、`main_schedule.rs`、`sub_app.rs`、`crates/bevy_render/src/lib.rs`、`crates/bevy_asset/src/server/mod.rs` |
| Godot | `3000096f9aa6` | `modules/register_module_types.h`、`main/main.cpp`、`core/io/resource_uid.h`、`resource_loader.h`、`scene/resources/packed_scene.h` |
| WickedEngine | `f4a0d2635d52` | `WickedEngine/wiApplication.cpp`、`wiInitializer.cpp`、`wiResourceManager.h`、`wiScene.h` |
| Unreal Engine 5.8.1 (`release`, sparse source reference) | `71fe36aac5a8` | `Renderer/Private/DeferredShadingRenderer.cpp`、`MobileShadingRenderer.cpp`、`RenderCore/Private/RenderGraphBuilder.cpp`、`ShadowSetup.cpp`、`VirtualShadowMaps/`、`Nanite/`、`Lumen/`、`PostProcess/TemporalSuperResolution.cpp`、`InstanceCulling/` |

以后采用某个模式时必须记录到本表：来源只证明模式经过实践，不代替 Noemancer 自己的性能数据和需求判断。

## Adopt / Adapt / Reject

| 问题 | 参考仓中已经验证的机制 | Noemancer 决策 | 当前落点 |
|---|---|---|---|
| 模块依赖 | Bevy 插件不要求用户手排全部运行顺序；Godot 有明确初始化层级 | **Adapt**：模块可乱序注册，由稳定拓扑排序决定初始化；同时约束 Core → Services → Scene → Editor | `EngineHost::validate_and_sort_graph`，并测试逆序注册 |
| 固定更新 | Bevy FixedMain 可零到多次；Godot 有物理步数上限和插值比例；Wicked 使用 accumulator 并钳制异常大帧 | **Adopt semantics**：累积真实帧时长，0..N 次 60 Hz 固定更新，最多 8 次，保留插值 alpha，长帧丢弃债务并明确报告 | `EngineHost::plan_frame` 与测试 |
| 帧阶段 | Bevy 把输入、固定、更新、Extract、渲染分开；Wicked 把 Render 与 Compose 分开 | **Adapt**：九个稳定粗粒度阶段；模块声明订阅掩码，不让全部模块接收全部回调 | `FramePhase`、`ModuleDescriptor::phase_mask` |
| 渲染世界 | Bevy Main World → Extract → Render World，支持流水化 | **Adopt boundary, defer storage**：现在先固定 Extract 边界；真正 Render Snapshot/World 在 Render Graph 里实现，绝不让渲染线程任意读取可变 gameplay world | S0 只有阶段，S2 实现数据面 |
| 初始化耗时 | Bevy 有 build/ready/finish/cleanup；Wicked 在 job system 启动后并行初始化并可显示进度 | **Adapt later**：保留 readiness/progress 需求，但当前没有真实异步工作，不伪造 async API | 等 Job System 和 Shader/Asset 初始化出现后实现 |
| 资产状态 | Bevy 区分资产自身、直接依赖、递归依赖的加载状态；Godot 使用持久 UID 与导入缓存 | **Adapt**：稳定 Asset ID + source/import/cooked 三态 + 依赖 DAG；错误保存完整依赖路径 | S1 Asset Registry/Cooker |
| 场景 | Godot PackedScene 保留树、所有权与可实例化状态；Wicked Scene 以组件数据为运行时核心 | **Adapt**：人类/Agent 可读场景文档是源，运行时编译为 ECS；稳定 ID 不绑定树路径 | S1 Scene Document |
| 插件生命周期 | Bevy 把配置、异步就绪、收尾、清理分开 | **Adapt**：插件声明 schema/capability/dependency；动态库边界用 C ABI + plain data，不暴露 C++ STL/ECS 类型 | S3 Plugin Host |
| 性能诊断 | Godot/Wicked 对固定、更新、渲染等阶段分别计时 | **Adopt**：所有调度阶段天然成为 CPU/GPU evidence span；Agent 查询同一套结构化指标 | S0/S2 逐步接入 |
| 渲染图 | Filament FrameGraph、Bevy Render Graph 与 UE RDG 都把 pass/resource 使用作为调度事实；UE 进一步编译 transient lifetime、barrier、async compute 与 pass culling | **Adapt in layers**：先保持稳定 ID、依赖和读写验证；随后增加 lifetime/barrier/aliasing 计划与后端执行证据，不复制 UE 宏和对象体系 | `RenderGraphCompiler`；下一阶段 `ResourcePlan/BarrierPlan` |
| 主渲染路径 | Godot 桌面采用 Clustered Forward+，Filament以移动带宽和 MSAA 为约束采用 Forward+；UE 高端桌面以 Deferred + Nanite/Lumen/VSM 为主并保留 Mobile/Forward 路径 | **Profile-gated**：当前 Forward PBR 先形成正确基线；灯光压力数据决定 Clustered Forward+，不预先承诺 Deferred；Hybrid Pixel 不承担 UE 高端默认路径的成本 | S4 商业化渲染回炉 |
| 阴影 | Godot/Filament 提供传统 CSM/PCF/PCSS；UE 高端路径使用 Virtual Shadow Maps，同时仍保留传统 shadow setup | **Adapt staged**：先做 texel-stable 4 级 CSM 和可量化预算；只有大世界/高几何密度验证确实需要时再研究 virtualized pages | S4 下一提交 |
| GPU 规模化 | Wicked bindless + 多后端、Bevy phase/render world、UE GPU Scene/Instance Culling/Nanite 分别验证不同层次 | **Adapt boundary, not feature name**：先 bounds/frustum culling、排序和 batching，再按瓶颈进入 indirect、bindless、meshlet；不把 meshlet 宣称为 Nanite | S4 第三阶段 |

## 中间件采用账本

| 领域 | 决策 | 后端/候选 | 引擎保留的所有权 |
|---|---|---|---|
| ECS | Adopt | Flecs | 稳定 Scene/Entity ID、Schema、事务和 Render Extract |
| 物理 | Adopt/Adapt | Jolt | Collider/Query 领域契约、Character Motor、语义 Trace |
| 动画 | Adopt/Adapt | ozz + 待接 ACL | Graph IR、Root Motion、事件、压缩误差证据 |
| UI/文字 | Adopt/Adapt | ImGui、ImGuizmo、RmlUi、FreeType、HarfBuzz、ICU | Semantic UI、Binding/Action、布局/文字观察与 GPU Adapter |
| 音频 | Stop/Replace | miniaudio；可选 Steam Audio/FMOD/Wwise | Audio Event/Bus/Emitter、稳定 Voice ID、事务与诊断投影 |
| glTF | Stop parser / Keep pipeline | fastgltf | 规范化、Asset ID、材质/动画转换、Cook 与 provenance |
| 纹理/网格 | Adopt | KTX2/BasisU、meshoptimizer、TinyEXR/OpenEXR | 平台 Profile、Cook Manifest、预算和误差证据 |
| 网络 | Replace transport / Keep replication | GameNetworkingSockets | Net Entity、Snapshot/Delta、Authority、Prediction 与玩法协议 |
| 导航 | Adopt when demanded | Recast/Detour | NavMesh 资产、Agent Profile、场景来源和查询投影 |
| Shader/诊断 | Adopt tools / Own correlation | Slang、DXC、SPIRV-Tools、Tracy、RenderDoc | Shader Manifest、Pipeline 回滚和跨域证据关联 |

完整理由、退出条件和验收模板见 [2026 中间件采用与自研边界审计](2026-middleware-adoption-and-build-vs-buy-audit.zh-CN.md)。

## 明确不采用

- 不复制 Bevy 的 Rust trait、动态 schedule 或整套 ECS API；Noemancer 热路径需要更可预测的 C++ 数据布局和较小的调度表面。
- 不采用 Godot 大量全局 singleton / Variant 作为底层热路径和公开 Agent ABI；只吸收初始化层级、稳定资源身份和文本场景经验。
- 不采用 WickedEngine 的全局静态子系统模式；只吸收异步初始化、固定步进、Render/Compose 分离和 profiler 分段。
- 不复制 Unreal Engine 源码、宏体系、UObject/RHI/RDG 类型或受 EULA 约束的实现到本项目；UE sparse checkout 只用于理解生产约束和建立对照测试。Noemancer 的实现必须独立设计并可追溯到公开算法/规范或本项目实验。
- 不因“参考引擎已经这样做”就引入中间件。进入核心前仍需许可证、维护状态、平台覆盖、可替换边界和基准测试。

## 对后续开发的约束

1. 新增核心模块前，先指出参考实现解决了什么失败模式。
2. 只记录可以转成接口、断言、基准或验收场景的调研结论。
3. 每项结论标注 Adopt、Adapt、Reject；Adapt 必须说明为什么不原样采用。
4. 参考源码不能泄漏第三方类型到持久格式、公共 C++ API 或 Agent ABI。
5. 没有测量收益的“高级架构”停留在研究账本，不进入运行时代码。
