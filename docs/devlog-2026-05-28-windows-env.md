# 开发日志：Windows 环境准备与 CMake 配置修正

**日期**：2026-05-28  
**目标**：完成 Windows 端开发环境准备，确保 VS Code + CMake Tools 可以使用 VS2022、vcpkg 和项目依赖正常配置并生成 `BubbleRender.exe`。

---

## 一、做了什么

根据 README 中 Step 1 的要求，完成 Windows 环境准备：

```text
Visual Studio 2022
CMake
Git
vcpkg
```

并通过 vcpkg 安装项目所需依赖：

```text
glfw3
glad
glm
stb
```

本次 vcpkg 放在项目目录内：

```text
vcpkg/
```

这样可以避免依赖机器上的全局 `C:/dev/vcpkg` 路径，项目配置也更容易复现。

---

## 二、工具链确认

本机已确认可用：

| 工具                         | 状态                       |
| ---------------------------- | -------------------------- |
| Visual Studio 2022 Community | 已安装                     |
| MSVC x64 编译器              | 已由 vcpkg 和 CMake 检测到 |
| Git                          | 已安装                     |
| CMake                        | 已安装                     |
| vcpkg                        | 已克隆并 bootstrap         |

Windows 11 64 位环境下，VS Code CMake Kit 应选择：

```text
Visual Studio Community 2022 Release - amd64
```

`amd64` 表示使用 64 位编译器生成 64 位程序，和当前 `x64-windows` vcpkg triplet 匹配。

---

## 三、VS Code CMake 配置修正

原 `.vscode/settings.json` 中写死了旧的 vcpkg 路径：

```text
C:/dev/vcpkg/scripts/buildsystems/vcpkg.cmake
```

该路径在当前机器上不存在，导致 CMake 配置时报错：

```text
Could not find toolchain file:
C:/dev/vcpkg/scripts/buildsystems/vcpkg.cmake
```

已修正为项目内路径：

```json
"-DCMAKE_TOOLCHAIN_FILE=${workspaceFolder}/vcpkg/scripts/buildsystems/vcpkg.cmake"
```

相关文件：

```text
.vscode/settings.json
```

---

## 四、构建验证

已使用 vcpkg toolchain 完成 CMake 配置：

```powershell
cmake -S windows -B windows\build -DCMAKE_TOOLCHAIN_FILE=C:/Users/funhxj/Desktop/BubbleRender/vcpkg/scripts/buildsystems/vcpkg.cmake -G "Visual Studio 17 2022"
```

配置结果：

```text
Configuring done
Generating done
Build files have been written to: windows/build
```

VS Code CMake Tools 中 Debug 构建也已通过，生成文件：

```text
windows/build/Debug/BubbleRender.exe
```

命令行 Release 构建也曾验证通过，生成文件：

```text
build/windows-vcpkg/Release/BubbleRender.exe
```

---

## 五、.gitignore 更新

由于 `vcpkg/` 是本地依赖管理工具目录，体积较大，不应提交到仓库。

已在 `.gitignore` 中新增：

```gitignore
vcpkg/
```

---

## 六、当前警告说明

构建过程中出现 `C4819`：

```text
warning C4819: 该文件包含不能在当前代码页(936)中表示的字符。
请将该文件保存为 Unicode 格式以防止数据丢失
```

这是 MSVC 在中文系统代码页 936 下读取源码时产生的编码警告。当前程序已经成功编译生成，警告不影响运行。

后续可以在 `windows/CMakeLists.txt` 中为 MSVC 增加 UTF-8 编译选项：

```cmake
if(MSVC)
    add_compile_options(/utf-8)
endif()
```

或者统一将源码文件保存为 UTF-8 编码。

---

## 七、当前结论

Windows 端 Step 1 环境准备已经完成：

```text
VS2022 + CMake + Git + vcpkg + glfw3/glad/glm/stb
```

项目当前可以在 Windows 上完成配置、生成和编译。下一步可以继续在 Windows 桌面端实现泡泡渲染算法，再按 OpenGL ES 3.0 兼容要求迁移回 HarmonyOS。

---
