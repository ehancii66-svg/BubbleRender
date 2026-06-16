# Soap Bubble Rendering Windows

本文档说明 Windows 版本的源码结构、环境配置、编译运行方式，以及哪些文件需要提交到仓库。

## 1. 提交结构

仓库根目录建议保留：

```text
BubbleRender/
  windows/                 # Windows OpenGL 源代码与运行资源
  docs/                    # 开发日志、论文方法说明
  papers/                  # 参考论文 PDF
  tools/                   # 辅助脚本，例如 LUT 生成
  homepage/                # GitHub 首页展示材料
  demo.mp4                 # 最终演示视频
  readme.md                # 项目总说明
```

不需要提交的本地内容：

```text
vcpkg/                     # 本地依赖环境
build/                     # 根目录构建产物
windows/build/             # Windows 版本构建产物
out/
report/
report.zip
demo/                      # 本地字幕、剪辑中间文件
demo.raw/
.asr_cache/
.asr_pkgs/
.venv-asr/
.venv-asr312/
.tmp/
.vscode/
```

## 2. Windows 目录

```text
windows/
  CMakeLists.txt           # CMake 构建脚本
  CMakePresets.json        # CMake Tools / 命令行 preset
  vcpkg.json               # vcpkg manifest，声明 glfw3 / glad / glm
  main.cpp                 # 程序入口、渲染循环、交互、FBO 管线
  read.md                  # 本文档

  assets/
    skybox/                # 天空盒贴图，运行必须
    lut/                   # 薄膜干涉 LUT，运行必须

  render/
    camera.*               # 相机控制
    model.*                # 球体、天空盒、动态网格模型创建
    shader.*               # shader 编译与 uniform 设置
    mesh.h                 # OpenGL 网格封装
    stb_image.h            # 图片加载库

  shader/
    shader_refraction.h    # 折射、Fresnel、环境反射、薄膜干涉
    shader_background.h    # 背景参照物 shader
    shader_skybox.h        # 天空盒 shader
    shader_quad.h          # 备用屏幕 quad shader

  simulation/
    vortex_sheet.*         # DBSTT / vortex sheet 主泡泡表面形变仿真
```

## 3. 环境要求

推荐环境：

```text
Windows 10 / Windows 11
Visual Studio 2022
CMake 3.21 或更高版本
vcpkg
支持 OpenGL 3.3 Core Profile 的显卡驱动
```

Visual Studio 2022 安装时建议勾选：

```text
Desktop development with C++
MSVC v143 C++ build tools
Windows 10/11 SDK
CMake tools for Windows
```

项目使用 C++17，依赖如下：

```text
glfw3
glad
glm
```

`stb_image.h` 已包含在 `windows/render/` 中，不需要额外安装。

## 4. 配置 vcpkg

推荐把 vcpkg 安装在项目目录外，例如：

```powershell
cd D:\dev
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg
.\bootstrap-vcpkg.bat
```

设置当前终端环境变量：

```powershell
$env:VCPKG_ROOT = "D:\dev\vcpkg"
```

也可以在系统环境变量中永久新增：

```text
VCPKG_ROOT = D:\dev\vcpkg
```

依赖由 `windows/vcpkg.json` 声明。执行 CMake configure 时，vcpkg 会根据 manifest 自动安装所需依赖。

## 5. 编译运行

在仓库根目录执行：

```powershell
$env:VCPKG_ROOT = "D:\dev\vcpkg"
cmake --preset default -S windows
cmake --build windows/build --config Release
```

运行：

```powershell
cd windows/build/Release
.\BubbleRender.exe
```

CMake 会在构建后把 `windows/assets/` 复制到可执行文件目录：

```text
windows/build/Release/assets/
```

如果程序提示找不到 skybox 或 LUT，请检查该目录是否存在。

## 6. 操作方式

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

窗口标题会实时显示 FPS。

## 7. 当前功能

当前 Windows 版本实现：

```text
1. OpenGL 3.3 实时渲染窗口
2. 天空盒环境贴图
3. 屏幕空间折射 FBO 管线
4. 主泡泡与副泡泡之间的一层双向折射近似
5. Fresnel 边缘高光与环境反射
6. Kim2012 / Spectral LUT / Belcour Airy 三种薄膜干涉模式
7. 动态膜厚、虹彩条纹与右键触摸扰动
8. DBSTT / vortex sheet 主泡泡表面形变仿真
9. 18 个 Belcour Airy 副泡泡随风轻微漂移
10. 键盘参数调节、鼠标交互与标题栏 FPS 显示
```
