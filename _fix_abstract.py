# -*- coding: utf-8 -*-
import os

abstract = r'''% !TeX root = ../main.tex

\ustcsetup{
  keywords = {
    肥皂泡渲染，HarmonyOS，屏幕空间折射，薄膜干涉，虹彩，多泡泡交互，接触图，共享膜，融合，DBSTT，OpenGL ES
  },
  keywords* = {
    soap bubble rendering, HarmonyOS, screen-space refraction,
    thin-film interference, iridescence, multi-bubble interaction,
    contact graph, shared film, fusion, DBSTT, OpenGL ES
  },
}

\begin{abstract}
  本项目面向【CCF CAD/CG 2026】高质量实时渲染技术挑战赛赛题二"基于鸿蒙的透明物体实时色散渲染系统"，实现了一个运行于 HarmonyOS 设备上的透明泡泡实时渲染与交互系统。系统采用 HarmonyOS ArkTS + XComponent + NAPI + C++ OpenGL ES 3.0 的整体架构。在初赛工作的基础上，决赛版本重点扩展了多泡泡交互系统：建立了基于空间哈希和接触图的多泡泡仿真框架，实现了接触圆动力学、持久表面控制点变形、共享膜渲染、Young-Laplace弯曲共享膜、以及分离/稳定双泡/融合三种接触结果；引入了风场系统和每泡DBSTT涡流片仿真；支持运行时动态创建与删除泡泡。本文按照渲染管线、光学模型、单泡物理模拟、多泡泡交互系统、交互演示与结果分析的顺序，介绍决赛提交版本的系统设计、关键实现与当前局限。
\end{abstract}

\begin{abstract*}
  This project targets Track 2, "Real-Time Dispersion Rendering of Transparent
  Objects on HarmonyOS," and implements a real-time transparent bubble
  rendering and interaction system on HarmonyOS devices. The system is
  built with HarmonyOS ArkTS, XComponent, NAPI, and C++ OpenGL ES 3.0.
  Building on the preliminary-round submission, the final-round version
  adds a comprehensive multi-bubble interaction system: a contact-graph-based
  simulation framework with spatial hashing, contact-circle dynamics,
  persistent surface control points, shared-film rendering, Young-Laplace
  curved shared films, and three contact outcomes (separation, stable double
  bubble, fusion). It also introduces a wind field system, per-bubble DBSTT
  vortex sheet simulation, and runtime bubble creation/deletion. This report
  presents the final-round submission covering the rendering pipeline, optical
  models, single-bubble physics, multi-bubble interaction, demonstration
  features, and current limitations.
\end{abstract*}
'''

with open(r'D:\USTC\CG\Final\BubbleRender\USTC latex template\chapters\abstract.tex', 'w', encoding='utf-8') as f:
    f.write(abstract)
print('abstract.tex written')
