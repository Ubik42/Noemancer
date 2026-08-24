# 2026 Agent 原生引擎生态调研

> 文档类别：Historical research（非当前计划）。技术候选只有被当前 ADR/架构/计划采纳后才生效。

> 调研日期：2026-08-18
>
> 研究问题：怎样做一套便于 Git/SVN 协作、编译迭代快、脚本友好，并且能让 Agent 调试到 CPU、GPU、Shader、驱动和构建系统的引擎。
>
> 结论性质：项目方向建议。项目事实、社区经验和本文判断分开陈述；具体选型仍需用本项目的基准测试验证。

关于通用 Coding Agent、Shell、direct tool、LSP/DAP/MCP 以及运行时热补丁接口的进一步修正，见 [Agent 接口架构补充调研](2026-agent-interface-architecture.zh-CN.md)。该补充报告明确指出：仅有文本资源、Headless、MCP 和运行时桥在 2026 年已经不能构成充分差异。

## 先说结论

我们不应该把项目定位成“又一个可以让 AI 创建实体的引擎”。Unity、Godot 周边已经出现大量 MCP 项目，编辑器遥控很快会成为普通能力。真正稀缺的方向是：让引擎具备可追溯、可查询、可比较的工程证据，使 Agent 能回答下面这些问题。

- 为什么改了一行代码，却重新编译了 184 个编译单元？
- 哪个头文件、模板实例化或生成步骤导致构建时间回退？
- 当前 Shader 二进制来自哪个源码版本、宏组合和编译器版本？
- 哪个 ECS System 产生了这个 Render Pass，哪条 Barrier 之前出现了错误状态？
- GPU Device Removed 发生前，使用的是哪张显卡、哪个驱动、哪个 Pipeline 和哪个资源？
- 两次运行之间，代码、资产、World 状态、CPU/GPU 时间和硬件环境分别发生了什么变化？

本文暂时把这项核心能力称为 **Engine Evidence Plane（引擎证据层）**。它不是新的 Profiler，也不取代 RenderDoc、PIX、Nsight、Tracy、LLDB 或 Perfetto。它负责给已有工具和引擎内部对象建立稳定身份，并把来源、时间线和诊断产物关联起来。

建议保留当前 C++20 核心、Flecs 反射、Headless、JSON-RPC 和 MCP sidecar。接下来的优先级应当是：

1. 先建立确定性序列化、稳定 ID 和语义 Diff/Merge，保证人和 Agent 的修改可以审查。
2. 把构建过程变成可观测系统，提供 `build explain`、`build compare`，而不只是显示编译输出。
3. 建立统一的诊断 ID 和 Diagnostic Bundle，把构建、运行时、Shader、GPU 与崩溃证据串起来。
4. 在宿主 API 和状态所有权稳定以后，再对 Luau、AngelScript、daScript 做本项目实测；不要现在自创语言。
5. MCP 只作为一个适配器。CLI、编辑器、人类开发者和 Agent 应使用同一套受权限约束的查询与操作能力。

## 调研方法和边界

本轮查看了引擎官方文档、项目仓库、调试器和性能工具文档，以及游戏开发、引擎开发、C++ 社区的讨论。项目仓库用于确认能力是否真实存在；Reddit 只用来了解开发者遇到的摩擦和争议，不能代替技术验证。

重点样本超过 30 个，覆盖以下类别：

- 引擎与资产格式：Godot、Unity、Unreal Engine、O3DE、Bevy、ezEngine、Wicked Engine、The Forge。
- 构建与原生迭代：CMake/Ninja、sccache、Clang Time Trace、ClangBuildAnalyzer、IWYU、Unreal Build Accelerator、Live++/Live Coding。
- 脚本与隔离执行：Luau、AngelScript、daScript、Beef、Wasmtime。
- 运行时与 GPU 诊断：Perfetto、Tracy、Rerun、RenderDoc、DRED、Nsight Aftermath、Vulkan Validation、Fossilize。
- Shader 工具链：DXC、SPIRV-Tools、Slang。
- Agent 调试桥：LLDB MCP、Microsoft DebugMCP、gdb-mcp、renderdoc-mcp、perfetto-mcp-rs。
- 崩溃采集：Crashpad、sentry-native。

没有把 GitHub Star 当作技术质量排序。Star 可以说明关注度，不能证明架构适合本项目。商业引擎的闭源部分也无法像开源仓库一样审计，所以只采用其公开文档能确认的事实。

## 一、Git 与 SVN 友好：文件可提交还远远不够

### 成熟项目实际采用的办法

[Godot 的 TSCN](https://docs.godotengine.org/en/4.2/contributing/development/file_formats/tscn.html) 是人类可读的文本场景格式，节点、资源和连接具有明确结构。资源 UID 用来追踪移动后的文件，导入后的平台产物则保存在缓存中。它说明“权威源文件”和“生成/导入产物”应分开。

[Unity SmartMerge](https://docs.unity3d.com/ja/current/Manual/SmartMerge.html) 说明另一个现实：YAML 可读并不等于 Git 能正确合并。Unity 为 `.unity` 和 `.prefab` 提供理解对象结构的三方合并工具，并给出 Git、Mercurial 和 SVN 的接入方式。语义合并是引擎能力，不该甩给普通文本合并器。

[Unreal One File Per Actor](https://dev.epicgames.com/documentation/unreal-engine/one-file-per-actor-in-unreal-engine) 没有强行把所有资产改为文本，而是把关卡 Actor 拆成外部文件，降低多人同时编辑主关卡文件的冲突概率。它也暴露了代价：外部文件名需要编辑器解释，引用和 Changelist 仍需专门工具处理。

[O3DE Source Assets](https://www.docs.o3de.org/docs/user-guide/assets/pipeline/source-assets/) 使用 JSON 表示材质、Prefab 等源资产，并用时间戳、内容哈希和 Builder 指纹判断处理状态。[O3DE Prefab 架构](https://www.docs.o3de.org/docs/engine-dev/architecture/prefabs/) 以 JSON DOM 和 Patch 表达嵌套实例的覆盖。这套设计值得研究，但“尽力应用 Patch”也提醒我们：路径漂移和结构升级必须产生可见诊断，不能静默猜测。

### 社区反馈

近期关于[大型游戏资产仓库版本控制](https://www.reddit.com/r/gamedev/comments/1rrl0mg/is_version_control_for_large_game_asset_repos/)的讨论仍然频繁提到二进制锁、Perforce 集成和场景冲突。另一类开发者则喜欢 [Godot 文本场景](https://www.reddit.com/r/gamedev/comments/1dpu699) 带来的普通 Diff 体验。这些是个体经验，但共同说明：文本格式、对象拆分、锁和语义合并解决的是不同问题。

### 对本项目的要求

建议把版本控制兼容写进资产和场景协议，而不是以后补一个 Git 插件。

- 一个语义对象对应一个文件，或对应有明确上限的 Chunk。不要把整个世界塞进一个文件，也不要把每个微小字段拆成海量文件。
- 每个资产、实体原型、组件 Schema、Render Pass 和脚本模块都有持久 GUID；运行时句柄不得写入源文件。
- 序列化必须规范化：字段顺序、浮点格式、默认值省略规则、换行和编码固定。
- 所有格式带显式版本；迁移命令输出迁移前后摘要、丢失字段和验证结果。
- 提供 `engine diff` 和 `engine merge`。输出既有人类摘要，也有 JSON 冲突对象、路径、双方值和建议操作。
- 编辑器操作记录为事务。Agent 的一次修改能够预览、提交、撤销，也能对应到一次 Git/SVN Changelist。
- 源资产、派生数据、平台 Cook 产物和本地缓存分目录、分身份管理。
- 核心层不绑定 Git 或 SVN；上层适配器负责状态、锁和 Changelist，语义 Diff/Merge 对两者通用。

这一部分可以直接参考 Godot/O3DE 的文本源格式和 Unity SmartMerge 的协议思路，但格式和合并器需要我们自己围绕 ECS、反射 Schema 实现。

## 二、语言与编译：先优化反馈回路，再谈换语言

### C++ 仍适合当前目标

本项目关注 D3D12/Vulkan、驱动诊断、内存、任务系统和 CPU/GPU 关联。C++20 具有成熟的平台 SDK、调试器、Sanitizer、Profiler 和图形工具支持。现在改写成 Rust、C# 或自有语言，会同时改变太多变量，也会推迟真正有辨识度的诊断层。

这不等于所有代码都必须是 C++。建议形成四条边界：

- C++20：运行时核心、渲染、任务、资产 Cook 和性能敏感模块。
- C ABI 或版本化 RPC：插件与工具边界，避免把 C++ ABI 扩散到所有扩展。
- TypeScript：保留在 MCP 和开发服务侧，不进入逐帧热路径。
- 脚本 VM：以后承担玩法和快速迭代，状态由引擎 Schema 管理。

### 社区项目怎样缩短原生迭代

[Bevy 的快速编译配置](https://bevy.org/learn/quick-start/getting-started/setup/) 同时使用动态链接、LLD 和开发依赖优化；项目文档也明确承认默认配置的编译等待较长。[Godot GDExtension](https://docs.godotengine.org/en/stable/engine_details/engine_api/gdextension/index.html) 让原生共享库在不重编引擎的情况下装载，[示例文档](https://docs.godotengine.org/en/stable/tutorials/scripting/cpp/gdextension_cpp_example.html) 还包含可重载扩展配置。

[Unreal Live Coding](https://dev.epicgames.com/documentation/en-us/unreal-engine/using-live-coding-to-recompile-unreal-engine-applications-at-runtime) 使用 Live++ 在进程运行时补丁 C++ 二进制，并处理对象重实例化；它也有构造默认值、平台和对象生命周期方面的限制。[Unreal Build Configuration](https://dev.epicgames.com/documentation/unreal-engine/build-configuration-for-unreal-engine) 展示了 Unity Build、自适应 Unity、依赖缓存、并行执行、XGE、FASTBuild 和 UBA 等组合。没有一项技术单独解决编译速度。

[ezEngine 的 C++ Code Reload](https://ezengine.net/pages/docs/custom-code/cpp/cpp-code-reload.html) 和插件边界也值得作为小型引擎参考。[Wicked Engine](https://github.com/turanszkij/WickedEngine) 与 [The Forge](https://github.com/ConfettiFX/The-Forge) 则展示了 C++ 引擎怎样把 Shader 编译、运行时重载、自动测试和性能工具接入一套开发流程。

### 可以直接采用的构建轮子

- [CMake](https://cmake.org/) 与 [Ninja](https://ninja-build.org/)：继续作为可移植生成和细粒度并行构建基础。
- [sccache](https://github.com/mozilla/sccache)：作为 MSVC/Clang/GCC/Rust/NVCC 的编译缓存候选，先做单机命中率和正确性基准，再决定远程缓存。
- [Clang `-ftime-trace`](https://clang.llvm.org/docs/UsersManual.html)：导出每个编译单元的前端、模板和后端时间线。
- [ClangBuildAnalyzer](https://github.com/aras-p/ClangBuildAnalyzer)：分析全量构建中头文件解析和模板实例化成本。
- [Include What You Use](https://github.com/include-what-you-use/include-what-you-use)：找出不必要包含；只作为建议工具，不应无审查地批量改代码。
- LLD/LLD-link：在平台兼容时作为快速链接器实验项。

### 不建议现在押注的方案

C++ Modules 值得试验，但不适合作为当前架构前提。社区关于[实际收益](https://www.reddit.com/r/cpp/comments/1hv0yl6)仍有构建系统、IDE、第三方库和增量表现方面的分歧。Unity Build 对干净构建常有帮助，但[可能伤害小改动的增量构建并掩盖包含关系](https://www.reddit.com/r/cpp/comments/1cfug59)。两者都应在本仓库采样后选择，而不是按口号采用。

### 我们可以做出特色的地方：Build Explain

工具已经能生成时间数据，社区缺的是面向日常修改的解释层。建议引擎提供：

- `engine build trace`：采集配置、编译器版本、目标、每个动作、缓存结果、耗时和依赖摘要。
- `engine build explain <change>`：解释某文件为什么让哪些目标失效，列出最短依赖路径和主要成本。
- `engine build compare <A> <B>`：比较两次构建的前端、模板、代码生成、链接、缓存和关键路径。
- 每次 CI 保存 Build Manifest：源码 revision、工具链、宏、依赖锁、产物 hash、符号文件和 Shader hash。
- 设立反馈预算，例如“编辑一个普通 System 后，P50 增量构建不超过某值”，以真实硬件基线校验。

这比单纯宣称“编译快”更适合 Agent：它能判断自己扩大了哪些依赖，并用数据证明优化是否有效。

## 三、脚本层：先确定状态边界，再选择 VM

热重载最难的部分通常不是重新编译代码，而是旧实例、引用、协程和状态迁移。[AngelScript 动态构建文档](https://angelcode.com/angelscript/sdk/docs/manual/doc_adv_dynamic_build.html) 明确要求宿主跟踪实例和文件，并建议先在临时模块中验证编译，再替换正式模块。这正是引擎必须承担的职责。

### 候选项目

| 候选 | 已有能力 | 适合本项目的部分 | 主要疑问 |
| --- | --- | --- | --- |
| [Luau](https://github.com/luau-lang/luau) | 可嵌入、渐进类型、解释器/JIT、类型检查和 Lint | Agent 可先做类型检查；编译快；Lua 风格易嵌入；可设计能力沙箱 | 通用引擎热重载与状态迁移需要宿主实现 |
| [AngelScript](https://www.angelcode.com/angelscript/sdk/docs/manual/doc_overview.html) | 强类型、C++ 风格、字节码 VM、运行时反射和调试接口 | 原生开发者容易理解；宿主绑定明确；热重载文档成熟 | 生态和编辑器工具规模较小；绑定维护成本需实测 |
| [daScript](https://github.com/GaijinEntertainment/daScript) | 强类型、面向游戏、接近 C++ 布局、项目宣称支持快速编译和热重载 | FFI 与游戏场景导向很有吸引力 | 主要能力来自项目自身说明，需要本项目压测和工具审计 |
| [Beef](https://github.com/beefytech/Beef) | 原生性能语言、手动内存、热代码替换、配套 IDE/调试器 | 可研究其热替换和开发体验 | 引入的是更完整的语言工具链，不适合首个脚本里程碑 |
| [Wasmtime](https://github.com/bytecodealliance/wasmtime) | WebAssembly、资源限制、fuel、epoch interruption、组件/宿主接口 | 适合以后做不可信插件、工具扩展和可中断任务 | 游戏玩法 API、调试体验、跨边界开销和热状态迁移仍需额外建设 |

[Luau 性能文档](https://luau.org/performance/) 给出了编译、解释器和可选 JIT 的设计目标；这些是上游项目数据，采用前必须用我们的 ECS、数学类型、事件和实体访问工作负载复测。[Wasmtime Store](https://docs.wasmtime.dev/api/wasmtime/struct.Store.html) 能限制内存/实例，使用 fuel 或 epoch 中断执行，这对 Agent 生成的不可信扩展很有价值，但没必要抢在玩法脚本之前接入。

### 推荐路线

第一阶段不要公开脚本 API。先完成反射 Schema、稳定对象 ID、错误模型和状态所有权：

- 权威玩法状态放在 ECS Component 或版本化资源中，不藏在不可检查的 VM 对象图里。
- 脚本模块主要提供行为；重载时先编译与类型检查，再在安全帧边界交换行为。
- 状态迁移由 Schema 版本和迁移函数显式完成，失败后保留旧模块并输出结构化错误。
- 同一份 API Schema 生成 C++ 注册、脚本类型声明、文档和 Agent 工具描述。
- 每个脚本错误带 module ID、entity ID、system ID、source span、frame ID 和相关日志。

第二阶段做 Luau、AngelScript、daScript 三方 Bake-off。统一测试：冷/热编译时间、调用和数据搬运成本、调试信息、类型诊断、热重载、状态迁移、沙箱、内存上限和 Agent 修复成功率。当前倾向是 Luau 作为通用玩法脚本，C++ 插件作为性能通道，Wasmtime 留作以后不可信扩展；这只是待验证假设。

## 四、底层调试：轮子很多，信息仍然割裂

### CPU、任务和时间线

[Perfetto](https://github.com/google/perfetto) 的 Trace Processor 把 Trace 导入列式 SQL 数据库，并提供 [Python API](https://perfetto.dev/docs/analysis/trace-processor-python) 和[嵌入/命令行查询](https://perfetto.dev/docs/contributing/embedding)。它非常适合 Agent 做可复现查询、统计和回归比较。

[Tracy](https://github.com/wolfpld/tracy) 已覆盖 CPU/GPU Zone、内存、锁、上下文切换和截图，交互式体验成熟。建议把 Tracy 作为人类实时分析前端之一，把可长期保存、可 SQL 查询的数据同时映射到 Perfetto 或自有稳定 Schema。二选一会损失另一侧优势。

[Rerun](https://github.com/rerun-io/rerun) 擅长时间—空间、多模态数据的记录和可视化。以后可以用于 World、相机、路径、传感器或渲染中间结果，不宜把它当底层 CPU/GPU Profiler 的替代品。

[OpenTelemetry Semantic Conventions](https://opentelemetry.io/docs/concepts/semantic-conventions/) 值得借鉴其稳定命名和 Trace/Metric/Log 关联方式。初期不建议把完整 OTel SDK 放进帧热路径，但应学习 trace ID、resource attributes 和跨信号关联。

### GPU、驱动和 Shader

[RenderDoc](https://github.com/baldurk/renderdoc) 是跨 API 帧捕获基础设施。Khronos 已发布 [AI-assisted RenderDoc 集成教程](https://github.khronos.org/Vulkan-Site/tutorial/latest/AI_Assisted_Vulkan/06_debugging/03_renderdoc_ai_integration.html)，说明结构化 Pipeline、资源和验证信息确实适合 Agent 分析。[renderdoc-mcp](https://github.com/JiaboLi-GitHub/renderdoc-mcp) 进一步证明帧捕获可以通过结构化工具读取。我们应优先做适配和关联，不重写帧调试器。

Windows D3D12 可以使用 [DRED](https://devblogs.microsoft.com/directx/dred/) 获取 Auto-breadcrumb、Page Fault 和最近释放对象等设备移除信息；NVIDIA 的 [Nsight Aftermath](https://developer.nvidia.com/nsight-aftermath) 可生成 GPU Mini-dump，并把故障关联到 Shader 与应用元数据。Vulkan 侧可使用 [GPU-assisted validation](https://docs.vulkan.org/tutorial/latest/Advanced_Vulkan_Compute/11_Diagnostics_and_Refinement/02_compute_validation.html) 和 Debug Printf。[Fossilize](https://github.com/ValveSoftware/Fossilize) 能序列化 Vulkan Pipeline 等持久对象，用于跨设备回放、缓存预热和驱动问题复现。

Shader 工具链可直接建立在 [DXC](https://github.com/microsoft/DirectXShaderCompiler)、[SPIRV-Tools](https://github.com/KhronosGroup/SPIRV-Tools) 和以后可评估的 [Slang](https://github.com/shader-slang/slang) 上。DXC API 能返回错误、反射、PDB、Remarks 和 Time Trace；SPIRV-Tools 提供验证、反汇编、优化、Reducer 和 Fuzzer。这些数据应进入 Shader Manifest，而不是只显示在编译终端。

### 原生调试器已经开始面向 Agent

- [LLDB MCP](https://lldb.llvm.org/use/mcp.html) 是 LLVM 官方提供的 MCP 接口，可执行 LLDB 命令、断点、步进和内存检查。
- [Microsoft DebugMCP](https://github.com/microsoft/DebugMCP) 通过 VS Code 调试协议向 Agent 提供多语言调试能力。
- [gdb-mcp](https://github.com/BeaCox/gdb-mcp) 使用 GDB/MI 暴露会话、Core Dump、远程调试、栈帧和局部变量。
- [perfetto-mcp-rs](https://github.com/0xZOne/perfetto-mcp-rs) 与 renderdoc-mcp 表明性能 Trace 和 GPU Capture 也在形成 Agent 接口。

这些项目适合直接拉取做参考或作为外部工具接入。它们共同的局限是：调试器不知道哪个 ECS System 创建了 Render Pass，RenderDoc 不知道哪次 Git 修改触发了 Shader 重编译，Perfetto 也不知道一个 Zone 对应哪个资产版本。关联信息必须由引擎生成。

## 五、建议形成的核心架构：Engine Evidence Plane

```text
Git / SVN / 场景语义事务
          |
          v
Build Observer ---- Compiler / Linker / DXC time trace
          |
          v
Artifact Registry -- build_id / symbol_id / shader_hash / asset_guid
          |
          v
Runtime Telemetry -- frame / task / ECS system / allocation
          |
          v
Render Diagnostics - pass / resource / barrier / command / GPU marker
          |
          v
Capture Adapters --- Perfetto / Tracy / RenderDoc / DRED / Aftermath
          |
          v
Diagnostic Bundle -- manifest + logs + traces + captures + repro recipe
          |
          v
query / explain / compare / CLI / editor / MCP
```

### 统一身份

至少需要下列稳定字段：

- `source_revision`：Git commit、SVN revision 或工作区快照 ID。
- `build_id`、`toolchain_id`、`artifact_id`：构建与二进制身份。
- `asset_guid`、`schema_id`、`entity_guid`：持久内容身份。
- `shader_source_id`、`shader_hash`、`pipeline_hash`：Shader/Pipeline 身份。
- `run_id`、`frame_id`、`trace_id`：运行与时间线身份。
- `system_id`、`task_id`、`render_pass_id`、`resource_id`：执行图身份。
- `adapter_id`、`driver_version`、`capability_set_id`：硬件环境身份。

ID 不能只是随机标签。每种 ID 都要有生成规则、生命周期、版本和查询入口。

### Diagnostic Bundle

诊断包建议是可离线读取的目录或压缩包，含：

- `manifest.json`：版本、平台、硬件、驱动、源码、构建、启动参数和随机种子。
- 结构化日志与错误事件。
- CPU/任务/内存 Trace；必要时附 Perfetto 或 Tracy 原始数据。
- Shader Manifest、PDB/符号索引和 Pipeline 元数据，不默认携带私有源码。
- 可选 RenderDoc Capture、DRED、Aftermath 或 Vulkan Validation 产物。
- World 摘要、相关实体/组件快照和输入重放片段。
- `repro.json`：最小复现步骤、所需资产 hash 和预期断言。

诊断分级很重要：默认运行只记录低开销 ID、Marker 和环形缓冲；检测到错误或用户请求时再升级到详细 Trace/Capture。DRED、Validation、GPU Capture 等高开销能力只在诊断模式开启。

### Agent 能力边界

Agent 不应获得任意进程、内存和 GPU 操作权限。控制面采用 capability token 或明确 allow-list，例如：

- 允许读取 World Schema，不等于允许修改所有组件。
- 允许启动 120 帧 Headless Run，不等于允许运行任意可执行文件。
- 允许创建一次 RenderDoc Capture，不等于允许永久开启驱动层注入。
- 原生调试的读内存、写内存、继续执行和调用函数是不同权限。
- 每次高风险操作留下审计事件，并能关联到 Agent session 与源码变更。

## 六、项目与轮子的采用清单

### 现在就用或尽快接入

| 项目 | 用法 |
| --- | --- |
| CMake + Ninja | 保留当前构建基础，导出动作图和依赖信息 |
| sccache | 作为编译缓存实验，记录命中、miss 原因和产物正确性 |
| Clang Time Trace / Build Analyzer / IWYU | 建立构建基线和 `build explain` 数据源 |
| Flecs Reflection | 继续作为 ECS Schema 来源，但持久 ID 和文件格式由引擎定义 |
| Perfetto | 作为可查询 Trace/回归数据的主要候选 |
| Tracy | 人类实时 CPU/GPU/内存分析前端 |
| RenderDoc | 帧捕获与 GPU 证据；通过适配层接入，不 fork 核心 |
| DXC + SPIRV-Tools | Shader 编译、反射、验证、Time Trace 和最小化复现 |
| DRED | 当前 D3D12 路线的设备移除诊断基础 |
| LLDB MCP / DebugMCP / gdb-mcp | 作为协议与交互设计参考，按平台选择外部调试器 |

### 借鉴架构，不直接搬整套引擎

| 项目 | 借鉴点 |
| --- | --- |
| Godot | 文本场景、源/导入分离、UID |
| Unity SmartMerge | VCS 无关的语义三方合并 |
| Unreal OFPA | 限制冲突域、外部对象和 Changelist 体验 |
| O3DE | JSON Prefab/Patch、Builder 指纹和资产处理诊断 |
| Bevy | 动态链接和开发依赖优化策略 |
| ezEngine | 小型 C++ 引擎插件与代码重载边界 |
| Wicked Engine | Shader 重载、CPU/GPU Profiler、轻量 C++ 引擎组织 |
| The Forge | 自动化、Shader Server、GPU 配置和驱动规则库 |
| OpenTelemetry | 稳定语义约定与多信号关联 |
| Fossilize | Pipeline 序列化、跨设备回放和驱动复现 |

### 做完基准再决定

| 项目/方案 | 验证内容 |
| --- | --- |
| Luau / AngelScript / daScript | 真实玩法 API、热重载、状态迁移、调试与 Agent 修复效率 |
| Wasmtime | 不可信插件的隔离、资源限制、调试和跨边界成本 |
| C++ Modules | MSVC/Clang、CMake、IDE、第三方依赖与增量构建成熟度 |
| Unity Build | 干净构建收益与小改动重编译放大之间的平衡 |
| Live++ | 授权、平台、对象重实例化和本项目模块边界是否匹配 |
| Slang | 反射、跨后端 Shader、编译速度、调试信息与 DXC 兼容性 |

### 暂时不要做

- 自创编程语言或自研通用脚本 VM。
- 自研 RenderDoc/Profiler/Debugger 的替代品。
- 为了“AI-first”让 MCP 直接调用所有内部函数。
- 在稳定 Schema 之前制作庞大的可视化编辑器。
- 把二进制缓存、Cook 产物和权威源资产混在同一个提交面。
- 只统计 MCP 工具数量，不衡量诊断成功率、复现率和回归发现率。

## 七、社区当前真正拥挤和真正稀缺的部分

[Unity MCP](https://github.com/CoplayDev/unity-mcp) 和 [Godot MCP](https://github.com/Coding-Solo/godot-mcp) 已经获得大量关注，GitHub 上还有许多相似项目。它们证明 Agent 操作编辑器有需求，也说明“接入 MCP”很难长期成为引擎特色。

[GameCraft-Bench](https://github.com/FreedomIntelligence/gamecraft-bench) 等项目在尝试衡量 Agent 从需求到可运行游戏的端到端表现；公开结果仍显示完成复杂任务并不稳定。社区讨论也开始对只展示 AI 生成技术 Demo 的项目感到疲劳，例如这则[引擎开发者讨论](https://www.reddit.com/r/gameenginedevs/comments/1tr2x5y/a_lot_has_changed_since_ai/)。这些只能反映社区情绪，却提示了一个合理的作品集策略：用可复现的调试案例和工程指标说话。

当前拥挤的是：聊天框、自然语言创建场景、把编辑器命令包装成 MCP。

当前相对稀缺的是：

- Agent 能解释构建失效和编译性能回退。
- Agent 能把代码、ECS、任务图、Render Graph、Shader 和 GPU Capture 串起来。
- 同一问题可以在另一台硬件上通过诊断包离线分析。
- Agent 的修改有语义 Diff、影响范围、复现步骤和性能证据。
- 人类和 Agent 共享工具与安全边界，而不是存在一个绕过引擎规则的 AI 后门。

这正好与本项目的关注点重合。

## 八、建议用来证明特色的三个案例

### 案例 A：一行头文件修改导致增量构建回退

系统采集 Ninja/CMake 依赖、编译器 Time Trace 和缓存事件。Agent 输出最短失效路径，指出传播最广的 Include，并提交边界调整。验收不是“感觉变快”，而是同一台机器、同一修改下的编译动作数、关键路径和 P50/P95 时间对比。

### 案例 B：D3D12 Device Removed

引擎打包 DRED Breadcrumb、Page Fault、最近资源、Pipeline/Shader hash、GPU/驱动、帧和源码 revision。Agent 从故障命令追到 Render Pass、ECS System 和创建资源的事务，给出最小 Headless Repro；必要时附 RenderDoc/Aftermath 证据。

### 案例 C：Agent 修改脚本后性能回退

脚本热重载保留版本化 ECS 状态。两次运行使用相同输入和随机种子，Perfetto 查询显示某 System 调用次数、分配和 GPU Wait 的变化。Agent 找到变化对应的脚本 source span，修复后用 Trace Diff 和 World 断言验证。

这三个案例横跨用户最关心的 Git、编译、脚本、Agent 和硬件层，也比“让 AI 放一个方块”更能体现引擎研究价值。

## 最终判断

当前技术栈不需要推倒重来。C++20、Flecs、SDL、Headless、结构化输出和 MCP sidecar 可以继续使用。项目下一步最值得投入的不是更多渲染功能或编辑器 UI，而是打通工程证据链。

可以把产品概念进一步收敛为：**一套能向人和 Agent 解释自己如何被构建、如何运行、为什么失败的引擎。**

如果这条路线做成，引擎的 AI 友好不依赖某个模型或聊天界面。未来 Agent 更换、MCP 演进、编辑器变化，底层的稳定 Schema、诊断身份、语义事务和可复现证据仍然有效。这才是适合长期开发的“Agent 原生”。
