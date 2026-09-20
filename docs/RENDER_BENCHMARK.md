# Renderer replay benchmark

`gt2renderbench` replays a private draw/texture snapshot through the production Vulkan stereo renderer. It builds for Windows and Android, independently of the game APK. Captures contain disc-derived assets and must stay private; `.gtr` files are ignored and rejected by the source audit.

To capture, set `GT2_RENDER_CAPTURE` to a path under `work/`, then run a desktop race with `--shot N work/frame.png`. For example, `--race --track tahiti_t --car ccrcn --cars 6 --no-sound --no-countdown --shot 240 work/frame.png --modern`. Remove the environment variable afterwards. The environment capture is performed only for a requested screenshot.

Usage: `gt2renderbench capture.gtr [scale%=175] [MSAA=2] [frames=300] [screenshot.png] [smooth=1] [direct=0]`.

The benchmark uses 1680x1760 per eye at 100%, and duplicates the captured camera into both layers. It warms up 60 frames, then reports GPU timestamps and host render/wait duration. `direct=1` exercises the external render target path with an owned-image alias; it cannot validate an actual runtime-owned OpenXR swapchain. Use `GT2_VK_VALIDATION=1` on Windows to enable the Vulkan validation layer; validation runs are correctness checks, not performance measurements.

## Quest 3 measurements, 2026-09-20

Adreno 740, 175% (2940x3080 per eye), smooth textures, MSAA 2x, 240 measured frames:

| Scene / renderer | GPU average | GPU p99 | Host render + GPU wait |
| --- | ---: | ---: | ---: |
| Tahiti Road, original path | 14.330 ms | 14.340 ms | 15.016 ms |
| Same capture, 0.1.5 | 10.414 ms | 10.423 ms | 11.240 ms |
| Tahiti dirt start, 0.1.5 | 10.198 ms | 10.205 ms | 10.987 ms |

The Road capture contains 258 draw items; dirt contains 237. Both were captured with the modern preset (maximum scenery, +1000 m distance). These are shell-process offscreen measurements, without live gameplay, head tracking, compositor or XR performance hints. Do not convert them into a claimed sustained game FPS. The 90 Hz frame budget is 11.11 ms.

A Windows image comparison against the original renderer differed by at most 2/255 per RGB component (mean absolute difference 0.011471/255), consistent with hardware bilinear weight precision. Mixed transparency-class pages use the original filter. Validation-layer replay of the direct render target path reported no errors. The actual OpenXR direct path still needs the player's headset check.

Implementation references: [Vulkan attachment load/store guidance](https://docs.vulkan.org/samples/latest/samples/performance/render_passes/README.html) and [OpenXR swapchain usage flags](https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html#XrSwapchainUsageFlagBits).
## Corrected replay and 0.1.6

The 0.1.5 benchmark forced every item to world space. That changed the real sky/HUD shader selection and underestimated the real render path. The corrected benchmark preserves captured draw spaces. It still duplicates a desktop camera, so it is not a live VR workload and does not predict game FPS.

On the same Quest capture at 175% / MSAA 2x / smooth textures, 240 measured frames:

| Path | GPU average | p99 | Host render + wait |
| --- | ---: | ---: | ---: |
| 0.1.5, corrected item spaces | 11.787 ms | 11.796 ms | 12.657 ms |
| 0.1.6, sky cache + mixed STP layers + specialized shader, foveation off | 9.239 ms | 9.247 ms | 10.064 ms |
| Final 0.1.6, Balanced foveation, full 256-material capacity | 7.805 ms | 7.847 ms | 8.684 ms |

Final Road material coverage is 1855/1855 references. An intermediate 128-material-capacity build with Balanced foveation measured 6.817 ms on the dirt start scene. These figures are offscreen GPU timings, not confirmed game FPS.

The CLI now accepts a final `[foveation=0]` argument after `[direct=0]`; 0/1/2/3 means Off/Low/Balanced/High. Validation mode also exercises foveation changes and MSAA 1/4/2 target recreation. Vulkan validation reports no errors for those transitions. A same-GPU Adreno image comparison before/after the cache/shader changes, without foveation, differed by at most 1/255 per RGB component, mean 0.001157/255; no component differed by more than 2. The foveated image was also inspected separately.

The live 0.1.5 report (`work/audit/quest015-user-performance.log`) measured much worse GPU times, 17-26 ms, with CPU build around 0.4 ms. Real head poses, FOV, moving scene and compositor compete for resources absent from the replay. The next headset test is required to assess 0.1.6.

API reference: [Vulkan shading rate attachment](https://docs.vulkan.org/refpages/latest/refpages/source/VkRenderingFragmentShadingRateAttachmentInfoKHR.html). This implementation uses its own static rate image and does not depend on an OpenXR foveation profile.
## 0.1.7 cached shading

The dedicated cached shader uses decoded RGB directly, and compatible screen-space HUD/mirror materials now use the cache at full shading rate. Geometry, shading-rate settings and texture filtering are unchanged. Vertex variants omit unused palette outputs. A same-Adreno comparison of the final Road frame with 0.1.6 is pixel-identical across all 27,165,600 RGB components.

At 175% / MSAA 2x / smooth / Balanced, final Road replay measured 7.428 ms GPU average, 7.453 p99, 8.289 ms host render+wait; dirt measured 7.166 / 7.694 / 7.987 ms. A same-session 0.1.6 Road run measured 7.983 ms average (8.651 p99); its earlier final run measured 7.805 ms. Clock/load variation matters. These are private fixed desktop-camera captures, so they neither exercise the new scenery selection nor measure the separate asynchronous OpenXR submission change. Sustained 90 FPS remains a headset acceptance test.

## 0.1.8 capture alignment correction

Capture loading now reconstructs triangle UV rectangles separately for each original draw range. Dynamic buffers are not necessarily aligned to vertex zero modulo three. Reconstructing the whole capture as one triangle list gave some HUD triangles the neighbouring sprite's bounds. Nearest filtering previously hid most of this error; the new atlas clamp exposed it. This is a replay-tool correction, not a gameplay speed improvement. Re-measure captures with the corrected loader before using them for comparisons.
## 0.1.9 mirror clipping diagnosis

A fixed Arcade start capture at field 750 takes 26.721 ms GPU on Quest at 175% / MSAA 2x / smooth / Balanced. The same capture without only its 35 mirror-car draws takes 8.300 ms; without cars anywhere, 7.668 ms; without track geometry, 23.592 ms. There are no smoke draws in this frame. Replaying removes simulation and race-time loading from the measurement.

The mirror stereo projection divided by clip W before mapping geometry to the HUD, losing homogeneous clipping across the mirror camera. Preserving W and matching the screen-coordinate interpolation reduces the complete capture to 8.343 ms average / 8.860 ms p99. The Adreno image was inspected and the desktop replay passed Vulkan validation. Check live frame pacing separately.

Set `GT2_BENCH_HANDS=1` to replace captured draw items with a synthetic wheel and the embedded animated hands for asset/pipeline inspection. This is a diagnostic pose, not a physical-controller or OpenXR acceptance test. Clear the variable before ordinary scene timing.

## 0.1.10 hands and wheel

Hand albedo now uses its own sampled Vulkan image and hardware linear filtering. Native hand draws skip PS1 material/UV-rectangle reconstruction. Finger meshes are retained until squeeze travel changes by 1/32; wheel and hand positions use draw matrices every frame. The wheel's lit rim, spokes and hub are generated only when its radius changes.

At 175% / MSAA 2x / Balanced on Quest, the synthetic hands/wheel frame changed from 4.704 ms GPU to 3.450 ms with the more detailed wheel. Final preparation with unchanged finger poses is 0.002 ms/frame. Those frames include the full offscreen target clear/resolve cost; 3.450 ms is not the incremental cost of adding hands to a race.

`GT2_BENCH_HANDS=2` appends the same synthetic hands/wheel to the captured scene. The tested start frame measured 8.847 ms without and 9.386 ms with them (about 0.54 ms incremental GPU cost). The combined test includes a capture-list copy in its 0.094 ms hand preparation measurement. No live simulation, OpenXR or compositor is included. Normal headset acceptance still applies.

The synthetic mode now updates draw matrices each frame and reports hand preparation separately. Finger pressure remains constant in these measurements; changing finger pose triggers a mesh update. Desktop Vulkan validation and the final Adreno image were checked after the transform change.

## 0.1.11 frame uploads

`GT2_BENCH_HANDS=3` animates trigger pressure on both hands over the captured scene. `GT2_BENCH_DEFER=0` uses immediate uploads for comparison with the default staged path. `GT2_BENCH_REUPLOAD=1` also replays vertex, VRAM and external-buffer writes, including overlapping vertex writes, from the supplied private capture. Windows Vulkan validation passed both paths; the resulting PNGs are byte-identical. This tests output and synchronization correctness, not live OpenXR pacing.

An experimental hardware scissor for projected HUD/mirror rectangles produced inconsistent Quest GPU results (about 8.5-9.2 ms versus 8.84 ms for the prior path on the same 175% / MSAA 2x / Balanced capture). It was removed from the final build. No GPU speedup or sustained 175% / 90 FPS is claimed for 0.1.11; its performance change targets CPU/GPU overlap during live scene preparation.

The VRAM upload path now compares rows with its CPU shadow. Identical rows avoid transfers and texture-cache invalidation; changed contiguous rows update normally. `GT2_BENCH_REUPLOAD=2` additionally changes and restores a VRAM word before drawing, checking ordering and cache refresh against the immediate path. Both outputs match byte-for-byte. This deliberately repeated-upload workload is a correctness stress test and is not representative gameplay FPS.
