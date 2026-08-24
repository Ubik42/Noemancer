# ADR 0004: Semantic 2D Character Rig 与 Bake-to-Pixel

- Status: Proposed
- Date: 2026-08-18

## Context

Hybrid Pixel 游戏可能包含上百名二维角色。若每个角色独立制作所有方向、动作和序列帧，成本按角色、动作、方向和帧数相乘；直接由生成模型输出完整 Sprite Sheet 仍存在身份、比例、锚点、遮挡和帧间一致性问题。纯二维骨骼虽然复用性高，又容易破坏像素轮廓和夸张逐帧动画。

## Decision

1. 建立 Git 友好的 Semantic 2D Character Schema，分别描述 Archetype/Skeleton、Skin/Part、Direction Profile、Motion Clip、Cel Override 与 Hybrid Pixel Material。
2. 同 Archetype 角色共享骨架和动作；装备、头发、脸、服装和武器以语义 slot/attachment 组合。
3. 动画允许 rigid cutout、mesh deform、local cel swap、full-frame override 四级混合。
4. 编辑器保留 Puppet 权威源；Cook 默认烘焙为 Sprite Sheet/Texture Array。Dynamic Puppet 仅用于确有运行时变形价值的对象。
5. AI 生成部件、分割、绑定、动作曲线和局部修正候选；拓扑、像素网格、锚点、接触、导出和 QA 由确定性工具约束。
6. 角色系统接入 Semantic Observation Graph，使 Agent 能从视口中的某一帧追到 bone、slot、part、motion、material 和源文件。
7. Spine、Rive、Inochi2D、Godot 和 Unity 作为算法/格式/导入参考，不把任何外部二进制格式设为引擎权威源。

## Consequences

- 大量角色可复用动作，同时保持发布运行时接近传统 Sprite 的低成本。
- 需要自研二维 Rig Schema、方向映射、Pixel Compiler、混合动画预览和自动 QA。
- 角色差异过大时仍需新 Archetype；关键动作仍需要美术修订，系统不会消除艺术劳动。
- Source Rig 与 Cook 产物严格分离，减少 Git 冲突，但必须维护稳定的构建缓存和资产依赖图。

## Validation

详细原型矩阵和资料见 [2026 AI 模块化二维角色与序列帧管线调研](../research/2026-ai-modular-2d-character-pipeline.zh-CN.md)。完成三角色/四方向/三动作原型及 baked/dynamic 性能对比前，本决策保持 Proposed。

