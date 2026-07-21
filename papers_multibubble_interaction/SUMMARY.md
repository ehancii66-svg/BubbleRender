# 多泡泡交互仿真论文调研

本文件夹保存的是偏向“多泡泡交互、碰撞、融合、破裂、分离、泡沫拓扑变化”的新增论文。它们和之前的薄膜渲染、干涉颜色、单泡泡形变论文不同，重点更靠近泡泡之间的物理关系和拓扑事件。

## 论文清单与总结

### 1. Simulation of Bubbles in Foam With The Volume Control Method

- 本地文件：`2007_Simulation_of_Bubbles_in_Foam_with_the_Volume_Control_Method.pdf`
- 作者：Byungmoon Kim, Yingjie Liu, Ignacio Llamas, Xiangmin Jiao, Jarek Rossignac
- 年份/发表：2007，ACM SIGGRAPH / ACM Transactions on Graphics 相关论文
- 做什么：解决 level set 泡泡/泡沫仿真中的体积损失问题，尤其是很多泡泡长时间存在时，数值误差会让泡泡慢慢变小或变形。
- 核心原理：为每个连通泡泡区域跟踪体积变化，通过额外的 divergence correction 把体积误差补回去。它不是直接手动缩放泡泡，而是把体积控制写进速度场/散度场，让泡泡随流体演化时尽量保持体积。
- 我们能借鉴什么：如果我们做融合、分离、碰撞，最容易出现的问题是“泡泡合并后体积不守恒”或“分离后半径跳变”。可以给每个泡泡维护目标体积，在交互事件后通过半径、形变振幅或隐式场阈值做轻量体积校正。

### 2. Animation of Air Bubbles with SPH

- 本地文件：`2011_Animation_of_Air_Bubbles_with_SPH.pdf`
- 作者：Markus Ihmsen, Julian Bader, Gizem Akinci, Matthias Teschner
- 年份/发表：2011，GRAPP 2011
- 做什么：用 SPH 粒子法模拟水中气泡，包含气泡上浮、路径不稳定、形变、合并，以及气泡到达水面后的简化泡沫模型。
- 核心原理：水相和气泡相用粒子表示，并用分开的密度/压力力处理高密度比问题；空气泡与液体速度场双向耦合。论文还用启发式方法在可能困住空气的位置生成泡泡。
- 我们能借鉴什么：它给了一个很工程化的思路：不一定完整模拟空气相，而是把副泡泡看成带半径、速度、浮力、阻尼的粒子，在接近时用合并/排斥/扰动规则处理。适合做手机端可实时运行的“伪物理”交互层。

### 3. Animating Bubble Interactions in a Liquid Foam

- 本地文件：`2012_Animating_Bubble_Interactions_in_a_Liquid_Foam.pdf`
- 作者：Oleksiy Busaryev, Tamal K. Dey, Huamin Wang, Zhong Ren
- 年份/发表：2012，ACM Transactions on Graphics 31(4)，SIGGRAPH 2012
- 做什么：专门模拟液体泡沫中大量小泡泡的交互，包含泡泡聚集、泡泡-液体耦合、泡泡-固体耦合、破裂和融合。
- 核心原理：把每个泡泡看成 weighted Voronoi diagram 的 site。Voronoi 邻接关系天然提供“哪些泡泡接触”的拓扑信息；泡泡半径/权重控制体积，邻接面用于计算接触、挤压、合并、破裂等交互力。
- 我们能借鉴什么：这是最值得参考的一篇。我们现在是 15 个副泡泡和一个主泡泡，可以不用完整 Voronoi 网格，而是先实现它的简化版：基于距离建立邻接图，邻接泡泡之间施加排斥、膜桥吸附、合并触发、破裂触发，并保持体积。

### 4. A Hybrid Lagrangian-Eulerian Formulation for Bubble Generation and Dynamics

- 本地文件：`2013_Hybrid_Lagrangian_Eulerian_Formulation_for_Bubble_Generation_and_Dynamics.pdf`
- 作者：Saket Patkar, Mridul Aanjaneya, Dmitriy Karpman, Ronald Fedkiw
- 年份/发表：2013，ACM SIGGRAPH/Eurographics Symposium on Computer Animation，SCA 2013
- 做什么：模拟不同尺度泡泡的生成和动态变化，小泡泡用 Lagrangian 粒子，大泡泡用 Eulerian 流体网格。
- 核心原理：小泡泡作为粒子与周围流体双向耦合，体积变化参考 Rayleigh-Plesset 方程；大泡泡则转为网格上的解析界面。它关注气泡在压力驱动下的生长、收缩、合并和运动。
- 我们能借鉴什么：可以做“多尺度切换”：很小的副泡泡用粒子和 billboard/球体表示；当多个副泡泡聚集或融合到主泡泡时，再转成主泡泡表面上的局部扰动或一个较大的合并泡泡。

### 5. Multiscale Modelling of Evolving Foams

- 本地文件：`2016_Multiscale_Modelling_of_Evolving_Foams.pdf`
- 作者：R. I. Saye, J. A. Sethian
- 年份/发表：2016，Journal of Computational Physics
- 做什么：从泡泡重排、薄膜排液、膜破裂三个尺度统一模拟泡沫演化。
- 核心原理：宏观上用不可压流体求解泡泡重排；薄膜区域用 thin-film drainage 方程模拟膜厚变化；界面追踪使用 Voronoi Implicit Interface Method，并通过粒子方法在拓扑变化时守恒液体体积。
- 我们能借鉴什么：它适合作为报告里的高保真理论背景。工程实现上可以提取三个事件：接触后形成膜桥、膜厚随时间变薄、膜厚低于阈值触发破裂或融合。我们不需要完整求解薄膜方程，可以用低成本状态机近似。

### 6. A Hyperbolic Geometric Flow for Evolving Films and Foams

- 本地文件：`2017_A_Hyperbolic_Geometric_Flow_for_Evolving_Films_and_Foams.pdf`
- 作者：Sadashige Ishida, Masafumi Yamamoto, Ryoichi Ando, Toshiya Hachisuka
- 年份/发表：2017，ACM Transactions on Graphics 36(4)，SIGGRAPH 2017
- 做什么：用几何流模拟肥皂膜和泡沫，能处理非流形薄膜结构和拓扑变化。
- 核心原理：借鉴 Plateau 定律，认为稳定肥皂膜趋向常平均曲率曲面/极小曲面；用 hyperbolic mean curvature flow 的思想驱动表面演化，并加入体积保持项来对应压力效应。
- 我们能借鉴什么：我们可以把泡泡接触后的“膜桥/颈部”看成几何流平滑过程，而不是硬碰撞。实时版本可以只做局部曲率平滑：接触区域先拉出连接，再用阻尼弹簧把表面恢复成平滑形状。

### 7. Computing Foaming Flows Across Scales: From Breaking Waves to Microfluidics

- 本地文件：`2022_Computing_Foaming_Flows_Across_Scales.pdf`
- 作者：Petr Karnakov, Sergey Litvinov, Petros Koumoutsakos
- 年份/发表：2022，Science Advances
- 做什么：提出 Multi-VOF 框架，模拟从微流控泡泡晶体到瀑布泡沫的大尺度 foaming flow。
- 核心原理：扩展传统 volume-of-fluid 方法，对多个相邻泡泡进行分层/多标签处理，保留不同泡泡的身份，减少界面碎片，并支持大量泡泡的并行仿真。
- 我们能借鉴什么：它强调“泡泡身份标签”很重要。我们的副泡泡系统也应该维护 id、radius、volume、state、neighbors，而不是只画一堆球。这样融合、分裂、破裂后才能有稳定的事件逻辑。

### 8. A Moving Eulerian-Lagrangian Particle Method for Thin Film and Foam Simulation

- 本地文件：`2022_MELP_Thin_Film_and_Foam_Simulation.pdf`
- 作者：Yitong Deng, Mengdi Wang, Xiangxin Kong, Shiying Xiong, Zangyueyang Xian, Bo Zhu
- 年份/发表：2022，ACM Transactions on Graphics 41(4)，SIGGRAPH 2022
- 做什么：模拟薄膜和泡沫的复杂几何演化，覆盖膜面移动、泡沫结构变化和拓扑事件。
- 核心原理：把 Eulerian 网格和 Lagrangian 粒子结合起来，粒子跟踪薄膜局部结构，网格负责整体场更新。这样既能保留薄膜细节，又能处理大范围流动。
- 我们能借鉴什么：适合我们做“局部仿真 + 全局渲染”的架构。主泡泡和副泡泡仍然用低模球面渲染，只有接触区域生成少量局部控制点/粒子来表现拉伸、融合颈部和破裂回弹。

### 9. A Moving Least-Squares/Level-Set Particle Method for Bubble and Foam Simulation

- 本地文件：`2024_MLS_LevelSet_Particle_Method_for_Bubble_and_Foam_Simulation.pdf`
- 作者：Hui Wang, Zhi Wang, Shulin Hong, Xubo Yang, Bo Zhu
- 年份/发表：2024 online / IEEE Transactions on Visualization and Computer Graphics，正式卷期为 2025 年 TVCG 31(8)
- 做什么：为每个泡泡分配独立粒子系统，模拟大量泡泡/泡沫中的界面演化、拓扑变化和表面流动细节。
- 核心原理：每个泡泡的粒子既是表面离散点，也是 level-set 追踪点；所有泡泡粒子共同生成 unsigned level-set field，再与多相体积流体求解器耦合。
- 我们能借鉴什么：这篇非常贴近“多泡泡之间的交互”。我们可以简化为：每个泡泡维护一圈或一组表面控制点，碰撞时在接触方向上修改控制点位移；合并时把两个泡泡的控制点/体积合成一个新泡泡；分裂时按方向拆分控制点集合。

### 10. Multi-Material Mesh-Based Surface Tracking with Implicit Topology Changes

- 本地文件：`2024_Multi_Material_Mesh_Based_Surface_Tracking_with_Implicit_Topology_Changes.pdf`
- 作者：Peter Heiss-Synak, Aleksei Kalinov, Malina Strugaru, Arian Etemadi, Huidong Yang, Chris Wojtan
- 年份/发表：2024，ACM Transactions on Graphics 43(4)，SIGGRAPH 2024
- 做什么：多材料非流形网格界面追踪，能把自交转换为拓扑变化，示例中包含数千个互相作用的泡泡/肥皂膜。
- 核心原理：保留 mesh-based surface tracking 的表面细节，同时借鉴 level set 的拓扑鲁棒性；当不同材料/界面发生自交或接触时，隐式地重建拓扑。
- 我们能借鉴什么：如果后面要做更真实的融合/分裂，关键是不要让两个泡泡表面简单穿插。实时版本可以检测表面距离或球体重叠，把重叠事件转成“连接、合并、分裂、破裂”的离散状态变化。

### 11. Kinetic Free-Surface Flows and Foams with Sharp Interfaces

- 本地文件：`2025_Kinetic_Free_Surface_Flows_and_Foams_with_Sharp_Interfaces.pdf`
- 作者：Haoxiang Wang, Kui Wu, Hui Qiao, Mathieu Desbrun, Wei Li
- 年份/发表：2025，ACM Transactions on Graphics / SIGGRAPH Asia 2025 方向论文
- 做什么：提出 HOME-FREE LBM 求解器，用锐利自由表面模拟水-气交互、飞溅、气泡和泡沫。
- 核心原理：将 kinetic/LBM 流体求解与 volume-of-fluid 锐界面结合，利用水和空气密度/黏度差异很大这一事实，避免完整模拟空气相，从而节省计算。
- 我们能借鉴什么：它的思想对手机端很重要：不显式求解所有空气，只保留视觉上必要的界面和泡泡事件。我们的项目可以继续走“只模拟可见泡泡壳层与事件状态”的路线，而不是做完整双相流。

### 12. A Unified Multi-Scale Method for Simulating Immersed Bubbles

- 本地文件：`2025_Unified_Multi_Scale_Method_for_Simulating_Immersed_Bubbles.pdf`
- 作者：Joel Wretborn, Alexey Stomakhin, Christopher Batty
- 年份/发表：2025，Computer Graphics Forum / Eurographics 2025
- 做什么：统一模拟水下不同尺度的气泡，小气泡是 Lagrangian 粒子，大泡泡/气泡团转成 Eulerian 体积区域。
- 核心原理：泡泡粒子稀疏时代表独立小球；粒子密度高时逐渐聚合成可解析的含气体积区域；表面张力由局部气体体积分数梯度定义，因此可以在尺度切换时保持连续。
- 我们能借鉴什么：这篇给了非常适合工程实现的尺度策略。副泡泡靠近主泡泡或彼此聚集时，可以从“独立球”平滑过渡成“共享局部膜/融合泡泡”；距离拉开时再恢复为独立泡泡。

## 对我们项目最值得借鉴的实现路线

1. 建立泡泡状态结构：每个泡泡维护 `id`、`position`、`velocity`、`radius`、`targetVolume`、`filmThickness`、`state`、`neighbors`。
2. 用距离阈值构建轻量邻接图：距离小于 `r1 + r2 + contactMargin` 的泡泡进入接触状态。
3. 接触后不是马上合并，而是进入膜桥状态：在两个泡泡之间生成一个 neck/contact 参数，视觉上表现为局部拉伸和压扁。
4. 根据膜厚和相对速度触发事件：膜厚变薄到阈值触发融合或破裂；相对速度过大触发反弹/分离。
5. 合并时保持体积：新泡泡体积等于旧泡泡体积之和，半径按体积反推，形变振幅继承碰撞动量。
6. 分裂时保持体积：按指定比例拆成两个泡泡，分配相反速度和局部扰动，避免凭空增减体积。
7. 渲染上只做局部控制点/低阶球谐扰动：不要在手机端完整求解 VOF、LBM 或高分辨率 level set。

## 优先级建议

- 第一优先级：参考 2012 weighted Voronoi 泡沫交互和 2024 MLS/Level-Set 粒子法，实现邻接图、接触状态、合并/分离事件。
- 第二优先级：参考 2007 体积控制和 2025 unified immersed bubbles，保证融合/分裂时体积连续，避免半径突变。
- 第三优先级：参考 2017 HGF 和 2022 MELP，只在接触区域增加局部膜桥和曲率平滑，让视觉效果更像泡泡膜。
- 报告理论背景：可以引用 2016 JCP、2022 Science Advances、2024 multi-material surface tracking、2025 HOME-FREE 说明高保真方法存在，但我们的实现选择实时近似。

## 下载来源/参考链接

- `2007_Simulation_of_Bubbles_in_Foam_with_the_Volume_Control_Method.pdf`：https://faculty.cc.gatech.edu/~jarek/papers/foam.pdf
- `2011_Animation_of_Air_Bubbles_with_SPH.pdf`：https://cg.informatik.uni-freiburg.de/publications/2011_GRAPP_airBubbles.pdf
- `2012_Animating_Bubble_Interactions_in_a_Liquid_Foam.pdf`：https://wanghmin.github.io/publication/busaryev-2012-abi/Busaryev-2012-ABI.pdf
- `2013_Hybrid_Lagrangian_Eulerian_Formulation_for_Bubble_Generation_and_Dynamics.pdf`：https://physbam.stanford.edu/papers/stanford2013-04.pdf
- `2016_Multiscale_Modelling_of_Evolving_Foams.pdf`：https://math.lbl.gov/~saye/1-s2.0-S0021999116300158-main.pdf
- `2017_A_Hyperbolic_Geometric_Flow_for_Evolving_Films_and_Foams.pdf`：https://sadashigeishida.bitbucket.io/hgf/hgf.pdf
- `2022_Computing_Foaming_Flows_Across_Scales.pdf`：https://arxiv.org/pdf/2103.01513
- `2022_MELP_Thin_Film_and_Foam_Simulation.pdf`：https://www.cs.dartmouth.edu/~bozhu/papers/melp.pdf
- `2024_MLS_LevelSet_Particle_Method_for_Bubble_and_Foam_Simulation.pdf`：https://hhuiwangg.github.io/assets/pdf/mlsls_compressed.pdf
- `2024_Multi_Material_Mesh_Based_Surface_Tracking_with_Implicit_Topology_Changes.pdf`：https://pub.ista.ac.at/group_wojtan/projects/2024_MultimatMeshing/SuperDuperTopoFixer.pdf
- `2025_Kinetic_Free_Surface_Flows_and_Foams_with_Sharp_Interfaces.pdf`：https://haoxiang-wang.com/homefree/static/pdfs/WWQD25.pdf
- `2025_Unified_Multi_Scale_Method_for_Simulating_Immersed_Bubbles.pdf`：https://cs.uwaterloo.ca/~c2batty/papers/Wretborn2025/ImmersedBubbles.pdf
