# Agent 原生引擎研究实施路线

> 文档类别：Superseded historical roadmap。当前唯一排期是 `../development-plan.zh-CN.md`；本文只保留早期推导。

> 配套报告：[2026 Agent 原生引擎生态调研](2026-agent-native-engine-ecosystem.zh-CN.md)
>
> 用途：把调研判断转成当前仓库可执行的里程碑。时间是顺序估算，不是承诺日期。

## 路线原则

当前仓库已经有 C++20 Runtime、Flecs World、SDL_GPU、Headless、结构化 CLI、JSON-RPC Schema 和 MCP sidecar。下一阶段不横向扩张成完整编辑器，而是建立一条最小但完整的证据链：

```text
源码修改 -> 构建原因 -> 二进制/Shader -> 可复现运行 -> CPU/GPU 证据 -> 查询与比较
```

每个里程碑都必须同时提供：稳定 Schema、CLI/JSON 输出、测试、失败示例和一份可离线保存的产物。MCP 只包装已经稳定的引擎能力。

## M0：Engine Agent ABI

目标：先消除 CLI、MCP、文档和未来运行时控制协议之间的重复定义。

交付：

- 扩展 Command Registry：输入/输出 Schema、权限、幂等、dry-run、运行态、长任务和 Evidence 类型。
- `noemancer tools list --format json`，供 Agent 发现能力。
- `noemancer tool call <name>`，从 stdin 读取 JSON 参数，只向 stdout 输出结构化结果。
- 统一 Result Envelope、错误码和 Action Receipt。
- MCP sidecar 根据 Tool Manifest 动态注册，不再手工复制命令参数。
- CLI、direct tool 和 MCP 的契约一致性测试。

验收：`world.snapshot` 和 `run.headless` 只定义一次，三种入口产生同一 Schema 和等价结果。详细设计见 [Agent 接口架构补充调研](2026-agent-interface-architecture.zh-CN.md)。

## M1：持久身份与确定性 World 快照

目标：让同一份 World 的无关加载顺序不会污染 Diff，让实体和组件能跨运行追踪。

交付：

- `asset_guid`、`entity_guid`、`schema_id` 的格式、生成规则和版本。
- 基于反射的规范化 JSON 快照；固定字段顺序、浮点和默认值规则。
- `engine world snapshot` 与 `engine world diff`。
- Diff 输出 JSON Patch 风格的路径、旧值、新值和 Schema 信息。
- 确定性测试：不同插入顺序得到相同权威快照。

验收：Agent 能修改一个实体字段，只产生一个范围清楚的语义变化；快照不包含 Flecs ID、指针或 SDL Handle。

## M2：Build Manifest 与构建基线

目标：第一次能够回答“本次构建做了什么、用了什么、花了多久”。

交付：

- 每次构建输出 `build-manifest.json`：source revision、dirty state、preset、编译器、SDK、依赖版本、目标、动作和产物 hash。
- Debug Clang 配置接入 `-ftime-trace`；MSVC 保留等价事件的扩展接口。
- 可选 sccache 实验配置和命中率记录。
- `engine build summarize` 汇总前端、后端、链接、缓存和关键路径。
- 固定硬件上的 clean build 与一组典型增量修改基线。

验收：同一构建产物能追溯到源码、工具链和配置；报告能列出最慢编译单元和头文件/模板热点。

## M3：Build Explain

目标：从“显示耗时”升级到“解释为什么失效”。

交付：

- 读取 Ninja/CMake 依赖图和 compiler depfile。
- `engine build explain --changed <file>` 输出受影响目标、最短传播路径和预计动作。
- `engine build compare <manifest-a> <manifest-b>` 比较动作数、关键路径、缓存、编译和链接。
- Include fan-out、模板实例化和代码生成成本的规则化诊断。
- CI 性能预算，先记录趋势，稳定后再作为合入门槛。

验收：准备一个故意造成头文件传播的提交，Agent 能定位原因、调整模块边界，并用前后 Manifest 证明改进。

## M4：统一 Telemetry ID 与 Perfetto 导出

目标：让帧、任务、System、实体和日志处在同一条时间线上。

交付：

- `run_id`、`trace_id`、`frame_id`、`system_id`、`task_id`。
- 低开销 Marker API，运行时事件使用稳定 Schema。
- Headless 固定帧数、固定时间步、固定随机种子和输入重放元数据。
- Perfetto Trace 导出或适配层；保留 Tracy 接入位置。
- `engine trace query` 的有限、安全查询模板，避免直接允许任意重负载 SQL。

验收：Agent 能查询一帧内各 System 的顺序和耗时，并将日志错误定位到对应 frame/system/entity。

## M5：Render Diagnostic Graph

目标：给 Render Pass、资源、Pipeline、Barrier 和 Shader 建立可追溯关系。

交付：

- `render_pass_id`、`resource_id`、`shader_hash`、`pipeline_hash`。
- 记录逻辑资源、物理资源、状态转换、提交队列和 GPU Marker。
- Shader Manifest：源码、Include、宏、DXC 版本、目标、反射、PDB、Time Trace 和产物 hash。
- D3D12 DRED 诊断模式；设备移除后输出结构化事件。
- RenderDoc Capture 与引擎 ID 对照表。

验收：制造一个可控的资源状态错误或错误 Pipeline，诊断包能从 GPU 证据回到 Pass、System、Shader 源和源码 revision。

## M6：Diagnostic Bundle 与离线分析

目标：把另一台机器、另一张显卡上的问题交给 Agent 分析，不要求 Agent 直接控制故障机器。

交付：

- 版本化 `diagnostic-bundle.schema.json`。
- Bundle 包含 Manifest、日志、Trace、World 摘要、Shader/Pipeline 索引、硬件/驱动和 Repro Recipe。
- 可选附加 RenderDoc、DRED、Aftermath、Mini-dump；大文件采用引用和 hash，不强塞进 Git。
- 隐私与体积策略：路径脱敏、源码默认不打包、大小预算和保留时间。
- `engine diagnose inspect|compare|validate`。

验收：在无原仓库进程、无编辑器连接的环境中，仍能校验 Bundle、查询关键事件并比较两个运行。

## M7：C# 托管脚本与语言边界验证

目标：验证 C# 能否承担玩法与开发者脚本，同时保持快速重载、批量数据访问和可控发布成本。TypeScript 继续用于工具与 Agent 控制面，不进入每帧热路径。具体判断见 [2026 性能核心与 Agent 全面编辑技术选型](2026-performance-and-agent-editable-stack.zh-CN.md)。

第一轮使用 .NET 10 / CoreCLR 实现实体查询、组件读写、事件、数学运算、异步任务和错误调用；Luau 或 WASM 只作为失败时的对照后端。测量：

- CoreCLR 启动、冷编译、增量编译和候选 Assembly 替换时间。
- 单次与批量组件访问、跨边界调用、分配和 GC pause。
- Roslyn 诊断、source span、托管/原生调用栈和调试器质量。
- `AssemblyLoadContext` 卸载失败诊断、行为交换和版本化 ECS 状态迁移。
- 开发态 CoreCLR 与可选 NativeAOT 发布路径的功能、体积、启动和性能差异。
- 能力白名单、文件/网络访问边界和失控脚本终止。
- Agent 在固定问题集上的首次修复成功率与所需上下文。

验收：决定是否接受 [ADR 0002](../adr/0002-language-and-performance-stack.md)。无论最终采用哪种托管或 VM 后端，权威状态仍由版本化 ECS Schema 管理。

## M8：受约束的 Agent 调试闭环

目标：让 Agent 使用前面所有证据完成一次真实底层修复。

交付：

- Query、Explain、Compare 与 Capture 能力进入 JSON-RPC。
- MCP sidecar 只暴露固定参数、时间/帧数/文件范围和结果大小上限。
- 原生调试器适配；读状态、设断点、继续、写内存和调用函数使用不同权限。
- 所有 Agent 操作记录 session、输入摘要、产物和源码 revision。
- 三个公开案例：构建回退、D3D12 Device Removed、脚本性能回退。

验收：每个案例都能从故障复现开始，经过结构化诊断、代码修改、测试和前后证据比较结束。最终报告不依赖聊天记录才能成立。

## M9：Engine Semantic Observation Graph

目标：让 Agent 不依赖全屏截图，也能理解用户正在看的编辑器控件、视口对象和相关渲染证据。

交付：

- UI Tree、Viewport Scene Projection、Render Evidence 与视觉附件的统一 Schema。
- 稳定 UI ID、revision、过滤查询和 `added/changed/removed` delta stream。
- selection、hover、focus、cursor pick、最近交互和局部 screenshot crop。
- 评估 AccessKit，使 Agent 语义、可访问性和 UI 测试共用数据源。
- `ui.query/action`、`viewport.describe/pick` 与 `observation.bundle`。

验收：固定任务中，“过滤语义树 + 局部图片”相对纯截图降低上下文量和误操作率；语义采集在关闭、基础和完整模式下都有可复现开销报告。

## M10：GPU VFX Graph 最小闭环

目标：建立 AI 可编辑、可测量、可回滚的高性能粒子系统。

交付：

- 版本化 VFX Graph Schema、语义参数、单位/范围和迁移规则。
- Slang 内核生成、共享 GPU Pool、alive/dead compact、Data Channel、indirect dispatch/draw。
- 固定 seed 预览，输出模拟、排序、碰撞、渲染、过绘制、显存和编译指标。
- 分级碰撞与透明策略，以及用于 Hybrid Pixel 的 Pixel Mode。
- Effekseer 资产桥接尖峰。

验收：Agent 能从自然语言生成受约束的 Graph Diff，经确定性预览和性能比较后提交或回滚；全屏 torture scene 的瓶颈可归因到具体 emitter、pass 和 shader。

## M11：Hybrid Pixel 2.5D 验证游戏

目标：用一条可玩的纵向切片验证 2D/3D 渲染、VFX、脚本、资产 Cook 和 Agent 观察闭环。

交付：

- virtual resolution、integer scale、pixel snapping 和 Sprite depth/normal/material。
- Sprite/3D 遮挡、光照、阴影与受控后处理顺序。
- 3D-to-Sprite baker、Hybrid tilemap/3D 场景编辑与 VFX Pixel Mode。
- Semantic 2D Character Rig：Archetype、Skin/Part、Direction Profile、共享 Motion、局部 cel swap 和 Sprite bake。
- 三角色/四方向/三动作原型，对比 baked sprite 与 dynamic puppet 的画质和成本。
- 一间场景、一个可控角色和一场高密度战斗。
- 固定画质/硬件上的 CPU、GPU、显存、loading/cook 与视觉稳定性基线。

验收：Agent 根据用户对运行中场景的描述，定位实体并修改材质或 VFX，完成热预览、前后证据比较和提交；游戏在目标硬件 Profile 上满足明确帧预算。

## 第一批技术尖峰

在完整里程碑前，建议先做四个一到两天的尖峰，快速暴露集成风险：

1. 用 Clang Time Trace 编译当前 Runtime，生成最慢编译单元和 Include 摘要。
2. 把 Flecs System 与帧事件导出成最小 Perfetto Trace。
3. 给 SDL_GPU 的 D3D12 Backend 增加 Debug Marker，并确认 RenderDoc 中能看到稳定 ID。
4. 人为触发一个 D3D12 Debug Layer 错误，设计第一版结构化 GPU Diagnostic Event。

这四个尖峰会验证证据层能否真正跨越构建、ECS 和 GPU。若其中某项受 SDL_GPU 抽象限制，应尽早决定扩展 SDL_GPU、访问 Native Handle，还是增加独立 D3D12 实验 Backend。

## 持续衡量的指标

- 典型修改的 P50/P95 增量构建时间与编译动作数。
- 构建缓存命中率，以及每类 miss 的可解释比例。
- World/资产无语义修改时的零 Diff 比例。
- 同一输入重复运行的 Trace 和 World 断言稳定性。
- 诊断事件能关联到源码、构建、帧、System 和硬件的覆盖率。
- Diagnostic Bundle 在另一台机器上的可解析、可查询和可复现比例。
- Agent 修复任务的首次成功率、回滚率和能够提供完整证据的比例。
- 诊断关闭、基础 Marker、完整 Capture 三种模式的性能开销。

这些指标会让“AI 友好”“编译快”“容易调试”变成可以验证的工程属性。
