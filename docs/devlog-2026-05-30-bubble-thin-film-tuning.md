# 2026-05-30 泡泡薄膜效果调优日志

## 背景

上一版 Kim2012 初级实现已经把薄膜干涉虹彩接入了 Windows OpenGL demo，但整体视觉仍然偏向“透明玻璃球”：折射偏移较强，前后表面都会叠加比较明显的虹彩、高光和边缘白光。

肥皂泡的真实物理特征更接近一层极薄皂膜，而不是一个实心透明球体。因此这次修改重点是：

- 降低折射强度，减少实心玻璃球式的强背景扭曲。
- 区分正面 pass 和背面 pass，让背面更像“穿过薄膜后的中间采样”，避免前后表面重复把颜色堆得太厚。

## 修改文件

- `windows/main.cpp`
- `windows/shader/shader_refraction.h`

## 1. 降低默认折射强度

原参数偏强，适合展示透明球体折射，但不像薄泡膜：

```cpp
g_RefractionStrength = 4.0f;
g_EdgeDistortionBoost = 2.2f;
g_MaxOffsetRatio = 1.5f;
```

调整为：

```cpp
g_RefractionStrength = 1.4f;
g_EdgeDistortionBoost = 1.7f;
g_MaxOffsetRatio = 0.7f;
```

含义：

- `RefractionStrength` 降低整体背景偏移。
- `EdgeDistortionBoost` 保留边缘稍强的折射感，但不再夸张。
- `MaxOffsetRatio` 限制最大采样偏移，减少球体边缘过度扭曲。

同时收窄了键盘调参上限：

```cpp
RefractionStrength <= 4.0
EdgeDistortionBoost <= 3.0
MaxOffsetRatio <= 1.2
```

这样仍然可以交互调试，但默认和上限都更偏泡泡薄膜效果。

## 2. 区分正面和背面 pass

原先 Pass 2 和 Pass 3 使用同一套 shader 参数，shader 不知道当前正在渲染正面还是背面。

这次给 `SetRefractUniforms` 增加了参数：

```cpp
auto SetRefractUniforms = [&](bool isBackFace)
```

并传入 uniform：

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

## 3. 背面法线翻转

shader 中新增：

```glsl
uniform int uIsBackFace;
```

并在片元阶段处理背面法线：

```glsl
vec3 normal = normalize(vWorldNormal);
if (uIsBackFace == 1) {
    normal = -normal;
}
```

原因：背面 pass 实际上是在画球壳的另一侧。如果继续使用外法线，折射方向、Fresnel 角度和高光计算会和“内侧表面”不一致。翻转法线后，背面更接近薄膜内侧的表面行为。

## 4. 背面贡献降低

背面 pass 现在只保留较弱的折射、虹彩和高光：

```glsl
float surfaceRefractionScale = (uIsBackFace == 1) ? 0.35 : 1.0;
float surfaceColorScale = (uIsBackFace == 1) ? 0.18 : 1.0;
```

折射偏移：

```glsl
vec2 offsetPixels = refractVec.xy
    * uRefractionStrength
    * surfaceRefractionScale
    * uSpherePixelRadius
    * edgeBoost;
```

虹彩：

```glsl
vec3 iridescence = filmReflectance * 1.9 * surfaceColorScale;
```

高光和边缘白光：

```glsl
color += specularLight * 0.35 * surfaceColorScale;
color = mix(color, vec3(1.0), fresnelTerm * 0.12 * surfaceColorScale);
```

这样 Pass 2 更像给 Pass 3 提供一个轻微折射后的背景结果，而不是又完整渲染一遍可见泡泡表面。

## 5. 透明度调整

原透明度中心和边缘都偏重：

```glsl
float transparency = 1.0 - fresnelTerm * 0.5;
```

调整为：

```glsl
float transparency = mix(0.97, 0.78, fresnelTerm);
```

效果目标：

- 泡泡中心几乎透明，只保留轻微虹彩。
- 轮廓和掠射角区域更明显。
- 整体不再像厚玻璃球。

## 当前效果定位

这次修改没有改变球体几何。主泡泡仍然是一个单层经纬球面网格：

```cpp
g_RefractModel = Model::CreateSphere(1.5f, 64, 32, true);
```

这对泡泡是合理的第一版实现，因为真实皂膜厚度是纳米级，没有必要在几何上做出可见厚度。当前方向是使用单层球壳作为表面，在 shader 中用膜厚、干涉、Fresnel 和轻微折射模拟泡泡。

后续如果需要进一步增强真实感，可以继续做：

- 加入 cubemap 环境反射，让泡泡表面真正反射天空盒。
- 用更物理的薄膜前后界面模型替代当前经验缩放。
- 引入膜厚沿重力方向的渐变，让顶部更薄、底部更厚。
- 增加局部流动条纹或破裂前的薄区。

## 验证

已运行：

```powershell
cmake --build windows\build --config Release
```

结果：

- 编译通过。
- 生成 `windows/build/Release/BubbleRender.exe`。
- 仍有原先存在的 MSVC `C4819` 中文编码警告，不影响本次修改。
