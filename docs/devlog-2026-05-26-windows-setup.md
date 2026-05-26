# 开发日志：Windows 桌面端框架搭建

**日期**：2026-05-26  
**目标**：在 Windows 上搭建桌面 OpenGL 渲染框架，用于透明泡泡折射/色散算法的快速验证。

---

## 一、做了什么

在 `windows/` 目录下新建了一个独立的桌面 OpenGL 工程，从官方 HarmonyOS 基础代码中提取并移植了渲染核心，用 GLFW + GLAD 替换了 EGL + OpenGL ES。

### 新建文件清单

```
windows/
  CMakeLists.txt                        # CMake 构建（vcpkg + C++17）
  main.cpp                              # 入口：窗口、渲染循环、输入交互
  render/
    camera.h / camera.cpp               # 相机（移除 HarmonyOS hilog 依赖）
    mesh.h                              # 网格/顶点/VAO 管理
    model.h / model.cpp                 # 球体和天空盒几何体生成
    shader.h / shader.cpp               # Shader 编译链接（GLES → GLAD）
    stb_image.h                         # 图片加载（直接复制官方）
  shader/
    shader_background.h                 # 背景球 Shader（300 es → 330 core）
    shader_quad.h                       # 全屏四边形 Shader
    shader_refraction.h                 # 折射球 Shader（300 es → 330 core）
    shader_skybox.h                     # 天空盒 Shader（300 es → 330 core）
  assets/skybox/                        # 天空盒 6 面纹理（从官方复制）
```

### 原始文件

`refraction/` 下所有文件未做任何修改。

---

## 二、平台层对比

| 层级 | HarmonyOS 官方 | Windows 桌面端 |
|------|---------------|---------------|
| 窗口 | XComponent | GLFW |
| 图形 API | OpenGL ES 3.0 | OpenGL 3.3 Core |
| GL 加载 | EGL | GLAD |
| 上下文创建 | `eglCreateContext` | `glfwCreateWindow` |
| 资源读取 | HarmonyOS rawfile | 文件系统 `stbi_load` |
| 输入 | 触摸（NAPI onTouch） | 鼠标 + 键盘（GLFW 回调） |
| 构建 | HarmonyOS NDK CMake | VS2022 + vcpkg CMake |
| 日志 | `OH_LOG_ERROR/DEBUG` | `std::cerr` / `std::cout` |

---

## 三、Shader 改动

每个 Shader 统一做了三项修改：

| 项目 | 原 (OpenGL ES 3.0) | 新 (Desktop GL 3.30) |
|------|---------------------|----------------------|
| 版本声明 | `#version 300 es` | `#version 330 core` |
| 精度限定 | `precision highp float;` | 删除（桌面 GL 默认 highp） |
| 纹理采样 | `texture2D(...)` | `texture(...)` |

折射算法（`refract()`、菲涅尔、高光）完全不变。

---

## 四、渲染管线（3-Pass）

原封不动保留了官方 `napi_init.cpp` 中的渲染逻辑：

```
Pass 1 → backgroundFBO：渲染天空盒 + 5×5 背景小球
Pass 2 → backFaceFBO：渲染折射球背面（Cull Front），采样 backgroundTexture
Pass 3 → 屏幕：渲染天空盒 + 背景小球，再渲染折射球正面（Cull Back），采样 backFaceTexture
```

---

## 五、交互控制

| 操作 | 功能 | 实现位置 |
|------|------|----------|
| 鼠标拖拽 | 旋转视角（arcball 相机） | `CursorPosCallback` |
| 鼠标滚轮 | 缩放（调整相机距离，范围 2~20） | `ScrollCallback` |
| R / T | Fresnel Power -/+ | `KeyCallback` |
| F / G | Diffuseness -/+ | `KeyCallback` |
| V / B | Shininess -/+ | `KeyCallback` |
| ESC | 退出 | `KeyCallback` |

---

## 六、已知问题

### 1. 缩放过大/过小时出现黑边

**原因**：FBO 清屏色为黑色，纹理 wrap 模式为 `GL_CLAMP_TO_EDGE`。当折射 UV 偏移超出 `[0,1]` 范围时，采样到纹理边缘的黑色像素。

**解决方向**（后续优化）：
- 限制缩放范围避免极端情况
- 或将背景纹理 wrap 改为 `GL_MIRRORED_REPEAT`

### 2. C4819 编译警告

MSVC 报告文件包含不能在代码页 936 中表示的字符（文件中有中文注释但未保存为 UTF-8 BOM）。不影响编译和运行。

---

## 七、参数说明

| 参数 | 物理意义 | 调大效果 | 调小效果 | 默认值 |
|------|----------|----------|----------|--------|
| Fresnel Power | 边缘菲涅尔光晕锐度 | 边缘光更细更锐 | 边缘光更宽更柔 | 8.0 |
| Diffuseness | 漫反射强度 | 球体更亮 | 球体更暗 | 0.2 |
| Shininess | 高光集中度 | 亮斑更小更尖锐 | 亮斑更柔和分散 | 15.0 |

---
