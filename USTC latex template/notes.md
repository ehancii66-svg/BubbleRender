# 肥皂泡渲染原理说明

这份文档用于配合 `Beamer.tex` 汇报使用。PPT 中只保留关键公式和图示，这里把折射、薄膜干涉和三种实现方式讲得更完整一些，方便答辩时解释。

## 1. 比赛任务与项目定位

本项目面向计算机图形学课程比赛/大作业展示，目标是在 Windows 桌面端实现一个可实时运行、可交互演示的图形系统。我们选择肥皂泡作为主题，是因为它同时包含多个具有代表性的图形学问题：

- 透明物体折射：泡泡内部可以看到被扭曲的背景。
- 薄膜干涉：几百纳米厚的液膜会产生随视角和厚度变化的虹彩。
- 动态表面：真实泡泡不是静止球体，膜厚和形状会随时间缓慢变化。
- 交互展示：用户可以调节折射、Fresnel、膜厚和模式，并用右键触发局部波纹。

因此项目不是单独实现一个 shader，而是把渲染管线、薄膜材质、表面动态和交互控制组织成一个完整 demo。

## 2. 折射部分原理

真实肥皂泡是一个极薄液膜。光线从空气进入液膜，再从液膜离开回到空气，因此理论上至少经历两次折射：

```text
air -> soap film -> air
```

如果严格模拟，需要对每个像素追踪光线、求交泡泡前后表面，还要处理屏幕外物体、环境光和多次反射。这对实时 OpenGL rasterization 来说成本较高。

本项目采用屏幕空间折射近似。基本思路是：

1. 先把背景渲染到一张纹理。
2. 在泡泡 shader 中根据法线和视线方向计算折射方向。
3. 把折射方向的屏幕分量转成纹理坐标偏移。
4. 用偏移后的坐标采样背景纹理，得到被泡泡扭曲后的背景颜色。

为了比单层透明球更接近肥皂泡，本项目使用三阶段管线：

- 背景阶段：渲染天空盒和背景参照物到 `backgroundFBO`。
- 背面阶段：只绘制泡泡背面，采样 `backgroundTexture`，近似第一次界面折射，输出到 `backFaceFBO`。
- 正面阶段：绘制泡泡正面，采样 `backFaceTexture`，再叠加薄膜虹彩、Fresnel 边缘和环境反射。

折射方向使用 Snell 定律在 shader 中的 `refract` 函数近似：

```glsl
vec3 refractVec = mat3(vView) * refract(eye, normal, iorRatio);
```

其中：

- `eye` 是视线方向。
- `normal` 是泡泡表面法线。
- `iorRatio = 1.0 / 1.33`，近似空气到肥皂液/水膜的折射率比例。

屏幕空间偏移可概括为：

```text
offset = refractVec.xy
       * refractionStrength
       * faceScale
       * filmScale
       * bubbleScreenRadius
       * edgeScale
       * touchScale
```

这个 `offset` 的含义是：当前泡泡片元最终要到背景纹理上的哪个位置采样。渲染时不是直接读取当前屏幕坐标处的背景颜色，而是读取 `当前坐标 + offset` 位置的颜色，从而制造背景被泡泡折射扭曲的视觉效果。

各个因子的作用如下。

### 2.1 `refractVec.xy`

`refractVec` 是由 `refract(eye, normal, iorRatio)` 计算出的三维折射方向。它描述光线穿过泡泡表面后大致会朝哪个方向偏折。

由于背景纹理是屏幕空间二维纹理，最终采样偏移只能在屏幕平面上移动，所以 shader 取它的 `xy` 分量：

```glsl
vec3 refractVec = mat3(vView) * refract(eye, normal, iorRatio);
vec2 dir = refractVec.xy;
```

`refractVec.xy` 只决定偏移方向。如果它指向右侧，背景采样点就向右偏；如果它接近零，说明该片元几乎不产生屏幕空间扭曲。

### 2.2 `refractionStrength`

这是全局折射强度，对应程序中的 `uRefractionStrength`，也就是终端菜单里的：

```text
Y / H : Refraction strength
```

它控制整个泡泡的折射效果强弱：

```text
值越大：背景扭曲越明显；
值越小：泡泡越接近透明，折射越弱。
```

如果展示时想让中心折射更明显，主要应该调大这个参数；如果泡泡看起来像厚玻璃球、背景扭曲过强，则应该调小。

### 2.3 `faceScale`

`faceScale` 用于区分背面 pass 和正面 pass。

项目用两次泡泡 pass 近似肥皂泡的双界面折射：背面 pass 先对背景进行一次折射，正面 pass 再进行第二次折射并合成薄膜颜色。如果两次都使用同样强度，背景会被过度扭曲。因此背面 pass 会使用较小的折射权重。

汇报时可以这样解释：

```text
faceScale 用于控制双界面折射中每一层界面的贡献。
背面 pass 较弱，正面 pass 较强，从而避免两次折射叠加后过度变形。
```

### 2.4 `filmScale`

`filmScale` 表示薄膜厚度和边缘程度对折射强度的影响。

虽然折射方向主要由法线和折射率决定，但从视觉表现上看，肥皂泡边缘和膜厚变化明显的地方通常会有更强的扭曲。本项目用膜厚归一化值和边缘权重构造 `thinFilmRefraction`：

```glsl
float thicknessRefraction = mix(0.08, 0.45, thicknessNorm);
float thinFilmRefraction =
    mix(0.08, 1.28, pow(edgeProfile, 1.15)) * thicknessRefraction;
```

它的作用是让泡泡中心更清透，让边缘和膜厚变化处折射更明显。

### 2.5 `bubbleScreenRadius`

`bubbleScreenRadius` 用于把折射偏移缩放到合适的像素尺度。

同样的折射方向，如果泡泡在屏幕上很大，允许的偏移可以更大；如果泡泡很小，偏移也应该更小，否则副泡泡会出现过度扭曲、采样越界甚至变黑的问题。

因此它的作用是：

```text
让折射偏移随泡泡屏幕尺寸自适应。
大泡泡可以有更大的背景偏移；
小泡泡需要更小的偏移，保证稳定。
```

### 2.6 `edgeScale`

`edgeScale` 是边缘增强项。泡泡轮廓处视线更接近掠射角，Fresnel 反射、薄膜干涉和折射扭曲都应该更明显。

项目用：

```text
edge = 1 - abs(dot(V, N))
```

衡量边缘程度，再用 `smoothstep` 得到平滑边缘权重：

```glsl
float edge = 1.0 - NdotV;
float edgeProfile = smoothstep(0.18, 0.92, edge);
float edgeBoost = mix(1.0, uEdgeDistortionBoost, pow(edgeProfile, 0.85));
```

这样中心不会过度扭曲，轮廓处会有更明显的玻璃感和虹彩边缘。

### 2.7 `touchScale`

`touchScale` 是右键触摸交互带来的局部增强。

当用户右键按下或拖动泡泡时，shader 会计算当前片元到触摸点的距离，并构造高斯影响区域和环形波纹：

```glsl
float touchMask = exp(-touchDistance * touchDistance / 0.0065) * uTouchStrength;
float touchRipple = sin(...) * exp(...) * uTouchStrength;
float touchBoost = 1.0 + touchMask * 2.4 + abs(touchRipple) * 1.1;
```

它的意义是：

```text
越靠近触摸点，折射越强；
拖动越快，环形波纹越明显；
增强只发生在局部，不影响整个泡泡。
```

因此右键拖动时看到的局部背景扭曲和彩色波纹，就是 `touchScale` 和额外 ripple offset 共同作用的结果。

### 2.8 小结

这条公式可以总结为：

```text
refractVec.xy        决定偏移方向
refractionStrength   控制整体折射强弱
faceScale            区分正面/背面 pass 的折射权重
filmScale            让膜厚和边缘影响折射
bubbleScreenRadius   让偏移适配泡泡屏幕大小
edgeScale            增强轮廓区域折射
touchScale           增强右键触摸附近的局部扰动
```

更正式的汇报表述可以是：

```text
该公式将物理折射方向转化为屏幕空间纹理采样偏移，并通过全局折射强度、正背面 pass、薄膜厚度、泡泡尺寸、边缘 Fresnel 和触摸扰动共同调节最终背景扭曲效果。
```

之前副泡泡变黑的问题，本质上和 FBO 采样坐标有关。折射偏移可能让采样坐标越界，或者窗口坐标没有正确映射到更大的 FBO 坐标。当前版本使用 1.3 倍 overscan，并在 shader 中区分“渲染到屏幕”和“渲染到 FBO”的坐标换算，因此主泡泡和副泡泡可以复用同一套折射逻辑。

## 3. 薄膜干涉原理

肥皂泡的颜色主要来自薄膜干涉。液膜厚度通常是数百纳米，和可见光波长接近。当光在空气-液膜界面和液膜-空气界面分别反射时，两束反射光会产生光程差。

对某个波长 `lambda`，相位差可以写成：

```text
phi = 4 * pi * n * d * cos(theta_t) / lambda
```

其中：

- `n` 是薄膜折射率。
- `d` 是薄膜厚度。
- `theta_t` 是光进入薄膜后的折射角。
- `lambda` 是光的波长。

如果两束反射光相位接近，就会增强；如果相位相反，就会抵消。由于不同波长的 `lambda` 不同，同一个膜厚对红光、绿光、蓝光的增强/抵消程度不同，最终就形成彩色条纹。

因此薄膜虹彩颜色主要由两个输入控制：

- 视角项：`NdotV = dot(N, V)`。
- 膜厚：`thickness`。

项目中的动态膜厚不是静态贴图，而是由噪声、排液趋势、时间和右键触摸项共同生成。这样虹彩会缓慢流动，触摸位置附近也会产生局部颜色变化。

## 4. 三种薄膜实现的区别

### 4.1 Kim2012 模式

Kim2012 模式是最直接的实时公式方法。它在 shader 中选取 RGB 三个代表波长，例如：

```text
R: 615 nm
G: 535 nm
B: 465 nm
```

然后分别计算每个波长下的 Fresnel 系数、相位差和反射率。最后把三个波长的结果直接当作 RGB 颜色。

优点：

- 公式直观，适合解释薄膜干涉原理。
- 不依赖外部纹理，参数变化后可以实时响应。

缺点：

- 只采样三个波长，不能代表完整连续光谱。
- 容易产生比较强的 RGB 分离，颜色可能偏“硬”。

适合汇报时说明“干涉色是如何从相位差来的”。

### 4.2 Spectral LUT 模式

Spectral LUT 模式在整体 pipeline 中对应“正面阶段”的薄膜颜色计算模块。前面的流程仍然不变：

```text
backgroundFBO
-> backFBO
-> front pass
-> 计算折射采样 + 薄膜虹彩 + Fresnel + 环境反射
```

LUT 模式只替换其中的“薄膜虹彩怎么从视角和膜厚得到 RGB”这一步。也就是说，折射偏移、Fresnel 边缘增强、cubemap 环境反射仍然沿用同一套 pipeline，只是薄膜反射率不再在 shader 中逐波长实时积分。

Spectral LUT 模式把复杂的光谱计算预先做完，存到一张二维纹理中。运行时 shader 根据：

```text
x = NdotV
y = normalized thickness
```

去查 LUT，直接得到 RGB 结果。

其中：

- `NdotV` 表示法线和视线方向的夹角关系。它决定观察角度，也决定光在薄膜中的有效光程。
- `normalized thickness` 是归一化后的膜厚，用来表示不同位置薄膜厚度变化。
- LUT 纹理的每个像素可以理解成“某个视角 + 某个膜厚下，预先算好的薄膜反射颜色”。

所以它在运行时的计算路径可以写成：

```text
normal, viewDir, thickness
-> NdotV, normalizedThickness
-> sample spectralLUT
-> filmColor
-> 与折射背景、Fresnel、环境反射合成
```

这和前面的 pipeline 是相容的：LUT 并不负责生成泡泡轮廓，也不负责折射背景，它只是给正面 shader 提供一个稳定快速的 `filmColor`。

优点：

- 运行开销最低。
- 颜色稳定，适合实时展示。
- 适合把复杂光谱积分提前离线完成。
- pipeline 中只增加一次纹理采样，适合多泡泡展示。

缺点：

- 外观依赖 LUT 的生成方式。
- 如果要修改折射率、波长采样、颜色匹配函数等物理参数，需要重新生成 LUT。
- LUT 的输入维度有限，难以同时表达更多变量，例如光源方向、粗糙度或复杂偏振状态。

适合汇报时强调“预计算换实时效率”的思想。可以说：LUT 模式把物理计算前移到离线阶段，在实时渲染时只保留查表，因此非常稳定，但灵活性较弱。

### 4.3 Belcour Airy 模式

Belcour Airy 模式同样位于 pipeline 的正面阶段。它也不改变背景 FBO、背面 FBO 和屏幕空间折射的结构，而是替换“薄膜反射率 \(R_\lambda\) 如何计算”这一部分。

前面介绍的薄膜干涉可以理解为上下两个界面的反射光相互叠加。Kim2012 更偏向实时公式和少量代表波长；Spectral LUT 把这个关系预计算成表；Belcour Airy 则在 shader 中显式近似薄膜内部的多次反射。

Airy 模型不是只考虑两束反射光，而是把薄膜内部反复反射、透射形成的复振幅级数考虑进去。简化表达为：


```text
A_lambda = r12 + (t12 * r23 * t21 * exp(i phi))
                 / (1 - r21 * r23 * exp(i phi))
R_lambda = |A_lambda|^2
```

这里每个符号可以这样理解：

- `r12`：第一层界面的直接反射。
- `t12 * r23 * t21`：光进入薄膜、在第二层界面反射、再透出的路径。
- `exp(i phi)`：薄膜内部传播带来的相位变化。
- `1 - r21 * r23 * exp(i phi)`：把薄膜内部多次往返反射压缩成一个级数形式。
- `R_lambda`：该波长最终得到的反射强度。

项目中没有完整实现 Belcour 论文中的微表面预积分，而是在实时 shader 中实现了一个适合肥皂泡展示的简化版本：

- 分别计算 s/p 偏振。
- 采样多个可见光波长。
- 用经验 RGB 权重合成最终颜色。
- 将环境反射也按薄膜反射率染色。

它在项目中的计算路径可以写成：

```text
normal, viewDir, thickness
-> cosTheta, phase(phi)
-> for each wavelength: Airy reflectance
-> spectral weights to RGB
-> filmColor / envTint
-> 与折射背景、Fresnel、环境反射合成
```

因此 Belcour Airy 和 pipeline 的关系是：它提供更连续、更接近光谱效果的薄膜反射颜色；最终画面仍然由“折射背景 + 薄膜颜色 + Fresnel 边缘 + 环境反射”共同决定。

优点：

- 比 Kim2012 的三波长结果更平滑。
- 边缘和环境反射更自然。
- 最适合作为最终 demo 的默认外观。
- 不依赖外部 LUT，运行时参数更容易调节。

缺点：

- 仍然是项目级实时近似。
- 不是完整物理光谱渲染，也不是完整微表面理论实现。
- 比 LUT 模式计算量更高，因此更适合主泡泡或质量优先的最终展示模式。

汇报中可以这样总结三者：

```text
Kim2012：实时公式，容易解释。
Spectral LUT：预计算查表，效率最高。
Belcour Airy：多波长 Airy 近似，最终效果最好。
```

## 5. 动态膜厚与触摸交互

真实泡泡表面会有流动、排液和局部扰动。项目没有完整求解膜厚流体方程，而是在 shader 中用程序化近似：

```text
thickness = baseThickness
          + noisePattern
          + drainageTrend
          + touchMask
          + ripple
```

右键触摸时，鼠标位置会被转成屏幕空间坐标 `uTouchPoint`。shader 计算当前片元到触摸点的距离，构造局部高斯影响区域和环形波纹。这个扰动同时影响：

- 折射偏移：触摸点附近背景扭曲更明显。
- 膜厚：触摸点附近虹彩颜色发生变化。
- 色散：拖动时出现更强的局部彩色波纹。

这使交互不是简单调一个全局参数，而是像真的接触泡泡表面一样产生局部响应。

## 6. DBSTT / Vortex Sheet 动态

DBSTT 论文把肥皂膜看作 vortex sheet。核心思想是不用体积网格模拟泡泡内外空气，而是在薄膜表面上描述速度场的不连续性。

项目中保留了其核心视觉思想：

- 用三角网格表示主泡泡表面。
- 在顶点上维护环量 `Gamma`。
- 根据曲率和表面张力更新环量。
- 通过 Biot-Savart 近似计算诱导速度。
- 推进顶点位置并上传到 OpenGL VBO。

当前实现是单泡泡近似，不支持完整泡沫拓扑变化。但它足以让主泡泡轮廓和法线随时间轻微变化，使折射和虹彩不显得完全静止。

## 7. 多泡泡展示与未来拓展

当前的 18 个副泡泡主要用于展示，不做完整物理模拟。它们复用主泡泡 shader，但使用独立的模型矩阵动画，在风向、上下浮动和轻微缩放下漂移。

未来可以继续扩展：

- 多泡泡之间的碰撞和吸附。
- 多泡泡相互折射和反射。
- 更真实的膜厚流动与排液。
- 泡泡破裂、寿命变化和局部干涸。
- 完整 DBSTT 泡沫拓扑结构，例如 Plateau 边界。
- 更严格的光谱到 RGB 积分，减少经验权重。

如果答辩被问到“为什么现在副泡泡不是物理模拟”，可以回答：当前副泡泡的目标是增强展示构图和空间层次；物理模拟集中在主泡泡上，保证交互和视觉重点清晰。多泡泡相互作用属于后续扩展方向。
