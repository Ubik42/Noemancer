# 2026 渲染、UI 与引擎模块覆盖审计

> 文档类别：Historical coverage audit。本文不再作为模块清单或当前排期。

> 日期：2026-08-18  
> 范围：Noemancer 当前工程、Godot/Bevy/Wicked Engine 本地参考仓、`GAMES104.md` 的结构化知识清单，以及官方技术文档。本文只服务工程决策，不维护 Vault 中的项目副本。

## 结论

1. **当前架构方向正确，但渲染方案还不能保证优秀画质。** SDL_GPU 是很好的启动和兼容层，却明确不把 bindless、硬件光追和 mesh shader 作为近期能力。我们必须保留 engine-owned RHI/Render Graph，并把画质变成可测量的 Quality Contract。
2. **React + shadcn 值得借鉴，但不等于引擎里运行 React。** 真正有价值的是声明式树、开放组件源码、样式/设计令牌、无样式行为原语、组合和可访问语义。RmlUi 是目前最贴合 C++ 游戏引擎的开源轮子候选。
3. **编辑器 UI 和游戏 UI 应共享 Core，不应共享全部组件和生命周期。** 可以共用文档 IR、布局、样式、文字、动画、输入、可访问性、Localization 和测试；Dock/Profiler 与 HUD/Dialogue 保持两套组件库。
4. **Unity 不是“做不了本地化”。** Unity 6 已有 UXML/USS、Localization Tables、Smart Strings、资产本地化和伪本地化。社区项目做坏通常是内容、字体、布局和测试链问题，而不是换一个 UI 框架就会自动解决。
5. **与 GAMES104 对照，路线仍漏写了若干一级模块。** 最重要的是平台/内存/数学/任务基础、完整文字与本地化、输入、导航、网络、插件 SDK、协作、打包部署和发布质量门。它们不必现在实现，但必须进入总图，避免中后期补架构。

## 画面表现如何保证

### SDL_GPU 的真实边界

[SDL_GPU 官方文档](https://wiki.libsdl.org/SDL3/CategoryGPU)把目标定义为广泛硬件上的现代、可移植 GPU API；官方同时说明 mesh shader 和 ray tracing 不是近期计划，[bindless 也不准备加入 SDL3 GPU](https://wiki.libsdl.org/SDL3/HowToReportBugs)。所以：

- 它足以做优秀的跨平台光栅画面；
- 它不等于完整 renderer，更不等于 UE5 级特性；
- 把它直接当最终 RHI 会让未来高级特性、资源驻留和 GPU 崩溃诊断受限。

Noemancer 采用两层策略：SDL_GPU 保留为 portable backend；Render Graph、资源 ID、材质、Shader Schema 和 Render Evidence 属于引擎；出现经过测量的能力缺口时增加 native D3D12/Vulkan backend。

### 第一条画质基线

第一阶段不追逐 Lumen/Nanite 名称，而是把以下能力一次做正确：

- 线性 HDR、明确色彩空间、曝光与 tone mapping；
- PBR 材质、物理灯光/相机单位、IBL；
- Clustered Forward+ 或等价的可扩展多光源路径；
- 方向光/点光/聚光阴影及稳定级联；
- 法线、AO、透明、天空/雾、Bloom；
- TAA/SMAA 与可替换的 temporal upscaler 接口；
- glTF 2.0/KTX2/Basis、纹理压缩、mip 与 streaming；
- GPU marker、pass/resource timing、overdraw 和对象 ID。

[Filament 的 PBR 文档](https://google.github.io/filament/Filament.md.html)适合作为材质、物理单位和色彩基线；[Filament FrameGraph](https://google.github.io/filament/notes/framegraph.html)和本地 Wicked Engine 可作为实现对照。Wicked 已覆盖 bindless、DX12/Vulkan、光追、DDGI、GPU 粒子和 path tracing，适合提供压力场景，但不应整块复制成我们无法语义化的黑盒。

### Quality Contract

画质必须通过工程证据保证：

- 固定测试场景、相机、时间、灯光、曝光、随机种子与材质；
- 每个画质档保存 golden image、允许误差、GPU 时间和显存预算；
- 每次渲染修改输出 pass/resource/shader/pipeline 的稳定 ID；
- 自动检测 NaN、爆亮、闪烁、ghosting、shadow acne、漏光和颜色漂移；
- 同一场景对比参考 renderer，而不是只看“有没有 PBR”复选框；
- 图像回归失败与 Render Graph Diff、Shader Diff、GPU capture 绑定。

当前三个 Kenney GLB 只能验证导入和基础绘制，不能验证画质。后续还需许可明确的 Damaged Helmet、Sponza/类似复杂场景、材质球、透明/毛发、皮肤、极端光照和 Hybrid Pixel 专项场景。

## React/shadcn 思路到底是什么

[React](https://react.dev/learn/reacting-to-input-with-state)的核心不是 CSS，而是声明“某个状态下 UI 应长什么样”，框架维护组件树并增量更新。[shadcn/ui](https://ui.shadcn.com/docs)甚至明确说自己不是传统组件库：它把开放的组件源码交给项目，强调组合、可预测接口、漂亮默认值和 AI 可读性；主题主要由[语义 CSS 变量/Design Tokens](https://ui.shadcn.com/docs/theming)控制。

我们可以把它翻译成原生引擎概念：

```text
UI Document / Component Tree
        + typed state and actions
        + stylesheet and semantic design tokens
        + headless behavior primitives
        + layout / text / animation
        + stable semantic IDs
                    ↓
        Render Graph UI passes
                    ↓
      pixels + accessibility + Agent graph
```

这比“ImGui 换一套颜色”更深：结构、状态、行为、视觉、文字和语义是可分离的数据层。

### 为什么先评估 RmlUi

[RmlUi](https://github.com/mikke89/RmlUi)是 MIT 的 C++ HTML/CSS-like UI：有 DOM、Flexbox、动画、模板、数据绑定、Localization hook、运行时调试、SDL3 和 SDL_GPU backend，体积远小于完整浏览器。它与我们的技术栈吻合。

限制也必须承认：它不是完整浏览器；官方矩阵显示现成 SDL_GPU renderer 目前只有基础绘制和 transform，不完整支持 clip masks、filters 和 shader decorators。因此正确用法是：

- 借用成熟的文档、样式、布局和事件核心；
- 自己接 Noemancer Render Graph、纹理系统、文字 atlas 和诊断；
- 公共 Schema 保存我们自己的 UI Node/Style/Action，不保存 RmlUi 指针或内部枚举；
- 先做 spike，不立即替换整个 ImGui 编辑器。

## Unity 本地化为什么常被做坏

[Unity UI Toolkit](https://docs.unity3d.com/kr/6000.0/Manual/ui-systems/introduction-ui-toolkit.html)本身已经是 retained-mode：UXML 类似 HTML，USS 类似 CSS。[Unity 6 Localization](https://docs.unity3d.com/jp/current/Manual/best-practice-guides/ui-toolkit-for-advanced-unity-developers/localization.html)也支持 String/Asset Tables、Smart Strings、运行时切换和数据绑定。所以问题通常来自：

- 把句子拼成多个字符串，翻译者无法调整语序；
- 固定像素宽高，德语等文本增长后裁切；
- 只替换字符串，没有字体 fallback、字形覆盖和动态 atlas；
- 阿拉伯语/希伯来语的 shaping、BiDi、镜像布局和输入没有打通；
- CJK 换行、禁则、IME 和组合字符按英文模型处理；
- 数字、日期、单位、复数、性别仍由代码手拼；
- 插件和编辑器扩展写死字符串，不能进入统一 String Table；
- 伪本地化和截图回归做得太晚。

Unity 自己的[伪本地化文档](https://docs.unity3d.com/ja/Packages/com.unity.localization%401.4/api/UnityEngine.Localization.Pseudo.PseudoLocale.html)就列出了文本增长、RTL、缺字和语序问题。[HarfBuzz](https://harfbuzz.github.io/what-is-harfbuzz.html)负责把 Unicode 字符正确 shaping 成 glyph；[ICU MessageFormat](https://unicode-org.github.io/icu/userguide/format_parse/messages/)解决整句模板、参数和语言规则。我们若只做 `key -> string` 字典，同样会失败。

Noemancer 的特色应是：Localization 不是一个后期插件，而是 Semantic UI Node 的原生字段。Agent 能直接查询“这个控件在德语下溢出”“这个阿拉伯语节点用了哪个 fallback font”“哪段硬编码文本没有 message key”。

## 编辑器 UI 与游戏 UI 能复用多少

| 层 | 应共享 | 应分开 |
| --- | --- | --- |
| 文档与状态 | Node/Component IR、binding、action、revision | 编辑器 session 状态与游戏存档状态 |
| 视觉 | design tokens、stylesheet、字体、图标管线 | Editor theme 与 Game art direction |
| 交互 | focus、pointer、keyboard、IME、controller navigation | Dock/shortcut 与 HUD/gameplay input policy |
| 组件 | Button、Text、List、Tree、Scroll、Popup、Virtualization | Inspector/Graph/Timeline 与 HUD/Dialogue/Inventory |
| 渲染 | batching、clip、atlas、Render Graph pass | world-space UI、后处理组合、编辑器多窗口 |
| 语义 | accessibility tree、stable ID、test/Agent query | 权限、暴露范围和 shipping telemetry |

结论是“同内核、不同产品层”。这比编辑器一套 ImGui、游戏一套完全独立 UI 更省长期成本，也比强行让 HUD 使用 Docking 组件更干净。

## 与 GAMES104 和参考仓的模块差距

本地 Godot 模块已经覆盖资产格式、物理、导航、音频、多人、XR、文本、脚本与平台服务；Bevy 将 asset/render/PBR/UI/a11y/audio/input/scene/tasks/remote 等拆成独立 crate；Wicked 直接展示了 graphics device、renderer、BVH、job、audio、input、network、localization、physics、terrain、video、Lua 和粒子等生产模块。

| 一级模块 | 当前实现 | 已进入路线 | 审计结论 |
| --- | --- | --- | --- |
| 平台/窗口/GPU | SDL3/SDL_GPU 启动 | 部分 | 缺文件系统、线程、动态库、崩溃、平台能力与打包边界 |
| 数学/内存/容器/任务 | 仅简单数据与固定 tick | 不完整 | **重大漏项**；应在玩法系统前建立基线和 profiler |
| World/ECS/场景 | Flecs demo | S1 | 缺 scene lifecycle、streaming、prefab、event/phase contract |
| 反射/Schema/序列化 | ABI Schema 雏形 | S1 | 方向正确；需统一 C++/C#/场景/Shader/UI Schema 生成 |
| 资产/Cook | 测试 GLB | S2 | 已规划；需 importer SDK、依赖图、版本迁移、DDC/缓存 |
| 渲染 | 编辑器 clear + 程序化预览 | S3 | 已规划但需 Quality Contract 与 native backend 逃生口 |
| GUI/工具 | ImGui 壳 + semantic model | S0/S4 | 缺 retained UI、text/i18n、插件面板、layout persistence |
| 输入 | SDL event 仅窗口退出 | 未单列 | **重大漏项**；键鼠/手柄/action map/rebinding/IME |
| 物理 | 无 | S6 | 已规划 Jolt spike |
| 动画 | 无 | S6 | 已规划 ozz+ACL；还需 graph/retarget/IK/2D runtime |
| 音频 | 无 | S6 一笔带过 | 应单列 mixer/bus/streaming/spatial/authoring |
| Gameplay/脚本 | 无 | S6 | C# 方向明确；缺 lifecycle、event、save/replay contract |
| 导航与游戏 AI | 无 | 未单列 | **重大漏项**；NavMesh/query/crowd/debug evidence |
| 网络/服务器 | 无 | 未单列 | **重大漏项**；先定义 optional profile，不必近期实现 |
| VFX/粒子 | 研究完成 | S7 | 顺序正确，应依赖 renderer/asset/evidence |
| 插件/SDK | 无 | 未单列 | **重大漏项**；编辑器、importer、runtime module 均需要版本边界 |
| 协同编辑 | Git/SVN 目标 | S1 局部 | 缺 lock/merge/conflict/session/ownership 的远期设计 |
| 测试/诊断 | CTest/MCP smoke | S5 | 方向突出；需 visual/perf/replay/crash compatibility matrices |
| 发布/平台/XR | 无 | 未单列 | 至少需 build/package/sign/patch/save migration 总入口 |

没有必要立刻实现全部模块，但总架构必须承认这些位置。近期仍坚持：持久 ID/事务 → UI/Text spike 与 Asset Registry → 真渲染视口；网络、XR、服务器和协同只先定义 profile 与边界，不挤占底层闭环。

## 对工程计划的直接修正

- 产品名改为 **Noemancer**，窗口名为 **Noemancer Editor**。
- 新增 Render Quality Contract 与 Semantic UI Platform ADR。
- S1 加入平台、数学/内存、任务和统一 Schema 的最低基线，而不只是场景 JSON。
- 增加 UI/Text/Localization spike，并与 Observation Graph 共用语义树。
- 玩法阶段明确输入、音频、导航；远期增加网络、插件 SDK、协作和发布平台阶段。
- 不修改或同步工程目录之外的 `AI Native引擎开发.md`。
