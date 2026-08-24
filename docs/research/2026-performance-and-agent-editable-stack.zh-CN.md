# 2026 性能核心与 Agent 全面编辑技术选型

> 文档类别：Historical research。候选工具与性能结论必须用当前 workload 重验，本文不构成待办列表。

> 调研日期：2026-08-18  
> 状态：方向已收敛，具体版本与性能结论仍须通过技术尖峰和基准确认。  
> 配套文档：[Agent 原生引擎研究实施路线](2026-agent-native-engine-roadmap.zh-CN.md)

## 结论

这套引擎不应把 TypeScript 放进每帧热路径。建议采用四层语言结构：

| 层 | 选择 | 用途 |
| --- | --- | --- |
| 性能核心 | C++20，逐步采用经过验证的 C++23 能力 | 内存、任务、ECS、渲染、动画、物理和平台接入 |
| GPU 程序 | Slang 2026，HLSL/DXIL 与 SPIR-V 作为目标和兼容路径 | Shader、GPU 算法、反射、跨后端编译 |
| 玩法与开发者脚本 | C# / .NET 10 | 玩法系统、关卡逻辑、编辑器扩展和快速迭代 |
| 工具与 Agent 控制面 | TypeScript | MCP、IDE 扩展、资产自动化、远程控制和工作流编排 |

C# 是推荐的主脚本层，TypeScript 保留在工具平面。开发态通过 `hostfxr` 托管 CoreCLR，优先保留 Roslyn、调试器和热重载能力；发布态再评估 NativeAOT。微软的原生托管接口可以从 C++ 进程启动 .NET 并取得托管入口函数，但同一进程只能装载一个运行时，而且 `nethost` / `hostfxr` 面向 framework-dependent deployment。这个边界要从第一版集成开始写进设计。[.NET 原生托管文档](https://learn.microsoft.com/en-us/dotnet/core/tutorials/netcore-hosting)

NativeAOT 不应成为开发态默认值。它可以缩短启动时间并降低运行时内存，也适用于禁止 JIT 的平台，但 AOT、裁剪和优化会限制动态代码与热重载。Visual Studio 当前的 .NET Hot Reload 也不支持启用 trimming 或 ReadyToRun 的调试配置。[Native AOT 部署说明](https://learn.microsoft.com/en-us/dotnet/core/deploying/native-aot/)、[.NET Hot Reload 限制](https://learn.microsoft.com/en-us/visualstudio/debugger/hot-reload?view=visualstudio)

这与 Unity 仍然同时保留 Mono 和 IL2CPP 的原因一致：JIT 路径换取较快迭代，AOT 路径换取平台覆盖与发布性能，但要付出更长的生成、编译和链接时间。[Unity 脚本后端说明](https://docs.unity3d.com/ja/current/Manual/scripting-backends-intro.html)

## 为什么不是 TypeScript 主运行时

AI 确实很擅长写 TypeScript，但模型熟悉某种语法不等于该语言适合承担引擎热路径。TypeScript 最终仍要依赖 JavaScript VM 或转译目标。把它作为主玩法运行时，会额外引入 VM 体积、GC、值类型表达、原生边界、调试一致性、AOT 和主机平台部署问题。

C# 在这几个方面更合适：

- Roslyn 能提供稳定的语义模型、诊断、源码位置和代码生成接口，Agent 不必只处理文本。
- `struct`、`Span<T>`、泛型和 SIMD 能较自然地表达游戏数学与批量数据。
- Visual Studio 的托管、原生和混合调试路径成熟。
- C# 与 C++ 可以由同一份 Engine Schema 生成绑定，避免手写两套 API。

近期引擎开发社区的讨论也给出了相似但不完全一致的结论：C# 的类型、值类型、跨平台和混合调试受到认可，主要疑问集中在 `AssemblyLoadContext` 卸载、`GCHandle` 生命周期与沙箱；另一些开发者仍倾向 Lua、Luau 或 WASM，因为嵌入和热替换更简单。这说明我们不能把 C# 集成本身当作卖点，更不能把程序集卸载等同于可靠的状态热重载。[2026 年 8 月 r/gameenginedevs 讨论](https://www.reddit.com/r/gameenginedevs/comments/1vidmyw/which_scripting_language_should_i_use_in_my_engine/)

我们的解决方法是让权威玩法状态留在版本化 ECS Component 中。托管对象只保存短生命周期句柄，不拥有引擎资源，也不把复杂对象图作为存档。脚本重载按下面的事务进行：

```text
编译候选 Assembly
-> 校验 Engine ABI 与 Component Schema
-> 建立新的脚本上下文
-> 在安全帧点停止旧系统
-> 迁移显式版本化状态
-> 运行验证场景
-> 切换或回滚
```

TypeScript 继续承担 MCP sidecar、Agent 工作流、编辑器前端和资产工具。它不需要跨越每帧 C++/JS 边界，也不会决定游戏运行时的性能上限。

## 性能核心的当前选型

### Runtime、ECS 与任务系统

C++20 核心维持数据导向设计。性能来自数据布局、批处理、离线 Cook、任务调度和减少边界调用，不来自把所有逻辑都写成 C++ 类。

Flecs 继续用于当前阶段，但必须藏在 Engine World API 后面。现在换自研 ECS 会消耗大量时间，却还没有证据表明 Flecs 是瓶颈。每个高频 System 都应暴露查询签名、读写集合、批大小、任务依赖、耗时和缓存指标。等代表性场景建立后，再把 Flecs 与定制 archetype/chunk 实现赛马。

任务系统应由引擎拥有稳定的 Job Graph IR，底层执行器暂不锁死。这个图需要显示任务依赖、读写资源、线程亲和性、阻塞原因和关键路径。执行器可以替换，Agent 看到的 ID、Schema 和 Trace 不变。

### 物理：Jolt Physics

Jolt 是第一选择。它是面向多核的 C++ 刚体与碰撞库，支持 x86 SIMD 与 ARM NEON，并提供性能测试和记录工具。其确定性也有明确边界：相同二进制、相同调用顺序时可确定；跨平台确定性需要专门选项，官方说明其代价约为 8%，查询结果和多线程回调仍可能需要排序。[Jolt 仓库](https://github.com/jrouwe/JoltPhysics)、[Jolt 架构与确定性说明](https://github.com/jrouwe/JoltPhysics/blob/master/Docs/Architecture.md)

Godot 4.6 已把 Jolt 设为新 3D 项目的默认物理后端，因此“使用 Jolt”在 2026 年已经是稳妥选型，不是项目特色。[Godot 4.6 发布说明](https://godotengine.org/releases/4.6/)

我们要增加的是 Physics IR 与证据层：

- Body、Shape、Constraint、Material、Layer 和 Character 使用稳定 ID。
- Agent 查询一帧的 broad phase、contact manifold、constraint island、solver iteration 和休眠变化。
- 所有修改先生成 Diff，可 dry-run，并能在固定时间步、固定输入的场景中回放。
- 物理修改的 Receipt 包含穿透、能量漂移、接触抖动、步进耗时和确定性散列。
- Jolt 的原始记录和引擎实体、场景资产、源码 revision 可以互相定位。

### 动画：ozz-animation、ACL 与自研 Animation Graph IR

ozz-animation 负责骨骼采样、混合和运行时数据布局。它本身就是 renderer-agnostic、data-oriented 的低层 C++ 运行库，并针对性能与内存约束设计。[ozz-animation](https://github.com/guillaumeblanc/ozz-animation)

Animation Compression Library（ACL）负责离线压缩与快速解压。ACL 把误差、解压速度和内存占用作为显式目标，还提供中间 Clip 格式与基准数据，适合接入可验证的资产管线。[ACL](https://github.com/nfrechette/acl)

ozz 和 ACL 不能替代动画系统。引擎仍要拥有版本化 Animation Graph IR，覆盖 State、Blend、Layer、IK、Root Motion、Pose Search、Motion Matching 和事件。Agent 修改的是图和参数，不是编辑器二进制资产。每次修改至少验证：

- 每角色采样、混合、IK 与 skinning 的 CPU/GPU 时间。
- Clip 内存、解压吞吐和 ACL 误差。
- foot sliding、姿态跳变、Root Motion 误差和事件时序。
- 图节点、Pose Search 选择、输入轨迹和最终骨骼姿态之间的可追溯关系。

Unreal Engine 5.8 已有成熟的 Pose Search、Motion Matching、PCA/KDTree 搜索和 Rewind Debugger。我们短期内不可能在动画功能广度上超过它。[UE 5.8 Motion Matching](https://dev.epicgames.com/documentation/unreal-engine/motion-matching-in-unreal-engine?lang=en-US)、[Motion Matching 调试](https://dev.epicgames.com/documentation/unreal-engine/motion-matching-debugging-in-unreal-engine?lang=en-US)

可形成差异的方向是让 Agent 直接查询“为什么选了这一帧 Pose”、修改搜索权重、重建数据库，并自动比较脚滑、响应延迟、内存和耗时，而不需要理解编辑器私有对象图。

### Shader：Slang 2026

Shader 建议采用 Slang 2026 作为权威源码语言。Slang 提供模块、泛型、接口、反射和中间 IR，一次前端分析可以生成 DXIL、SPIR-V 等多个目标；官方目标还覆盖 Metal、CUDA、CPU 与 WebGPU。它比预处理器宏堆叠更适合机器查询和安全重构。[Slang 编译模型](https://shader-slang.org/slang/user-guide/compiling)、[支持的编译目标](https://shader-slang.org/slang/user-guide/targets)、[模块系统](https://shader-slang.org/slang/user-guide/modules)

接入时使用 Compilation API，而不是只调用 `slangc`。Reflection API 能给出参数布局和绑定信息，编译产物再生成 Shader Manifest。需要注意，Slang 当前模块会缓存在 Session 中；社区维护者给出的热更新做法是重建 Session。我们的编译服务要把 Session 生命周期、缓存键和失效原因显式化。[Slang Reflection API](https://docs.shader-slang.org/en/stable/external/slang/docs/user-guide/09-reflection.html)、[Slang 模块热更新讨论](https://github.com/shader-slang/slang/discussions/10054)

Shader Agent 能力应覆盖：

- 查询模块、入口、宏/特化、资源布局、Render Pass、Pipeline 与后端产物。
- 修改 Slang 源码或 Shader Graph IR 后，先编译候选版本并检查所有目标。
- 自动运行截图、像素差、GPU 时间、寄存器压力、occupancy 和 Pipeline 数量检查。
- 热替换只发生在安全点；失败时继续使用旧 Pipeline，并返回结构化诊断。
- 每个 Shader、Pipeline 和 Render Pass 都能回到源码、编译器版本、参数和 Git revision。

### 渲染：保留 SDL_GPU 启动层，建立自有 RHI 与 Render Graph

SDL_GPU 当前覆盖 D3D12、Vulkan 与 Metal，适合快速建立跨平台渲染切片。[SDL_GPU 文档](https://wiki.libsdl.org/SDL3/CategoryGPU) 现阶段不应立即删掉它，但也不能把其便携特性集当成最终性能上限。

引擎应先在 SDL_GPU 之上建立自己的 RHI、Render Graph 和资源身份层。M5 技术尖峰再决定三件事：扩展 SDL_GPU、取得 Native Handle，或增加独立 D3D12/Vulkan 后端。若 mesh shader、细粒度 barrier、residency、GPU crash diagnostics 或 bindless 模型受阻，就启用独立后端，SDL3 仍负责窗口、输入和平台启动。

渲染数据路线建议采用：

- Render Graph 编译资源生命周期、barrier、aliasing 和队列同步。
- GPU-driven instance/meshlet culling，能力不足的设备回退到 indexed indirect。
- meshoptimizer 负责 mesh cache/fetch 优化、简化、meshlet 和 cluster 数据；2026 版本已经提供面向分层 cluster LOD 的分组能力，但其中一些 API 仍标为 experimental，必须固定版本并保存 Cook Manifest。[meshoptimizer](https://github.com/zeux/meshoptimizer)
- D3D12 使用 D3D12 Memory Allocator，Vulkan 使用 Vulkan Memory Allocator；内存预算、分配、aliasing 和 residency 进入统一资源 Trace。[D3D12 Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator)、[Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator)

这条路线不是复制 Nanite。UE 5.8 的 Nanite 已有专用压缩格式、细粒度流送和像素尺度几何，单靠 meshlet 加 GPU culling 不能称为同等级方案。[UE 5.8 Nanite](https://dev.epicgames.com/documentation/unreal-engine/nanite-in-unreal-engine?lang=en-US)

## 与 2026 SOTA 的差异

现在不能宣称性能突破。UE 5.8 在 Nanite、Motion Matching、Mass 和完整工具链上远超本项目；Unity 已把 C#、Entities、Burst 和 AOT/JIT 双路径做成成熟产品；Godot 4.6 的工作流、平台覆盖和 Jolt 集成也比我们完整。UE 5.8 的 Mass 甚至已经加入无锁 archetype 调度、稀疏 Fragment 和更细的多核依赖解析。[UE 5.8 Mass 更新](https://dev.epicgames.com/documentation/unreal-engine/unreal-engine-5-8-release-notes)、[Unity Entities 1.4](https://docs.unity3d.com/kr/current/Manual/com.unity.entities.html)

我们有机会做出的突破不在某个渲染算法或脚本语言，而在下面这组组合能力：

1. 同一份 Engine Schema 生成 C++、C#、CLI、MCP、编辑器和文档接口，Agent 不需要在多套绑定之间猜测。
2. World、Build、Job、Physics、Animation、Render Graph 和 Shader 都有版本化、可查询、可 Diff 的中间表示。
3. 一次 Agent 修改是事务：查询影响范围、生成候选、编译、在安全点替换、运行场景、比较证据、提交或回滚。
4. Evidence Plane 用稳定 ID 把源码、构建动作、脚本、实体、任务、动画 Pose、物理接触、Render Pass、Shader、GPU 事件和硬件连成一条证据链。
5. Agent 的结论必须附 Action Receipt、性能比较和正确性检查，而不是以“编译通过”结束。

现有 Agent 接入方案已经能控制编辑器、运行场景、读日志、设断点或包装 MCP。仅仅增加命令和聊天框并不新颖。我们的研究问题更窄也更难：Agent 能否在不知道引擎全部实现细节的情况下，安全地修改一条底层执行图，并证明性能更好、结果仍然正确。

## “极优性能”的验收方式

“最快”必须绑定目标硬件、场景和质量条件。项目不设一个模糊总分，而是维护一组可复现预算：

| 子系统 | 首批指标 |
| --- | --- |
| Build | clean/incremental P50、P95，编译动作数，关键路径，缓存命中 |
| ECS/Job | 每实体成本，chunk 利用率，任务开销，关键路径，等待时间 |
| Animation | 每角色 CPU/GPU 时间，Clip 内存，解压吞吐，姿态误差，脚滑 |
| Physics | 每步耗时，active body/constraint 数，solver iteration，穿透与漂移 |
| Render | CPU submit、GPU frame、barrier、显存峰值、occupancy、可见 meshlet 比例 |
| Shader | 冷/热编译时间，variant 数，Pipeline 创建时间，寄存器与指令统计 |
| Script boundary | 单次和批量跨边界成本，分配量，GC pause，热替换时间与成功率 |
| Agent | 首次修复成功率、上下文量、工具调用数、回滚率、证据完整率 |

每项比较都固定源码 revision、编译器、依赖、硬件、驱动、场景、随机种子与质量设置。没有这些信息，只能说某次运行更快，不能说架构更快。

## 实施顺序

1. 先完成现有 M1-M4：稳定身份、World Diff、Build Explain 和统一 Trace。没有这些基础，后面的“Agent 全面编辑”无法验证。
2. 做 C# 托管尖峰：`hostfxr` 启动、Schema 生成绑定、批量 ECS API、异常映射、程序集替换和显式状态迁移。
3. 接入 Jolt，先交付 Physics IR、固定步长回放和结构化 contact/solver Trace。
4. 接入 Slang Compilation API，建立 Shader Manifest、Reflection、候选 Pipeline 和失败回滚。
5. 接入 ozz 与 ACL，建立最小 Animation Graph IR、压缩报告和姿态验证。
6. 建立 Render Graph，完成 SDL_GPU 能力审计后再决定独立 RHI Backend，避免现在重写整个渲染层。
7. 最后才做 Motion Matching、cluster LOD、GPU-driven rendering 等高阶能力。每项都要同时交付 Agent 查询、事务修改和基准，不接受只有画面 Demo 的实现。

这个选型保留了快速做出引擎的现实路径：成熟中间件承担物理、动画运行时、压缩和 Shader 编译；项目把有限研发时间投向 Engine Agent ABI、可执行中间表示、证据关联与安全热替换。性能目标很高，但每个“极优”都必须由报告里的预算证明。
