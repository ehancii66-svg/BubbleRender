# 论文参考方案：泡泡折射、薄膜干涉与触摸交互

**日期**：2026-05-28  
**目标**：明确后续实现泡泡折射、题目所称“色散”视觉效果以及触摸交互时，各篇论文分别参考什么内容。

---

## 一、总体结论

本项目建议以 **Kim & Park 2012** 作为主复现论文，其他论文作为专项补充。

```text
主论文：
Real Time Rendering Iridescent Colors Appearing on Soap Bubbles

折射辅助：
An Approximate Image-Space Approach for Interactive Refraction

交互辅助：
Physically Based Soap Bubble Synthesis for VR

薄膜干涉 LUT / 背景参考：
Real-time Rendering of Soap Bubbles Taking into Account Light Interference
```

赛题中使用“色散”描述泡泡彩色光效，但从肥皂泡物理现象来看，更准确的核心机制是 **薄膜干涉**（thin-film interference / iridescence）。技术文档中可以表述为：

```text
本文将赛题要求中的“色散光效”实现为肥皂泡更符合物理特性的薄膜干涉彩虹色效果，
并额外保留可调的 RGB 折射偏移作为视觉增强。
```

---

## 二、主参考论文

### Real Time Rendering Iridescent Colors Appearing on Soap Bubbles

**作者**：Namjung Kim, Kyoungju Park  
**年份**：2012  
**文件**：`papers/Real Time Rendering Iridescent Colors Appearing on Soap Bubbles.pdf`

这篇最贴合本项目，因为它同时覆盖：

```text
泡泡实时渲染
薄膜干涉彩虹色
折射近似
Fresnel 反射
环境映射
OpenGL / GLSL
多泡泡性能
```

重点参考章节：

| 章节 | 参考内容 | 对应本项目 |
|------|----------|------------|
| Section 3 Interference Mapping | 薄膜干涉彩虹色计算 | 题目中的色散/彩虹色效果 |
| Section 3.1 Optical Phase Reflectance Computation | 根据膜厚、入射角、波长计算反射率 | shader 中的 iridescence 颜色 |
| Section 3.2 Spectrum Color Transformation | 光谱到 CIE-XYZ / RGB 的颜色转换 | 将干涉结果转成屏幕颜色 |
| Section 4 Refraction Mapping | Snell 折射与图像空间近似 | 泡泡折射背景 |
| Section 5 Reflection and HDRI Mapping | Fresnel 与环境映射 | 天空盒反射/环境反射 |
| Section 6 Sloshing Simulation | 用噪声扰动膜厚，模拟液膜流动 | 泡泡表面动态彩纹 |

实现时建议优先复现：

```text
1. 保留当前 FBO 折射管线
2. 在 refraction shader 中加入薄膜干涉颜色
3. 用噪声/时间扰动膜厚，形成流动彩纹
4. 用 Fresnel 控制边缘高光和彩虹色强度
```

---

## 三、折射辅助论文

### An Approximate Image-Space Approach for Interactive Refraction

**作者**：Chris Wyman  
**年份**：2005  
**文件**：`papers/An Approximate Image-Space Approach for Interactive Refraction.pdf`

这篇只讲实时折射，不讲泡泡彩虹色，但非常适合支撑当前工程里的 FBO/back-face 设计。

重点参考章节：

| 章节 | 参考内容 | 对应本项目 |
|------|----------|------------|
| Section 2 Basic Refraction | Snell 定律和基础折射方向 | shader 中的 `refract()` |
| Section 3 Image-Space Refraction | 图像空间两界面折射近似 | 当前 `backgroundFBO` / `backFaceFBO` |
| Section 3.1 Approximating the Point P2 | 用前后表面深度近似出射点 | 后续改进 back-face 采样 |
| Section 3.2 Determining the Normal N2 | 获取第二个折射面的法线 | 后续可加入 back-face normal texture |

本项目中的用途：

```text
用于解释和改进泡泡折射变形：
背景先渲染到纹理，再根据泡泡表面法线和折射方向偏移采样背景纹理。
```

当前代码已经具备相关基础：

```text
backgroundFBO
backgroundTexture
backFaceFBO
backFaceTexture
shader_refraction.h
```

---

## 四、交互辅助论文

### Physically Based Soap Bubble Synthesis for VR

**作者**：Sangwook Yoo, Cheongho Lee, Seongah Chin  
**年份**：2021  
**文件**：`papers/Physically Based Soap Bubble Synthesis for VR.pdf`

这篇适合参考交互设计和泡泡表面动态，不建议作为主渲染算法论文。

重点参考章节：

| 章节 | 参考内容 | 对应本项目 |
|------|----------|------------|
| Section 4.1 Flow Speed of Soap Film Surface | 液膜表面流动速度与 motion vector texture | 彩纹流动和膜厚扰动 |
| Section 4.2 Soap Bubble Rendering Parameters | 泡泡渲染参数、薄膜干涉、Fresnel、折射率 | 参数设计 |
| Section 5.2 Implementation through the Soap Bubble Tool | VR 工具和手势交互 | HarmonyOS 触摸交互 |
| Figure 15 | Clap / Grab / Fanning / Bouncing / Piercing 手势 | 触摸操作映射 |

可迁移到 HarmonyOS 的触摸设计：

| VR 手势 | HarmonyOS 触摸映射 | 功能 |
|---------|------------------|------|
| Clap | 双指/双触点靠近 | 生成泡泡 |
| Grab | 长按泡泡 | 抓取/拖动泡泡 |
| Fanning | 快速滑动 | 推动泡泡或增强流动 |
| Bouncing | 点击/拖动碰撞 | 泡泡反弹 |
| Piercing | 单指点击泡泡中心 | 戳破泡泡 |

---

## 五、背景参考论文

### Real-time Rendering of Soap Bubbles Taking into Account Light Interference

**作者**：Kei Iwasaki, Keichi Matsuzawa, Tomoyuki Nishita  
**年份**：2004  
**文件**：`papers/Real-time Rendering of Soap Bubbles Taking into Account Light Interference.pdf`

这篇可以作为薄膜干涉和动态环境映射的背景参考，但不建议作为主复现论文。

重点参考章节：

| 章节 | 参考内容 | 对应本项目 |
|------|----------|------------|
| Section 3 Simulation of Soap Bubble Dynamics | 泡泡粒子/三角网格动力学 | 后续复赛扩展 |
| Section 4.2 Reflected Light | 光源和环境反射的干涉计算 | 彩虹色 LUT 思路 |
| Section 4.2.1 Reflected light from the light sources | 根据波长、膜厚、角度预计算 light source texture | 可选优化 |
| Section 4.2.2 Reflected light from the environment | 动态 cubemap 环境反射 | 天空盒/环境映射 |
| Section 5.2 Interaction with Objects | 风力、平面碰撞、泡泡碰撞 | 交互物理参考 |

注意：该论文在 `Section 4.3 Transmitted Light` 中基本忽略了视线折射，因此不适合作为本项目折射变形的主要依据。

---

## 六、建议实现顺序

### 初赛优先

```text
1. 基于 Wyman / Kim & Park 改进当前折射 shader
2. 基于 Kim & Park Section 3 实现薄膜干涉彩虹色
3. 加入触摸/鼠标交互参数：
   - 折射强度
   - 彩虹色强度
   - 膜厚扰动强度
4. 做一个清晰可演示的单泡泡 Demo
```

### 复赛扩展

```text
1. 多泡泡渲染
2. 泡泡生成、拖动、戳破
3. UI 参数调节
4. 膜厚流动动画 / motion vector texture
5. HarmonyOS XComponent + NAPI + EGL / OpenGL ES 迁移
```

---

## 七、文档引用建议

技术文档中可以这样组织参考关系：

```text
本文主要复现 Kim 和 Park 提出的实时肥皂泡虹彩渲染方法，
使用薄膜干涉模型生成泡泡表面彩色光效；
折射部分参考 Wyman 的图像空间实时折射近似方法；
交互设计参考 Yoo 等人在 VR 泡泡系统中的手势交互方案；
Iwasaki 等人的工作作为薄膜干涉 LUT 和动态环境映射的补充参考。
```

---
