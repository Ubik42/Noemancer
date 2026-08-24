# 公开测试素材库与 Noemancer 素材策略

> 文档类别：Historical resource catalog。仅在当前资产任务需要时按许可重新核验并取用。

> 调研日期：2026-08-18

## 参考仓并不缺素材

本地三个参考引擎仓库的可视与音频素材粗略统计如下。这里只统计常见图片、纹理、模型和音频扩展名，不包括场景、Shader 和引擎自有格式。

| 参考仓 | 文件数 | 约占空间 | 主要内容 |
|---|---:|---:|---|
| Bevy `9f4ff89` | 362 | 48.5 MB | 241 PNG、28 KTX2、28 glTF/GLB、HDR、UI、Sprite、动画和音频 |
| Godot `3000096` | 1,229 | 1.35 MB | 主要是编辑器 SVG；测试目录有多图片格式、Suzanne 与少量 glTF |
| WickedEngine `f4a0d26` | 182 | 296.7 MB | Sponza、Dragon、Bunny、Damaged Helmet、terrain/PBR 贴图、脚本示例 |

前一版 Noemancer 只取了 Bevy 中三个很小的 Kenney GLB，是为了先保证许可清楚和仓库轻量，不代表参考仓只有这些。WickedEngine 的素材体积最大，但不同模型和贴图的来源许可需要逐项核对，不能因为引擎代码是 MIT 就自动假定全部内容都可再次分发。

## 推荐素材库

### 第一优先：可直接建立自动化下载与缓存

1. [Kenney](https://kenney.nl/assets)：大量 2D Sprite、Tileset、UI、图标、字体和少量 3D；官方说明各素材页资源为 CC0。适合作为 2D、HD2D、UI、Atlas、九宫格和输入图标的基础回归库。
2. [Poly Haven](https://polyhaven.com/)：全部资产 CC0，提供高质量 PBR 贴图、HDRI 和模型，并有公开 API。适合材质导入、色彩空间、IBL、纹理压缩、Mip 和流送测试。
3. [ambientCG](https://ambientcg.com/)：以 CC0 PBR 材质为主，常见资产同时提供 Color、Normal、Roughness、Metalness、Displacement 等贴图。适合 Material Schema、通道约定和 Cook 测试。
4. [Khronos glTF Sample Assets](https://github.com/KhronosGroup/glTF-Sample-Assets)：不是单一许可证，但每个模型有清楚的元数据和用途；覆盖动画、Morph、材质、压缩与边界情况，是 glTF 兼容测试的核心来源。

### 第二优先：适合人工挑选，不能只信站点标签

1. [OpenGameArt](https://opengameart.org/)：2D、像素、Tileset、角色和音效丰富，但同时存在 CC0、CC BY、CC BY-SA 与 GPL 等多种许可。每个素材必须保存原始页面、作者、许可证和归属文本。
2. [itch.io 的 2D CC0 筛选](https://itch.io/game-assets/assets-cc0/free/tag-2d)：数量很大，适合寻找模块化角色、序列帧、像素 Tileset 和 VFX。上传者标签可能出错，进入仓库前仍需读取具体素材页和压缩包内许可证。

## 当前素材策略

| 集合 | 许可 | 覆盖 |
|---|---|---|
| Kenney Alien/Cake/Tile | CC0 | 静态 glTF、多个简单 Mesh |
| 用户提供的三个 Mixamo FBX | 本地使用，原文件不再分发 | FBX、Humanoid、Skinning、Root Motion、动画导入 |

现阶段不把公开素材包复制进仓库。除原有三个很小的 Kenney GLB 外，公开 2D、贴图、材质和动画资源只保留上面的来源链接；等对应 Importer/Cooker 里程碑开始时，再按测试矩阵下载到内容寻址缓存。三个用户提供的 FBX 保留在被 Git 忽略的本机测试区，不进入当前构建与 CI。

## 后续下载批次

仓库不应该随机堆满素材。每个批次围绕一个失败模式，控制尺寸，并保留 source/license/hash：

1. 2D：一个 Tile Atlas、一个多方向角色序列帧、一个模块化角色、一个 UI 九宫格、一个粒子 Flipbook。
2. 贴图：同一材质的 sRGB Base Color、线性 Normal/Roughness/Metalness、16-bit Height、带 Alpha 贴图和 NPOT 贴图。
3. 压缩：PNG/JPEG、BC/ASTC/ETC KTX2、Basis Universal，以及错误或截断文件。
4. 动画：无 Skin Clip、多 Clip Skin、Root Motion、不同骨架重定向、非均匀缩放和超长骨骼名。
5. 渲染：HDRI、PBR 标准球、透明/双面材质、法线方向、UV seam 和极端 Mip 场景。

素材元数据最终进入 Asset Registry，而不是继续硬编码在编辑器模型中。公开 CI 只依赖仓库内小型可再分发素材；大型 CC0 包进入内容寻址缓存；来源许可不清楚的文件只作为本机可选测试。
