# Agent 接口架构补充调研：从 MCP 外挂到 Engine Agent ABI

> 文档类别：Historical research（非当前计划）。实现状态与优先级以 `../architecture.md`、`../development-plan.zh-CN.md` 和 `../current-state.json` 为准。

> 调研日期：2026-08-18
>
> 研究问题：怎样让 Codex、Claude Code、Gemini CLI、IDE Agent 或未来的新 Agent 直接理解、编译、启动、附加和调试引擎，同时尽量减少协议转换、上下文消耗和命令学习成本。

## 结论修正

如果本项目只做到“文本化场景 + Headless + MCP + 运行时状态读写”，方向会明显接近 Godot 周边的 Agent 工具，而且在 2026 年已经不算新颖。

[godot-breakpoint-mcp](https://github.com/jlivingston-Cipher/godot-breakpoint-mcp) 已同时接入 Godot 的编辑器桥、Headless CLI、运行时桥、LSP 和 DAP，支持断点、单步、变量、性能断言、截图比较和长任务管理。[Summer Engine Agent](https://github.com/SummerEngine/summer-engine-agent) 已把 CLI、MCP、Agent Skills、Hooks 和多种 Agent 配置打包。[Roblox Studio MCP](https://github.com/Chrrxs/robloxstudio-mcp) 也覆盖运行时求值、多人 Playtest、Profiler 与自动验证。Unity、Unreal 周边正在出现类似能力。

所以，以下能力应视为基础设施，而不是项目特色：

- Agent 能启动或停止引擎。
- Agent 能创建实体、修改属性、读取日志和截图。
- 提供 MCP Server。
- 提供文本场景或 JSON/YAML 资产。
- 能在运行时读取对象和调用方法。
- 接入普通断点与 Profiler。

项目仍有机会形成差异，但需要把重点进一步推进到两个相互配合的概念：

1. **Engine Agent ABI**：一套稳定、可发现、可组合的 Agent 操作面，不依赖某个模型或某个 Agent 产品。
2. **Engine Evidence Plane**：每次操作都能追溯到源码、构建、运行态和硬件证据。

Agent ABI 解决“怎样低损耗地操作”；Evidence Plane 解决“怎样证明操作结果和解释失败”。两者缺一不可。

## 一、通用 Agent 实际擅长什么

当前主流 Coding Agent 的共同基础不是某个游戏引擎插件，而是文件系统、Shell 命令、标准输出、退出码、Git 和可自动发现的工具。

[OpenAI Codex](https://github.com/openai/codex)、[Claude Code](https://github.com/anthropics/claude-code) 和 [Gemini CLI](https://github.com/google-gemini/gemini-cli) 都把终端作为主要工作环境。Gemini 的[Shell Tool](https://github.com/google-gemini/gemini-cli/blob/main/docs/tools/shell.md) 明确返回命令、工作目录、stdout、stderr 和 exit code；它还支持基于命令前缀的权限策略。Codex 的 [app-server](https://github.com/openai/codex/blob/main/codex-rs/app-server/README.md) 使用 JSONL/stdin 传输双向消息，并把命令表示为 argv 数组，而不是只能传一整段 Shell 字符串。

[AGENTS.md](https://agents.md/) 已成为跨 Codex、Gemini、Cursor、Zed、Copilot 等工具的项目说明约定。它很朴素，但说明 Agent 生态更愿意复用文件和命令，而不是为每个软件学习一套封闭界面。

这意味着引擎不需要“模拟一个 Bash”。更合适的办法是提供一个真正符合命令行习惯的 `noemancer` 可执行文件，让 Agent 在 Bash、PowerShell、cmd、CI 或直接进程调用中获得一致行为。

伪造 Bash 会增加新的解析器、转义规则和安全漏洞。在 Windows 上还会出现“模拟 Bash”和真实 PowerShell/进程语义不一致。我们应该模仿 Unix CLI 的可组合性，不模仿 Shell 语言本身。

## 二、Agent ABI：一份能力定义，生成所有入口

实施前的仓库已经有 `engine-rpc.schema.json`，但 MCP sidecar 仍在 TypeScript 中手工重复注册 `engine_schema`、`world_snapshot`、`run_headless`。随着工具增多，这会产生名称、参数、权限和文档漂移。

建议把反射和 Command Registry 升级为唯一事实来源。每项能力至少声明：

- 稳定名称与版本。
- 输入、输出和错误 JSON Schema。
- `read`、`write`、`control`、`debug` 等权限。
- 是否幂等、是否可撤销、是否支持 dry-run。
- 预计耗时、输出上限和是否为长任务。
- 所需运行态：离线、编辑器、运行中、暂停、安全点。
- 可能产生的 Artifact 和 Evidence 类型。

从这份 Registry 自动生成：

```text
Command Registry / Reflection Schema
        |-- noemancer CLI help and completion
        |-- JSON stdin/stdout tool calls
        |-- JSONL persistent control protocol
        |-- MCP tools and resources
        |-- documentation and AGENTS.md snippets
        |-- TypeScript/C++ client types
        `-- capability and audit policy
```

MCP sidecar 因而只负责协议封装，不再拥有业务知识。以后接 ACP、IDE 插件或其他 Agent 协议，也只是生成或适配另一层薄壳。

### 建议的三个入口

#### 1. 普通 CLI

面向人类、CI 和已经具有 Shell Tool 的 Agent：

```powershell
noemancer schema list --json
noemancer world query --component Transform --jsonl
noemancer build affected --changed src/render/pass.cpp --json
noemancer run --headless --frames 120 --seed 42 --record
noemancer debug attach --latest --json
noemancer capture gpu --frame next --output capture.json
noemancer evidence compare run-a run-b --json
```

遵循通用 [CLI Guidelines](https://clig.dev/)：主结果写 stdout，日志写 stderr，成功返回 0，失败使用稳定非零退出码；支持 `--help`、`--json`、`--jsonl`、`--quiet` 和用 `-` 表示 stdin/stdout。彩色表格只在 TTY 中出现。

#### 2. 直接工具调用

面向支持自定义工具发现、但不想运行完整 MCP Server 的 Agent：

```powershell
noemancer tools list --format json
'{"frames":120,"seed":42}' | noemancer tool call run.headless
```

Gemini CLI 已支持 [`tools.discoveryCommand` 与 `tools.callCommand`](https://github.com/google-gemini/gemini-cli/blob/main/docs/reference/configuration.md)：发现命令输出工具 Schema，调用命令从 stdin 读取 JSON、向 stdout 输出 JSON。我们的接口若遵循这个简单约定，就能在几乎没有中间逻辑的情况下接入此类 Agent。

其他 Agent 即使没有专门的发现机制，也可以直接运行相同命令。这样不会被 MCP 的普及程度绑定。

#### 3. 持久 Control Channel

面向已编译并正在运行的编辑器、游戏或 Headless Runtime：

- 开发构建启动本地 Named Pipe/Unix Domain Socket；也可由父进程以 stdin/stdout 托管。
- 使用 JSONL 或长度前缀 JSON，支持 request、response、event、cancel 和 backpressure。
- 握手阶段交换协议版本、Build ID、Schema hash、权限与运行态。
- 长任务返回 handle，支持 `status`、`wait`、`cancel` 和事件订阅。
- Release 构建默认不包含或不启用控制面。

本地 CLI 发现运行实例后，可直接转发 argv 对应的结构化调用：

```powershell
noemancer instance list --json
noemancer --instance latest world query --component Transform --json
noemancer --instance latest runtime pause --at safe-point
```

Agent 无需知道 Pipe、端口、进程 ID 和握手细节；也不需要让 MCP Server 再启动一次 CLI、CLI 再启动一次 Runtime。对于高频查询，CLI 可以作为轻客户端连接已有进程。

## 三、标准协议各管一层，不设计万能协议

### LSP：源码和 Schema 的语义

[Language Server Protocol](https://microsoft.github.io/language-server-protocol/specifications/specification-current) 适合定义跳转、引用、重命名、补全、诊断和符号。我们的脚本语言、Shader、反射 Schema 或场景查询语言应尽量提供 LSP，而不是为 MCP 重做一套代码智能。

### DAP：断点和程序执行状态

[Debug Adapter Protocol](https://microsoft.github.io/debug-adapter-protocol/) 已标准化 launch、attach、breakpoint、stack、scope、variable 和 evaluate。原生 C++、脚本 VM 和 Shader 调试应优先接现有 DAP Adapter。DAP 本身偏向面向 IDE 的高层字符串结构，不能承担 GPU 资源图和构建归因，所以仍需引擎语义诊断层。

### MCP：动态工具发现与上下文入口

[MCP](https://modelcontextprotocol.io/specification/2025-03-26/basic/transports) 使用 JSON-RPC，并保留本地 stdio 和远程 Streamable HTTP。适合把少量高价值工具和 Resource 暴露给 Agent。它不应成为引擎内部 ABI，也不应让 60 到 150 个细碎工具长期占用模型上下文。

建议 MCP 默认只暴露九类高层能力：

- `schema`
- `inspect`
- `query`
- `apply`
- `build`
- `run`
- `debug`
- `capture`
- `compare`

每类能力通过 Registry 查询具体 action 和参数，或由 MCP Tool Profile 按任务动态加载。高频、危险或需要精确 Schema 的操作仍可生成专用工具，但不是所有 C++ 函数都变成一个 MCP Tool。

### ACP/A2A：不要放进引擎核心

ACP 解决 Agent 与编辑器/客户端之间的会话、权限、进度和 Tool Call 展示；A2A 解决不同 Agent 之间的任务协作。它们位于引擎之外。引擎提供稳定 Agent ABI 后，可以编写 ACP Client/Plugin 或让多个 Agent 共用它，但没有理由让 Runtime 理解“哪个模型在思考”。

## 四、文件接口仍然重要，但不要把整个运行态伪装成文件系统

Agent 非常擅长 `rg`、`git diff`、`jq` 和普通文件操作。可以把稳定、适合 Diff 的状态物化到项目中：

```text
.noemancer/
  project.json
  capabilities.json
  schemas/
  generated/             # 可再生类型与文档
  runs/<run_id>/manifest.json
  evidence/<id>/manifest.json
  cache/                 # 不进入版本控制
```

场景、资源和配置继续使用规范化文本源文件。运行中的每个 Entity、GPU Resource 或线程不需要映射成虚拟文件；这会制造海量陈旧快照、竞争条件和奇怪的写入语义。动态状态由 `query` 与事件流读取，需要长期比较时显式 `snapshot`。

所有写操作走事务：

```powershell
noemancer apply scene.patch.json --dry-run --json
noemancer apply scene.patch.json --expect-revision 1842 --json
```

结果返回 Action Receipt：

```json
{
  "status": "applied",
  "operation_id": "op_...",
  "before_revision": 1842,
  "after_revision": 1843,
  "changed_objects": ["entity:player"],
  "artifacts": [],
  "evidence": ["evidence://operation/op_..."],
  "warnings": []
}
```

Action Receipt 是降低理解损耗的关键。Agent 不需要从“Done!”或几千行日志中猜测发生了什么，也能在下一步直接引用 operation、artifact 和 evidence。

## 五、源码编译和运行时调整应成为同一个热补丁事务

用户指出的两个难点是同一条循环的两端：Agent 修改源码以后必须付出编译开销；编译完成后又要把变化安全地送进正在运行的状态。

推荐将引擎拆成：

- 稳定 Runtime Kernel：平台、内存、任务、渲染后端、Schema Registry 和 Control Channel。
- 可重载 Native Module：玩法 System、工具扩展和非核心功能，使用明确 ABI 和版本。
- 数据/脚本层：绝大多数高频行为迭代，不触发核心 C++ 构建。
- Build Service：常驻依赖图、缓存和编译进程状态，计算最小受影响集合。

一次 Agent 修改采用以下协议：

```text
edit
 -> check affected graph
 -> compile candidate module in background
 -> run ABI/schema compatibility check
 -> pause runtime at declared safe point
 -> snapshot versioned ECS state
 -> load candidate beside old module
 -> run migration and smoke assertions
 -> atomic switch
 -> keep old module until validation passes
 -> emit Action Receipt + Build/Run Evidence
```

失败时不污染当前运行实例：候选 DLL/so、Schema 或脚本先在旁路验证；迁移失败便恢复旧模块和状态。运行时继续使用旧版本时，Build Service 可以在后台工作。Agent 得到的不是“编译成功”一个布尔值，而是一份 Patch Receipt：受影响模块、编译时间、缓存命中、ABI 变化、迁移结果、运行断言和回滚句柄。

### 两条调试平面

已编译 Runtime 的调试应同时保留：

1. Native Debug Plane：LLDB/GDB/MSVC DAP，负责线程、栈、变量、内存和断点。
2. Engine Semantic Plane：负责 frame、ECS system、task、render pass、resource、shader、asset 和 transaction。

通过 `run_id`、`frame_id`、`thread_id`、`task_id`、`system_id`、`build_id` 关联。Agent 可以从 ECS 异常下钻到 C++ 栈，也可以从 DRED/RenderDoc 反查生成该 GPU Command 的 System 和源码版本。

## 六、这在 2026 年是否新颖

需要诚实区分“组件新颖”和“系统组合新颖”。

单独看，下面都不新：CLI、JSON、MCP、LSP、DAP、Headless、热重载、结构化日志、RenderDoc、增量编译。社区已经有人把其中多项组合起来。

仍然相对少见的是：

- Command Registry 同时生成 Shell、direct tool、MCP、类型、文档和权限策略，避免每一层手工翻译。
- 编译候选、ABI 检查、状态迁移、运行断言和回滚作为一个可查询事务。
- Native Debug 与 Engine Semantic Debug 通过稳定 ID 关联。
- 每次 Agent 操作都返回可组合的 Receipt，而不是自然语言成功信息。
- 同一 Evidence 从源码 revision 一直追到 GPU/驱动故障。
- 用公开基准测量 Agent 完成真实 C++ 引擎任务的成功率、编译等待、Tool Call 数和诊断证据质量。

2026 年的 [GameEngineBench](https://arxiv.org/abs/2607.03525) 在真实 Unreal C++ 项目上，最强配置的 pass@1 为 55.5%，110 个任务中仍有 31 个没有任何配置解决。它说明真正困难的不是生成一段看起来合理的代码，而是跨生命周期、编译、运行态和多系统验证。我们的特色若正面优化这条闭环，仍然有研究和作品集价值。论文索引和项目定位摘要保存在[研究档案](papers/README.md)，原文从 arXiv 获取。

更准确的定位不是“像 Godot 一样对 AI 友好”，而是：

> 一个把编译、热补丁、运行时语义和底层诊断统一成稳定 Agent ABI 与可验证事务的 C++ 引擎。

## 七、对当前仓库的直接调整

下一步不需要先实现完整热重载。可以从接口的一致性开始：

1. 将原有 `engine-rpc.schema.json` 升级为 Agent ABI Schema，加入输入/输出 Schema、权限、幂等、dry-run、运行态与任务类型。
2. 让 C++ Command Registry 导出完整 Tool Manifest。
3. 实现 `noemancer tools list --format json` 和 `noemancer tool call <name>`，参数从 stdin JSON 读取。
4. MCP sidecar 根据 Tool Manifest 动态注册工具，删除手写的重复参数描述。
5. 所有命令统一输出 Result Envelope 与 Action Receipt。
6. 实现第一个持久 JSONL Control Channel，再让 CLI 连接正在运行的实例。
7. 用 `world.snapshot`、`run.headless` 和未来的 `build.summarize` 验证同一能力从 CLI、direct tool 和 MCP 得到等价结果。

这七步完成后，我们才真正拥有可供不同 Agent 复用的引擎操作面。后续编译服务、热补丁、DAP、Perfetto 和 GPU Evidence 都可以沿同一 ABI 生长。
