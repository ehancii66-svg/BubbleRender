# BubbleRender：透明物体实时折射与色散渲染系统

本项目面向 CADCG 渲染相关比赛赛题 **“基于鸿蒙的透明物体实时色散渲染系统”**。比赛目标是在给定基础代码的基础上，实现透明泡泡的实时折射、色散、交互和展示效果。

当前阶段我们先在 **Windows 平台** 上搭建桌面 OpenGL 开发框架，用于快速验证渲染算法、调试 Shader、实现折射与色散效果。待核心效果稳定后，再将渲染逻辑迁移回 HarmonyOS / OpenGL ES 工程中。

---

## 一、赛题背景

随着移动端图形硬件和操作系统图形框架的发展，实时渲染技术正在从传统 PC 设备扩展到移动端与鸿蒙设备。透明物体，例如玻璃、水晶、水滴、肥皂泡等，具有复杂的光学行为：

- 光线穿过不同密度介质时会发生折射；
- 不同波长的光折射率不同，会产生色散；
- 透明物体表面会产生反射、高光、边缘光等视觉现象；
- 多个透明物体叠加时，会出现更复杂的视觉交互。

本赛题以“肥皂泡”的折射和色散效果为核心，要求在 HarmonyOS 平台上实现一个实时、可交互、具有视觉表现力的透明物体渲染系统。

---

## 二、比赛要求总结

### 1. 初赛要求

初赛主要考察基础渲染效果是否实现，重点包括：

- 实现泡泡的折射效果；
- 实现泡泡的色散效果；
- 支持通过触摸交互改变折射和色散强度；
- 程序能够稳定运行；
- 具备基本的 Demo 展示能力。

### 2. 复赛要求

复赛要求在初赛基础上进一步完善系统，重点包括：

- 实现多个泡泡之间的折射、色散效果；
- 展示泡泡模型交互；
- 具备完整 UI 和 Demo 演示；
- 系统能够稳定运行；
- 具有清晰的 UI、合理的交互流程和可展示的现场效果；
- 在工程完整性、视觉效果、创新性和现场答辩表现上进一步提升。

### 3. 最终提交材料

参赛队伍需要整理并提交以下内容：

- 项目源码；
- 可运行程序；
- Demo 演示视频；
- 技术文档；
- 项目介绍 PPT。

---

## 三、官方基础代码说明

官方提供的基础程序是一个 HarmonyOS 工程，核心技术路线为：

```text
ArkTS 页面
    ↓
XComponent 原生渲染组件
    ↓
NAPI 调用 C++ 渲染层
    ↓
EGL 创建 OpenGL ES 上下文
    ↓
OpenGL ES 3.0 实时渲染透明球体
```

基础工程已经完成了以下内容：

- 创建 HarmonyOS ArkTS 页面；
- 使用 XComponent 承载 Native 渲染画面；
- 通过 NAPI 调用 C++ 渲染层；
- 使用 EGL 创建 OpenGL ES 3.0 渲染上下文；
- 编译并加载顶点着色器和片元着色器；
- 构建球体网格；
- 支持基本的手指拖动旋转；
- 初步实现透明球体、背景和折射相关渲染流程。

基础程序可以理解为一个“可运行的渲染起点”。后续工作需要在此基础上继续扩展折射、色散、交互和系统功能。

---

## 四、相关技术名词解释

### 1. OpenGL ES

OpenGL ES 是 OpenGL for Embedded Systems 的缩写，可以理解为移动端和嵌入式设备上的 OpenGL。

它主要用于手机、平板、车机、鸿蒙设备等平台上的实时图形渲染。比赛基础代码使用的是 OpenGL ES 3.0。

### 2. OpenGL

OpenGL 是桌面平台常用的图形 API，Windows、Linux、macOS 等平台上都可以使用。

OpenGL 和 OpenGL ES 的渲染思想非常接近，核心概念相同：

- 顶点缓冲；
- Shader；
- 纹理；
- Framebuffer；
- 相机矩阵；
- 光照；
- 透明混合；
- 离屏渲染。

因此，可以先在 Windows 上使用桌面 OpenGL 开发和调试核心渲染算法，再迁移到 OpenGL ES。

### 3. Shader

Shader 是运行在 GPU 上的小程序，通常包括：

- Vertex Shader：顶点着色器，负责处理顶点位置、法线、纹理坐标等；
- Fragment Shader：片元着色器，负责计算每个像素最终显示的颜色。

本项目中的折射、色散、边缘光、高光等视觉效果主要都在 Fragment Shader 中完成。

### 4. EGL

EGL 是 OpenGL ES 和系统窗口之间的连接层。

在 HarmonyOS 工程中，EGL 用于把 OpenGL ES 渲染上下文绑定到 XComponent 提供的原生窗口上。

### 5. XComponent

XComponent 是 HarmonyOS 中用于承载原生图形渲染的组件。

可以把它理解为 ArkTS 页面中的一块“画布”，C++ 层通过 OpenGL ES 把图像渲染到这块区域上。

### 6. NAPI

NAPI 是 ArkTS / JavaScript 与 C++ Native 层之间的桥梁。

ArkTS 页面不能直接调用 C++ 渲染函数，因此需要通过 NAPI 暴露接口，例如：

- initGraphics；
- renderFrame；
- onTouch；
- releaseResource。

---

## 五、为什么当前先在 Windows 上开发

虽然比赛最终要求是在 HarmonyOS 平台上运行，但当前阶段选择先在 Windows 上搭建桌面 OpenGL 框架，原因如下。

### 1. Windows 上调试效率更高

在 Windows 上可以使用：

- Visual Studio；
- CMake；
- GLFW；
- GLAD；
- 桌面 OpenGL；
- RenderDoc 等图形调试工具。

修改 Shader 或 C++ 代码后，可以快速编译、运行和观察效果。

### 2. 避免一开始被鸿蒙环境配置卡住

HarmonyOS 工程涉及：

- DevEco Studio；
- HarmonyOS SDK；
- 模拟器；
- 真机调试；
- ArkTS；
- XComponent；
- NAPI；
- EGL；
- OpenGL ES；
- rawfile 资源读取。

这些内容对工程集成很重要，但不是早期算法开发的核心。如果一开始就从鸿蒙环境入手，容易把大量时间花在配置、编译和平台适配上。

### 3. 渲染算法可以跨平台迁移

透明物体渲染的核心逻辑主要包括：

- 球体网格构造；
- 相机控制；
- Shader 编写；
- 背景纹理采样；
- 折射向量计算；
- 色散采样；
- Framebuffer 离屏渲染；
- 多泡泡绘制。

这些逻辑在 Windows OpenGL 和 HarmonyOS OpenGL ES 中基本一致。

后续只需要调整：

- Shader 版本；
- OpenGL API 细节；
- 纹理加载方式；
- 窗口和上下文创建方式；
- 平台相关资源读取方式。

---

## 六、当前 Windows 阶段的工作内容

当前 Windows 阶段的目标不是直接替代鸿蒙工程，而是搭建一个便于开发和调试的桌面端渲染实验框架。

### 阶段一：搭建最小 OpenGL 框架

目标：

- 在 Windows 上打开一个 OpenGL 窗口；
- 完成基础 OpenGL 初始化；
- 清屏并绘制简单几何体；
- 确认 GLFW、GLAD、CMake、Visual Studio 环境可用。

计划使用：

```text
C++17
CMake
Visual Studio 2022
GLFW
GLAD
GLM
stb_image
```

### 阶段二：移植基础球体渲染

目标：

- 从官方基础代码中提取球体网格生成逻辑；
- 渲染一个基础球体；
- 加入 MVP 矩阵；
- 加入相机控制；
- 支持鼠标拖动旋转视角或旋转物体。

需要重点复用的模块：

```text
render/model.*
render/camera.*
render/shader.*
shader/*.h
```

暂时不复用的模块：

```text
napi_init.cpp
Index.ets
EGL 初始化代码
XComponent 相关代码
HarmonyOS rawfile 资源读取代码
```

### 阶段三：实现单泡泡折射

目标：

- 先渲染背景；
- 将背景渲染结果保存到纹理；
- 渲染透明球体；
- 在泡泡 Shader 中根据法线和视线方向计算折射方向；
- 对背景纹理进行偏移采样；
- 得到“透过泡泡看背景发生扭曲”的效果。

核心思想：

```text
背景图像 → 背景纹理
球体法线 + 视线方向 → 折射方向
折射方向 → 背景纹理采样偏移
采样颜色 → 泡泡颜色
```

### 阶段四：实现色散效果

目标：

- 模拟不同颜色光的不同折射率；
- 对 R、G、B 三个通道分别使用略有差异的折射偏移；
- 产生泡泡边缘的彩色分离效果。

基本思路：

```text
R 通道使用较强折射偏移
G 通道使用中等折射偏移
B 通道使用较弱折射偏移
```

或者：

```text
iorR ≠ iorG ≠ iorB
```

最终效果应表现为泡泡边缘出现彩虹状色散。

### 阶段五：实现多个泡泡

目标：

- 支持多个不同位置、不同大小的泡泡；
- 每个泡泡拥有独立模型矩阵；
- 多个泡泡可以同时显示；
- 初步处理泡泡之间的遮挡关系和透明混合问题。

后续可以进一步研究：

- 多层折射；
- 屏幕空间折射；
- 深度排序；
- 多泡泡交互；
- 泡泡之间的视觉叠加。

### 阶段六：加入交互控制

目标：

- 鼠标拖动旋转视角；
- 鼠标滚轮缩放；
- 键盘调整参数；
- 可选 UI 面板调节折射强度、色散强度、泡泡数量、背景类型等。

建议参数：

```text
折射强度 refractionStrength
色散强度 dispersionStrength
泡泡透明度 alpha
菲涅尔强度 fresnelPower
泡泡数量 bubbleCount
背景切换 backgroundMode
```

### 阶段七：迁移回 HarmonyOS

当 Windows 端核心效果稳定后，再进行 HarmonyOS 迁移。

迁移重点：

- 将桌面 OpenGL Shader 改回 OpenGL ES 3.0 版本；
- 将 GLFW 窗口逻辑替换为 XComponent；
- 将桌面 OpenGL 上下文替换为 EGL；
- 将普通文件读取替换为 HarmonyOS rawfile 资源读取；
- 将鼠标交互替换为触摸交互；
- 将参数控制接入 ArkTS 页面 UI。

---

## 七、Windows 与 HarmonyOS 工程的关系

本项目不是要放弃 HarmonyOS，而是采用“两阶段开发”策略：

```text
第一阶段：Windows 快速验证渲染算法
第二阶段：HarmonyOS 完成平台适配和最终展示
```

二者关系如下：

```text
Windows 端：
用于算法开发、Shader 调试、视觉效果验证。

HarmonyOS 端：
用于最终比赛提交、平台展示、现场演示。
```

可复用部分：

```text
球体网格生成
相机数学
Shader 核心算法
折射计算
色散计算
多泡泡绘制逻辑
参数设计
```

需要替换部分：

```text
窗口创建
OpenGL 上下文创建
资源读取
输入事件
UI 交互
平台日志
工程构建方式
```

---

## 八、当前开发计划

### Step 1：Windows 环境准备

安装：

```text
Visual Studio 2022
CMake
Git
vcpkg
```

通过 vcpkg 安装：

```text
glfw3
glad
glm
stb
```

### Step 2：建立 Windows 桌面端工程

新增目录：

```text
windows/
  CMakeLists.txt
  main.cpp
  shader/
  src/
  assets/
```

### Step 3：实现最小窗口

完成：

- GLFW 初始化；
- 创建 OpenGL 窗口；
- GLAD 加载 OpenGL 函数；
- 设置 viewport；
- 每帧清屏；
- 程序正常退出。

### Step 4：绘制球体

完成：

- 创建球体 mesh；
- 编写基础 vertex shader；
- 编写基础 fragment shader；
- 使用 MVP 矩阵；
- 显示一个可见球体。

### Step 5：实现背景与离屏渲染

完成：

- 创建背景；
- 创建 Framebuffer；
- 将背景渲染到纹理；
- 将背景纹理传入泡泡 Shader。

### Step 6：实现折射和色散

完成：

- 根据法线和视线方向计算折射；
- 对背景纹理进行偏移采样；
- 为 RGB 通道使用不同折射参数；
- 得到彩色边缘和透明泡泡效果。

### Step 7：完善 Demo 效果

完成：

- 多泡泡；
- 鼠标交互；
- 参数调节；
- 背景切换；
- 更好的视觉风格；
- 稳定帧率。

### Step 8：迁移回 HarmonyOS

完成：

- 替换平台相关代码；
- 接入 XComponent；
- 接入 NAPI；
- 使用 EGL / OpenGL ES；
- 在 DevEco Studio 模拟器或真机中运行；
- 录制 Demo 视频。

---

## 九、项目目标

本项目最终希望实现一个具有以下特点的实时渲染系统：

- 透明泡泡具有明显折射效果；
- 泡泡边缘具有自然色散；
- 支持多个泡泡同时渲染；
- 支持触摸或鼠标交互；
- 运行稳定，帧率流畅；
- UI 简洁，Demo 展示效果清晰；
- 代码结构清楚，便于迁移和答辩说明。

---

## 十、当前阶段结论

当前阶段的重点不是立即配置 HarmonyOS 模拟器，而是先在 Windows 上快速完成渲染核心：

```text
窗口 → 球体 → 背景 → 折射 → 色散 → 多泡泡 → 交互
```

等 Windows 端效果成熟后，再迁移到 HarmonyOS 工程中，完成比赛最终要求。
