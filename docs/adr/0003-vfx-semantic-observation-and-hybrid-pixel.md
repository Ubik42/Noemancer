# ADR 0003: VFX、语义观察与 Hybrid Pixel Profile

- Status: Accepted for VFX semantics and the Semantic Observation Graph; Hybrid Pixel remains a planned product profile
- Date: 2026-08-18

## Context

引擎需要高性能粒子/VFX、Agent 能理解的实时编辑器，以及用于验证能力的 HD-2D 游戏。现有方案分别提供 GPU VFX Graph、Accessibility Tree、Scene MCP 和 2.5D 渲染，但没有形成统一、可追踪的开发闭环。

## Decision

1. VFX 采用版本化 Graph IR、Slang 生成内核、共享 GPU Pool、Data Channel、indirect dispatch/draw 和分级碰撞/透明策略。玩法权威状态不放在视觉粒子中。
2. Agent 默认修改带类型、单位、范围、成本和语义描述的 Graph；自由 Shader 代码是受限能力。候选修改必须经过确定性预览、指标比较和提交/回滚。
3. 编辑器建立 Engine Semantic Observation Graph，同时覆盖 UI Tree、Viewport Scene Projection、Render Evidence 和按需视觉附件。
4. 语义树以稳定 ID、revision、过滤查询和 delta stream 为基础；评估 AccessKit 作为 UI 可访问性与语义数据底座。
5. 第一个验证游戏采用 Hybrid Pixel 2.5D Profile：固定虚拟分辨率、pixel snapping、Sprite 深度/法线/材质、2D/3D 光照遮挡、像素化 VFX 与受控后处理。
6. 所有“高性能”结论只接受固定 workload 和硬件下的可复现 CPU/GPU/显存/构建证据。

## Consequences

- Agent、屏幕阅读器和 UI 自动化可以共用一份语义数据源。
- 需要在编辑器 UI 初期就维护稳定语义身份；不能等界面完成后再从绘制调用逆向推断。
- Graph 编译器、GPU 内存池、观测 delta 和跨图 ID 映射是必须自研的核心，而不是简单接入 MCP。
- Effekseer、AccessKit 等项目可以降低启动成本，但外部格式和 API 不成为引擎权威模型。
- Hybrid Pixel 会暴露透明填充、排序、后处理和 2D/3D 资产管线问题，是有效验证场景，但不会成为引擎的硬编码唯一用途。

## Validation

VFX Graph、GPU 执行纵切、语义观察和事务接口已经验证，因此相关架构成立。Hybrid Pixel 的像素网格、专项材质/光照和完整 Profile 仍按当前开发计划等待后续里程碑。历史推导见 [2026 VFX、语义观察与 Hybrid Pixel 技术报告](../research/2026-vfx-semantic-observation-and-hybrid-pixel.zh-CN.md)。
