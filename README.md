<div align="center">

# Noemancer

**一套正在开发的 C++20 游戏引擎与编辑器。**

[简体中文](README.md) · [English](README.en.md)

[![License](https://img.shields.io/github/license/Ubik42/Noemancer?style=flat-square)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?style=flat-square&logo=cplusplus)
![Platform](https://img.shields.io/badge/platform-Windows%20x64-0078D4?style=flat-square&logo=windows)
![Status](https://img.shields.io/badge/status-pre--alpha-F59E0B?style=flat-square)

[当前能力](#当前能力) · [快速开始](#快速开始) · [架构](#架构) · [开发状态](#开发状态)

</div>

![Noemancer 中文编辑器](docs/media/editor-preview.webp)

<p align="center"><sub>中文 Editor：Scene View、World Outliner、声明式 Inspector、Asset Browser 与 Agent Context 共用同一份场景权威状态。</sub></p>

## 最新渲染画面

![Intel Sponza 2022 宫殿中庭](docs/media/renderlab-sponza-atrium.webp)

<p align="center"><sub>Intel Sponza 2022 中庭：约 205 万顶点、1124 万索引、405 个 primitive 与 72 张材质图；图示为 D3D12，Vulkan 已取得同构捕获。</sub></p>

| Sponza 上层回廊 | 商业 Raster 基准 |
| --- | --- |
| ![Intel Sponza 2022 上层回廊](docs/media/renderlab-sponza-balcony.webp) | ![PBR、材质响应、阴影、Bloom 与 Tone Mapping](docs/media/renderlab-commercial-raster.webp) |

| 动态天空与大气 | SSR 与 SSGI |
| --- | --- |
| ![四 LUT 动态天空与 Aerial Perspective](docs/media/renderlab-sky-atmosphere.webp) | ![RenderLab 中启用 SSR 与 SSGI 的公开经典资产场景](docs/media/renderlab-ssr-ssgi.webp) |

以上图片来自 Release 隐藏捕获，不是离线渲染或概念图。Classic 基线已走通 Project → Cook → Package → Player；Sponza 当前证明的是正式外部 Source Project、JSON glTF 依赖闭包和 D3D12/Vulkan 实时画面，大型场景 Cook/Package 仍待收口。两后端分别留有画面、Render Graph、Shader 指纹和结构化回执。Sponza 2022 由 Frank Meinl 制作、Anton Kaplanyan 赞助并由 Intel 发布，采用 [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/)；RenderLab 使用保留完整几何、将材质派生为 1K 的外部测试版本，不把大型素材提交到引擎仓库。

Noemancer 不是现成引擎的编辑器外壳。目前仓库已经包含原生 Editor、游戏 Runtime、资产 Cook、独立 Player 打包、C# 项目脚本，以及由同一套引擎命令驱动的 CLI 和 MCP 接口。

当前版本可以从 Project Hub 创建或打开工程，在 Editor 中编辑 Scene、组件、输入和项目 UI，进入隔离的 Play World 运行 C# 游戏逻辑，再把资产与运行时依赖打成可独立启动的 Windows Player。引擎仍处于 **pre-alpha**：公开 API 会变化，只完整验证了 Windows x64；面向项目画面的生产级硬件光追、RTGI、VSM 与跨平台发行仍不能写成现有能力。

## 当前能力

### 编辑器与项目工作流

- 官方入口 `Noemancer Editor.cmd` 默认使用简体中文，也可切换 English。
- Project Hub 支持创建工程、打开工程与最近工作区；Starter 和 Hybrid Pixel 模板使用同一 Project Workspace 服务。
- Editor 包含 Scene View、World Outliner、Inspector、Asset Browser、Console、Animation Graph 与 Agent Context。
- Edit World 与 Play World 相互隔离；运行结果可以选择性 Apply Back，并作为一次可撤销的 Scene 事务提交。
- Scene 属性、Project Input、Project UI 和 Hybrid Pixel Profile 都经过 revision 检查、候选校验、原子写入与 undo/redo，而不是由界面直接改文件。

### 游戏运行时

- Flecs ECS、Jolt 刚体/传感器/查询、ozz 骨骼动画与 GPU Skinning；脚本可对动态刚体施加力、线性冲量与角冲量。
- .NET 10 / C# 项目脚本，支持项目编译、热重载状态迁移、调试会话和类型化 World Command Buffer。
- RmlUi/CSS 项目 UI、HarfBuzz/ICU 文本整形、输入映射、miniaudio 音频、GPU 粒子 VFX。
- Prefab 生成与销毁、固定步长模拟、Save/Replay 文档及项目级持久化请求。

### 物理

- Box、Sphere、Capsule 与 Convex Collider，Static/Dynamic/Kinematic RigidBody，以及质量、重力、摩擦、弹性、线/角阻尼、CCD 和休眠。
- Contact/Trigger 生命周期、带过滤的 Ray/Sphere/Box/Capsule Sweep 与 Box/Sphere/Capsule Overlap，以及 2D Character Motor。
- Fixed、Distance、Hinge、Slider、Spring 使用稳定 plain-data 合同，并在与刚体相同的 Jolt Physics System 中创建、更新、移除和观测；不会另建一套“演示物理世界”。
- “物理关系测绘台”可在编辑器中选择刚体、标定锚点/轴/限位/弹簧参数，并通过 revision、dry-run、receipt 和 undo 提交；同一关系还可由项目 C# Command Buffer 与 Agent 命令创建或修改。
- 视口会直接绘制锚点、连接线、铰链角度、滑轨行程与弹簧线圈；声明式 Inspector、Scene JSON、生成式 C# Schema 与 Agent 观测继续使用同一稳定身份，Collision Layer/Mask 已贯通上述边界。
- 内置 `--physics-showcase` 场景除摩擦、堆叠、多米诺、弹性、CCD、运动平台与 Trigger 外，还包含五组命名清楚的约束装置。

### 实时渲染

- SDL_GPU 后端，D3D12 与 Vulkan 使用同一套引擎资源和 Render Graph 合同。
- Forward PBR、split-sum IBL、方向光四级 CSM、Point/Spot 局部阴影与稳定阴影缓存。
- 静态不透明物体支持 GPU 视锥裁剪、compact visible-index stream、indexed indirect draw 和稳定批次复用；不满足条件的对象回退到直接提交路径。
- 四 LUT 动态天空与 Aerial Perspective、共享 HiZ、生产 SSR、生产 SSGI、独立时域 History、TAA、GTAO 与双边降噪、四级 Bloom、曝光/调色、ACES Tone Mapping。
- Hybrid Pixel / HD2D Profile 支持虚拟分辨率、整数倍显示、像素对齐 Sprite/VFX、2D/3D 混合光照和受控后处理。

当前默认 Raster 路径已经启用动态天空、SSR 与 SSGI，并在 RenderLab 取得 D3D12/Vulkan 的固定画面和逐 Pass GPU 时间证据。Native Ray Tracing 已跑通双后端执行闭环，并能由生产 `SceneRenderer` 提交真实场景几何；SDL_GPU 3.4.14 的固定补丁现可在 Runtime 私有边界提供借用的原生设备/队列与非 cycling 导出纹理。D3D12 context 能复用同一设备，使用仓库锁定 DXC 构建的 `noemancer.native-rt-full-frame/0.1` 为每个像素发射射线，并将结果纯 GPU Copy 到 SDL 纹理；可选 Render Graph v18 随后以专用全屏 Pass 显示该整数纹理。Vulkan 已由 pinned SPIR-V 写入 Runtime 私有 storage image，但目前只证明 1×1 诊断 texel，尚未接通 SDL 同设备呈现。项目可见 RTGI 与 VSM 仍在开发计划中；未进入真实项目管线、跨后端验证和性能证据的能力不会列为已完成。

如果不熟悉 PBR、HiZ、SSR、SSGI、时域降噪或 Tone Mapping，可以阅读[当前开发程度与渲染小白说明](docs/rendering-explained.zh-CN.md)。它按一帧画面的真实加工顺序解释每项功能，也明确区分“已经形成画面”“只有底层基础”和“尚未实现”。

Native Ray Tracing 当前采用独立的 bounded Adapter，而不是把 D3D12/Vulkan Handle 泄漏进 Scene 或 Agent API。RTX 4080 已在 DXR 1.1 与 Vulkan RT 上真实完成 BLAS/TLAS、光追 Pipeline、SBT、Trace Dispatch 与诊断 Readback。两端 context 会跨调用持有并复用 device/queue/command/fence、几何、BLAS/TLAS、Pipeline、SBT、输出与回读资源。生产 `SceneRenderer` 已保留 builtin/imported 几何，通过稳定 Source Adapter、`RenderWorld` 实例变换、世界空间 Geometry Cache 与长寿命 Bridge 提交真实场景；`--enable-native-rt-session` 才启用这条实验路径，默认 Raster 不承担额外成本。当前 640×480 RenderLab 以 28,952 个三角形证明 `fullFrameShaderReady=true`、`outputTraceWritten=true`、`outputTransferCompleted=true`、`composite.recordedThisFrame=true`，隐藏捕获为石墨/铜色的真实命中诊断图。D3D12 可借用 SDL 的同一 COM 设备与队列，把线性输出纯 GPU Copy 到精确匹配的 SDL 导出纹理，再由 `native-rt-debug-composite/0.1` 用整数 `Texture2D.Load` 呈现；异步 fence API 也已具备 submit-only/poll/wait 状态，但单 allocator/output 的生产 session 仍保持同步，不能据此声称多帧并行。Vulkan pinned SPIR-V 已真实写 storage image，不过当前只证明一 texel，且没有 SDL image/semaphore interop。成果因此仍不是 RTGI 或商业性能结论；状态固定 `claimsRtgi=false`、`rtgiReady=false`。

### 资产与发布

- GLB/JSON glTF/FBX 离线导入，KTX2 BasisLZ/UASTC 纹理 Cook，meshoptimizer 几何处理，Sprite Atlas 与 Tilemap 增量数据。
- Cook 产物由源文件、配方、目标 Profile 和工具版本共同寻址；Runtime 在加载前检查范围、Schema 与 SHA-256。
- 打包后的 Player 只加载 `.meshbin`、`.animbin`、KTX2 等运行时资产，不在玩家机器上解析源 GLB/FBX 或执行离线编译。
- Windows 包包含 app-local .NET、VC Runtime、Shader Manifest、第三方许可证和 NOTICE；包体通过原子 staging 提交。

### Editor、CLI 与 Agent 共用同一套命令

引擎的 C++ Command Registry 同时服务 Editor、direct JSON、CLI 和 MCP。它公开稳定 ID、Schema、revision、有限观察结果与结构化错误。写操作沿用同一个流程：

```text
Observe -> Plan(base revision) -> Apply -> Receipt -> Undo/Redo
```

运行中的 Editor 会发布本机 same-user 会话。MCP 可以连接到这个 Editor 已有的 World、Asset Registry、Project UI 和撤销记录；没有活动 Editor 时，自动化流程仍可启动隔离的 `serve --project` 会话。Agent 不直接持有 Flecs、Jolt、SDL 或 ImGui 句柄，也不会建立第二份 Scene 数据库。

## 已验证的独立工程

| 工程 | 验证内容 |
| --- | --- |
| `D:\3D\NoemancerProjects\NoemancerPlatformer` | Project UI/Input、C# 脚本、Sprite/Tilemap、Cook、Package 与独立 Player |
| `D:\3D\_tools\StarfallGauntlet` | 不含项目 Native C++ 的 clean-room 2D 游戏行为纵切 |
| `D:\3D\NoemancerProjects\NoemancerRenderLab` | 使用公开 Project/Scene/Registry 路径验证真实 GLB、双后端画面、质量与性能合同；当前仍在推进 |

测试、画面与机器可读收据的准确边界记录在[当前能力与验收边界](docs/first-acceptance-status.zh-CN.md)。README 只列当前已成立的产品能力，不替代证据索引。

## 快速开始

### 环境要求

- Windows 10/11 x64
- Visual Studio 2022，安装 **Desktop development with C++** 与 Windows SDK
- Git、CMake 3.28+、PowerShell 5.1+

首次配置会通过 CMake `FetchContent` 下载锁定版本的依赖。

### 构建并启动 Editor

```powershell
git clone https://github.com/Ubik42/Noemancer.git
cd Noemancer

# 在忽略的 _tools 目录安装锁定的 .NET SDK
./scripts/bootstrap-dotnet.ps1

./scripts/engine.ps1 configure
./scripts/engine.ps1 build -Config Release -Target noemancer
./scripts/engine.ps1 run -Config Release
```

构建完成后，也可以直接运行物理能力展示：

```powershell
./build/windows-msvc-debug/src/runtime/Release/noemancer.exe run --physics-showcase
```

日常使用可以直接双击 `Noemancer Editor.cmd`。`Open Platformer Editor.cmd` 和 `Play Platformer.cmd` 是验收工程快捷入口，不是通用引擎启动器。

### Headless 与结构化输出

```powershell
./scripts/engine.ps1 run -Config Release --headless --frames 3 --format json

./scripts/engine.ps1 run -Config Release tools list --format json
'{"frames":3}' | ./scripts/engine.ps1 run -Config Release tool call run.headless
```

### 测试

```powershell
# 聚焦检查
./scripts/engine.ps1 check -Config Release `
  -Target noemancer_simulation_runtime_tests `
  -TestRegex noemancer.simulation_runtime

# 里程碑完整门禁
./scripts/engine.ps1 test -Config Release
./scripts/engine.ps1 test -Config Release -WithMcp
```

## 架构

```mermaid
flowchart LR
    Project[Project / Scene / Assets / C# / UI] --> Import[Validate · Import · Cook]
    Import --> World[World and domain authorities]
    Editor[Editor UI] --> Commands[Command and transaction registry]
    CLI[CLI / MCP] --> Commands
    Commands --> World
    World --> Runtime[Physics · Animation · Scripting · Audio]
    World --> Extract[Render extraction]
    Extract --> Renderer[SDL_GPU · D3D12 / Vulkan]
    Runtime --> Evidence[Semantic state and receipts]
    Renderer --> Evidence
```

实际依赖方向为：

```text
noemancer_engine <- noemancer_editor <- noemancer runtime

tools/mcp   独立 TypeScript 进程，通过引擎 Command ABI 调用
managed/*   独立 .NET 合同与项目脚本运行时
```

第三方库只存在于引擎私有 Adapter 内。Scene、Prefab、C# 合同与公共 RPC 不保存第三方句柄或枚举。

### 仓库结构

```text
src/engine/     World、资产、物理、动画、UI 与命令权威
src/editor/     Editor 模型和界面
src/runtime/    SDL 平台、Renderer 与可执行入口
managed/        C# API 与 HostFXR Host
schemas/        版本化 Project 和语义合同
tools/mcp/      MCP Adapter
tests/          聚焦测试与集成测试
docs/           架构、ADR、开发计划与验收索引
```

文档入口：

- [当前架构](docs/architecture.md)
- [当前开发计划](docs/development-plan.zh-CN.md)
- [当前能力与验收边界](docs/first-acceptance-status.zh-CN.md)
- [文档权威关系](docs/README.md)

## 开发状态

商业 Raster 的动态天空、共享 HiZ/History、SSR、SSGI、GPU Scene 遮挡决策和阴影扩展决策均已形成双后端证据。Native D3D12/Vulkan 的光追闭环、统一执行 Receipt、跨帧逻辑资源/Pass 规划、持久 AS/SBT/Pipeline/输出 context、生产 SceneRenderer 的真实场景接入，以及 SDL 原生设备/非 cycling 纹理桥与 Engine-safe 互操作状态机已经成立；D3D12 的版本化逐像素 RayGen、同设备纯 GPU Copy 和可选 Render Graph v18 诊断合成已经形成真实隐藏画面，Vulkan storage-image ABI 已完成一 texel 原生写入。当前步骤是把固定诊断相机升级为项目相机/材质/光照的线性 radiance 合同，同时完成 Vulkan SDL image/synchronization interop。之后才进入 RTGI 独立 History 与时域降噪；现阶段 native trace 不能描述成生产 RTGI 画面。

仍未完成的产品边界包括：稳定 SDK/插件生态、可再分发 CJK/Arabic 字体、全编辑器 RTL 与 cluster-aware 编辑、签名安装器、独立机器矩阵、生产网络与跨平台支持。编辑器 Chrome 的高 DPI 响应式布局、窄窗 Project Hub、图标优先顶栏和随 DPI 重算的 Dock 尺寸已完成首轮收口。

机器可读的即时开发队列位于 [`docs/current-state.json`](docs/current-state.json)。

## 参与开发

架构与公共合同仍会快速变化。提交 Issue 或 PR 前请阅读 [CONTRIBUTING.md](CONTRIBUTING.md)。漏洞请按 [SECURITY.md](SECURITY.md) 说明提交，不要公开披露。

## 主要依赖

SDL、Flecs、Jolt Physics、ozz-animation、RmlUi、Dear ImGui、miniaudio、fastgltf、ufbx、meshoptimizer、KTX-Software、FreeType、HarfBuzz、ICU 与 nlohmann/json。

## 许可证

Noemancer Engine 与 Runtime 使用 [Apache License 2.0](LICENSE)。游戏工程、测试资产和第三方内容保留各自声明的许可证。
