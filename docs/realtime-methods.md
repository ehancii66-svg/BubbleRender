# BubbleRender 实时性技术总结

本项目是一个透明肥皂泡实时渲染系统，目标平台为 HarmonyOS / OpenGL ES 3.0 移动端。
"实时"意味着在移动 GPU 上以 30-60 FPS 同时完成折射、薄膜干涉彩虹色、多泡泡仿真与融合。
本文档梳理整个流程中针对实时性做出的具体设计，以及它们与论文方法的关系。

## 一、总览：为什么肥皂泡渲染天然困难

肥皂泡的完整物理模型涉及三维多相 Navier-Stokes 方程、微米级液膜厚度场、以及薄膜干涉光谱计算。
直接求解在移动端完全不可行。我们的核心策略是 **分层降维**：

```
传统离线方法                         本项目实时方法
─────────────                      ─────────────
三维多相 NS 求解器        →        纯表面拉格朗日仿真（无体积网格）
微米级膜厚 PDE           →        shader 程序化噪声 + 世界空间坐标
光谱积分（64 波长）        →        LUT 预计算 + 3 波长解析近似
体积折射光线追踪          →        屏幕空间 FBO 折射近似
完整 Voronoi 泡沫拓扑     →        接触图 + 空间哈希 broad phase
```

每层降维都对应一个具体的实时性优化点。下面逐层展开。

## 二、渲染管线：屏幕空间折射（Wyman 2005 + Kim & Park 2012）

### 2.1 多 Pass FBO 架构

渲染管线分为 5 个 Pass，每帧执行：

```
Pass 1: 背景渲染到 backgroundFBO（天空盒 + 背景物体）
Pass 2: 主泡后方场景渲染到 sceneBehindFBO（天空盒 + 显示泡泡 + 融合面 + 接触桥）
Pass 3: 主泡背面渲染到 backFaceFBO（剔除前面，只画背面）
Pass 4: 主泡正面渲染到 mainSceneFBO（含背面纹理）
Pass 5: 最终合成到屏幕
```

**实时性设计：**

- **屏幕空间折射而非光线追踪。** shader 不对每个片元发射折射光线穿过三维场景，而是
  根据法线和 Snell 定律计算折射方向，把折射方向转换为屏幕空间像素偏移，直接采样
  背景纹理。这是一个 O(1) 的纹理查找，而非 O(光线步进) 的体积积分。

- **FBO 尺寸与屏幕 1:1（kFBOOverscan = 1.0）。** 折射偏移不会超出 FBO 边界，
  避免了 overscan 带来的额外像素填充开销。如果后续增大折射强度，可以适当提高
  overscan，但当前 1:1 是最优的填充率选择。

- **背面 Pass 单独渲染。** 主泡正面折射需要看到泡内背面，但不会对所有泡泡都做
  背面 Pass——只有主泡做，副泡和融合面只做正面 + 背景采样。这把最贵的双界面折射
  限制在一个物体上。

### 2.2 折射偏移计算

片元着色器中的折射核心（shader_refraction.h）：

```glsl
vec3 refractVec = mat3(vView) * refract(eye, normal, iorRatio);
float localSpherePixelRadius = uSpherePixelRadius * max(vOpticalRadiusScale, 0.01);
vec2 offsetPixels = refractVec.xy
    * uRefractionStrength
    * surfaceRefractionScale
    * thinFilmRefraction
    * localSpherePixelRadius
    * edgeBoost;
offsetPixels = clamp(offsetPixels, vec2(-maxOffset), vec2(maxOffset));
vec2 samplePixel = clamp(fboPixel + offsetPixels, vec2(0.0), uFBOSize - vec2(1.0));
vec3 bgColor = texture(uBackgroundTexture, samplePixel / uFBOSize).rgb;
```

**实时性设计：**

- **uSpherePixelRadius 一次性计算。** 在 C++ 端根据相机距离和 FOV 算出球体在
  屏幕上的像素半径，作为 uniform 传入。shader 不需要每片元重新计算，只需做一次
  乘法。

- **逐顶点 opticalRadiusScale。** 融合面上每个顶点携带一个折射缩放因子
  （location 6 顶点属性），shader 用 uSpherePixelRadius * vOpticalRadiusScale
  得到该位置的局部折射幅度。这避免了融合面用统一 uniform 导致的折射跳变，
  同时不需要在 shader 里重新计算局部半径。

- **refract() 是 GPU 内置函数。** GLSL 的 refract() 在硬件上是单条指令，
  比 Snell 定律手动展开更快。

- **偏移 clamp 防止越界采样。** clamp(offsetPixels, ±maxOffset) 确保不会
  采样到 FBO 外部，避免了 border color 设置或边界检查分支。

### 2.3 背面折射降权

```glsl
float surfaceRefractionScale = (uIsBackFace == 1) ? 0.16 : 1.0;
float surfaceColorScale = (uIsBackFace == 1) ? 0.18 : 1.0;
```

背面片元的折射偏移和颜色贡献都降到约 16%。这既符合物理（背面折射经过两倍膜厚，
偏移应更小），又减少了背面片元对最终颜色的干扰——背面 Pass 本身就是为了给正面
提供"泡内可见内容"，不需要它产生强折射。

## 三、薄膜干涉彩虹色：三种模式（Kim 2012 / LUT / Belcour Airy）

### 3.1 LUT 预计算（Iwasaki 2004 + 离线光谱积分）

tools/generate_thinfilm_lut.py 在离线阶段预计算一张 256x256 的查找表：

- 横轴：NdotV（0 ~ 0.99），即视线与法线的夹角余弦
- 纵轴：膜厚（100nm ~ 900nm）归一化
- 每个像素：64 个波长（380nm ~ 780nm）的薄膜干涉反射率，经 CIE-XYZ 到 sRGB
  转换后的 RGB 颜色

**实时性设计：** 光谱积分（64 个波长 x Fresnel x 干涉相位 x XYZ 匹配函数）
在离线完成，运行时只需一次 texture() 采样。这把 O(64) 的逐波长计算
降为 O(1) 的纹理查找。

### 3.2 解析近似（Kim 2012 三波长）

```glsl
vec3 kim2012Iridescence(float NdotV, float thickness) {
    float r = thinFilmReflectance(NdotV, thickness, 615.0);
    float g = thinFilmReflectance(NdotV, thickness, 535.0);
    float b = thinFilmReflectance(NdotV, thickness, 465.0);
    return vec3(r, g, b);
}
```

只用 R/G/B 三个代表波长，每个波长做一次 Fresnel + 干涉相位计算。
比 LUT 模式多了几条算术指令，但不需要纹理采样，在带宽受限的移动端有优势。

### 3.3 Belcour Airy 多阶反射

对 R/G/B 各做一次 Airy 级数求和（含所有内部反射阶），再加一个 7 波长加权版本。
虽然比三波长版本多几条复数运算，但仍然是解析公式，无纹理依赖。

**三种模式可在运行时切换**（`;` 键 / 触摸），让用户根据设备性能选择。

### 3.4 程序化膜厚场（替代 PDE 膜厚演化）

Ishida 2020 用二维 PDE 在网格上求解膜厚演化。我们用 shader 内的 3D Simplex Noise
程序化生成膜厚：

```glsl
vec3 filmCoordinate = vFilmCoordinate + uFilmNoiseOffset;
float broadNoise = filmNoise(filmCoordinate * 2.0 + warp + slowFlow);
float flowNoise = filmNoise(filmCoordinate * 3.4 + ...);
float drainage = smoothstep(-0.85, 0.85, -normalize(filmCoordinate).y);
float dynamicThickness = uThickness * uFilmBaseThicknessScale
    + thicknessPattern * uThicknessVar * uFilmThicknessAmplitudeScale;
```

**实时性设计：**

- **不存储膜厚纹理。** 膜厚在 shader 里实时计算，不占用纹理内存或带宽。
- **世界空间坐标采样。** vFilmCoordinate 是世界空间坐标（经 textureBasis 变换），
  几何切换时纹路不跳。不需要额外存储或传输膜厚场数据。
- **重力排液近似。** drainage 项用世界 Y 方向的 smoothstep 近似重力排液，
  不需要求解排液方程。
- **4 倍频 FBM。** filmNoise 是 4 次 simplex noise 叠加（振幅递减），
  每次调用约 50 条指令，4 次约 200 条——在移动 GPU 上可接受。

## 四、泡泡仿真：控制点表面 + 接触图

### 4.1 26 个表面控制点（替代完整网格仿真）

每个泡泡不维护完整三角网格做物理仿真，而是用 26 个表面控制点
（[-1,0,1]^3 方向）驱动形变：

```
26 个控制点 → 每帧弹簧-阻尼更新 → 插值到渲染网格顶点
```

**实时性设计：**

- 26 个控制点的弹簧更新是 O(26) per bubble，远低于完整网格的 O(N_vertices)。
- 渲染网格顶点通过 weight = max(dot(dir, controlDir), 0)^8 加权插值，
  在 GPU 端逐顶点完成，不需要 CPU 回传。
- 控制点携带位移和速度，具有惯性和记忆，不需要每帧重新计算。

### 4.2 空间哈希 broad phase

BuildBubbleBroadPhasePairs 用三维均匀空间哈希：

```
cellSize = maxRadius * 2.40
每个泡泡放入对应格子
只检查当前格 + 相邻 26 格
```

**实时性设计：** 避免 O(N^2) 全配对检查。泡泡数量从默认 3 增到最多 16 时，
broad phase 仍然是 O(N)（每个泡泡只检查常数个邻居格子）。

### 4.3 接触图代替 Voronoi 泡沫

不构建完整的 weighted Voronoi diagram（Busaryev 2012），而是用接触图
（BubbleContactPair 列表）管理泡泡间关系：

- 每条 pair 独立维护接触时间、接触圆、压缩量、共享膜参数
- 多泡同时接触通过 6 次迭代的位置约束收敛
- 接触激活强度 contactActivation 从近场连续增长，统一驱动所有视觉效果

**实时性设计：** Voronoi 构建是 O(N log N)，接触图维护是 O(E)（E 是接触边数，
通常远小于 N^2）。位置约束迭代次数固定为 6，不随泡泡数量增长。

### 4.4 DBSTT 涡流片仿真：仅在破裂时激活

Da et al. 2015 的涡流片仿真（VortexSheetSimulation）为每个泡泡预创建，
但 **只在破裂时激活**：

```
正常状态：控制点表面驱动形变，DBSTT 休眠
破裂触发：激活 DBSTT，增强表面张力驱动膜回缩
```

**实时性设计：**

- DBSTT 的 Biot-Savart 积分是 O(N^2)（N = 顶点数），对 subdiv=2 的 icosphere
  约几百个顶点。只在破裂的 0.35 秒内运行，不影响正常帧率。
- 正常渲染完全不走 DBSTT，避免每帧 O(N^2) 开销。
- 仿真在单位半径上进行，模型矩阵负责缩放——仿真与气泡半径变化解耦。

### 4.5 仿真节流

```cpp
static constexpr int kSimFrameInterval = 6;
if (!g_SimPaused && ((g_SimFrameCounter++ % kSimFrameInterval) == 0))
    g_Sim.update(1.0f / 60.0f);
```

DBSTT 仿真每 6 帧更新一次（约 10 Hz），而非每帧。渲染保持 60 FPS，
仿真以更低频率运行，视觉上通过控制点插值平滑过渡。

## 五、融合仿真：有状态轴对称表面（fusion_profile）

### 5.1 拉格朗日材料节点

融合不使用 morph（在双球轮廓和目标球之间插值），而是维护 97 个拉格朗日
材料节点，通过表面张力 + 体积约束的半隐式积分连续演化：

```
每帧：力 → 速度 → 位置（子步进，最大 16 步，dt <= 1/480s）
球形由固定体积下面积最小化自然产生，不采样目标球
```

**实时性设计：**

- 97 个节点的轴对称仿真远低于完整三维网格仿真。
- 子步进有上限（maximumSubsteps = 16），最坏情况下 dt 被截断而非无限细分。
- 体积投影是位置级修正（4 次迭代），不是线性系统求解。
- 只在颈部曲率加权恶化的边长比超过阈值（resamplingEdgeRatio = 2.35）时
  才触发重采样，避免每帧重分配节点。

### 5.2 旋转网格生成

融合面网格（96 段 x 97 环）直接从当前节点 (z_i, r_i) 旋转生成，
法线用弧长一致的经线切线计算。每帧只更新顶点缓冲，不重建索引。

### 5.3 持久视觉状态

BubbleVisualState 跨几何切换存活，携带膜厚、相位、噪声种子、时间原点、
纹理基等。完成交接时 PromoteFusion 移动同一个 Model（同一个 VAO），
不创建新网格、不重置材质——避免了视觉跳变，也避免了重新上传顶点数据的开销。

## 六、多泡泡数量管理

### 6.1 数量上限

```
kHardMaxDisplayBubbleCount = 16    // 硬上限，保护 GPU 资源
g_MaxLiveBubbleCount = 8           // 运行时可调 live 上限
```

**实时性设计：** 泡泡数量直接决定 draw call 数和顶点数。上限 16 保证
总顶点数在可控范围（16 x 球壳 + 融合面 + 接触桥 < 50K 顶点）。

### 6.2 Render-only 装饰泡泡

背景中的小装饰泡泡用低模球体 + 简单 shader 绘制，不参与仿真和接触检测，
只做漂浮动画。这些泡泡提供了视觉丰富度，但几乎不增加计算开销。

## 七、Shader 级优化

### 7.1 诊断模式早退

```glsl
if (uFusionDiagnosticMode == 1) {
    FragColor = vec4(0.18, 0.68, 0.92, uOutputAlpha * vShellCoverage);
    return;
}
```

开发调试时可以只看法线、膜厚、纹理坐标等单一通道，跳过后续所有计算。
正式运行时 uFusionDiagnosticMode = 0，这些分支被 GPU 剔除。

### 7.2 接触平面裁切在 shader 完成

球壳顶点不被 CPU 裁切，而是把最多 4 个接触平面传给 shader：

```glsl
float signedDistance = dot(worldPos.xyz - uVisualContactPlanePoints[i],
                           normalize(uVisualContactPlaneNormals[i]));
float clippedCoverage = 1.0 - smoothstep(-blendWidth, blendWidth, signedDistance);
shellCoverage *= mix(1.0, clippedCoverage, uVisualContactStrengths[i]);
```

**实时性设计：** 避免了 CPU 端的网格布尔运算（高代价），用 shader 的
smoothstep 软裁切实现，每片元只需 1 次 dot + 1 次 smoothstep。

### 7.3 透明度由 shader 控制

vShellCoverage 在最终 alpha 中乘入：

```glsl
float localAlpha = uOutputAlpha * mix(1.0, 0.65, vSharedFilmMask) * vShellCoverage;
```

裁切区域的 alpha 平滑过渡到 0，不需要单独的裁切 Pass。

## 八、平台迁移：Windows 到 HarmonyOS

### 8.1 两阶段开发策略

先在 Windows 桌面 OpenGL 3.3 上开发和调试核心算法，再迁移到
HarmonyOS OpenGL ES 3.0。核心 shader 逻辑一致，只替换：

- #version 330 core 到 #version 300 es + precision highp float
- GLFW 窗口到 XComponent + EGL
- 文件读取到 rawfile 资源读取
- 鼠标到触摸

### 8.2 移动端适配

- **precision 限定符。** 鸿蒙 shader 显式声明 precision highp float/int，
  确保浮点精度足够做折射和干涉计算。
- **极点法线修复。** 移动端 GLES 对退化三角形的插值行为和桌面不同，
  融合面极点顶点直接用几何法线而非输运的薄膜方向，避免 pinwheel 奇点。
- **shader 一致性。** 两端的折射、干涉、膜厚逻辑通过 windows_interaction_port.inc
  共享，确保算法行为一致。

## 九、与论文方法的对应关系

| 论文 | 原始方法 | 本项目实时近似 | 实时性考量 |
|------|---------|-------------|----------|
| Wyman 2005 | 图像空间两界面折射 | 单界面屏幕空间 FBO 偏移 | O(1) 纹理查找替代光线步进 |
| Kim & Park 2012 | 64 波长光谱积分 | 3 波长解析 / 256^2 LUT | LUT 预计算；3 波长避免纹理带宽 |
| Iwasaki 2004 | 光源 + 环境分别积分 | 合并为单一 shader pass | 减少 Pass 开销 |
| Belcour 2017 | Airy 级数 + 光谱 | 7 波长加权 Airy | 解析公式无纹理依赖 |
| Da et al. 2015 | 非流形涡流片 + Los Topos | 单泡 icosphere + 控制点 | 仅破裂时激活；无 Los Topos 依赖 |
| Ishida 2020 | 二维膜厚 PDE | shader Simplex Noise | 程序化生成无存储 |
| Busaryev 2012 | Weighted Voronoi | 空间哈希 + 接触图 | O(N) broad phase 替代 O(N log N) |
| Ishida 2017 | 双曲几何流 | 接触圆弹簧 + 控制点弹簧 | 局部弹簧替代全局 PDE |

## 十、总结

本项目的实时性策略可以归纳为三个层次：

1. **计算降维：** 把三维体积问题降为表面问题，把 PDE 降为弹簧/噪声，
   把光谱积分降为 LUT 或 3 波长解析。
2. **渲染简化：** 用屏幕空间 FBO 折射替代光线追踪，用 shader 程序化纹理
   替代膜厚场存储，用软裁切替代网格布尔运算。
3. **惰性计算：** DBSTT 仿真仅在破裂时激活，仿真以 10 Hz 节流运行，
   融合面重采样仅在边长比恶化时触发，控制点仅 26 个。

这些选择共同保证了在移动 GPU 上同时完成折射、薄膜干涉、多泡泡仿真与融合动画，
同时保持 30-60 FPS 的实时帧率。
