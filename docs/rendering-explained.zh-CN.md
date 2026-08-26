# Noemancer 当前开发程度与渲染小白说明

> 性质：面向非渲染专业读者的当前实现解读  
> 更新：2026-08-25  
> 准确状态仍以 [`current-state.json`](current-state.json)、[`first-acceptance-status.zh-CN.md`](first-acceptance-status.zh-CN.md) 和真实代码/验收证据为准。本页负责解释，不替代能力合同。

## 先说结论：它现在到底算什么程度？

Noemancer 已经不是“开一个窗口、画几个三角形”的引擎玩具，也不只是把一堆模块名字摆在 README 里。它已经能够完成一条真正的游戏生产纵切：

```text
创建工程 → 编辑场景 → 导入资产 → 编写并热重载 C# 逻辑
         → Play 调试 → Cook 资产 → Package → 启动独立 Player
```

编辑器、运行时、资产 Cook、C# 脚本、物理、动画、音频、项目 UI、VFX、CLI/MCP 和独立游戏打包都有可运行的实现。平台跳跃与 Hybrid Pixel/HD2D 小型工程已经证明这些模块可以一起工作，而不是彼此孤立的测试桩。

但它仍然是 **pre-alpha 自研引擎**。更准确的定位是：

- **基础闭环已经成立**：可以开始拿它制作和验证小型真实游戏。
- **现代 Raster 渲染骨架已经较完整**：PBR、阴影、天空、AO、SSR、SSGI、时域处理和后期链路都已经进入真实 GPU Render Graph。
- **正在进入商业画质与大型场景强化期**：公开经典场景已经能实时跑，但大型资产 Cook、流送、复杂阴影、极端负载和画面调优还没有达到成熟商业引擎的宽度。
- **硬件光追刚跨过底层可行性门槛**：D3D12/Vulkan 都能真实构建 BLAS/TLAS，但还没有发射光线、形成光追画面，更没有 RTGI。
- **产品成熟度仍明显不足**：跨平台、安装器、插件生态、显存管理、各种硬件矩阵、稳定 SDK 和大量真实游戏生产检验仍待完成。

如果把引擎成熟度粗略分成五层：

1. 能开窗口、画几何体；
2. 有现代渲染和几个独立系统；
3. 能从 Editor 一路做到独立游戏包；
4. 能在大型真实项目中稳定提供商业画质、性能和工具体验；
5. 拥有成熟平台、生态、兼容性与长期生产验证；

Noemancer 目前已经越过第 3 层，正在啃第 4 层，离 Unity、Unreal、Godot 这类经过多年生产验证的第 5 层还有明显距离。这里的“越过第 3 层”指的是闭环成立，不是说所有模块都已经成熟。

## 整个引擎已经有哪些东西？

可以把 Noemancer 想成一座已经通水通电、可以住人的实验住宅：主体结构与管线已连通，但离大型商业楼盘的可靠性、装修细节和配套生态还有距离。

| 部分 | 说人话的当前状态 |
|---|---|
| Editor | 有正式启动入口、Project Hub、场景视图、层级树、Inspector、资源浏览器、动画图、Console 和 Agent Context；编辑态与运行态 World 分离，运行结果可以选择性写回并撤销。 |
| 项目与场景 | 可以创建/打开/保存工程和场景，编辑实体层级、组件、输入和项目 UI；关键写入有 revision、校验、事务和 undo/redo。 |
| C# 脚本 | 项目逻辑不用硬写进引擎 C++；支持编译、热重载、状态迁移、调试和类型化 World Command Buffer。API 仍会扩展。 |
| 资产 | 能导入 GLB、外部依赖形式的 JSON glTF、FBX、PNG/JPEG；几何用 meshoptimizer，纹理 Cook 为 KTX2，发行 Player 不再现场解析源模型。 |
| 物理 | Jolt 已提供 Box/Sphere/Capsule/Convex、刚体、传感器、Contact/Trigger、Ray/Sphere Sweep 和 2D Character Motor；高级车辆、布料、破坏与大规模压力尚未验证。 |
| 动画 | ozz 骨骼动画、GLB/FBX、Root Motion、Cross-fade、GPU Skinning、状态机、Blend 1D、Layer、骨骼 Mask 与动画图编辑已经打通；图能力仍比成熟商业引擎窄。 |
| UI | 项目/游戏 UI 使用 RmlUi 的 DOM/CSS/Flex；部分 Editor 面板也走声明式 Semantic UI → RmlUi，复杂编辑器壳仍使用 Dear ImGui。支持中文、整形、IME 与 RTL 基础，但高 DPI 和可再分发多语字体仍未完全收口。 |
| 音频 | 使用 miniaudio，不是从零造解码和设备轮子；已有资源管理、流式播放、Bus/Voice/Listener、空间化和独立音频线程。 |
| VFX/粒子 | 已有版本化 VFX Graph、GPU Spawn/Simulate/Group/Sort、Indirect Draw、Data Channel 和 Agent patch/rollback；当前 Billboard 是程序化无纹理形状，完整 Flipbook/贴图粒子仍缺。 |
| Agent | GUI、CLI、MCP 和测试尽量调用同一套 C++ 命令与权威状态。Agent 能看到稳定 ID、Schema、revision、Render Graph、计时和回执，而不只是猜窗口截图。 |

## 渲染先理解一个核心：一帧画面是流水线，不是一张图突然蹦出来

游戏每显示一帧，CPU 先从 World 中提取“相机在哪里、有哪些模型、灯光、粒子和 Sprite”，形成只面向渲染的 Render World。随后 GPU 像一条电影后期流水线一样逐步加工画面。

Noemancer 当前的 Forward Render Graph v17 有 **29 个有依赖关系的 Pass**。用人话压缩后，一帧大致这样走：

```text
World/Scene
   ↓ 提取当前帧可画数据
GPU 可见性判断 + 阴影深度
   ↓
动态天空 + Forward PBR 不透明物体
   ↓
深度金字塔 HiZ
   ↓
环境遮蔽 AO + 空气透视
   ↓
屏幕空间间接光 SSGI
   ↓
屏幕空间反射 SSR
   ↓
透明物体
   ↓
时域稳定 / TAA
   ↓
自动曝光 + 四级 Bloom
   ↓
ACES Tone Mapping
   ↓
显示器上的 sRGB 画面
```

Render Graph 的意义类似“有依赖关系的施工计划”：它明确哪一步读取什么、写入什么、必须等谁完成。这样可以发现“资源还没生产就读取”“两个 Pass 错误地同时覆盖一张纹理”等问题，也让 Agent、测试和性能工具能用稳定名称理解每一步，而不是面对一整坨黑盒 GPU 命令。

## 当前渲染功能逐项解释

### 1. D3D12 与 Vulkan 双后端

**它解决什么：** 同一套引擎渲染逻辑需要落到不同图形 API。D3D12 是 Windows/DirectX 路线，Vulkan 是更跨平台的显式图形 API。

**Noemancer 怎么做：** 当前主要通过 SDL_GPU 管理普通 Raster 资源与命令，在 Windows 上同时验证 D3D12 和 Vulkan。引擎自己的 Scene、材质、Render Graph 和 Agent Schema 不保存 D3D12/Vulkan 的私有 Handle。

**做到哪：** 两后端都能渲染同一套 RenderLab 场景，并有固定画面、Shader 指纹和逐 Pass GPU 时间证据。它证明的是当前 Windows + RTX 4080 上两条后端成立，不能外推为 Linux、AMD、Intel 或主机平台已经通过。

### 2. Forward PBR：让材质看起来“像真的材料”

**Forward** 可以理解为：每个可见物体在主绘制阶段直接结合材质、灯光和阴影算出颜色。它直观、适合透明物体和中等灯光规模，但在极多灯光下需要额外优化。

**PBR（基于物理的渲染）** 不等于物理模拟，而是一套尽量遵守能量与材质规律的光照模型。美术主要描述：

- Base Color：材料本来的颜色；
- Metallic：它更像金属还是非金属；
- Roughness：表面是镜面光滑还是粗糙发散；
- Normal：用贴图伪造细小凹凸，而不增加大量模型面数；
- Occlusion：小缝隙和遮蔽处通常有多暗；
- Emissive：材料自己发出的亮光。

Noemancer 使用 glTF metallic-roughness 工作流、GGX 高光，并加入粗糙度感知的多重散射补偿。后者可以粗略理解为：粗糙表面上的光会多次反弹，简单模型容易凭空“吃掉”能量；补偿让粗糙材质不会不自然地越来越黑。

**当前边界：** 这条材质主链已经真实运行，也能消费导入模型的 PBR 贴图。但它不是 UE 那种拥有庞大节点编辑器、Substrate/复杂分层材质、Nanite 专用材质约束和多年 Shader permutation 管理的完整材质生态。

### 3. IBL：即使没有灯泡，环境也会照亮物体

现实中物体不会只被一盏太阳直射，天空、墙壁和周围环境也会贡献光。IBL（Image-Based Lighting）用环境图近似这些来自四面八方的光。

Noemancer 当前使用 split-sum IBL：提前把环境光分成较慢变化的漫反射部分、不同粗糙度的高光环境反射，以及一张 BRDF 查找表。运行时不必让每个像素现场积分成百上千个方向，成本更可控。

可以把它想成“把复杂的环境照明预先烤成几本答案手册，运行时按材质粗糙度查答案”。当前支持外部 HDR 环境和程序化 HDR 环境，结果缓存为引擎工件。

### 4. 直接光与阴影：太阳、点灯和手电筒

当前支持：

- Directional Light：像太阳，所有光线大致平行；
- Point Light：像裸灯泡，向四周发光；
- Spot Light：像手电筒，有方向和锥角；
- Clustered Forward+：先把屏幕/视锥划成小区域，让像素只检查可能影响自己的局部灯，而不是遍历场景全部灯。

方向光阴影使用四级 CSM（Cascaded Shadow Maps）。它会把相机附近到远处切成四段：近处用更细的阴影图，远处用更粗的阴影图。就像地图软件给你当前位置加载高清瓦片、远方只给概览，从而把有限阴影分辨率花在玩家最能看清的地方。

点光/聚光使用局部阴影 Atlas，并带稳定缓存：没有变化的灯光和投影物尽量复用旧阴影，避免每帧重画。

**当前边界非常重要：** 这还不是 VSM。1,024 个投影物、6 个局部阴影请求的压力场景里，当前预算只选择 3 个、丢弃 3 个；团队当前决策是先扩展和优化 Atlas，再证明是否真的需要虚拟阴影页。VSM 目前只有数据合同原型，没有页表、物理页池、Shader 或真实 Render Graph Pass。

### 5. 动态天空与空气透视：不只是贴一张天空盒

天空为什么蓝、日落为什么红，是光在大气中被不同粒子散射的结果。直接让每个像素沿视线做大量 Raymarch 会比较贵，因此 Noemancer 的高档路径把大气问题拆成四张查找表：

1. Transmittance：光穿过大气还剩多少；
2. Multi-scattering：光多次散射后的补充；
3. Sky View：从当前观察条件看到的天空颜色；
4. Camera Volume：相机附近不同深度的空气颜色与透过率。

物体画完后，Aerial Perspective 用近似公式：

```text
最后颜色 = 原场景颜色 × 空气透过率 + 空气本身散射进来的光
```

于是远山会被空气“洗淡”，夕阳下远处偏暖，空间不再像模型贴在真空里。太阳方向和天气参数可变化，稳定时不会无脑每帧重建所有 LUT；低档设备还有固定成本的 analytic fallback。

**澄清：** 当前可以称“动态物理大气/四 LUT 天空”，不能笼统声称已经实现了某套完整的全屏 Raymarching 天空方案。它的核心恰恰是用预计算 LUT 降低逐像素长距离步进成本。

### 6. HiZ 深度金字塔：GPU 的“远近速查表”

普通深度图记录每个像素离相机多远。HiZ 会把它逐级缩小成一座金字塔：顶层保留细节，越往上越粗略。Noemancer 用 RG32F 同时保存每块区域的最近/最远线性深度。

这样很多算法不用逐像素慢慢找：先看粗层判断“这一大片是否可能相交”，只有可能时再下钻到更细层。当前 SSR、SSGI 和保守 GPU 遮挡剔除共用同一座 HiZ，而不是每个效果各造一份。

1920×1080 时该金字塔固定 12 个 mip，约 21.095 MiB。已有双后端真实 GPU 计时，建造成本处于十分之一毫秒量级以内；这只是当前固定机器/场景证据，不是所有设备保证。

### 7. AO / GTAO：让接触处和墙角不再像悬浮

AO（Ambient Occlusion）估计环境光有多少被附近几何挡住。例如杯子压在桌面上，接触处应该略暗；两面墙夹角也比空旷平面更暗。

Noemancer 当前以八方向 horizon 搜索做 GTAO 风格估计，再进行横向、纵向两遍 bilateral 去噪。Bilateral 的意思是：在平滑噪点时参考深度和法线，不让阴影随便糊过物体边缘。

它只削弱 IBL 的间接漫反射/高光，不会粗暴地把太阳直射、自发光一起乘黑。这一点决定了画面是自然加深接触，还是出现老游戏式的“黑脏边”。

**边界：** 当前是自有的八方向 horizon AO，不是完整移植的 XeGTAO；名字和质量上不能冒充后者。

### 8. SSGI：用屏幕里已经看见的东西补一点间接光

GI（Global Illumination）指光照到红墙后，红色能量再反弹到旁边白地板这种间接照明。完整 GI 很贵。

SSGI（Screen-Space Global Illumination）走的是聪明但有局限的捷径：它只利用当前屏幕已有的颜色、深度和法线，从每个像素附近向半球方向发少量“屏幕空间探针”，借 HiZ 加速查找可能命中的表面，再把这些表面颜色作为局部反弹光。

当前高档预算最多 8 个方向 × 8 步，半径约 4 个世界单位；之后经过：

1. Gather：寻找可能的间接光；
2. Spatial Resolve：借邻居减噪，同时尊重深度/法线边界；
3. Temporal Resolve：借上一帧积累更多样本；
4. Composite：只替换/补充 IBL 的 diffuse 部分，不把整张画面重复加亮。

它还能输出 bent normal 和 visibility，帮助描述“哪个方向更开阔”。

**天然缺陷：** 屏幕外、相机背后、被前景完全挡住的东西它不知道；镜头一转，信息会出现或消失。因此它是便宜的局部补光，不等于 Lumen、探针 GI 或 RTGI。现有 A/B 证明确实改变了目标区域，但效果量是克制的，不是夸张的“整屋弹射光”。

### 9. SSR：用屏幕内容制造实时反射

SSR（Screen-Space Reflections）从当前像素沿反射方向在屏幕深度里找交点。Noemancer 先在 HiZ 粗层快速前进，发现可能穿过表面时再二分细化；粗糙度过高、方向不合理或自相交会被拒绝。

命中结果经过独立的时间历史稳定，再只替换材质原本的 IBL 镜面反射部分。没有命中时保留 IBL，不让材质突然变黑。

**它擅长：** 湿地面、光滑桌面、画面中可见物体的反射。

**它做不到：** 反射屏幕外物体、被挡住但本应出现在镜子里的物体，以及稳定的完美镜子。镜头边缘和遮挡变化仍是 SSR 的天生难点。真正解决这些问题需要反射探针、Planar Reflection 或硬件光追等其它路径。

### 10. Temporal History、Temporal Denoising 与 TAA：向上一帧借信息

一帧内给每个像素很多采样很贵，所以现代实时渲染大量使用“时间”：相机每帧做极小的亚像素偏移，把当前帧与上一帧历史按运动向量对齐，逐渐积累更稳定的结果。

Noemancer 为 TAA、SSR、SSGI 和未来 RTGI 保留彼此独立的 History 身份与 revision。共享 Temporal Resolve 会参考：

- Motion Vector：这个像素从上一帧移动到了哪里；
- Depth/Normal：是不是已经换成另一块表面；
- Reactive Mask：这里是否变化太剧烈，不应相信旧画面；
- Disocclusion：之前被挡住、现在突然露出的区域；
- Neighborhood Clamp：把历史颜色限制在当前邻域的合理范围，减少拖影鬼影。

当相机切换、分辨率改变或内容版本变化时，历史必须明确重置，不能拿错误的旧画面继续混。

**边界：** 这已经是实际运行的时域基础，但尚不能等同于 UE TSR、DLSS 或 FSR 这类成熟超分辨率系统。Hybrid Pixel 模式为了像素稳定，明确关闭 jitter、SSR、SSGI 和这些时间历史。

### 11. Bloom、曝光、调色和 ACES：把线性 HDR 变成“能看的成片”

PBR 内部会产生比显示器白色亮很多的 HDR 值。直接截断会让天空、高光和灯牌变成死白色块，所以最后还需要一条摄影式后期链路。

- Auto Exposure：根据画面亮度缓慢调节曝光，类似相机/眼睛适应明暗；
- Bloom：把超过阈值的亮部逐级缩小、扩散再叠回，形成镜头/视觉上的光晕；
- Color Grading：在 scene-linear 空间做 lift/gamma/gain、饱和度、对比度、色温和 tint；
- ACES Tone Mapping：把巨大 HDR 亮度范围压进普通显示器，同时尽量保留高光层次和颜色观感；
- 显式 sRGB 输出：最终才把线性颜色编码为显示器习惯的非线性数值。

当前 Bloom 是 half、quarter、eighth、sixteenth 四级 dual-filter；Tone Mapping 使用 ACES 风格的矩阵与 fitted RRT/ODT，再限制到 Rec.709。它已经比“最后简单做一个 gamma”完整得多，但不等于具备完整 HDR10、Dolby Vision、专业 LUT 工作流和所有显示设备校准。

### 12. GPU-driven 可见性与间接绘制：别让 CPU 一件件喊 GPU 画

传统方式是 CPU 遍历全部物体，然后一个 Draw Call 一个 Draw Call 地告诉 GPU。对象多时，CPU 提交会成为瓶颈。

Noemancer 对满足条件的静态、不透明、未蒙皮物体建立 GPU Scene Buffer。Compute Shader 先做视锥裁剪，把可见实例压紧到 compact index stream，再由 indexed indirect draw 一次提交稳定批次。对象没变化时复用批次，只上传真正变脏的数据范围。

上一帧 HiZ 还可以保守判断物体是否被大墙完全挡住。若相机快速变化、正交相机、蒙皮物体、动态物体或缓冲溢出，则回退到可靠的直接路径，而不是错误消失。

**当前真实问题：** 这条技术路径已成立，但压力场景的整体 CPU Frame p95 仍远未达到 60 Hz。也就是说“GPU 会裁剪”不等于整个引擎已经高性能；场景提取、Editor、资源组织、同步和其它 CPU 工作仍需要继续优化。

### 13. VFX、Sprite、Tilemap 与 Hybrid Pixel / HD2D

3D Raster 与 HD2D 不是两套完全断开的引擎：Lit Sprite、Tile Cell 和 3D Mesh 可以共享方向光、Point/Spot Light、阴影、AO 和资源表。

Hybrid Pixel Profile 提供固定虚拟分辨率、整数倍放大、Letterbox、相机/Sprite/Tile/VFX 像素对齐以及 nearest presentation。它主动牺牲普通 3D 的时域抗锯齿和屏幕空间效果，换取像素画稳定、不抖、不被后期糊掉。

GPU 粒子已经能在显卡上完成 Spawn、Simulate、Group、Alpha Sort 和双 Indirect Draw，也能在 Hybrid 网格上量化位置和尺寸。当前缺口是贴图、Flipbook 和更低成本的大规模排序；因此目前可以验证粒子系统架构和程序化效果，不能宣称已有 Niagara 级内容制作能力。

## 已经能看到什么真实效果？

### Intel Sponza 2022 宫殿

![Noemancer 渲染的 Intel Sponza 2022 宫殿中庭](media/renderlab-sponza-atrium.webp)

这个场景不是几颗测试球，而是约 205 万顶点、1124 万索引、405 个 primitive、72 张已解码材质图的经典建筑压力素材。Noemancer 已能通过正式外部 Project、JSON glTF 依赖闭包，在 D3D12 和 Vulkan 下实时画出它。

它证明了“大量真实几何 + 大量 PBR 材质 + 双后端主渲染”成立；但当前展示使用保留完整几何、把材质派生为最高 1K 的外部版本。原始大包超过引擎 1 GiB 不可变源快照预算，Sponza 的大型场景 Cook/Package/Player 闭环还没有完成。因此这张图不能被解释为“开放世界资产流送已经解决”。

### 动态天空与屏幕空间效果

| 动态天空与 Aerial Perspective | SSR 与 SSGI |
|---|---|
| ![动态天空与空气透视](media/renderlab-sky-atmosphere.webp) | ![SSR 与 SSGI](media/renderlab-ssr-ssgi.webp) |

这些是 Release Runtime 的隐藏实时捕获，不是 Blender 离线渲染或 AI 概念图。经典较小场景已经走通 Project → Cook → Package → Player；Sponza 当前只证明 Source Project 实时路径。

## 性能现在到底怎么样？

不能用一个 FPS 概括整个引擎，因为空场景、三颗球、Sponza、1,024 个阴影物体和带 Editor 的整窗负载完全不同。

已经有价值的事实是：

- D3D12/Vulkan 都能按稳定 Pass ID 返回真实 GPU Timestamp，不再拿 CPU 录命令时间冒充 GPU 时间；
- 1920×1080 固定场景中，HiZ Seed/Reduce 与共享 Temporal Resolve 已取得稳定的亚毫秒级证据；
- SSR 在固定场景里的 Trace 大约是 0.18–0.19 ms，Composite 大约 0.03 ms；Vulkan 的 SSR Temporal 在该次证据中明显比 D3D12 慢，说明双后端对照确实能暴露问题；
- SSGI 是当前更贵的屏幕空间效果：高档 Gather 在该固定场景约 1.87 ms（D3D12）/2.51 ms（Vulkan），另外还有空间和时间 Resolve；
- 小型 Classic Package Player 曾取得约 1.4–1.6 ms 的 CPU Frame p95，但这不能代表 Sponza 或复杂游戏；
- 1,024 阴影投影物和 GPU 遮挡压力场景的 CPU Frame p95 仍可高达 40–150 ms 量级，说明大型负载的 CPU 组织与整体管线距离 60 FPS 目标还有真实缺口。

因此最诚实的评价是：**Noemancer 已经建立了测性能和定位 Pass 的基础，也有一些成本合理的现代效果；但还没有证明在大型复杂游戏里具备极致性能。** 目前比“看起来能跑”更进一步，但还没有资格说“已全面性能最优”。

## 目前明确没有完成什么？

### 没有完成硬件光追画面与 RTGI

硬件光追通常先把三角形组织成便于查询的空间结构：

- BLAS：一组模型三角形的加速结构；
- TLAS：场景里各个模型实例的上层加速结构。

Noemancer 已经在 RTX 4080 上让 D3D12 DXR 1.1 和 Vulkan RT 真实创建三角形 Vertex Buffer、BLAS、单实例 TLAS，并完成 Barrier、提交、Fence 等待和释放。这相当于已经修好“高速公路和索引仓库”。

但是还缺：

- 长寿命 Native RHI 设备/资源所有权；
- SBT（Shader Binding Table，告诉不同射线命中后执行哪个 Shader）；
- Trace Dispatch（真正发射射线）；
- Render Graph 光追 Pass；
- 结果纹理与 Raster 画面的合成；
- RTGI 的采样、降噪、History、降级和双后端性能证据。

所以当前 **没有可见的 Ray Tracing 画面，没有 RTGI**。BLAS/TLAS Build Probe 只是证明底层 API、资源和同步方向可行。

### 没有完成 VSM / Virtual Shadow Maps

当前生产路径仍是四级 CSM + Local Shadow Atlas。Virtual Page 只有 plain-data 实验合同，没有真正的页表、物理页池、缺页调度和阴影 Shader。

### FXAA 不是默认启用能力

仓库有 FXAA Shader、Pipeline 和兼容录制分支，但默认 Render Graph 使用 TAA/Temporal Resolve，并没有调度 FXAA Pass。Hybrid Pixel 则使用像素稳定的空间输出。因此可以说“有 FXAA 兼容分支”，不能说“默认画面已经启用 FXAA”。

### 还没有 Bindless

当前 Texture Resource Table 提供稳定资源身份和 slot，但这不是硬件 Bindless Descriptor Heap。大量材质/纹理的极端场景还不能拿“有资源表”冒充“已完成 Bindless”。

### 还不是大型世界渲染系统

尚未完成通用的场景分块 Cook、按需几何流送、真正物理显存驻留遥测、开放世界级 LOD/HLOD、跨平台大场景预算和成熟遮挡策略。Sponza 原始资产超预算正好暴露了这个缺口。

## 和商业引擎相比，最接近与差得最远的地方

最接近商业引擎“现代 Raster 基础”的部分是：

- 完整的线性 HDR → 曝光/调色/Bloom → Tone Mapping 输出链；
- PBR + IBL + 多类型灯光 + 方向/局部阴影；
- 共享 HiZ、AO、SSR、SSGI 和时域历史；
- GPU Scene、间接绘制、稳定批次和有限遮挡裁剪；
- 双图形后端、真实 Shader 工件、Render Graph 与逐 Pass 计时；
- 资产从源文件 Cook 为 Player 专用格式，而不是把 Editor 导入器塞进游戏包。

差得最远的部分是：

- 数年迭代的材质、灯光、阴影和后期内容工具；
- 大型世界、显存、LOD/HLOD、Shader permutation 和跨硬件性能工程；
- RTGI、成熟反射体系、VSM、上采样与高质量运动稳定性；
- 复杂透明、头发、皮肤、水体、体积云雾、地形和大量专用渲染功能；
- 在许多真实游戏、GPU、驱动和平台上的长期稳定性。

换句话说：今天可以用 Noemancer 做出“明显具备现代实时渲染结构”的画面，也能开始用真实游戏找问题；但只凭现有 Sponza 截图，还不能说画质和性能已经追平 UE5。

## AI Native 在渲染上的实际价值

AI Native 不是让模型读一遍 Shader 源码就结束。开源只能让 AI 理论上看懂实现，不代表它能低成本知道运行中的这一帧发生了什么。

Noemancer 额外做的是把运行事实也变成稳定、有限、可查询的数据：

- Render Graph 有稳定 Pass/Resource ID、依赖、格式与错误；
- Renderer Status 能说明当前后端、画质档、History 状态、Fallback、纹理缺失和各类计数；
- GPU Pass Timestamp 能告诉 Agent 时间花在哪一步；
- SSR/SSGI/阴影/天空提供 Debug View 或结构化状态；
- Project、Scene、材质和运行对象有稳定身份、revision 与来源；
- 修改走 Observe → Plan → Apply → Receipt，而不是 AI 盲改文件后祈祷。

这相当于把“医生只能读人体解剖书”提升为“医生还能读取当前病人的化验单、影像标记和治疗回执”。源码告诉 AI 引擎为什么这样工作，Semantic State Plane 和结构化证据告诉 AI **这一帧实际上怎样工作、哪里慢、哪项能力正在降级**。

当前这套可观察性已经覆盖不少渲染事实，但还需要跟随 RTGI、流送、显存和更复杂编辑器工具继续同步扩展，不能做成另一个脱离真实 Runtime 的报告数据库。

## 接下来渲染主线会做什么？

按当前权威队列，优先顺序是：

1. 把 D3D12/Vulkan 的光追执行事实接到统一 Receipt，而不是两个后端各说各话；
2. 建立可持续跨帧使用的 Native RHI 资源所有权；
3. 完成最小 SBT + Trace Dispatch + Readback，第一次得到真实光追像素；
4. 把 Ray Tracing 作为真实 Render Graph 节点，并保留 Raster fallback；
5. 在此基础上开发 RTGI 的采样、时空降噪、History reset、质量档与性能预算；
6. 同时用 Sponza、后续 Bistro/San Miguel 等大型场景继续推动分块 Cook、纹理/几何流送和 CPU 帧优化；
7. 阴影先优化现有 Atlas 和 CPU 总帧，再凭压力证据决定何时进入真正 VSM。

这条顺序看起来比“先写一个 RTGI Shader”慢，但它保证光追不会成为只能在一台机器、一个测试函数里运行的孤岛。

## 一句话记住每个术语

| 术语 | 最短的人话解释 |
|---|---|
| PBR | 用金属度、粗糙度等参数，让材质对光的反应更符合现实。 |
| IBL | 用周围环境来照亮和反射物体。 |
| CSM | 把太阳阴影按远近切成多张不同精度的图。 |
| AO/GTAO | 给接触处和凹角补上环境遮蔽感。 |
| HiZ | 一座从精细到粗略的深度速查金字塔。 |
| SSR | 只用屏幕里看得见的内容估计反射。 |
| SSGI | 只用屏幕里看得见的内容估计局部反弹光。 |
| Temporal | 借上一帧的信息换取更稳定、更便宜的结果。 |
| Bloom | 让极亮区域产生柔和光晕。 |
| Tone Mapping | 把 HDR 世界压进普通显示器还能保住层次。 |
| Render Graph | 明确每个 GPU 工序读取、写入与依赖什么。 |
| GPU-driven | 让 GPU 自己筛选并批量决定画哪些实例，减少 CPU 提交。 |
| BLAS/TLAS | 给硬件光追建立三角形与场景实例的空间索引。 |
| RTGI | 真正沿射线寻找表面并估计多次间接光；当前尚未完成。 |
| VSM | 把阴影图虚拟分页，只为真正需要的区域分配物理页；当前尚未完成。 |

## 最终判断

Noemancer 当前最值得肯定的不是“功能名很多”，而是现代 Raster 的关键功能已经进入同一条真实管线，并开始用公开场景、双后端、Package Player、固定机位 A/B 与 GPU Pass 时间证明它们。

当前最需要警惕的也很明确：**有现代结构，不等于已有商业引擎的成熟度；能渲染 Sponza，不等于已经解决大型世界；能 Build BLAS/TLAS，不等于已经有光追；有 GPU-driven，不等于所有压力场景已经达到 60 FPS。**

因此合理的阶段结论是：基础游戏引擎闭环已经形成，Raster 渲染进入商业化强化阶段，硬件光追处于底层奠基阶段，大型场景、极致性能和生产稳定性将决定它能否真正从“很完整的自研引擎”走向“有竞争力的生产引擎”。
