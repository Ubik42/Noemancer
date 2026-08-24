# 面向模型的引擎语义底座：社区与跨领域调研

> 文档类别：Historical research。当前语义架构由 ADR 0006 与 `../architecture.md` 定义。

> 调研日期：2026-08-18
> 目的：验证“让引擎状态直接符合模型理解直觉，而不只依赖 MCP/CLI 工具”的可行性，并据此调整 Noemancer 的全局开发顺序。

## 结论

我们的方向成立，但必须把概念说得更准确：**不是把整个引擎重复序列化成一棵巨大的文本树，也不是尽可能多地暴露工具；而是建立一个版本化、可查询、可增量、可追溯的语义状态层。**

社区已经广泛做到：

- 用 MCP/JSON-RPC/CLI 让 Agent 调用编辑器和运行时；
- 暴露场景树、组件、资源、截图和输入；
- 从反射系统生成 Schema 或远程方法；
- 对危险操作做分级、禁用、撤销或确认。

仍然普遍缺少的是：

- UI、场景、资产、源码、构建和渲染证据使用同一套稳定身份与关系；
- Agent 只取当前任务所需的语义切片，而不是消耗整棵树或上百个工具说明；
- “理解、审阅、执行”共享同一份计划与版本前提；
- 人类、Agent、编辑器和运行时并发修改时，能够解释字段归属与冲突；
- 从运行时异常一路追溯到场景字段、资源、Shader、构建产物和源码位置。

这正是 Noemancer 可以形成差异的地方。MCP 是接头，语义底座才是产品能力。

## 调研样本

### 游戏引擎与 DCC Agent 工具

| 样本 | 已验证的模式 | 对 Noemancer 的启示 |
|---|---|---|
| [hybridindie/godot-mcp](https://github.com/hybridindie/godot-mcp) | 约 175 个工具、按类别启用、默认只开放检查工具、安全分级、Undo、运行时探针 | 工具数量很快失控；默认最小能力面和动态加载是必要条件 |
| [IvanMurzak/Unreal-MCP](https://github.com/IvanMurzak/Unreal-MCP) | C++ 插件与 sidecar 分离、结构化错误、编译与截图、工具族可禁用 | 编辑器内核负责事实，协议适配器只负责传输；这与现有 Noemancer ABI 方向一致 |
| [CoplayDev/unity-mcp](https://github.com/CoplayDev/unity-mcp) | 成熟的 Unity Agent 桥与批处理工具 | “能控制主流编辑器”已经商品化，不能成为我们的核心特色 |
| [aivarsliepa/godot-mcp](https://github.com/aivarsliepa/godot-mcp) | Headless 编辑、运行时桥、UI 发现、截图和输入 | 编辑时与运行时必须是两个状态域，但应共享身份和协议 |
| [Fulviuus/godot-mcp](https://github.com/Fulviuus/godot-mcp) | 将 `.tscn/.tres/.gd/.cs` 解析成结构化 JSON，结合版本文档和运行时桥 | 文件可读很重要，但仅解析文件无法解释运行时派生状态 |
| [IvanMurzak/Godot-MCP](https://github.com/IvanMurzak/Godot-MCP) | 明确要求结构化数据而非临时字符串 | 公共边界应有 Schema，面向人类的摘要只是一个视图 |
| [glonorce/Blender_mcp](https://github.com/glonorce/Blender_mcp) | 场景图与矩阵理解、大量工具、阻止不安全渲染操作 | 图关系和安全元数据有价值；领域对象不应被展平成无类型文本 |

这些项目证明 Agent 接入的需求真实且活跃，也说明只增加工具会遇到三个瓶颈：工具描述占用上下文、多个工具返回不一致的数据形状、复杂任务缺少可审阅的统一计划。

### 引擎原生远程反射

[Bevy Remote Protocol](https://docs.rs/bevy/latest/bevy/remote/index.html) 已经通过 JSON-RPC 2.0 提供 ECS 查询和修改、反射 Schema、OpenRPC 发现、watch 方法以及主 World/Render World 的独立入口。本地参考仓 `bevy@9f4ff89` 中的 `crates/bevy_remote` 也包含 `registry.schema`、`rpc.discover` 和多种 `+watch` 实现。

这意味着“反射生成 Schema + 远程查询 + 增量观察”不是 Noemancer 独有创新，而应直接作为最低基线。我们的增量价值应放在：跨域稳定身份、来源锚点、可审阅变更计划、证据关联和上下文预算。

[Godot 的文本场景格式](https://docs.godotengine.org/en/4.7/engine_details/file_formats/tscn.html) 证明人类可读、版本控制友好的场景文件，以及资源 UID、节点唯一 ID、树路径、类型和属性引用可以共存。Noemancer 没必要发明难以被现有工具理解的场景 DSL；规范化 JSON、稳定 GUID 和显式引用已经足够作为第一版。

### 浏览器开发工具：语义与像素并存

Chrome DevTools Protocol 提供了很接近我们编辑器目标的先例：

- [DOMSnapshot](https://chromedevtools.github.io/devtools-protocol/tot/DOMSnapshot/) 能一次捕获扁平 DOM、布局、样式、矩形和绘制顺序；
- [Accessibility](https://chromedevtools.github.io/devtools-protocol/tot/Accessibility/) 能按名称和角色查询完整或局部可访问性树，并记录属性来源；
- [CSS](https://chromedevtools.github.io/devtools-protocol/tot/CSS/) 将稳定对象 ID、源码范围、匹配样式和计算样式连在一起。

它证明 GUI 无需在“给模型截图”和“暴露文本树”之间二选一。Noemancer 应同时提供语义节点、布局/视口投影、源码与绑定来源，以及按需视觉附件；通常先给语义切片，只有歧义或视觉质量问题才取截图、深度、对象 ID 或 Overdraw。

### 内容图与变更语义

[OpenUSD 的 SdfPath 和 API Schema](https://openusd.org/22.08/glossary.html) 展示了稳定路径、命名空间、Schema 与组合层之间的关系。[UsdNotice::ObjectsChanged](https://openusd.org/dev/api/class_usd_notice_1_1_objects_changed.html) 则区分“需要重同步子树的结构变化”和“只更新字段的值变化”，并返回有序路径和变更字段。

Noemancer 不应把 USD 整体搬成运行时场景格式，但应借用两点：

1. 用稳定语义路径表达对象在某个命名空间中的位置，同时用 GUID 保持重命名和移动后的身份；
2. Delta 明确区分结构重同步与字段变化，消费者不必每次重取整个对象树。

### 代码与来源锚点

[Language Server Protocol](https://microsoft.github.io/language-server-protocol/) 已经把 URI、位置、范围、诊断、Symbol、Reference 和 Workspace Edit 变成开发工具共同语言。Noemancer 的 `SourceAnchor` 应复用这些概念，把 ECS 字段、UI binding、Shader 参数和构建诊断定位到文件及范围，不再发明另一套行列协议。

### 语义约定而不只是 JSON

[OpenTelemetry Semantic Conventions](https://opentelemetry.io/docs/specs/semconv/) 的经验是：跨信号共享名称、类型、单位和值域，远比“所有东西都能输出 JSON”更重要。其[约定设计指南](https://opentelemetry.io/docs/specs/semconv/how-to-write-conventions/) 还强调复用现有属性、稳定等级、清晰用例、有限基数和数据量；[Schema](https://opentelemetry.io/docs/specs/otel/schemas/) 为约定版本提供迁移。

因此 Noemancer 需要自己的 Semantic Conventions Registry：每个字段声明稳定英文标识、类型、单位、坐标空间、可空性、敏感性、成本、稳定等级和废弃迁移。显示语言和本地化标签与程序标识分离。模型是否见过某一种私有格式无法保证，但标准 URI、JSON Schema、JSON Pointer、LSP Range 和通用领域名称能显著减少额外解释。

### 按需查询与上下文预算

[GraphQL 2025 规范](https://spec.graphql.org/September2025/) 的 Selection Set 让客户端只请求所需字段，并通过自省理解类型；[Protobuf FieldMask](https://protobuf.dev/reference/java/api-docs/com/google/protobuf/FieldMask.html) 同样用符号字段路径限制读取或修改范围。

Noemancer 第一版不必实现 GraphQL。应采用更小的选择器：`select`、`where`、`include`、`depth`、`fieldMask`、`sinceRevision`、`byteBudget`、`cursor`。返回值必须说明是否截断、可用的下一页和省略原因。上下文预算是协议语义，不是提示词技巧。

### 计划、并发与审阅

[Terraform Plan](https://developer.hashicorp.com/terraform/cli/commands/plan) 将无副作用的预览与实际 apply 分开；其 [JSON Format](https://developer.hashicorp.com/terraform/internals/json-format) 同时表达旧状态、配置、计划状态、变更和检查，[Machine-readable UI](https://developer.hashicorp.com/terraform/internals/machine-readable-ui) 用 JSONL 事件描述漂移、计划和进度。

[Kubernetes Server-Side Apply](https://kubernetes.io/docs/reference/using-api/server-side-apply/) 进一步证明多个控制器可以只声明自己关心的字段，并由服务端记录字段管理者、检测冲突、显式强制接管。[Kubernetes API watch](https://kubernetes.io/docs/reference/using-api/api-concepts/) 使用 `resourceVersion` 先取得一致快照，再连续接收之后的变更，避免快照与订阅之间漏事件。

对 Noemancer 而言，这比普通 Undo 更适合“用户和多个 Agent 同时开发”：

- Agent 提交部分意图，而不是覆盖完整对象；
- Change Plan 绑定基础 revision 和计划 hash；
- apply 前重新验证前提，过期计划不得静默执行；
- 字段记录最近管理 workflow，冲突必须可解释；
- 强制覆盖是显式高风险动作；
- 成功后返回实际 Delta、证据、产物和 rollback token。

[JSON Patch RFC 6902](https://www.rfc-editor.org/info/rfc6902/) 可作为低层文档编辑表示，特别是 `test` 操作适合表达前提；但高层仍应保留 `scene.reparent`、`material.set_parameter` 等领域动作，不能要求 Agent 每次手写脆弱的数组下标 Patch。

### MCP 的准确位置

[MCP Server 概览](https://modelcontextprotocol.io/specification/2025-06-18/server/index) 将 Resources、Prompts、Tools 分成不同控制方式；[Tool 规范](https://modelcontextprotocol.io/specification/2025-06-18/server/tools) 支持输入/输出 Schema、结构化内容、资源链接和注解。

这些能力适合承载 Noemancer 的资源和动作，但不应该反向塑造内核。同一个 `ObservationBundle` 和 `ChangePlan` 必须能经由进程内 C++、CLI JSON/JSONL、编辑器面板、自动测试和 MCP 使用。协议适配层只做分页、编码、权限映射与生命周期管理。

## Noemancer 的目标架构

将原来的 Engine Semantic Observation Graph 扩展为 **Semantic State Plane（语义状态层）**。Observation Graph 是它的只读投影视图，不是另一份权威 World。

```text
权威状态：场景文档 / ECS / 资产库 / UI 文档 / Render Graph / 构建与运行时
                              |
                    versioned semantic projection
                              |
         Semantic State Plane（identity / schema / path / revision /
             provenance / relationships / conventions / permissions）
                /                    |                    \
    Observation Query/Delta     Change Plan/Apply       Evidence Bundle
                \                    |                    /
         editor / CLI / JSONL / tests / accessibility / MCP / future Agents
```

关键约束：

- 权威状态只有一份；语义层是确定性投影、索引和变更协议，不能形成需要双向同步的第二个 World。
- GUID 表达身份，Semantic Path 表达当前位置，JSON Pointer 表达文档字段，LSP Range 表达源码位置；四者不能混为一个字符串。
- JSON Schema 描述结构，Semantic Conventions 描述含义；`[1, 2, 3]` 必须同时说明它是颜色、位置还是方向，以及单位和坐标空间。
- 快照与 Delta 有确定顺序和规范化编码；相同 revision、query 和权限产生相同语义结果。
- 默认返回摘要和聚焦切片。视觉附件、原始日志、大数组、网格和 GPU Capture 通过资源链接按需取得。
- 所有 mutation 走 Observation → Plan → Apply → Receipt；复杂工具只是对相同基础动作的计划器或宏。

## 第一版公共对象

| 对象 | 最小字段 | 作用 |
|---|---|---|
| `SemanticRef` | `id`、`path`、`type`、`schemaRef`、`revision`、`displayName?` | 跨域引用同一对象 |
| `SourceAnchor` | `uri`、`range/jsonPointer`、`generatedFrom?` | 从运行时状态回到源文件和生成链 |
| `SemanticRelation` | `subject`、`predicate`、`object`、`revision` | binding、contains、renders、uses、generatedFrom 等有类型关系 |
| `ObservationQuery` | scope、selector、fieldMask、include、depth、sinceRevision、budget | 精确限制上下文 |
| `ObservationBundle` | snapshot revision、objects、relations、diagnostics、affordances、evidence links、pagination | 将“理解与审阅”放在同一份结果里 |
| `SemanticDelta` | base/current revision、resync paths、changed fields、added/removed | 高效订阅与编辑器更新 |
| `ChangePlan` | plan ID/hash、base revision、manager、operations、preconditions、predicted delta、validation、risk | 无副作用地审阅修改 |
| `ActionReceipt` | applied revision、actual delta、evidence、artifacts、warnings、rollback token | 证明实际发生了什么 |

## 不做什么

- 不为了“AI Native”把所有内部 C++ 对象无差别反射到网络。
- 不让 MCP 工具清单成为领域模型；工具可以增删，语义对象和动作必须稳定。
- 不持续推送整棵 GUI/World 树；提供索引、局部查询、delta 和资源链接。
- 不把自然语言名称作为唯一身份；显示名可变且可本地化。
- 不承诺某种 JSON 排列“最符合所有模型训练数据”；只采用成熟标准、稳定约定、示例和可自省 Schema 来降低理解成本。
- 不在 S1 实现 USD、GraphQL、Kubernetes 或 Terraform；只吸收它们已经验证的最小语义。

## 对现有实现的审计

现有 Engine Agent ABI 0.2 已经正确做到命令元数据单一来源、输入输出 Schema、结构化 Result/Receipt、CLI 与 MCP 复用。它应该保留。

调研时的 `world.snapshot` 和编辑器 `semantic_snapshot_json()` 仍是演示数据：固定 revision、临时数字实体 ID、手拼 JSON、编辑器维护自己的对象副本，没有查询、来源、关系、delta、字段含义或一致性边界。随后启动的 S1 实现已让 World 首先具备稳定语义身份、动态 revision、坐标/单位约定与 `semantic.conventions`；编辑器副本、查询、delta 和修改计划仍待替换。因此当前能力仍应描述为“语义底座的首个纵切”，而不是完整实现。

## 规划调整

1. 将身份、Schema、Semantic Conventions、SourceAnchor、revision 和 canonical projection 前移到 S1。
2. S1 先完成真实 World ↔ 规范化 Scene Document ↔ Semantic State Plane 的纵向切片，再做 Semantic UI。
3. 原 S5 不再首次创建 Observation Graph，而是把已经存在的语义底座扩展到 UI、Viewport、Render Evidence 和视觉附件。
4. 每个后续模块的验收都增加“可查询、可增量、可追溯、可计划修改”要求，避免最后再补 AI 接口。
5. 近期开发顺序改为：语义核心类型与约定 → 场景投影/查询/Delta → Plan/Apply/Receipt → UI 尖峰 → 资产与渲染纵切。

这条路线不是偏向 Godot。Godot 为文本场景和节点语义提供了良好先例；Noemancer 的差异是把同一语义契约贯穿编辑器 UI、运行时 World、构建系统和 GPU 证据，并以可审阅计划和增量状态作为 Agent 的基础工作面。
