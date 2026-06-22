# BubbleRender HarmonyOS Performance Notes

## Current Rendering Setup

- HarmonyOS native renderer: `entry/src/main/cpp/napi_init.cpp`
- Main refraction shader: `entry/src/main/cpp/shader/shader_refraction.h`
- Full scene is rendered to a low-resolution final FBO and then upscaled with a fullscreen quad.
- Current internal render scale is `0.50`.
- On the tested emulator, the surface was `1320x2622`, so internal FBOs were `660x1311`.

## Decorative Bubbles

The HarmonyOS version currently renders 15 decorative bubbles.

The bubble list is stored in `g_DisplayBubbles` in `napi_init.cpp`. Each entry contains:

```cpp
basePosition, radius, phase, windAmplitude, floatAmplitude, speed
```

Decorative bubble animation is intentionally cheap on the CPU. Their positions are updated with low-cost `sin`/`cos` motion in `BubbleModelMatrix`. The expensive part is not the movement itself, but drawing each bubble with the refraction fragment shader.

## Main Bubble Deformation

The original high-cost path was full CPU-side DBSTT/vortex-sheet deformation:

- `g_SimSubdivs = 3`
- `substepsPerFrame = 3`
- simulation and vertex upload ran every rendered frame

That was too expensive on the HarmonyOS emulator. Measured FPS was around 7.

The current version keeps the deformation visible while lowering CPU cost:

- `g_SimSubdivs = 3`
- `g_SimPerturb = 0.16`
- `g_Sim.substepsPerFrame = 1`
- `kSimFrameInterval = 6`
- CPU simulation and vertex upload run once every 6 rendered frames
- the vertex shader adds a lightweight low-frequency wobble only to the main bubble

This combines slow physical deformation with continuous visual wobble.

## Measured Results So Far

Approximate emulator FPS from 15-second log windows:

```text
Original full CPU deformation: about 7 FPS
CPU deformation disabled:      about 32-39 FPS
Subdiv 2 + interval 4:         about 31-34 FPS
Subdiv 3 + interval 6 + wobble: built successfully, pending device FPS check
```

The app logs the current configuration every 15 seconds:

```text
perf avgFps15s=... smoothFps=... surface=... fbo=... renderScale=... displayBubbles=... simSubsteps=... simInterval=...
```

## Build Command

```powershell
cd E:\cgfinal\BubbleRender\refraction

$env:DEVECO_SDK_HOME='E:\huawei\DevEco Studio\sdk'
$env:HOS_SDK_HOME='E:\huawei\DevEco Studio\sdk\default'
$env:OHOS_SDK_HOME='E:\huawei\DevEco Studio\sdk\default\openharmony'
$env:JAVA_HOME='E:\huawei\DevEco Studio\jbr'
$env:Path='E:\huawei\DevEco Studio\jbr\bin;' + $env:Path

node 'E:\huawei\DevEco Studio\tools\hvigor\bin\hvigorw.js' assembleApp --no-daemon --stacktrace
```

Output app:

```text
build/outputs/default/refraction-default-unsigned.app
```
