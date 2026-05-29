# Kim2012 肥皂泡虹彩渲染 — 实现文档

> 基于论文 **"Real-time Rendering of Soap Bubbles Taking into Account Light Interference"** (Namjung Kim, Kyoungju Park) 的实现。

---

## 目录

1. [原始版本（玻璃球）](#1-原始版本玻璃球)
2. [Kim2012 论文核心原理](#2-kim2012-论文核心原理)
3. [实现映射](#3-实现映射)
   - [3.1 干涉映射 (论文第 3 节)](#31-干涉映射论文第-3-节)
   - [3.2 折射映射 (论文第 4 节)](#32-折射映射论文第-4-节)
   - [3.3 反射与 HDRI 映射 (论文第 5 节)](#33-反射与-hdri-映射论文第-5-节)
   - [3.4 晃动模拟 (论文第 6 节)](#34-晃动模拟论文第-6-节)
4. [Bug 修复记录](#4-bug-修复记录)
5. [视觉效果调优](#5-视觉效果调优)
6. [CPU 端验证代码](#6-cpu-端验证代码)
7. [文件变更总结](#7-文件变更总结)

---

## 1. 原始版本（玻璃球）

提交 `62e289c` 的原始版本是一个简单的玻璃球渲染器，使用三 Pass 屏幕空间折射管线：

```
Pass 1: 天空盒 + 背景球 → backgroundFBO
Pass 2: 气泡背面 + 折射采样 backgroundFBO → backFaceFBO
Pass 3: 天空盒 + 背景球 → 屏幕, 气泡正面 + 折射采样 backFaceFBO → 屏幕
```

**原始 Fragment Shader 的视觉效果**：

| 效果组件 | 公式 | 视觉效果 |
|---------|------|---------|
| 屏幕空间折射 | `refract(eye, N, 1/1.33)` 偏移 FBO 采样 UV | 背景扭曲，透明玻璃感 |
| Blinn-Phong 高光 | `pow(N·H, shininess) + N·L * diffuseness` | 白色高光点 |
| Fresnel 边缘 | `(1 - N·V)^8` 叠加白色 | 边缘发白 |

**问题**：只能渲染出透明玻璃球的效果，**没有任何薄膜干涉产生的虹彩颜色**——而这正是肥皂泡区别于玻璃球的核心视觉特征。

---

## 2. Kim2012 论文核心原理

Kim2012 提出的肥皂泡实时渲染管线包含以下关键组件：

```
论文流程图:
入射光 → [干涉映射] → 光谱反射率 R_λ → [光谱→RGB] → 虹彩颜色
       ↘ [折射映射] → FBO 背景采样 → 透射背景
       ↘ [反射映射] → Fresnel 近似 + HDRI 环境贴图
       ↘ [晃动模拟] → GPU Perlin Noise 改变膜厚 → 动态虹彩
```

论文的核心贡献是将**光学薄膜干涉的物理公式**引入实时渲染管线，并做了以下工程近似：

1. **干涉**：用光学相位反射率公式精确计算每个波长的反射率
2. **光谱转换**：用 CIE-XYZ 颜色匹配函数将光谱转为 RGB
3. **折射**：用 Wyman 的双 Pass 深度近似法代替完整光线追踪
4. **反射**：用 Schlick 近似的 Fresnel 项 + HDRI 环境贴图
5. **晃动**：用 GPU Perlin Noise 扰动膜厚模拟皂膜液体流动

---

## 3. 实现映射

### 3.1 干涉映射 (论文第 3 节)

这是本次修改**最核心的新增内容**。

#### 3.1.1 光学相位反射率计算 (论文 Eq 2-5)

论文给出薄膜反射的完整物理公式：

**Eq (2)**: 反射光强度 = 反射率 × 入射光光谱
$$I_r(\lambda) = R_\lambda \cdot I_{in}(\lambda)$$

**Eq (3)**: 总反射率 = s 偏振分量 + p 偏振分量
$$R_\lambda = \frac{R_s^2(1 - \cos\varphi)}{1 + R_s^4 - 2R_s^2\cos\varphi} + \frac{R_p^2(1 - \cos\varphi)}{1 + R_p^4 - 2R_p^2\cos\varphi}$$

**Eq (4)**: Fresnel 振幅系数（s 和 p 偏振方向）
$$R_s = \frac{\cos\theta - \sqrt{n^2 - \sin^2\theta}}{\cos\theta + \sqrt{n^2 - \sin^2\theta}}, \quad R_p = \frac{n^2\cos\theta - \sqrt{n^2 - \sin^2\theta}}{n^2\cos\theta + \sqrt{n^2 - \sin^2\theta}}$$

**Eq (5)**: 干涉相位差
$$\varphi(\lambda, \theta) = \frac{4\pi d}{\lambda}\sqrt{n^2 - \sin^2\theta}$$

#### 3.1.2 GLSL 实现

对应代码中三个函数 `fresnelCoefficients`、`interferencePhase`、`thinFilmReflectance`：

```glsl
// Eq (4): Fresnel 振幅系数 Rs, Rp
vec2 fresnelCoefficients(float cosTheta, float n) {
    float sinTheta2 = max(0.0, 1.0 - cosTheta * cosTheta);
    float cosT = sqrt(max(0.0, n * n - sinTheta2)) / n;  // Snell 定律
    float Rs = (cosTheta - cosT) / (cosTheta + cosT);
    float Rp = (n * n * cosTheta - cosT) / (n * n * cosTheta + cosT);
    return vec2(Rs, Rp);
}

// Eq (5): 干涉相位差
float interferencePhase(float cosTheta, float thickness, float wavelength, float n) {
    float sinT = sqrt(max(0.0, 1.0 - cosTheta * cosTheta)) / n;
    float cosT = sqrt(max(0.0, 1.0 - sinT * sinT));
    return (4.0 * PI * n * thickness * cosT) / wavelength;
}

// Eq (3): 单波长反射率
float thinFilmReflectance(float cosTheta, float thickness, float wavelength) {
    float n = 1.33;  // 皂膜折射率 (水)
    vec2 r = fresnelCoefficients(cosTheta, n);
    float Rs = r.x, Rp = r.y;
    float phi = interferencePhase(cosTheta, thickness, wavelength, n);

    float Rs2 = Rs * Rs, Rp2 = Rp * Rp;
    float cosPhi = cos(phi);

    float denom_s = 1.0 + Rs2 * Rs2 - 2.0 * Rs2 * cosPhi;
    float denom_p = 1.0 + Rp2 * Rp2 - 2.0 * Rp2 * cosPhi;

    float Rs_term = (denom_s > 1e-6) ? Rs2 * (1.0 - cosPhi) / denom_s : 0.0;
    float Rp_term = (denom_p > 1e-6) ? Rp2 * (1.0 - cosPhi) / denom_p : 0.0;

    return Rs_term + Rp_term;  // 总反射率 R_λ
}
```

**重要细节**：`fresnelCoefficients` 中的 cosT 计算必须**除以 n**。Snell 定律给出：
$$\cos\theta_t = \frac{\sqrt{n^2 - \sin^2\theta_i}}{n}$$

而 `interferencePhase` 中的 cosT 通过 `sinT → cosT` 路径隐式满足了这一关系：
$$\cos\theta_t = \sqrt{1 - (\sin\theta_i / n)^2}$$

两者在数学上等价，但实现路径不同。这也是首次实现时的 bug 来源（见第 4 节）。

#### 3.1.3 光谱颜色转换 (论文 Eq 6)

论文使用 CIE-XYZ 颜色匹配函数做积分：

$$X = \int I(\lambda)x(\lambda)d\lambda, \quad Y = \int I(\lambda)y(\lambda)d\lambda, \quad Z = \int I(\lambda)z(\lambda)d\lambda$$

然后将 XYZ 转为 RGB。我们的实现做了一个**简化近似**：直接采样 **RGB 三个波长**（615nm, 535nm, 465nm）的反射率，作为颜色的 RGB 分量：

```glsl
vec3 kim2012Iridescence(float NdotV, float thickness) {
    float r = thinFilmReflectance(NdotV, thickness, 615.0);  // 红
    float g = thinFilmReflectance(NdotV, thickness, 535.0);  // 绿
    float b = thinFilmReflectance(NdotV, thickness, 465.0);  // 蓝
    return vec3(r, g, b);
}
```

这个近似的合理性：
- RGB 三原色的主波长恰好接近 615nm / 535nm / 465nm
- 积分被简化为三点采样，避免了在 shader 中做完整光谱积分（论文使用 31 个采样点）
- 代价是颜色精度略低，但视觉上依然能产生正确的虹彩色调

> **论文笔记中的替代方案**：可以离线预计算一张 2D LUT 贴图（X 轴 = N·V，Y 轴 = 膜厚），在 shader 中只需 `texture(lut, vec2(NdotV, thickness))` 即可查表得到虹彩颜色。这比实时计算更快，但目前直接在 shader 中计算已经满足实时性能需求。

---

### 3.2 折射映射 (论文第 4 节)

论文使用 Wyman 的屏幕空间双 Pass 深度近似法来近似完整的光线折射路径。我们的实现**继承了原始版本的三 Pass FBO 管线**，这与论文思路一致：

```
Pass 1: 渲染背景到 backgroundFBO (overscan: 1.3x)
Pass 2: 渲染球体背面到 backFaceFBO，采样 backgroundFBO 做第一次折射
Pass 3: 渲染球体正面到屏幕，采样 backFaceFBO 做第二次折射
```

**折射向量计算** (论文 Eq 7)：

```glsl
vec3 refractVec = mat3(vView) * refract(eye, normal, 1.0/1.33);
```

- `eye = normalize(worldPos - cameraPos)`：从相机指向表面的入射方向
- `refract(I, N, eta)`：GLSL 内置，基于 Snell 定律计算折射方向
- `eta = 1.0/1.33`：空气折射率 / 皂膜折射率
- `mat3(vView)`：将折射向量从世界空间转到视图空间，用于屏幕空间 UV 偏移

**边缘增强**：折射偏移在边缘（N·V → 0）被放大，模拟光线穿过球体边缘时更大的偏折：

```glsl
float edge = 1.0 - NdotV;
float edgeBoost = mix(1.0, uEdgeDistortionBoost, pow(edge, 1.5));
```

---

### 3.3 反射与 HDRI 映射 (论文第 5 节)

论文使用 Schlick 近似的 Fresnel 项避免完整菲涅尔公式的计算开销。

**Eq (9)**: Fresnel 反射率近似
$$R(\theta) = R_0 + (1 - R_0)(1 - \cos\theta)^5$$

我们的实现使用了一个更简化的版本（原始代码已有）：

```glsl
float fresnel(vec3 eye, vec3 normal, float power) {
    float fresnelFactor = abs(dot(eye, normal));
    return pow(1.0 - fresnelFactor, power);  // 默认 power=8
}
```

其中 `fresnelFactor = |N·V|`，在中心为 1（Fresnel → 0），边缘为 0（Fresnel → 1）。

**HDRI 环境映射**：通过加载 6 张 cubemap 面（2048×2048 JP
G）构建天空盒纹理。在 Pass 1 和 Pass 3 中渲染天空盒，为折射和反射提供环境光。

---

### 3.4 晃动模拟 (论文第 6 节)

论文利用 GPU Perlin 噪声通过改变膜厚来近似皂膜中水分子运动产生的晃动。

我们的实现使用 **3D Simplex Noise**（Perlin 噪声的改进变体）：

```glsl
float noiseVal = snoise(vec3(vUV * 5.0, uTime * 0.6)) * 0.5 + 0.5;
float dynamicThickness = uThickness + (noiseVal - 0.5) * uThicknessVar;
```

参数说明：
- `vUV * 5.0`：空间频率，值越大噪声图案越细密
- `uTime * 0.6`：时间维度，驱动动画
- `uThickness = 350nm`：基础膜厚
- `uThicknessVar = 250nm`：厚度扰动幅度（噪声值范围 [-125nm, +125nm]）

**Simplex vs Perlin**：论文原版使用 Perlin 噪声，我们使用 Simplex 噪声，后者在 3D 中有更低的计算复杂度和更好的各向同性。

**为什么用噪声扰动膜厚**：薄膜干涉的颜色对膜厚极度敏感（几纳米的变化就能改变颜色）。通过时间变化的噪声扰动膜厚，产生皂膜液体流动导致的色彩流转效果。

可用键盘 N/M 调整基础膜厚，1/2 调整扰动幅度。

---

## 4. Bug 修复记录

在首次 Kim2012 实现中发现了以下 bug，均已在当前版本修正：

### Bug 1: Fresnel 系数 cosθ_t 计算错误（严重）

**位置**：`fresnelCoefficients()` 函数

```glsl
// 错误（原始实现）
float cosT = sqrt(max(0.0, n * n - sinTheta * sinTheta));

// 正确（当前版本）
float cosT = sqrt(max(0.0, n * n - sinTheta2)) / n;
```

**原因**：Snell 定律要求 $\cos\theta_t = \sqrt{n^2 - \sin^2\theta_i} / n$，漏掉了除以 $n$。

**影响**：cosT 被放大了约 1.33 倍，导致 Rs、Rp 系数错误，进而使 `thinFilmReflectance` 计算的反射率完全偏离物理值。是导致虹彩颜色不正确的**首要原因**。

**注意**：`interferencePhase()` 中 cosT 的计算走的是 `sinT → cosT` 路径（$\sin\theta_t = \sin\theta_i / n$，$\cos\theta_t = \sqrt{1 - \sin^2\theta_t}$），这个路径是**正确的**。两个函数的计算方式不同但应得到相同结果。

### Bug 2: Simplex Noise 梯度计算错误（中等）

**位置**：`snoise()` 函数最后一行

```glsl
// 错误（原始实现）
return 42.0 * dot(m*m, vec4(dot(p0,x0), dot(p1,p1), dot(p2,p2), dot(p3,p3)));

// 正确（当前版本）
return 42.0 * dot(m*m, vec4(dot(p0,x0), dot(p1,x1), dot(p2,x2), dot(p3,x3)));
```

**原因**：Simplex 噪声的每个角点贡献是梯度向量 p_i 与距离向量 x_i 的点积 `dot(p_i, x_i)`。错误实现将后三个角点计算成了 `dot(p_i, p_i)`（即梯度向量的自内积 ≈ 1.0），使噪声退化为几乎只依赖第一个角点。

**影响**：晃动模拟失效——噪声输出几乎均匀，皂膜厚度不会随位置产生有意义的波动，虹彩颜色在泡泡表面几乎是均匀的。

### Bug 3: 折射方向取反（轻微）

**位置**：`main()` 的折射向量计算

```glsl
// 错误（原始 Kim2012 实现）
vec3 refractVec = mat3(vView) * refract(-eye, normal, iorRatio);

// 正确（当前版本，与原始玻璃球版本一致）
vec3 refractVec = mat3(vView) * refract(eye, normal, iorRatio);
```

**原因**：`eye = normalize(worldPos - cameraPos)` 从相机指向表面。GLSL `refract(I, N, eta)` 要求入射向量 I 指向入射点。因此应使用 `eye`（相机→表面），而非 `-eye`（表面→相机）。

**影响**：折射方向错误，导致背景扭曲采样偏移方向不正确。原始玻璃球版本使用 `refract(eye, ...)` 是正确的。

### Bug 4: Shader 编译崩溃（严重）

**症状**：Intel Arc GPU 的 GLSL 编译器报 `ERROR: 0:188: 'maxIrid' : undeclared identifier`，程序闪退。

**原因**：shader 源码中包含中文注释（如 `// iridColor 归一化（把最大分量拉到1）再放大强度`）。中文字符在 UTF-8 编码下为多字节序列，Intel 的 GLSL 编译器在解析注释时遇到了问题，导致其后的变量声明无法被正确识别。

**修复**：将所有中文注释替换为等效的英文注释。

### Bug 5: vec3 动态索引潜在兼容性问题（轻微）

**位置**：`kim2012Iridescence()` 函数

```glsl
// 原始实现（可能不兼容某些 GLSL 编译器）
float wavelengths[3] = float[](615.0, 535.0, 465.0);
for (int i = 0; i < 3; i++) {
    color[i] = thinFilmReflectance(NdotV, thickness, wavelengths[i]);
}

// 当前版本（手动展开，保证兼容性）
float r = thinFilmReflectance(NdotV, thickness, 615.0);
float g = thinFilmReflectance(NdotV, thickness, 535.0);
float b = thinFilmReflectance(NdotV, thickness, 465.0);
```

**原因**：虽然 `i < 3` 是编译时常量，但某些 GLSL 编译器（尤其是较老或移动端版本）可能不支持 `vec3[i]` 的动态索引。手动展开避免了任何潜在的兼容性问题。

### Bug 6: 分母除零保护（轻微）

在 `thinFilmReflectance()` 中添加了对 $1 + R_{s,p}^4 - 2R_{s,p}^2\cos\varphi \approx 0$ 的保护：

```glsl
float Rs_term = (denom_s > 1e-6) ? Rs2 * (1.0 - cosPhi) / denom_s : 0.0;
```

防止在极端角度或特定膜厚下产生 NaN 值。

---

## 5. 视觉效果调优

### 5.1 问题：初版虹彩效果呈现"霓虹灯"状

第一版 Kim2012 实现虽然成功产生了虹彩颜色，但效果非常不自然：
- 颜色过饱和（RGB 值频繁达到 255）
- 泡泡中心不透明（transmission 仅 ~30%）
- 虹彩像一层不透明的霓虹涂层贴在上面

这是因为原始的混合公式直接将归一化的反射率乘以 40 倍：

```glsl
// 初版：霓虹效果
vec3 iridBoosted = (iridColor / maxIrid) * 5.0;  // 丢失强度信息
float transparency = mix(0.1, 0.9, NdotV);        // 中心几乎不透明
```

### 5.2 改进后的颜色合成

当前版本的合成逻辑基于**薄膜物理原理**重新设计：

```glsl
// === 虹彩层 (反射光) ===
vec3 iridescence = filmReflectance * 2.5;          // 适度放大反射率
iridescence *= (1.0 + fresnelTerm * 3.0);          // 边缘增强
iridescence = iridescence / (1.0 + iridescence);  // sigmoid 软钳制

// === 背景层 (透射光) ===
float transparency = 1.0 - fresnelTerm * 0.5;      // 中心 100%, 边缘 50%
vec3 color = bgColor * transparency + iridescence;

// === 高光 + Fresnel 白边 ===
color += specular * 0.5;
color = mix(color, vec3(1.0), fresnelTerm * 0.15);
```

**各参数的物理含义和调优理由**：

| 参数 | 值 | 物理含义 | 调优理由 |
|------|-----|---------|---------|
| `filmReflectance * 2.5` | 基础虹彩强度 | 薄膜反射率通常为 1-50%，需要放大才能肉眼可见 | 2.5x 是平衡"可见"与"不刺眼"的经验值 |
| `fresnelTerm * 3.0` | 边缘增强 | 掠射角反射率远大于法向入射（Fresnel 效应） | 3.0 使边缘色彩更鲜艳但不过度 |
| `x/(1+x)` sigmoid | 软钳制 | 物理上反射率不能超过 1 | 比硬 clamp 更平滑，保留颜色比例 |
| `transparency = 1 - f*0.5` | 透射率 | 中心应几乎完全透明，边缘因 Fresnel 反射而略暗 | 0.5 系数使边缘仍保留 50% 背景可见 |
| `fresnelTerm * 0.15` | Fresnel 白边 | 掠射角时环境光反射占主导 | 比初版的 0.25 更微妙，不遮盖虹彩 |

**sigmoid 软钳制的数学原理**：
$$f(x) = \frac{x}{1+x}$$

- 当 $x \ll 1$ 时，$f(x) \approx x$（小值线性，保留暗部细节）
- 当 $x \gg 1$ 时，$f(x) \to 1$（大值渐近饱和，不会爆白）
- 与硬 clamp `min(x, 1.0)` 相比：sigmoid 在饱和区过渡更平滑，颜色比例在接近饱和时依然保持

### 5.3 调优前后对比

| 属性 | 初版 Kim2012 | 调优后 |
|------|-------------|--------|
| 虹彩强度 | 霓虹灯般刺眼 (RGB 常达 255) | 自然柔和 |
| 泡泡中心 | 几乎不透明 (~30% 透明) | 清晰透明 (~100% 透明) |
| 泡泡边缘 | 过度发白 + 霓虹色块 | 虹彩渐变 + 微妙的 Fresnel 反光 |
| 颜色过渡 | 色块化，不自然 | Sigmoid 软过渡 |
| 晃动效果 | 几乎不可见 (Noise bug) | 膜厚扰动产生流畅的虹彩流转 |

---

## 6. CPU 端验证代码

在 `main.cpp` 中添加了 C++ 版本的薄膜干涉公式验证（启动时打印）。该代码与 GLSL shader 中的公式**完全相同**，用于：

1. **验证公式正确性**：在 CPU 端计算三个典型角度下的 RGB 反射率并打印
2. **调试参考**：如果视觉效果不对，可以对比 C++ 输出和 shader 实际表现
3. **参数调优参考**：修改膜厚等参数前可以先看 CPU 端数值是否合理

**典型输出**（膜厚 350nm）：

```
=== Kim2012 iridescence formula check ===
thickness=350nm, thicknessVar=250nm
NdotV=0.9: R=0.112 G=0.098 B=0.016    ← 中心：低反射，微暖色调
NdotV=0.5: R=0.020 G=0.067 B=0.089    ← 中角度：低反射，蓝青色调
NdotV=0.1: R=0.001 G=0.548 B=0.788    ← 边缘：高反射，蓝绿色调
```

这些数值直观展示了薄膜干涉的角度依赖性——法向入射时反射率很低（泡泡透明），掠射时反射率很高（虹彩明显）。

---

## 7. 文件变更总结

| 文件 | 变更类型 | 内容 |
|------|---------|------|
| `windows/shader/shader_refraction.h` | 重写 | Fragment shader 完全重写，新增 6 个函数实现 Kim2012 公式 |
| `windows/main.cpp` | 修改 | 新增虹彩参数、键盘控制、CPU 端验证代码、时间追踪 |

**新增的 Shader 函数**：
- `fresnelCoefficients()` — 论文 Eq (4)
- `interferencePhase()` — 论文 Eq (5)
- `thinFilmReflectance()` — 论文 Eq (3)
- `kim2012Iridescence()` — RGB 三波长采样
- `snoise()` + 4 个辅助函数 — 3D Simplex 噪声（晃动模拟）
- `fresnel()`, `specular()` — 保留自原始版本（论文 Eq 9 + Blinn-Phong）

**新增的 Uniform**：
- `uThickness` — 皂膜厚度 (nm)
- `uThicknessVar` — 厚度扰动幅度 (nm)
- `uTime` — 时间（驱动晃动动画）

**键盘控制**：
- N/M — 调整膜厚 (±20nm)
- 1/2 — 调整厚度扰动幅度 (±20nm)
