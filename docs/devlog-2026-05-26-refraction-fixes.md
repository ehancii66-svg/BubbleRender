# 开发日志：折射黑边修复与参数系统重构

**日期**：2026-05-26  
**关联**：[[devlog-2026-05-26-windows-setup]]

---

## 一、问题描述

缩放或增大折射强度时，泡泡边缘出现大面积黑色区域。

## 二、根因分析

两个层面的问题叠加：

### 2.1 采样越界

```glsl
// 原始代码
vec3 refractVec = mat3(vView) * refract(eye, normal, 1.0/1.33);
vec3 color = texture(uBackgroundTexture, uv + refractVec.xy).rgb;
```

`refractVec.xy` 是视空间方向分量（范围 `[-1, 1]`），直接被当 UV 偏移用。最大可偏出整屏宽度，而 FBO 纹理设了 `GL_CLAMP_TO_EDGE`，越界采样取到清屏色（黑色）。

### 2.2 偏移不随球缩放

偏移量以固定屏幕像素计，不随球的屏占比变化。球小如米粒时也会偏数百像素，必然冲出去。

## 三、修复过程

### 第一轮：UV Clamp（不彻底）

在 Shader 里加了 `clamp(uv, 0, 1)` 和 `uRefractionStrength` 控制。能缓解但不能根治——大偏移下球边缘像素全部被截到同一个边界 texel。

### 第二轮：FBO 扩边 + 像素空间采样 + 相机距离收紧

**FBO 扩边**（[main.cpp](windows/main.cpp#L110-L161)）：
```
FBO尺寸 = 屏幕尺寸 × 1.3
```
纹理包裹改为 `GL_CLAMP_TO_BORDER`，边框色透明黑。背景渲染到扩大的 FBO，内容四周有 15% 像素余量。

**像素空间采样**（[shader_refraction.h](windows/shader/shader_refraction.h#L79-L88)）：
```glsl
// 屏幕坐标 → FBO 坐标 → 加偏移 → clamp → UV
vec2 fboPixel = screenPixel + pad;
vec2 samplePixel = clamp(fboPixel + offsetPixels, vec2(0), uFBOSize);
vec2 sampleUV = samplePixel / uFBOSize;
```

**相机距离**：最近距离从 2.0 收紧到 3.5（球半径 1.5，保持至少 2.0 单位间隙）。

### 第三轮：球屏幕半径锚定 + 边缘增强

**核心改动**（[main.cpp](windows/main.cpp#L198-L205) / [shader_refraction.h](windows/shader/shader_refraction.h#L70-L86)）：

偏移量不再用固定像素数，而是锚定在球的屏幕投影半径上：

```cpp
// CPU 每帧计算
float dist = length(cameraPos);
float angularRadius = atan(1.5 / dist);
float pixelsPerRadian = (h / 2) / tan(fovVert / 2);
float spherePixelRadius = angularRadius * pixelsPerRadian;
```

```glsl
// Shader：边缘增强 + 上限保护
float edge = 1.0 - abs(dot(eye, normal));          // 0=球心, 1=边缘
float edgeBoost = mix(1.0, uEdgeDistortionBoost, pow(edge, 1.5));

vec2 offsetPixels = refractVec.xy * uRefractionStrength * uSpherePixelRadius * edgeBoost;

float maxOffset = uSpherePixelRadius * uMaxOffsetRatio;
offsetPixels = clamp(offsetPixels, vec2(-maxOffset), vec2(maxOffset));
```

## 四、最终参数

| 参数 | 默认 | 范围 | 按键 | 含义 |
|------|------|------|------|------|
| RefractionStrength | 4.0 | 0~7 | Y/H | 整体折射强度 |
| EdgeDistortionBoost | 2.2 | 1~5 | U/J | 边缘折射倍率 |
| MaxOffsetRatio | 1.5 | 0.1~2.0 | I/K | 偏移上限（相对球半径） |

### 效果逻辑

| 球面位置 | edge | boost | 偏移 |
|----------|------|-------|------|
| 球心 | 0.0 | 1.0 | 弱折射 |
| 中间 | 0.5 | ~1.42 | 适度增强 |
| 边缘 | 1.0 | 2.2 | 强烈扭曲（被 maxOffset 截断） |

## 五、修改文件

| 文件 | 改动 |
|------|------|
| `shader/shader_refraction.h` | 新增 `uEdgeDistortionBoost` `uMaxOffsetRatio` `uFBOSize` `uSpherePixelRadius`；像素空间采样 + 边缘增强 + clamp 保护 |
| `main.cpp` | FBO 扩边常量 `kFBOOverscan`；`spherePixelRadius` 每帧计算；6 个新参数 + 键盘控制；相机最近距离 3.5 |

---
