# 2026-06-01 Belcour Airy 与泡泡自然运动日志

## 一、目标

在单个泡泡的基础材质效果稳定后，本轮主要完成两件事：

1. 重新接入 Belcour2017 相关的 Airy 多次内反射路径，让它成为可展示的主模式。
2. 给单个泡泡加入轻微漂浮和形状呼吸，让画面不再像固定在原点的 shader demo。

## 二、Belcour Airy 模式

当前 `uIridescenceMode` 有三档：

```text
0 = Kim2012
1 = Spectral LUT
2 = Belcour Airy
```

程序默认使用 `Belcour Airy`，按 `L` 可以在三种模式间循环。

### 1. Airy 反射计算

在 `windows/shader/shader_refraction.h` 中保留了 Airy 多次内反射实现：

```glsl
float airyThinFilmReflectance(float cosTheta, float thickness, float wavelength)
```

该函数分别计算 s/p 偏振下的薄膜复振幅反射，并取平均反射率。当前用于泡泡的高层函数是：

```glsl
vec3 belcourAiryWeightedIridescence(float NdotV, float thickness)
```

它使用 7 个波长近似光谱反射：

```text
430, 470, 500, 535, 575, 615, 650 nm
```

这不是 Belcour2017 完整的解析光谱预积分，但比 RGB 三波长更接近连续光谱，也能较好展示 Airy 路径的视觉特征。

### 2. Airy 专属合成

早期尝试中，Airy 只替换 `filmReflectance`，后续仍走 Kim/LUT 的加色式合成，因此视觉区别被压弱。

当前 Airy 模式单独使用反射/透射合成：

```glsl
vec3 R = clamp(airyReflectance * 3.5, vec3(0.0), vec3(0.9));
vec3 reflected = envColor * R * edgeW;
vec3 transmitted = bgColor * (1.0 - R * 0.55);
vec3 airyColor = transmitted + reflected * 1.8;
```

这样 Airy 不再只是“背景上叠一层彩色”，而是同时控制反射光和透射光，更符合薄膜反射的直觉。

### 3. 默认膜厚

目前不同模式使用不同的默认膜厚：

```cpp
static float g_KimLutThickness = 350.0f;
static float g_AiryThickness = 740.0f;
```

原因：

- Kim2012 / LUT 在 `350nm` 附近效果更接近之前调好的展示效果。
- Belcour Airy 在 `740nm` 时当前画面更自然，色彩和反射关系更稳定。

`N/M` 会调整当前模式对应的厚度，不会把 Kim/LUT 和 Airy 的默认值混在一起。

## 三、Spectral LUT 与 Belcour Airy 的区别

### Spectral LUT

LUT 模式从 `windows/assets/lut/thinfilm_belcour_bubble.png` 查表：

```glsl
vec3 encodedReflectance = texture(uThinFilmLUT, vec2(NdotV, thicknessNorm)).rgb;
return encodedReflectance * encodedReflectance;
```

特点：

- 预计算，运行时开销低。
- 适合后续做更严格的 CIE/XYZ 光谱积分。
- 当前仍走原来的泡泡加色式合成，更像“薄膜颜色 tint + 环境反射 tint”。

### Belcour Airy

Airy 模式在 shader 中实时计算多波长 Airy 反射，并走专属反射/透射合成。

特点：

- 更直接体现薄膜的反射/透射能量关系。
- 当前默认厚度为 `740nm`。
- 代价更高，并且 7 波长权重仍是近似，不是完整 Belcour2017 解析预积分。

## 四、泡泡自然运动

为了让单个泡泡不再完全静止，本轮在 `windows/main.cpp` 中增加了 model matrix 级别的运动，不改动薄膜 shader 公式。

### 1. 基础椭球

泡泡不是完美球体，而是轻微椭球：

```cpp
static glm::vec3 g_BaseBubbleScale = glm::vec3(1.0f, 1.035f, 0.985f);
```

顶点 shader 中法线改为 inverse-transpose 变换，避免非均匀缩放导致反射方向错误：

```glsl
vWorldNormal = normalize(transpose(inverse(mat3(uModel))) * aNormal);
```

### 2. 缓慢漂浮

每帧根据时间计算整体偏移：

```cpp
glm::vec3 bubbleOffset = glm::vec3(
    sinf(t * 0.37f + 1.2f) * g_BubbleDriftAmp,
    sinf(t * 0.70f) * g_BubbleFloatAmp,
    cosf(t * 0.31f + 0.6f) * g_BubbleDriftAmp * 0.75f);
```

默认参数：

```cpp
static float g_BubbleFloatAmp = 0.08f;
static float g_BubbleDriftAmp = 0.04f;
```

### 3. 轻微形状呼吸

泡泡比例随时间发生约 1% 的缓慢变化：

```cpp
glm::vec3 bubbleScale = g_BaseBubbleScale + glm::vec3(
    sinf(t * 0.45f) * g_BubbleWobbleAmp,
    sinf(t * 0.38f + 2.1f) * g_BubbleWobbleAmp,
    sinf(t * 0.52f + 4.0f) * g_BubbleWobbleAmp);
```

默认：

```cpp
static float g_BubbleWobbleAmp = 0.012f;
```

该效果只改变整体模型矩阵，不做顶点级随机形变，因此不会破坏当前已经调好的薄膜反射和折射稳定性。

## 五、当前保留的控制

```text
L   : Cycle iridescence mode (Kim2012 / Spectral LUT / Belcour Airy)
N/M : Film thickness -/+ (nm)
1/2 : Thickness variation -/+
O/P : Environment reflection -/+
```

背景 skybox 已恢复为原始 `right/left/top/bottom/front/back` 六张图，不再保留 radiance 背景切换。

## 六、后续建议

1. 如果继续提高自然感，优先尝试 very subtle shader normal perturbation，而不是直接改网格顶点。
2. 如果继续提高论文复现度，应把 Airy 用于离线生成更准确的 2D LUT，而不是长期依赖 shader 内 7 波长近似。
3. 如果后续做多泡泡，当前漂浮/呼吸参数应改成 per-bubble 随机相位，避免多个泡泡同步运动。
