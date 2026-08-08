# 鸿蒙端有状态泡泡融合迁移日志（2026-08-08）

## 迁移目标

本次以 `windows/` 当前最终实现为基准，将聊天过程中形成的泡泡融合核心算法迁移到鸿蒙工程 `refraction/entry/src/main/cpp/`。迁移前，鸿蒙端的 `BuildFusionSurfaceVertices` 仍在每帧用 `relaxationProgress` 在双球轮廓与目标球之间做 `glm::mix`，稳定时还会切换对象所使用的程序化薄膜坐标，因此本质上仍是 morph。

迁移后的主要原则是：

- 拓扑连接时只创建一次闭合融合经线；
- 后续帧持续保存并积分同一批经线节点的位置和速度；
- 最终球形由固定体积下表面能下降自然产生，不采样目标球；
- 稳定判定只移交模拟所有权，不更换可见网格、VAO 或视觉身份；
- 膜厚、相位、随机种子、时间原点及纹理坐标基跨越整个生命周期。

## 已迁移的算法

### 1. 有状态轴对称表面

新增 `simulation/fusion_profile.h/.cpp`。每个经线节点持续保存：

- `z/r`；
- `vz/vr`；
- 等效质量；
- 轴对称总曲率；
- 法向力与体积修正诊断值；
- 随材料环输运的薄膜方向和光学半径；
- 仅供轨迹诊断使用的初始位置。

`AdvanceFusionProfile` 使用半隐式、可分子步的时间积分。主更新由当前曲面状态产生，接口中没有目标球、目标轮廓或插值进度参数。

### 2. 表面张力、欠阻尼运动和体积约束

求解器包含：

- 子午曲率与旋转方向曲率；
- 主要沿法线施加的等效表面张力；
- 保存惯性的节点速度；
- 整体毛细低阶模态加速度，而不是位置 morph；
- 均匀速度阻尼和抑制单节点锯齿的经线黏性；
- 以旋转面积/体积梯度为权重的全局压力速度修正；
- 小幅位置级体积漂移投影；
- 最大时间步、最大子步数和最大节点速度限制。

集中参数位于 `napi_init.cpp` 的 `kFusionProfileConfig`，默认值与 Windows 端保持一致：

| 参数 | 默认值 | 作用 |
| --- | ---: | --- |
| `surfaceTension` | 0.180 | 等效表面张力/面积恢复强度 |
| `velocityDamping` | 1.00 | 整体黏性耗散 |
| `velocityLaplacianDamping` | 2.5 | 高频速度模态阻尼 |
| `globalModeFrequency` | 2.10 | 主要毛细回弹频率 |
| `globalModeDampingRatio` | 0.40 | 欠阻尼比 |
| `initialNeckExpansionSpeed` | 0.42 | 破膜时颈部径向初速度 |
| `volumeConstraintStrength` | 0.92 | 全局体积投影强度 |
| `maximumTimeStep` | 1/480 s | 物理子步上限 |
| `maximumSubsteps` | 16 | 每帧最大子步数 |

### 3. 初始闭合轮廓和连接颈

`InitializeFusionProfile` 已从 Windows 端迁入：

- 用两个球冠和两段三次 Bézier 桥接曲线构造初始闭合经线；
- 桥接端点匹配球冠切线，达到至少 C1 连续；
- 颈部是显式采样点，不再用 `min/max` 将两条球弧硬拼接；
- 初始化后仅执行三次局部、保体积曲率公平化，用于去除离散折角；
- 保存实际闭合界面体积作为守恒目标，不在融合后注入“缺失体积”；
- 以气体体积加权中心建立融合中心，并保存中心速度；
- 破膜时只在颈部附近施加连续分布的初始径向速度。

经线使用 97 个节点，旋转网格使用 96 个角向分段。采样指标对颈部曲率加权，使小颈部和两端球冠都具有足够分辨率。

### 4. 极点、曲率和重采样

求解器对左右极点采用专门边界条件：

- `r = 0`；
- `vr = 0`；
- 极点只能沿融合轴运动；
- 使用光滑旋转曲面的极限曲率，不直接除以接近零的半径；
- 极点附近实施抛物球冠光滑约束。

节点分布恶化时才触发曲率加权弧长重采样。位置、速度和薄膜材料坐标一起连续插值，随后重新执行体积投影；这不是目标形状重建。

### 5. 网格与法线

`render/contact_geometry.cpp` 现在直接旋转当前 `fusionProfile` 生成顶点：

- 顶点位置只来自当前节点 `(z_i,r_i)`；
- 法线使用弧长一致的经线切线；
- 左右极点各自只使用一个共享顶点；
- 极点三角形不再由一圈重合顶点产生退化面；
- 薄膜方向和局部光学半径随材料节点输运；
- 删除了融合阶段到目标球的 `glm::mix`、目标球法线混合和局部径向体积补偿。

`render/mesh.h` 新增光学半径顶点属性（location 6）。

### 6. 持久视觉状态与稳定交接

`BubbleVisualState` 已加入鸿蒙端，并包含：

- 基础膜厚及变化幅度；
- 干涉相位；
- 噪声种子；
- 动画时间原点；
- 世界空间纹理原点和稳定方向基；
- 参考半径；
- IOR、Fresnel、透明度及光学法线松弛状态。

独立泡泡创建时初始化自己的视觉状态；破膜时只生成一次融合视觉身份；状态随后由有状态融合表面完整转交给稳定泡泡。

稳定交接继续使用 `BubbleSurfaceSystem::PromoteFusion` 移动同一个 `Model`：

- 不创建标准球；
- 不更换融合网格；
- 不更换 VAO；
- 不重置薄膜时间、相位或噪声；
- 不重算 UV；
- 不做 alpha cross-fade；
- 稳定判定不参与 shader 材质分支。

程序化薄膜坐标统一为：

```text
transpose(textureBasis)
* (worldPosition - persistentTextureOrigin)
/ persistentReferenceRadius
```

因此对象生命周期变化不会改变 shader 所观察到的坐标系。透明排序也保持“融合面/晋升面最后绘制”的交接顺序。

### 7. 诊断数据

每个融合对象持续记录：

- 选定材料节点的世界空间轨迹；
- 最大曲率及节点编号；
- 最大法向力及节点编号；
- 最大体积修正；
- 最小/最大边长及其比值；
- 最小转角；
- 极点径向速度；
- 最大相对体积误差和质心误差；
- 主形变参数过零次数。

鸿蒙运行时每约 0.5 秒通过 HiLog 输出一组融合诊断。Windows 专用的 GLFW/FFmpeg 调试视频录制器没有复制到移动端，因为它属于桌面验证工具而非融合算法；对应的轨迹、曲率、力和体积数据已经迁移。

## 修改文件

- `refraction/entry/src/main/cpp/CMakeLists.txt`
- `refraction/entry/src/main/cpp/napi_init.cpp`
- `refraction/entry/src/main/cpp/render/contact_geometry.h`
- `refraction/entry/src/main/cpp/render/contact_geometry.cpp`
- `refraction/entry/src/main/cpp/render/mesh.h`
- `refraction/entry/src/main/cpp/shader/shader_refraction.h`
- `refraction/entry/src/main/cpp/simulation/display_bubble.h`
- `refraction/entry/src/main/cpp/simulation/windows_interaction_port.inc`
- `refraction/entry/src/main/cpp/simulation/fusion_profile.h`（新增）
- `refraction/entry/src/main/cpp/simulation/fusion_profile.cpp`（新增）
- `refraction/entry/src/test/cpp/fusion_profile_smoke.cpp`（新增）

## 验证

### 静态检查

`fusion_profile.cpp` 已使用 MinGW g++ C++17 完成独立语法检查。仓库检查确认：

- 融合顶点生成器中不存在 `targetSphere`、`targetProfile`、`targetRadius` 或目标球 `glm::mix`；
- `relaxationProgress` 仅作为面积能量/状态诊断，不参与融合顶点位置计算；
- shader 的膜厚噪声使用持久世界坐标和 `uVisualTime`，不再直接依赖对象出生后的普通 `uTime` 坐标；
- `git diff --check` 无空白错误。

### 求解器冒烟测试

新增测试覆盖等比例、2:1、4:1 和 30/60 FPS。测试使用 97 个持久节点，运行 8 秒：

| 尺寸比例 | dt | 最大相对体积误差 | 形变过零次数 | 最终形变参数 | 最终 RMS 速度 | 边长比 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1:1 | 1/60 | 3.42e-7 | 4 | 2.47e-4 | 1.24e-3 | 2.103 |
| 2:1 | 1/60 | 4.45e-6 | 1 | -1.19e-2 | 4.40e-2 | 2.175 |
| 4:1 | 1/60 | 1.08e-5 | 1 | -1.96e-2 | 7.97e-2 | 2.321 |
| 2:1 | 1/30 | 4.45e-6 | 1 | -1.19e-2 | 4.38e-2 | 2.174 |

所有测试中左右极点 `vr` 始终为零，体积误差低于 0.0011%，30/60 FPS 结果接近。

### 构建环境说明

当前机器未安装或未暴露 DevEco Studio/OpenHarmony Native SDK、`hvigor` 命令及 GLES 头文件，因此本次无法在命令行生成 HAP。CMake 已加入新的求解器源文件；需要在安装了项目对应鸿蒙 SDK 的 DevEco Studio 中执行最终设备构建和真机渲染验收。

已额外执行桌面 CMake 配置检查，生成阶段成功，且构建日志确认新增的 `fusion_profile.cpp` 能被 MSVC 编译。完整目标随后按预期停止在鸿蒙平台头文件缺失处（`rawfile/raw_file_manager.h`、`GLES2/gl2.h`、`hilog/log.h`），不是本次迁移代码的编译错误。

## 与迁移前的根本区别

迁移前每一帧都会根据 `relaxationProgress` 重新计算“双球轮廓—目标球”的中间形状，振荡只是在改变 morph 权重。迁移后，`relaxationProgress` 不再进入顶点公式；顶点只能从上一帧节点位置经“力 → 速度 → 位置”连续移动。稳定时可见模型和视觉状态均保持原身份，因此不会再因创建标准球或重置材质而发生一帧换形、换色。

## 2026-08-09：手机端融合端点伪凹陷修复

真机画面中，融合结束后的外轮廓基本为圆形，但融合轴一端出现高光汇聚成尖嘴/凹坑的现象。检查后确认它主要是光学法线伪形变：

- 融合网格极点是单个共享顶点，不存在唯一的旋转方位角；
- 旧实现仍向该顶点写入带径向分量的材料方向；
- 极点扇形三角形对这个任意方向进行插值后，会形成 pinwheel 奇点；
- 远离极点时，随材料输运的 `FilmDirection` 也可能在表面松弛后明显偏离当前几何法线；
- 手机 GLES 的折射采样使该偏差比桌面 OpenGL 更明显，看起来像几何塌陷。

修复仅作用于鸿蒙渲染路径：

1. 左右共享极点的 `FilmDirection` 强制使用当前几何极点法线；
2. shader 根据 `dot(geometricNormal, filmDirection)` 计算连续可靠度；
3. 只有材料方向与当前几何法线可靠一致时，才允许它参与折射法线混合；
4. 膜厚噪声仍使用原有持久材料坐标，视觉状态、相位和随机种子不重置；
5. 没有增加完成时切换、颜色淡入或目标球插值。

该处理不会改变融合经线节点、体积、质心、曲率动力学或 Windows 渲染结果。
