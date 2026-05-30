# 2026-05-30 泡泡渲染调优与问题修正日志

## 背景

Kim2012 初级版已经把薄膜干涉虹彩接入 Windows OpenGL demo，但早期效果仍然偏向“透明玻璃球”：折射偏移较强，前后表面重复叠加虹彩、高光和白边，泡泡中心不够轻薄。

随后又观察到一个明显问题：彩色干涉条纹出现后，球体中间接近赤道的位置会出现一条明显的线。将 `ThicknessVar` 调到 0 后，这条线会消失，同时彩色条纹也消失，因此问题主要来自膜厚扰动场，而不是 FBO 折射或环境反射本身。

本日志合并记录 2026-05-30 两次调优：薄膜泡泡材质调参，以及赤道线/膜厚扰动修正。

## 一、薄膜泡泡材质调参

### 1. 降低默认折射强度

原参数偏向透明玻璃球：

```cpp
g_RefractionStrength = 4.0f;
g_EdgeDistortionBoost = 2.2f;
g_MaxOffsetRatio = 1.5f;
```

调整为更薄的泡泡膜效果：

```cpp
g_RefractionStrength = 1.4f;
g_EdgeDistortionBoost = 1.7f;
g_MaxOffsetRatio = 0.7f;
```

同时收窄键盘调参上限：

```text
RefractionStrength <= 4.0
EdgeDistortionBoost <= 3.0
MaxOffsetRatio <= 1.2
```

目的：减少实心玻璃球式的强背景扭曲，让中心更透明，边缘保留轻微折射。

### 2. 区分正面和背面 pass

原先 Pass 2 和 Pass 3 使用同一套 shader 参数，shader 不知道当前在渲染正面还是背面。

现在 `SetRefractUniforms` 增加参数：

```cpp
auto SetRefractUniforms = [&](bool isBackFace)
```

并传入：

```cpp
g_RefractionShader->SetInt("uIsBackFace", isBackFace ? 1 : 0);
```

Pass 2 渲染背面：

```cpp
SetRefractUniforms(true);
```

Pass 3 渲染正面：

```cpp
SetRefractUniforms(false);
```

### 3. 背面法线翻转与贡献降低

shader 中新增：

```glsl
uniform int uIsBackFace;
```

背面 pass 中翻转法线：

```glsl
vec3 normal = normalize(vWorldNormal);
if (uIsBackFace == 1) {
    normal = -normal;
}
```

同时降低背面的折射、虹彩、高光和边缘白光贡献：

```glsl
float surfaceRefractionScale = (uIsBackFace == 1) ? 0.35 : 1.0;
float surfaceColorScale = (uIsBackFace == 1) ? 0.18 : 1.0;
```

目的：让 Pass 2 更像“穿过薄膜后的中间采样”，避免前后表面重复把颜色和亮度堆得太厚。

### 4. 透明度调整

原透明度：

```glsl
float transparency = 1.0 - fresnelTerm * 0.5;
```

调整为：

```glsl
float transparency = mix(0.97, 0.78, fresnelTerm);
```

目的：

- 泡泡中心几乎透明。
- 轮廓和掠射角区域更明显。
- 整体不再像厚玻璃球。

### 5. 接入简单环境反射

当前实现已经把 skybox cubemap 接入主泡泡 shader：

```glsl
vec3 reflectDir = normalize(reflect(eye, normal));
vec3 envColor = texture(uEnvironmentMap, reflectDir).rgb;
```

并使用薄膜反射颜色和 Fresnel 控制环境反射强度：

```glsl
vec3 filmReflectionTint = filmReflectance * (1.0 + fresnelTerm * 2.4);
filmReflectionTint = filmReflectionTint / (1.0 + filmReflectionTint);
color += envColor * filmReflectionTint * envReflectionWeight;
```

新增交互参数：

```text
O/P : Environment reflection -/+
```

说明：这一步只是简单环境反射接入，不是 Belcour2017 完整 Airy/microfacet 模型。

## 二、赤道线与膜厚扰动修正

### 1. 问题原因

原先膜厚扰动使用球体 UV：

```glsl
float noiseVal = snoise(vec3(vUV * 5.0, uTime * 0.6)) * 0.5 + 0.5;
float dynamicThickness = uThickness + (noiseVal - 0.5) * uThicknessVar;
```

主泡泡的 UV 来自经纬球展开：

```cpp
vertex.TexCoords.x = (float)j / sectors;
vertex.TexCoords.y = (float)i / stacks;
```

经纬展开对球面不是均匀参数化。将 UV 直接喂给噪声函数时，噪声会继承展开坐标的纬向/经向结构。薄膜干涉又会放大膜厚变化，所以参数化痕迹会表现成球面上的明显线。

### 2. 膜厚噪声改为球面方向空间

新增从 vertex shader 传递世界位置：

```glsl
out vec3 vWorldPos;
...
vWorldPos = worldPos.xyz;
```

fragment shader 中接收：

```glsl
in vec3 vWorldPos;
```

膜厚扰动不再依赖 `vUV`，而是使用球面方向/法线方向：

```glsl
vec3 filmDir = normalize(vWorldNormal);
```

这样膜厚场贴在球面方向上，不经过经纬 UV 展开，赤道线问题消失。

### 3. 从单层噪声改成低频膜厚场

单层球面 3D 噪声虽然能去掉赤道线，但视觉过于规则。当前改为多层低频膜厚场：

```glsl
float broadNoise = filmNoise(filmDir * 2.0 + warp + slowFlow);
float flowNoise = filmNoise(filmDir * 3.4 + warp * 0.7 + slowFlow.yzx * 0.8);
float fineNoise = filmNoise(filmDir * 7.0 + slowFlow.zxy * 0.45);
float drainage = smoothstep(-0.85, 0.85, -filmDir.y);
```

组合方式：

```glsl
float thicknessPattern = (broadNoise - 0.5) * 0.52
    + (flowNoise - 0.5) * 0.30
    + (fineNoise - 0.5) * 0.06
    + (drainage - 0.5) * 0.42;
float dynamicThickness = uThickness + thicknessPattern * uThicknessVar;
```

设计意图：

- `broadNoise` 提供宽的、缓慢变化的大块膜厚区域。
- `flowNoise` 提供中尺度流动变化。
- `fineNoise` 只保留很弱的细节，避免彩色条纹过密。
- `drainage` 模拟重力排液导致的上下膜厚差。

### 4. 降低默认膜厚扰动幅度

原值：

```cpp
static float g_ThicknessVar = 250.0f;
```

调整为：

```cpp
static float g_ThicknessVar = 160.0f;
```

原因：膜厚扰动过大时，薄膜干涉颜色会快速跨越多个色带，导致条纹过密、跳变明显。

### 5. 提高主泡泡球体细分

原值：

```cpp
g_RefractModel = Model::CreateSphere(1.5f, 64, 32, true);
```

调整为：

```cpp
g_RefractModel = Model::CreateSphere(1.5f, 128, 64, true);
```

原因：薄膜干涉颜色对法线和视角变化非常敏感，球体细分偏低时三角面片边界容易被彩色条纹放大。

### 6. 流动速度调节位置

膜厚场的流动速度在 `windows/shader/shader_refraction.h` 中控制：

```glsl
float flowSpeed = 1.25;
vec3 slowFlow = vec3(uTime * 0.09, -uTime * 0.17, uTime * 0.07) * flowSpeed;
```

如果需要整体加快或减慢流动，优先调整 `flowSpeed`。如果要改变主要流动方向，再调整 `slowFlow` 三个分量。

## 三、未采用内容

本轮曾尝试接入 Belcour2017 / Airy reflectance 作为泡泡反射颜色来源，但实际效果偏灰、偏暗、彩色不如 Kim2012 鲜明。该部分已放弃，不作为当前实现。

目前保留的是：

```text
Kim2012 薄膜干涉公式
+ 简单 cubemap 环境反射
+ 球面方向空间膜厚扰动
+ 低频流动膜厚场
+ 轻微重力排液梯度
+ 更高细分的主泡泡球面
```

## 四、目前已经完成

### Windows OpenGL demo

- GLFW 窗口和主循环。
- skybox cubemap 加载与绘制。
- 5x5 背景小球阵列。
- 主泡泡球面模型。
- Release 构建通过，输出 `windows/build/Release/BubbleRender.exe`。

### 渲染管线

- Pass 1：渲染 skybox 和背景小球到 `backgroundFBO`。
- Pass 2：渲染泡泡背面到 `backFaceFBO`。
- Pass 3：渲染 skybox、背景和泡泡正面到屏幕。
- 使用 overscan FBO 减少折射采样越界。

### 折射

- 使用 `refract(eye, normal, 1.0 / 1.33)` 计算折射方向。
- 用屏幕空间 FBO 偏移采样背景。
- 默认折射强度已调低，更接近薄泡膜。
- 背面 pass 的折射贡献降低，避免厚玻璃感。

### 薄膜干涉

- 实现 Kim2012 的 `fresnelCoefficients`、`interferencePhase`、`thinFilmReflectance`。
- 使用 `615nm / 535nm / 465nm` 三波长近似 RGB 虹彩。
- 膜厚使用动态噪声扰动。
- 膜厚扰动已经从 UV 空间迁移到球面方向空间。

### 反射与高光

- 保留 Blinn-Phong 高光。
- 保留 Fresnel 边缘白光。
- 接入 skybox 环境反射，并用薄膜反射颜色调制反射光。
- 新增 `O/P` 控制环境反射强度。

### 交互参数

```text
Mouse drag  : Rotate view
Mouse wheel : Zoom in/out
R/T : Fresnel power -/+
F/G : Diffuseness -/+
V/B : Shininess -/+
Y/H : Refraction strength -/+
U/J : Edge distortion boost -/+
I/K : Max offset ratio -/+
O/P : Environment reflection -/+
N/M : Film thickness -/+
1/2 : Thickness variation -/+
ESC : Quit
```

## 五、下一步计划与展望

### 近期优先

1. 继续调整泡泡表面视觉。
   - 控制彩色条纹宽度。
   - 控制膜厚流动速度。
   - 找到更稳定的默认参数组合。

2. 增强反射效果。
   - 保留当前简单 cubemap 反射。
   - 进一步调节环境反射强度和 Fresnel 权重。
   - 避免白边过强或整体变灰。

3. 整理演示用参数。
   - 固定一组默认值用于比赛展示。
   - 保留键盘调参用于现场调试。

### 中期方向

1. 进一步改进膜厚流动。
   - 让膜厚沿重力方向更明显地排液。
   - 加入更像真实泡泡的宽条纹和局部薄区。

2. 多泡泡渲染。
   - 支持多个泡泡。
   - 处理泡泡之间的遮挡、折射和排序。

3. HarmonyOS 迁移。
   - 将 Windows OpenGL shader 和 C++ 渲染逻辑迁回 HarmonyOS OpenGL ES 工程。
   - 适配 XComponent / NAPI / EGL。

### 远期展望

1. 更准确的光谱颜色。
   - 当前三波长 RGB 近似容易产生颜色偏差。
   - 后续可考虑 LUT 或预积分方案替代实时三波长采样。

2. 更物理的薄膜模型。
   - Belcour2017 / Airy reflectance 暂时未采用。
   - 后续如果需要论文深度，可以作为材质模型升级方向，而不是直接替代当前 Kim2012 主流程。

3. 交互与展示系统。
   - 加入 UI 面板。
   - 支持参数预设。
   - 支持泡泡生成、拖动、破裂等演示行为。
