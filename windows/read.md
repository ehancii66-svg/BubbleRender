# Windows 版编译与项目结构说明

本目录是项目的 Windows OpenGL 版本，程序名称为：

```text
Soap Bubble Rendering - Thin Film + DBSTT
```

该版本用于展示肥皂泡的实时折射、薄膜虹彩、环境反射、右键触摸波纹以及 DBSTT 泡泡表面形变模拟。

## 1. 项目结构

```text
windows/
  CMakeLists.txt          # CMake 构建脚本
  CMakePresets.json       # 可选的 CMake preset，默认使用项目根目录下的 vcpkg
  main.cpp                # 程序入口、OpenGL 初始化、渲染循环、交互控制
  read.md                 # 本说明文件

  render/
    camera.h/.cpp         # 相机控制
    model.h/.cpp          # 球体、天空盒、动态网格模型创建
    mesh.h                # OpenGL VAO/VBO/EBO 网格封装
    shader.h/.cpp         # Shader 编译与 uniform 设置
    stb_image.h           # 图片加载库

  shader/
    shader_refraction.h   # 主泡泡 shader：折射、薄膜干涉、环境反射、触摸波纹
    shader_background.h   # 背景小球 shader
    shader_skybox.h       # 天空盒 shader
    shader_quad.h         # 保留的屏幕 quad shader

  simulation/
    vortex_sheet.h/.cpp   # DBSTT / vortex sheet 泡泡表面形变模拟

  assets/
    skybox/               # 天空盒 6 面贴图，运行必需
    lut/
      thinfilm_belcour_bubble.png   # 薄膜虹彩 LUT，运行必需
```

## 2. 必须保留的文件

源码交付时，至少需要保留：

```text
windows/CMakeLists.txt
windows/CMakePresets.json
windows/main.cpp
windows/render/
windows/shader/
windows/simulation/
windows/assets/
windows/read.md
```

其中 `windows/assets/` 必须保留。程序运行时会读取：

```text
assets/skybox/right.jpg
assets/skybox/left.jpg
assets/skybox/top.jpg
assets/skybox/bottom.jpg
assets/skybox/front.jpg
assets/skybox/back.jpg
assets/lut/thinfilm_belcour_bubble.png
```

## 3. 不需要随源码提交的内容

以下目录或文件不是编译源码所必需，通常不建议提交：

```text
windows/build/       # CMake / Visual Studio 构建产物
windows/out/         # 临时输出目录，如存在可忽略
```

如果只交付 Windows 版本，项目根目录下的 HarmonyOS 工程 `refraction/` 也不是 Windows 版运行必需。

项目根目录下的 `tools/` 不是 Windows demo 编译和运行必需。它主要用于重新生成 LUT 等辅助资源；当前程序直接使用已经生成好的：

```text
windows/assets/lut/thinfilm_belcour_bubble.png
```

因此助教只编译和运行 Windows demo 时，不需要执行 `tools/` 中的脚本。

## 4. 编译环境

推荐环境：

```text
Windows 10/11
Visual Studio 2022，需安装 Desktop development with C++
CMake 3.16 或更高版本
vcpkg
支持 OpenGL 3.3 的显卡驱动
```

项目使用 C++17，依赖如下：

```text
glfw3
glad
glm
```

`stb_image.h` 已包含在 `windows/render/` 中，不需要额外安装。

## 5. 依赖安装

如果项目根目录下没有 `vcpkg/`，可以自行安装 vcpkg。示例：

```powershell
git clone https://github.com/microsoft/vcpkg.git
.\vcpkg\bootstrap-vcpkg.bat
.\vcpkg\vcpkg install glfw3 glad glm --triplet x64-windows
```

如果使用自己的 vcpkg 路径，后续 CMake 命令中的 `CMAKE_TOOLCHAIN_FILE` 需要改成对应路径。

## 6. CMake 配置与编译

在项目根目录执行：

```powershell
cmake -S windows -B windows/build -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=.\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build windows/build --config Release
```

编译成功后，可执行文件位于：

```text
windows/build/Release/BubbleRender.exe
```

CMake 会在构建后自动把 `windows/assets/` 复制到可执行文件所在目录：

```text
windows/build/Release/assets/
```

## 7. 运行方式

进入 Release 目录运行：

```powershell
cd windows/build/Release
.\BubbleRender.exe
```

如果提示找不到 skybox 或 LUT，请检查 `BubbleRender.exe` 同级目录下是否存在：

```text
assets/skybox/
assets/lut/thinfilm_belcour_bubble.png
```

## 8. 运行时按键

```text
View
  Left drag      Rotate camera
  Right drag     Touch ripple on bubble
  Mouse wheel    Zoom in / out

Appearance
  R / T          Fresnel edge power       - / +
  Y / H          Refraction strength      - / +
  U / J          Edge distortion          - / +
  O / P          Environment reflection   - / +

Thin Film
  L              Cycle mode: Kim2012 / LUT / Belcour Airy
  N / M          Film thickness (nm)      - / +
  1 / 2          Thickness variation      - / +

Simulation
  Z              Pause / resume
  X              Reset bubble
  3 / 4          Surface tension          - / +

  ESC            Quit
```

## 9. 当前实现内容

当前 Windows 版本实现了：

```text
1. OpenGL 3.3 桌面窗口与渲染循环
2. 三 pass 屏幕空间折射管线
3. 天空盒环境贴图
4. 背景小球阵列，用于观察折射扭曲
5. Kim2012 / LUT / Belcour Airy 三种薄膜虹彩模式
6. Fresnel 边缘光与环境反射
7. 右键拖动触发局部膜厚和折射波纹
8. DBSTT / vortex sheet 单泡泡表面形变模拟
```

## 10. 常见问题

### 编译时出现 C4819 编码警告

Visual Studio 可能会提示：

```text
warning C4819: 该文件包含不能在当前代码页中表示的字符
```

这是部分注释中存在非当前代码页字符导致的警告，不影响程序编译和运行。

### CMake 找不到 glfw3 / glad / glm

通常是 vcpkg toolchain 路径没有传对。请确认 CMake 命令中：

```text
-DCMAKE_TOOLCHAIN_FILE=...\vcpkg\scripts\buildsystems\vcpkg.cmake
```

指向真实存在的 vcpkg 路径，并且已经安装：

```powershell
vcpkg install glfw3 glad glm --triplet x64-windows
```

### 程序启动后资源加载失败

请从 `windows/build/Release/` 目录运行程序，或确保 `BubbleRender.exe` 同级目录下存在 `assets/` 文件夹。
