# HarmonyOS Multi-Bubble Port

## Current scope

The HarmonyOS renderer now treats the Windows implementation as the behavioral source of truth:

- `DisplayBubble` and `BubbleContactPair` state data.
- The Windows broad-phase, candidate lifetime, six-iteration contact solver, size-based outcome selection, and exclusive-fusion ownership.
- Stable shared-film double bubbles, unequal-size fusion, and separation.
- Windows fusion timing, transported fusion frame, completion hold, surface promotion, and merged-bubble volume transfer.
- Windows persistent shell deformation, fusion-surface deformation, and curved contact-film deformation.
- The opening scene with a central three-bubble interaction, a fusion pair, an independent bubble, and render-only small bubbles.
- Per-bubble low-resolution DBSTT meshes used only while a bubble is bursting.
- Contact-film and fusion-surface mesh generation.
- Stable `FilmDirection` coordinates and contact-plane clipping in the OpenGL ES thin-film shader.

The HarmonyOS version keeps only the platform-specific EGL/XComponent lifecycle, touch mapping, half-resolution final FBO, and ArkTS parameter panel. The app is forced to landscape so the camera composition matches the Windows viewport.

The mirrored Windows interaction implementation is compiled from
`entry/src/main/cpp/simulation/windows_interaction_port.inc`. Behavioral changes should be made in Windows first and then synchronized into that file; do not add a second simplified HarmonyOS state machine.

## Touch controls

The bottom toolbar replaces the desktop keyboard controls:

| Control | Action |
| --- | --- |
| `Scene` | Restore the opening scene (`X` on desktop). |
| Demo button | Cycle stable double bubble, unequal fusion, and separation (`G` on desktop). |
| `Wind On/Off` | Enable or disable directional wind. Wind is off by default. |
| `Orbit` | One-finger drag rotates the camera. |
| `Add` | Tap the render surface to add a bubble. |
| `Pop` | Tap a bubble to start its DBSTT burst animation. |
| Two-finger pinch | Change camera distance. |

Bubble radius, spawn depth, and continuous wind strength are available in the settings panel.

## Rendering and performance

- Normal interactive bubbles use the same persistent contact-deformed shell algorithm as Windows.
- DBSTT is dormant until a bubble enters the burst state.
- Burst meshes use icosphere subdivision level 2.
- Fusion surfaces use the Windows `72 x 28` topology and contact films use the Windows curved-film topology.
- Shell, fusion, and contact meshes are uploaded at most once per rendered frame even though the mobile compositor uses two offscreen passes.
- The final scene remains rendered at scale `0.50` and is upscaled to the XComponent surface.

Current Mate 70 Pro measurements are approximately `21-23 FPS` in the six-interactive-bubble opening scene and `47-62 FPS` in the two-bubble demos. Further optimization must preserve the Windows state machine and surface formulas; suitable targets are staggered mesh uploads, topology LOD, and worker-thread vertex generation.

## Build

```powershell
cd E:\cgfinal\BubbleRender\refraction

$env:DEVECO_SDK_HOME='E:\huawei\DevEco Studio\sdk'
$env:HOS_SDK_HOME='E:\huawei\DevEco Studio\sdk\default'
$env:OHOS_SDK_HOME='E:\huawei\DevEco Studio\sdk\default\openharmony'
$env:JAVA_HOME='E:\huawei\DevEco Studio\jbr'
$env:Path='E:\huawei\DevEco Studio\jbr\bin;' + $env:Path

node 'E:\huawei\DevEco Studio\tools\hvigor\bin\hvigorw.js' assembleHap --no-daemon --stacktrace
```

Outputs:

- `entry/build/default/outputs/default/entry-default-unsigned.hap`
- `entry/build/default/outputs/default/entry-default-signed.hap` when a local signing profile is configured

Device installation requires a valid local `signingConfigs` entry in `build-profile.json5`. Signing credentials are machine-local and must not be committed or documented here.
