# 2026 Semantic UI 平台与 RmlUi 采用决策

> 文档类别：Historical decision input。RmlUi 当前角色由 ADR 0005 与权威架构定义。

更新日期：2026-08-18

## 要解决的问题

Noemancer 需要同时提供编辑器 GUI 和游戏内 UI。两者面对的用户、组件和运行条件不同，但布局、文字、输入、样式、渲染、可访问语义、自动化测试和 Agent 观察不应重复建设。

当前工程已经完成声明式 Inspector 的第一条可见、可交互纵向切片。Property Schema 生成稳定节点和 Action；人类编辑、Agent 调用和 undo/redo 使用同一套 World 事务。Inspector 可投影为通用 UI Document，通过 `ui.observe` 做有界语义查询，并经 RmlUi DOM/CSS/Flex、字体图集和自有 SDL_GPU Adapter 合成到 Scene View。完整游戏组件库、国际文字和样式热更新仍未实现。

实现进展补充：同一公共格式现已从 Gameplay Ability State 生成首个游戏 HUD，Health/Stamina、Tag、Ability Slot 分别携带稳定 Binding/Action，并与 Inspector 同时进入同一个 RmlUi Context 和 SDL_GPU 合成。Document 级 `designTokens`、resource stylesheet/hot reload、平台 fallback、SDL IME 已验证。引擎自有 Text ABI 固定 HarfBuzz 14.3.1/ICU 78.3，并输出 renderer-neutral glyph runs、BiDi 与 line-break plan；RmlUi 6.2 的固定最小补丁把同一 glyph run 接入默认 FreeType atlas/effect/geometry 路径，Arabic/RTL 自动化和 D3D12 离屏画面均已通过。完整组件库仍待后续，因此“共享 Core、分离 Editor/Game 组件库”的架构判断已有运行代码验证，但还不是完整 UI 产品。

## 主流引擎如何划分编辑器与游戏 UI

### Unity

Unity 过去常见的组合是编辑器使用 UI Toolkit、游戏使用 uGUI，但这已经不是完整现状。UI Toolkit 同时支持 Editor 和 Runtime，核心包括 Visual Tree、Binding、Flexbox、Event System、UXML 和 USS；Unity 也推荐新 UI 优先考虑 UI Toolkit。uGUI 仍有大量项目和现成组件，因此两者会长期共存。

参考：

- [Unity UI Toolkit](https://docs.unity3d.com/2023.2/Documentation/Manual/UIElements.html)
- [Unity 6 Runtime UI examples](https://docs.unity3d.com/ja/6000.0/Manual/UIE-runtime-examples.html)

### Unreal Engine

Unreal Editor 主要由 Slate 构成，游戏项目通常使用 UMG。UMG 是面向美术、Blueprint 和资产工作流的产品层，但布局、Widget、裁剪、失效和调试能力仍建立在 Slate 之上。Slate 的设计强调程序化构造、属性/Delegate 绑定、列表虚拟化和低粒度失效；UMG 为游戏团队补上可视化创作和游戏组件。

参考：

- [Slate UI Framework](https://dev.epicgames.com/documentation/en-us/unreal-engine/slate-ui-framework)
- [Slate Architecture](https://dev.epicgames.com/documentation/unreal-engine/understanding-the-slate-ui-architecture-in-unreal-engine)
- [Slate/UMG Invalidation](https://dev.epicgames.com/documentation/unreal-engine/invalidation-in-slate-and-umg-for-unreal-engine)

### Godot

Godot 的编辑器、插件和游戏 UI 都建立在 Control/Container/Theme 体系上。它证明了共享 UI 内核可以降低插件开发成本，但也说明共享内核不等于所有表面使用同一套组件：编辑器 Dock、Inspector 和游戏 HUD 仍有不同的组合规则与视觉资源。

参考：

- [Godot UI](https://docs.godotengine.org/en/stable/tutorials/ui/index.html)
- [Godot editor plugins](https://docs.godotengine.org/en/stable/tutorials/plugins/editor/making_plugins.html)
- [Godot keyboard/controller navigation](https://docs.godotengine.org/en/stable/tutorials/ui/gui_navigation.html)

### Bevy

Bevy 0.17 引入实验性的 Headless Widgets，并在其上建设偏编辑器的 Feathers。行为、状态和可访问语义与视觉样式分离，这一点适合 Noemancer；但官方仍提示 API 会变化，文字输入和组件覆盖也未完成，暂时不适合作为我们的实现依赖。

参考：[Bevy 0.17 Headless UI Widgets](https://bevy.org/news/bevy-0-17/)

## Noemancer 的边界

采用一套引擎拥有的 Semantic UI Core，在其上建设两套组件库：

```text
Semantic UI Core
├── UI Document / stable node ID / typed state
├── binding / action / focus / navigation
├── text / localization / accessibility
├── layout / style / animation / render extraction
└── observation / delta / test / evidence
    ├── Editor Components
    │   └── Dock、Inspector、Tree、Table、Graph、Timeline、Profiler
    └── Game Components
        └── HUD、Menu、Dialogue、Inventory、Subtitle、Safe Area、World-space UI
```

编辑器 Session 状态与游戏存档状态分开；Editor Theme 与项目美术风格分开；Dock/快捷键和 Gameplay Input Policy 分开。其余高成本基础设施尽量共享。

### 与其他引擎不同的部分

Noemancer 的公开 UI 身份不是 RML、RCSS 或 ImGui Widget，而是版本化 Semantic UI Document：

- 每个节点有稳定 ID、role、父子关系、类型、约束和 Source Anchor；
- Binding 指向稳定的引擎属性，不暴露 Flecs、SDL、GPU 或 RmlUi 指针；
- Action 指向可验证的 `Plan -> Apply -> Receipt`，编辑器写操作带 revision 和 undo/redo；
- GUI、Agent、自动化测试和可访问接口读取同一份语义；
- Agent 默认取得焦点区域和有界查询，布局、截图、GPU Capture 等证据按需返回；
- 本地化信息进入节点本身，后续可查询 resolved locale、message key、overflow、font fallback 和 missing glyph。

当前 Inspector 已经证明 Schema 驱动属性编辑可行。下一步要把它从 Inspector 专用 JSON 提升为通用 UI Document，而不是先重写外观。

## RmlUi 评估

RmlUi 6.2 是当前最合适的开源 retained UI 起点。它采用 MIT 许可证，提供 HTML/CSS-like 文档、DOM、Flexbox、动画、模板、数据绑定、空间导航、Localization hook、运行时 Debugger、SDL3 和 SDL_GPU 后端。应用负责更新循环、输入和渲染接口，适合嵌入自研引擎。

参考：

- [RmlUi repository and feature matrix](https://github.com/mikke89/RmlUi)
- [RmlUi 6.2 release](https://github.com/mikke89/RmlUi/releases/tag/6.2)
- [RmlUi 2026 developer survey](https://github.com/mikke89/RmlUi/discussions/894)

它不能直接作为完整生产 UI：

- 官方 SDL_GPU Renderer 目前只有基础绘制和 transform，缺少完整 clip mask、filters 和 shader decorators；
- 大型 Tree/Table 缺少成熟虚拟化。社区的大列表案例暴露了全量布局和高频更新成本；
- DataModel 对多模型、可变 alias 和局部 UI 状态仍有限制；
- Editor Dock、Graph、Timeline、OS Accessibility Bridge 和引擎级文字诊断需要自行建设；
- RCSS Custom Properties/`var()` 等能力仍在演进，Design Token 不能完全依赖上游语法。

参考：

- [RmlUi large data set discussion](https://github.com/mikke89/RmlUi/discussions/653)
- [RmlUi data binding limitations](https://github.com/mikke89/RmlUi/issues/748)
- [RmlUi active pull requests](https://github.com/mikke89/RmlUi/pulls)

## 采用方式

现阶段不 Fork RmlUi Core。采用以下三层结构：

1. 固定并跟踪上游版本，尽量不修改 Layout、DOM 和 Event Core。
2. 在工程内维护 RmlUi Adapter、Custom Elements、Semantic Bridge、Asset URI、Text/Font 接口和 Render Graph Backend。
3. 通用修复提交上游；只有必要 ABI 或性能修改长期无法合入时，才维护可重放的 patch stack。

RmlUi 负责成熟的文档解析、样式、布局、事件、动画和几何生成。Noemancer 保持以下能力的所有权：

- `noemancer.ui-document/*` 公共格式；
- Semantic State Plane 与响应式 Binding Graph；
- Virtualized List/Tree/Table；
- Design Token 编译和项目主题；
- HarfBuzz/ICU、字体 atlas 和本地化诊断；
- Render Graph UI Pass、纹理、clip/mask/filter 和 GPU evidence；
- Agent 查询、UI Delta、Action Receipt 和权限边界。

### 启动 Fork 的条件

满足下列条件之一才考虑 Fork：

- 在固定基准中，布局失效传播或 DOM 生命周期是无法绕开的主要瓶颈；
- Console/平台认证要求修改核心，而上游抽象不能承载；
- 必要补丁长期无法合入，工程持续维护多组互相依赖的核心 patch；
- 上游 API 破坏 Semantic UI 的稳定版本边界。

即使 Fork，也保留上游历史和独立 patch 清单，不把第三方源码改名伪装成引擎自研模块。

## 开发与验收顺序

### UI-0：通用 Semantic UI Document

状态：首纵切已完成并通过自动化测试。当前来源是 Inspector，后续 Outliner、Asset Browser 和游戏 UI 将接入同一文档边界。

- 把 Inspector 专用文档投影为 `noemancer.ui-document/0.1`；
- 建立稳定节点索引、role/ID 查询、depth、cursor 和 byte budget；
- 节点包含 typed binding、action、状态、presentation 和 localization fallback；
- 保留原 `editor.inspector.describe`，新增统一的聚焦 UI 观察入口。

### UI-1：RmlUi 集成尖峰

状态：生产纵切、国际文字分析和 retained glyph-run 渲染纵切已完成。工程固定 RmlUi 6.2、FreeType 2.14.3、HarfBuzz 14.3.1 和 ICU 78.3。Inspector 的通用 UI Document 进入真实 DOM/CSS/Flex；typed packet 与自有 SDL_GPU Adapter 已在 D3D12/Vulkan 上屏，稳定 Node ID、平台 fallback、资源 locale、SDL IME 均已贯通。公开 Schema 不暴露 RmlUi/SDL/ICU/HarfBuzz 对象；`noemancer.text-layout/0.1` 只暴露 renderer-neutral glyph/cluster/advance、visual run 与 line-break 数据。工程维护一个可重放、严格绑定 RmlUi 6.2 的小补丁，仅扩展 glyph-index atlas 装载和 shaped geometry 生成，不 Fork 布局/DOM/事件系统；补丁文件为 `cmake/patches/rmlui-6.2-harfbuzz-icu.patch`。可再分发 bundled fallback 字体、run cache 与 cluster-aware 编辑尚未完成。

离屏验收证据为 `generated/verification/retained-ui-text.bmp`。本机 D3D12 首帧实际记录 26 draws、1,428 vertices、2,142 indices、2 个 resident textures（白纹理与字体 atlas），`textureSampling:true`。这个数字是纵切正确性证据，不是最终批处理性能目标。

- 固定 RmlUi 6.2；
- 同时渲染一个游戏菜单和一个 retained Inspector；
- 验证 SDL3 输入、DPI、触摸、手柄导航、热重载和资源 URI；
- 不使用 RmlUi 指针作为公共身份。

### UI-2：文字、本地化与可访问语义

- 缓存 shaped run，并让 caret/selection 按 cluster 移动；
- 覆盖中文、阿拉伯文、BiDi、CJK 换行、IME、fallback 和伪本地化；
- 输出 overflow、clip、missing glyph、resolved font 和 locale evidence。

### UI-3：Render Graph Backend 与组件库

- 实现 atlas、batch、clip mask、filter、HDR composition 和 UI GPU marker；
- 建设无样式行为组件，再分别组合 Editor Kit 与 Game Kit；
- Tree/List/Table 必须虚拟化，不允许把数千行直接常驻 DOM。

### UI-4：迁移和验证

- 先迁移 Inspector，再迁移 Outliner 与 Asset Browser；
- Dockspace、Profiler 和底层 Render Debugger 可以继续使用 ImGui；
- 游戏侧依次实现 Menu/HUD、Dialogue、Inventory、Subtitle 和 World-space UI；
- 固定语义快照、截图 golden、键盘/手柄路径、DPI/语言矩阵和性能预算。

## 本阶段决定

- 接受“同内核、不同组件库”的 UI 架构。
- 接受 RmlUi 作为候选布局/样式/DOM 实现，不把它作为公开 UI Schema。
- 暂不 Fork RmlUi，不立即替换整个 ImGui 编辑器。
- UI-1 已证明“Semantic UI 公共层 + RmlUi 实现层 + 自有 GPU/Agent Adapter”可行；继续采用，但仍不让 RML/DOM 成为资产与 Agent 的权威格式。
- 下一阶段优先补文字塑形、fallback、本地化诊断和行为组件；只有真实基准触发既定条件时才 Fork RmlUi。
