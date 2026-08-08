# -*- coding: utf-8 -*-
import os

content = r'''% !TeX root = ../main.tex

\chapter{多泡泡交互系统}

\section{系统概述}

决赛阶段，系统从初赛的"单主泡泡 + 16个装饰展示泡泡"架构演进为完整的动态多泡泡交互系统。新系统支持运行时动态创建与删除泡泡、泡泡间接触检测与响应、共享膜的形成与演化、以及三种接触结果（分离、稳定8字双泡、融合吸收）。本章按照数据结构、空间加速、接触动力学、表面变形、共享膜几何、结果选择与辅助系统的顺序，介绍多泡泡交互的核心设计。

与初赛版本的关键区别在于：
\begin{enumerate}
  \item 每个泡泡不再是静态的展示实例，而是一个拥有独立身份（\codepath{BubbleId}）、物理状态和持久几何表示的仿真对象。
  \item 泡泡之间的接触关系由统一的接触图（contact graph）管理，每个泡泡可以同时参与多条接触关系。
  \item 接触过程不是单帧状态切换，而是通过连续激活函数驱动从接近、预接触、成膜到结果锁定的平滑过渡。
  \item 泡泡表面不再是纯球面，而是通过26个持久控制点进行局部变形，使接触区域的压扁、鼓包和回弹具有记忆性和惯性。
\end{enumerate}

本章的实现参考了多篇多泡泡/泡沫仿真的研究工作。接触图的构建借鉴了\cite{busaryev2012bubble}基于加权Voronoi图的邻接思想；表面控制点的弹簧-阻尼动力学借鉴了MELP\cite{deng2022melp}将局部粒子与全局几何结合的思路；弯曲共享膜的Young-Laplace曲率参考了多尺度泡沫模型\cite{saye2016multiscale}中的压力差关系；体积守恒策略参考了\cite{kim2007volume}的体积控制方法。需要强调的是，本系统的实现是面向实时可视化的工程近似，并非对上述论文算法的完整复现。

\section{核心数据结构}

\subsection{泡泡对象：DisplayBubble}

每个泡泡由 \codepath{DisplayBubble} 结构体表示，其核心字段包括：

\begin{itemize}
  \item \codepath{id}：稳定的64位泡泡标识（\codepath{BubbleId}），不随数组重排而改变。
  \item \codepath{position}、\codepath{velocity}：泡泡中心的运动状态。
  \item \codepath{radius}、\codepath{initialRadius}：当前半径与初始半径，半径可随时间向目标体积恢复。
  \item \codepath{targetVolume}：目标体积，由初始半径计算得出，用于体积守恒校正。
  \item \codepath{basePosition}：场景中的松弛位置，自由状态下泡泡围绕此位置做低频摆动。
  \item \codepath{phase}、\codepath{windAmplitude}、\codepath{floatAmplitude}、\codepath{speed}：自由漂移参数。
  \item \codepath{filmThickness}：泡泡膜厚状态。
  \item \codepath{surfaceControls}：26个持久表面控制点的数组。
  \item \codepath{state}：泡泡当前状态（Free、Touching、SharedFilm、Merged等）。
\end{itemize}

\subsection{接触关系：BubbleContactPair}

每一对候选或已接触的泡泡由 \codepath{BubbleContactPair} 管理，核心字段包括：

\begin{itemize}
  \item \codepath{a}、\codepath{b}：两个泡泡的稳定BubbleId。
  \item \codepath{candidate}：是否已进入近场候选范围。
  \item \codepath{bonded}：是否已建立持久的共享膜关系。
  \item \codepath{contactActivation}：连续接触激活强度，从0到1平滑增长。
  \item \codepath{contactTime}：接触历史累计时间。
  \item \codepath{contactRadius}、\codepath{contactRadiusVelocity}：接触圆半径及其变化速率。
  \item \codepath{interactionCompression}、\codepath{interactionVelocity}：交互压缩程度及其速率。
  \item \codepath{filteredNormal}：滤波后的接触轴方向（从A指向B）。
  \item \codepath{filteredPlaneCenter}：滤波后的接触平面中心。
  \item \codepath{filmThickness}：该接触对的共享膜简化厚度状态。
  \item \codepath{bridgeStrength}、\codepath{geometryBlend}：接触几何的成熟程度。
  \item \codepath{state}：该接触对的阶段（Touch、SharedFilm、Merged等）。
\end{itemize}

接触图以泡泡为节点、\codepath{BubbleContactPair}为边，支持一个泡泡同时参与多条接触关系。例如三个泡泡可同时存在AB、AC、BC三条接触边，每条边独立维护自己的接触圆和共享膜参数。

\subsection{表面控制点：SurfaceControl}

每个泡泡维护26个低分辨率表面控制点。控制方向来自$\{-1,0,1\}^3，均匀覆盖球面。每个控制点存储：

\begin{itemize}
  \item \codepath{localDir}：控制点在单位球面上的参考方向。
  \item \codepath{displacement}：相对于自由球面的持久三维位移。
  \item \codepath{velocity}：该局部表面的变形速率。
\end{itemize}

控制点使表面变形具有记忆、惯性和恢复过程。控制点对渲染顶点的插值权重近似为：

\[
w_{ik} = \max(\mathbf{d}_i \cdot \mathbf{c}_k,\, 0)^{8},
\]

其中$\mathbf{d}_i，$\mathbf{c}_k。所有控制点按权重归一化插值得到最终的持久位移。

\section{空间哈希与Broad Phase}

为避免泡泡数量增加后对所有泡泡对执行(N^2)，系统采用三维均匀空间哈希网格进行broad phase加速。网格尺寸约为最大泡泡直径的1.2倍。每帧仅检查当前泡泡所在网格及其26个相邻网格中的其他泡泡，从而快速生成候选接触对。

该步骤称为broad phase，只负责生成候选pair，不涉及具体的接触几何计算。

\section{接触激活与连续过渡}

两泡泡表面间距定义为：

\[
g = \|\mathbf{p}_A - \mathbf{p}_B\| - (r_A + r_B).
\]

当间距\codepath{nearRange}（约.08 \cdot \min(r_A, r_B)$）时，接触激活强度开始连续增长：

\[
a = \mathrm{Smooth01}\!\left(\frac{\mathrm{nearRange} - g}{\mathrm{nearRange}}\right),
\]

其中$\mathrm{Smooth01}(x) = x^2(3-2x)$，输入被限制在$[0,1]。该激活强度从接近阶段连续增长，并统一驱动以下所有效果：

\begin{itemize}
  \item 精细吸引力（毛细节 attraction）；
  \item 法向速度阻尼；
  \item 预接触局部压扁；
  \item 接触圆初始半径；
  \item 接触平面和法线滤波；
  \item 球壳clipping裁剪；
  \item 共享膜可见度；
  \item 表面控制点的局部压缩。
\end{itemize}

这种设计确保从candidate到bonded的过渡不会在单帧内突然切换一整套视觉逻辑。当，系统建立bonded contact，此时候选帧的进度和入射速度被继承。

\section{接触圆动力学与位置约束}

\subsection{接触圆半径}

\codepath{contactRadius}表示球壳与共享膜相交的圆形边界半径。它通过弹簧-阻尼系统追踪几何目标值：

\[
\ddot{r}_c = k_r(r_c^{\mathrm{target}} - r_c) - d_r \dot{r}_c,
\]

其中目标值^{\mathrm{target}}。参数选取接近临界阻尼，使接触圆快速增大但避免过冲回弹。

接触圆增大后，两泡泡的目标中心距离由几何关系反推：

\[
d_{\mathrm{target}} = \sqrt{r_A^2 - r_c^2} + \sqrt{r_B^2 - r_c^2}.
\]

当=0+r_B$；接触圆越大，两中心允许靠得越近，但相向球冠会被裁剪掉。

\subsection{多接触位置约束迭代}

系统每帧执行6次接触图位置约束迭代。每条接触边根据当前距离与目标距离的偏差修正两个泡泡的中心位置。多次迭代的作用是让AB、AC、BC等多条约束共同收敛。多接触变形还按活跃接触数量进行分摊。

\section{持久表面控制点系统}

\subsection{弹簧-阻尼更新}

每帧首先为每个SurfaceControl生成自由振荡目标，然后聚合该泡泡参与的所有接触的影响。控制点采用弹簧-阻尼更新：

\[
\ddot{\mathbf{d}}_k = k_s(\mathbf{d}_k^{\mathrm{target}} - \mathbf{d}_k) - d_s \dot{\mathbf{d}}_k.
\]

\subsection{接触区域的变形目标}

接触方向内部使用\codepath{capMask}（帽区遮罩），接触圆边缘使用\codepath{rimMask}（环区遮罩）：

\[
\mathbf{d}_k^{\mathrm{contact}} =
-\mathbf{n}_{\mathrm{contact}} \cdot c_{\mathrm{comp}} \cdot M_{\mathrm{cap}}
+ \mathbf{r}_{\mathrm{radial}} \cdot c_{\mathrm{bulge}} \cdot M_{\mathrm{rim}},
\]

其中$\mathbf{n}_{\mathrm{contact}}，$\mathbf{r}_{\mathrm{radial}}（远离接触轴）方向。含义为：
\begin{itemize}
  \item 接触帽内部沿接触轴向内压缩；
  \item 接触圆边缘向侧面鼓起；
  \item 多条contact的目标按权重聚合。
\end{itemize}

接触区域使用更高刚度和适中的欠阻尼，使表面快速响应但仍保留软膜惯性。

\section{接触区域速度共享}

借鉴MELP\cite{deng2022melp}的matching-velocity思想，系统对两侧接触帽计算表面材料速度：

\[
\mathbf{v}_{\mathrm{surface}} = \mathbf{v}_{\mathrm{center}} + \boldsymbol{\omega}_{\mathrm{control}} \cdot r,
\]

然后求质量加权的共享速度，让两侧接触帽局部趋向该速度。这并非将两个泡泡整体锁定，而是让共享膜附近的局部表面一起运动。每条pair独立计算自己的帽区权重。

\section{共享膜渲染}

每条candidate或bonded contact绘制一张共享膜圆盘。共享膜与球壳使用相同的\codepath{contactRadius}、\codepath{filteredPlaneCenter}和\codepath{filteredNormal}，因此球壳裁剪边界和共享膜边界由同一套接触状态驱动。

共享膜采用较低的alpha、折射强度和环境反射强度，以减少多个透明mesh叠加产生的黑带效应。

\reportfigure{placeholder_shared_film.png}{0.48}{0.26\textheight}{接触泡泡之间的共享膜渲染}

\section{弯曲共享膜与Young-Laplace压力}

等尺寸泡泡之间的共享膜近似为平面，但不同尺寸泡泡之间存在内部压力差。根据Young-Laplace关系，膜曲率与两侧压力差成正比：

\[
\kappa = \frac{1}{r_A} - \frac{1}{r_B},
\]

其中法线方向约定为从A指向B。于是：
\begin{itemize}
  \item 两泡半径相同时$\kappa=0$，共享膜退化为平面；
  \item A更大时膜向A一侧弯曲（A内部压力更低）；
  \item B更大时膜向B一侧弯曲。
\end{itemize}

共享膜使用解析球冠表示：

\[
z(\rho) = \mathrm{sign}(\kappa) \cdot
\left(\sqrt{R^2 - \rho^2} - \sqrt{R^2 - r_c^2}\right),
\]

其中 = 1/|\kappa|$，$\rho。当$\rho = r_c=0$，弯曲膜边界始终位于原接触平面，不因增加曲率产生位置缝隙。

\reportfigure{placeholder_curved_film.png}{0.48}{0.26\textheight}{不同尺寸泡泡间的弯曲共享膜：膜向大泡一侧凸出}

\section{接触结果选择}

接触成熟后，系统根据以下因素计算概率并锁定一次结果：

\begin{itemize}
  \item $\min(r_A, r_B) / \max(r_A, r_B)$：尺寸比；
  \item 接触持续时间；
  \item 共享膜厚度状态；
  \item 接触圆相对于小泡泡的尺寸；
  \item 法向碰撞速度；
  \item 当前风场扰动。
\end{itemize}

泡泡ID和当前场景代号共同生成稳定随机值，因此结果不随帧率抖动。三种结果如下。

\subsection{分离}

分离路径不会在首次碰撞时立即弹开，而是经历以下过程：
\begin{enumerate}
  \item 两泡泡短暂压缩并形成共享膜；
  \item 共享膜和接触圆逐渐收缩；
  \item 粘结合约被释放；
  \item 沿接触法线施加柔性分离速度；
  \item 两泡泡恢复独立运动。
\end{enumerate}

接触面积较小或冲击较强时，分离概率会提高。普通交互的分离概率限制在\%\%。

\subsection{稳定8字双泡}

尺寸接近的泡泡更倾向形成稳定8字双泡：
\begin{itemize}
  \item 两泡泡仍是独立腔体；
  \item 外轮廓形成8字形；
  \item 中间保留共享膜，而非成为单腔花生形泡泡；
  \item 等尺寸时共享膜接近平面，不等尺寸时共享膜按Young-Laplace关系弯曲；
  \item 接触圆有轻微周期波动，使双泡不显完全静止。
\end{itemize}

\reportfigure{placeholder_double_bubble.png}{0.48}{0.26\textheight}{稳定8字双泡：等尺寸泡泡间的平面共享膜}

\subsection{融合}

尺寸差越大，越倾向小泡泡并入大泡泡。融合过程为：
\begin{enumerate}
  \item 共享膜逐渐变薄；
  \item 接触膜局部破裂并扩大开口；
  \item 外表面颈部扩张；
  \item 两个外壳过渡为一张连续融合网格；
  \item 融合网格被提升为幸存泡泡的持久表面。
\end{enumerate}

融合中心采用体积加权：
\[
\mathbf{p}_{\mathrm{merged}} = \frac{\mathbf{p}_A V_A + \mathbf{p}_B V_B}{V_A + V_B},
\]

最终半径保持体积守恒：
\[
r_{\mathrm{merged}}^3 = r_A^3 + r_B^3.
\]

因此尺寸差较大时，最终中心更靠近大泡泡，小泡泡视觉上向大泡泡一侧被吸收。融合完成后移除被吸收泡泡的接触关系，幸存泡泡继承体积加权后的合速度。

\reportfigure{placeholder_fusion_sequence.png}{0.72}{0.28\textheight}{融合过程：小泡泡逐渐被大泡泡吸收}

\section{风场系统}

全局风场默认关闭（F键切换）。核心特征：

\begin{itemize}
  \item 风力强度范围.0.45$，默认.16$，单次调整步长.025$；
  \item 风向通过I/J/K/L键上下左右调整；
  \item 小泡泡具有更高的风力响应系数，大泡泡运动相对稳定；
  \item 关闭风后，已有速度通过阻尼平滑衰减，泡泡逐渐回到有界摆动范围；
  \item 融合后的泡泡也可整体随风移动。
\end{itemize}

无风状态下，泡泡并非完全静止，而是围绕各自的\codepath{basePosition}做有界低频摆动，使用不同相位和速度以避免同步运动。

\section{每泡DBSTT涡流片仿真}

每个展示泡泡可以拥有独立的\codepath{VortexSheetSimulation}实例。仿真以单位半径创建（模型矩阵处理视觉缩放），在泡泡破裂时激活：提高表面张力驱动膜收缩，体现真实的物理回弹。正常（非破裂）渲染和接触/融合仍使用控制点表面。该设计使得泡泡破裂时膜的回缩由DBSTT涡流片模型\cite{da2015doublebubbles}驱动而非预设动画。

\section{渲染连续性与体积守恒}

\subsection{三层连续性策略}

系统的视觉连续性分为三个层面：

\textbf{状态连续}：\codepath{contactActivation}从近场连续增长；candidate和bonded共用同一套接触参数；bonded首帧继承预接触进度和入射速度；contact pair不因单帧距离抖动而立即销毁。

\textbf{几何连续}：每个泡泡始终使用同一份持久球壳Model，不切换到另一套assembly几何；接触圆和接触平面连续变化；球壳通过同一平面裁剪；共享膜边界使用同一接触圆。

\textbf{渲染连续}：shell和共享膜保持稳定的thickness scale；共享膜使用柔性alpha和较弱折射；接触frame进行低通滤波；透明泡泡按距离排序。

\subsection{体积守恒}

每个泡泡保存\codepath{targetVolume}（由初始半径计算），半径会缓慢向目标体积对应的半径恢复。局部表面压缩后，系统根据控制点平均径向位移进行小范围整体缩放补偿。这是实时近似，并非严格的封闭三角网格体积积分。后续可考虑使用 = \frac{1}{6}\sum \mathbf{x}_i \cdot (\mathbf{x}_j \times \mathbf{x}_k)。

\section{动态泡泡管理}

系统支持运行时动态创建与删除泡泡：

\begin{itemize}
  \item \textbf{创建}：按B键在泡泡团外围生成新泡泡，带朝向中心的初速度和少量切向速度，随后自动进入broad phase和contact graph。
  \item \textbf{删除}：按V键删除最近一个动态新增的泡泡，同时清理该泡泡相关的contact pair和对应渲染model。
  \item \textbf{身份稳定}：泡泡身份使用稳定的\codepath{BubbleId}（而非vector下标），contact pair通过\codepath{ResolveContactPairIndices()}映射到当前数组下标，确保删除或重排后模型不错位。
\end{itemize}

此外，系统提供两套预置演示场景：按G键循环切换双泡专项演示（等尺寸稳定双泡$\to$\to），按X键重置综合开场场景（中央三个真实模拟泡泡形成接触网络，外围包含预设融合泡泡对和独立中型泡泡，以及若干仅参与渲染的装饰小泡泡）。

\section{当前局限}

\begin{itemize}
  \item 尚未形成统一的Plateau border：AB、AC、BC共享膜仍是独立contact patch，三张膜在中央可能互相穿过或留下小缝隙。
  \item 球壳边界与弯曲共享膜边界位置可对齐，但各自法线独立计算，透明折射中仍可能看到亮环或角度跳变。
  \item 膜厚仍为标量（每个pair一个值），无法表达重力排液、局部黑膜、风吹颜色流和接触区域材料交换。
  \item 透明多层渲染仍有限：使用排序、alpha混合和FBO折射。泡泡数量增加后可能出现绘制顺序错误和黑带。
  \item 球壳shader最多接收四个接触平面。真正多泡沫中一个泡泡可能有更多邻居。
  \item 融合概率模型是面向实时视觉的经验模型，不是完整流体或膜排液求解器。
\end{itemize}
'''

path = r'D:\USTC\CG\Final\BubbleRender\USTC latex template\chapters\chapter4.tex'
with open(path, 'w', encoding='utf-8') as f:
    f.write(content)
print('chapter4.tex written OK')
