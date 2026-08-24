# 2026 中间件采用与自研边界审计

> 文档类别：Historical audit。原则仍有效，但实现状态与顺序可能过时；以当前架构和计划为准。

> 日期：2026-08-21  
> 范围：Noemancer 当前 CMake 依赖、约 2.5 万行 `src + managed` 自有代码、四个固定参考引擎，以及近期规划中的音频、资产、网络、动画、导航、Shader 和诊断能力。  
> 结论：早期核心选型整体正确；快速纵切阶段在音频、glTF、网络传输和少量图像处理上留下了临时自研实现。沉没成本仍小，但必须现在冻结扩张并换回成熟后端。

## 决策摘要

Noemancer 的差异化不是物理解算器、音频解码器、字体塑形器或通用网络可靠层。成熟中间件负责算法和高性能执行；引擎负责稳定领域语义、Git 友好资产、编辑体验、跨系统关系、Agent 事务与验证证据。

采用四类决策：

- **Adopt**：直接固定成熟中间件，用窄 Adapter 隔离类型和生命周期。
- **Adapt**：保留成熟内核，自研与项目定位直接相关的资产、调度、语义和工具层。
- **Own**：场景、语义状态平面、事务、证据和 HD2D 等产品核心由引擎拥有。
- **Stop/Replace**：临时纵切不再扩张，先建立兼容测试，再替换后端。

任何第三方句柄、指针、枚举或序列化格式都不得进入 Scene、Prefab、项目资产、C# 公共 ABI 或 Agent RPC Schema。每项中间件接入必须同时记录固定版本、许可证、平台矩阵、能力表、失败降级、升级测试和可替换边界。

## 当前复用情况

| 领域 | 当前实现 | 决策 | 说明 |
|---|---|---|---|
| 平台、窗口、输入、GPU 抽象 | SDL 3.4.14 + SDL_GPU | Adopt/Adapt | 保留引擎 RHI、Render Graph 与证据层，不自研窗口和基础多后端设备创建。 |
| ECS | Flecs 4.1.6 | Adopt | 隐藏在 World API 后；没有基准证明前不自研 ECS。 |
| 物理 | Jolt 5.6.0 | Adopt/Adapt | Jolt 负责 Broadphase、Shape、Solver、Query；引擎负责 Scene 映射、稳定实体、Character Motor 与语义 Trace。 |
| 骨骼动画 | ozz-animation 0.17.0 | Adopt/Adapt | ozz 负责采样和混合；引擎负责 Graph IR、Root Motion、事件、编辑与证据。 |
| FBX | ufbx 0.23.0 | Adopt | 保留坐标/单位规范化、资产验证和 Cook。 |
| 编辑器 UI | Dear ImGui + ImGuizmo | Adopt | 只作为编辑器壳和操控器，不成为持久 UI Schema。 |
| Retained UI | RmlUi 6.2 | Adopt/Adapt | RmlUi 负责 DOM/CSS/Layout/Event；Semantic UI、Binding、Action、文字 ABI 与 GPU Adapter 归引擎。 |
| 文字 | FreeType + HarfBuzz + ICU | Adopt | 不自研字体栅格化、塑形、BiDi 和断行。 |
| C# | .NET 10 HostFXR/CoreCLR | Adopt/Adapt | 运行时采用官方宿主；引擎拥有安全批量 ABI、ALC 生命周期、状态迁移与调试投影。 |
| JSON/PNG | nlohmann/json + lodepng | Adopt | 只在领域边界增加验证、限额和稳定错误。 |

## 必须停止扩张的临时实现

### 音频：Stop/Replace

当前 SDL 只负责设备输出；WAV 解码、常驻 PCM、线性重采样、Bus/Voice Mixer、距离衰减和声像由引擎实现。该纵切已证明 Asset → Runtime → Agent 的边界，但不能继续扩展 OGG/MP3、Streaming、DSP、Voice Virtualization、HRTF、遮挡和混响。

决定：

- 默认开源后端评估并接入 [miniaudio](https://github.com/mackron/miniaudio)，使用其 Resource Manager、Streaming、Decoder、Node Graph、Mixer 与基础 Spatialization。
- 现有确定性 Mixer 收缩成 `Null/Test Audio Backend`，用于 Headless、服务器和逐样本回归。
- 高级空间声学通过 [Steam Audio](https://valvesoftware.github.io/steam-audio/doc/capi/index.html) 可选接入。
- FMOD/Wwise 仅作为许可和项目 Profile 控制的商业 Adapter，不成为开源引擎唯一后端。

### glTF：Stop parser / Keep pipeline

当前约 700 行自有 GLB/glTF 读取负责容器、Accessor、材质、动画和内嵌图片。它对最小素材有效，但不应继续承担完整规范、稀疏 Accessor、Extension、恶意输入和未来升级。

决定：

- 评估并接入 [fastgltf](https://github.com/spnda/fastgltf) 作为规范解析器。
- 保留 Noemancer 的坐标/单位规范化、稳定 Asset ID、材质语义、动画编译、严格验证、Cook Manifest、来源与诊断。
- 迁移前把当前合法/非法 GLB fixture 固定为兼容回归；同一输入的规范 Render/Animation 结果必须保持。

### 纹理与网格 Cook：Adopt

决定：

- 接入 [KTX-Software](https://github.com/KhronosGroup/KTX-Software) 与 Basis Universal，建立 KTX2、Mip、平台 GPU 格式和 Streaming Cook。
- 接入 [meshoptimizer](https://github.com/zeux/meshoptimizer)，负责 cache/fetch 优化、量化、压缩、简化、LOD 与稳定 API 范围内的 Meshlet/Cluster 数据。
- OpenEXR 采用 TinyEXR/OpenEXR；现有 Radiance HDR 小解码器只保留兼容入口，不扩成通用图像库。
- Cook Manifest 记录中间件版本、设置、输入 hash、误差、输出格式、耗时和平台能力。

### 网络传输：Stop/Replace transport / Keep replication

当前原始 TCP/UDP Socket、长度前缀、ACK 和超时只适合验证双进程边界。决定使用 [GameNetworkingSockets](https://github.com/ValveSoftware/GameNetworkingSockets) 承担连接、可靠/不可靠消息、分片重组、重传、加密、统计和通道调度。

保留引擎拥有的稳定 Net Entity ID、Snapshot/Delta、Authority、Prediction/Reconciliation、玩法协议和 Agent 投影。替换的是 Transport，不是 Replication Domain。

### 动画压缩：Adopt ACL

ozz 已接入，后续不自行发明生产级骨骼压缩。采用 [Animation Compression Library](https://github.com/nfrechette/acl) 负责压缩、解压和误差度量；引擎输出压缩率、姿态误差、解压吞吐和失败骨骼的 Semantic Evidence。

### 导航：Adopt Recast/Detour

`navigation.world` 当前只有模块边界。真正进入时采用 [Recast Navigation](https://github.com/recastnavigation/recastnavigation)：Recast 烘焙、Detour 查询、TileCache 分块更新、按需要启用 Crowd。引擎只拥有 NavMesh 资产、场景来源、区域/Agent Profile、查询 API 和调试投影。

### Shader 与诊断：Adopt tools / Own correlation

- [Slang Compilation API](https://docs.shader-slang.org/en/stable/compilation-api.html)：多目标编译、模块、反射和诊断。
- [DXC](https://github.com/microsoft/DirectXShaderCompiler)：DXIL/SPIR-V、PDB、Hash、Reflection、Remarks、Time Trace。
- [SPIRV-Tools](https://github.com/KhronosGroup/SPIRV-Tools)：验证、反汇编、优化、Reducer 与 Fuzzer。
- [Tracy](https://github.com/wolfpld/tracy)：CPU/GPU/内存/锁实时分析。
- [RenderDoc](https://github.com/baldurk/renderdoc)：帧捕获，不自研 GPU 调试器。

Noemancer 自研的是 Shader Manifest、Pipeline 候选/回滚，以及 Scene Entity → Material → Shader Variant → Render Pass → Capture Event → Source Revision 的关联链。

## 不属于重复造轮子的自研范围

- Canonical Scene、Prefab、Project 和 Git 友好源资产。
- Stable ID、Source Anchor、Schema、Revision 和 Capability Registry。
- Semantic State Plane 与 Observation → Plan → Apply → Receipt。
- Semantic UI Document、Binding、Action 和人类/Agent 共用事务。
- ECS → immutable Render World Extract。
- Sprite/Tilemap、Hybrid Pixel Profile 与 Semantic 2D Character Rig。
- Agent 可读的编译、调试、性能和 GPU 证据关联。
- 后端无关的 C# Gameplay API。
- 项目级 Character Motor、Ability/Effect/Event 等领域框架；只有验证游戏提出需求时扩展。

小型自有 Render Graph 当前是合理适配层。它必须继续增加资源 lifetime、barrier、aliasing、pass culling 和 backend evidence，但不应为了“复用”而嵌入另一套完整 Renderer。

## Semantic State Plane 与中间件的统一方式

Semantic State Plane 是权威状态的版本化投影和事务协议，不是第二份 ECS，也不是中间件对象镜像。

```text
Noemancer authoring/configuration
        ↓ domain service
Backend Adapter → Jolt/miniaudio/ozz/RmlUi/Recast/other
        ↓ events, counters, diagnostics
Semantic Projection
        ↓
Editor / C# / Agent / CLI / tests
```

每个字段声明 Authority：

- 引擎权威：资产 ID、场景绑定、配置、路由、Revision、Action Receipt。
- 后端权威：播放游标、求解器统计、设备缓冲、真实运行错误。
- 派生值：有效增益、可见性、距离、预算归因。
- 不投影：PCM、GPU Buffer、内部指针和无界对象图；只返回摘要和 Evidence URI。

公共 ABI 使用稳定基础 Schema + Capability Discovery + 可选扩展，不能把所有后端压成最低公分母。不支持的功能明确返回 `unsupported`；第三方原生句柄永不跨边界。

## 集成验收模板

每个中间件纵切必须同时交付：

1. 固定版本、许可证记录和平台能力矩阵。
2. `I<Domain>Backend` 或等价窄接口，第三方 include 只出现在 Adapter 实现。
3. 旧临时实现对应的兼容 fixture 与结果对照。
4. Headless/Null 后端和明确降级行为。
5. 有界 Semantic Observation、错误码、Capability 与 Evidence URI。
6. 热路径不逐对象 JSON 序列化；观察按需或从低开销计数器投影。
7. 升级时的行为、性能和资产 Cook 漂移测试。
8. 公共 Scene、C#、CLI/MCP Schema 中不存在第三方类型。

## 调整后的优先级

中间件整改不抢占已经接近完成的编辑器/脚本生产闭环，也不一次性推倒现有纵切：

1. 完成当前 Tilemap 稳定 Range Allocator，收口正在进行的 GPU 数据路径。
2. 回到 E1：Asset Browser 缩略图、后台导入/Cook、依赖和修复入口；Play World Inspector/Diff/Apply Back。
3. E2：长寿命 DAP Session、强类型脚本 API 和批量查询预算。
4. 接入 miniaudio，旧 Mixer 降级为测试后端。进度：已固定 0.11.25，替换手写 WAV decoder，并完成私有 Device + SPSC PCM Ring 输出 Adapter；专用生产线程消费保留 transport 游标、共享不可变样本的 World 快照，8 帧 D3D12 隐藏重渲染期间 0 underrun，实时生产已脱离渲染帧节奏。下一步迁移 Resource Manager、Streaming、Node Graph 与 Spatialization，继续删除自研生产混音职责。
5. fastgltf + KTX2/BasisU + meshoptimizer 组成生产 Asset Cook 纵切。
6. ACL 动画压缩和误差证据。
7. Slang/SPIRV-Tools/Tracy/RenderDoc 形成 Agent 可读诊断链。
8. 网络需求进入验证游戏时替换 Transport；导航需求出现时直接接 Recast/Detour。

Job System 在 Asset/Shader/Animation Cook 形成真实并行负载后，用 Taskflow/enkiTS 做基准选择；在此之前不自研线程池，也不为了模块名存在而伪造异步执行。

## 当前代码债务

重复造轮子的沉没成本仍是千行量级，尚未失控。更直接影响长期维护和 Agent 理解的是聚合文件过大：`world.cpp`、`scene_renderer.cpp`、`command_registry.cpp` 已承担过多领域。后续功能迁移同时按领域拆分服务、Adapter、Projection 和 Command Registration，并优先从 Schema 生成重复注册代码。

本轮审计后的首个实现收口也遵守该边界：Tilemap/Sprite 的 Stable Range Allocator、Draw Indirection 和结构化 residency 统计属于 Noemancer 自有的渲染提交策略与语义证据，不是在重写第三方格式解析器或通用音频/网络库。下一工程批次从 Stop/Replace 队列首项开始，把现有音频兼容纵切迁移到 miniaudio Adapter。

本报告是后续中间件采用的权威入口；具体版本被接受后，再分别写 ADR 和锁定依赖。
