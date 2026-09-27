# Renderer replay benchmark

`gt2renderbench` replays a draw/texture snapshot through the production Vulkan
stereo renderer. It builds for Windows and Android independently of the game
APK. Captures contain disc-derived assets and must stay private; `.gtr` files
are ignored and rejected by the source audit.

## Capture and replay

Set `GT2_RENDER_CAPTURE` to a path under `work/`, then run a desktop race with a
requested screenshot. For example:

```text
--race --track tahiti_t --car ccrcn --cars 6 --no-sound --no-countdown --shot 240 work/frame.png --modern
```

Remove the environment variable afterwards. A capture is written only for a
requested screenshot.

```text
gt2renderbench capture.gtr [scale%=175] [MSAA=2] [frames=300] [screenshot.png] [smooth=1] [direct=0] [foveation=0]
```

The benchmark uses 1680x1760 per eye at 100% and duplicates the captured camera
into both layers. It warms up 60 frames, then reports GPU timestamps and host
render/wait duration. Foveation values 0/1/2/3 mean Off/Low/Balanced/High.

`direct=1` exercises the external render-target path with an owned-image alias;
it does not use a runtime-owned OpenXR swapchain. Set `GT2_VK_VALIDATION=1` on
Windows for Vulkan validation. Keep validation disabled for timing comparisons.

## Diagnostic modes

| Variable | Effect |
| --- | --- |
| `GT2_BENCH_HANDS=1` | Replace captured items with a synthetic wheel and animated hands. |
| `GT2_BENCH_HANDS=2` | Append the synthetic wheel and hands to the captured scene. |
| `GT2_BENCH_HANDS=3` | Animate trigger pressure on both hands over the captured scene. |
| `GT2_BENCH_DEFER=0` | Use immediate uploads for comparison with the staged upload path. |
| `GT2_BENCH_REUPLOAD=1` | Replay vertex, VRAM and external-buffer writes, including overlapping writes. |
| `GT2_BENCH_REUPLOAD=2` | Also change and restore a VRAM word before drawing, to check ordering and cache refresh. |
| `GT2_BENCH_MIPS=0` | Compare level-zero bilinear sampling with the normal mipmap path. |

Unset diagnostic variables before ordinary scene timing. Hand modes report
preparation time separately; changing finger pressure can trigger mesh updates.
Repeated-upload modes are correctness stress tests, not gameplay workloads.

## Reading the results

Replay preserves captured draw spaces and reconstructs UV rectangles within
each draw range. It omits live simulation, head tracking, the compositor and XR
performance requests. Offscreen GPU time therefore cannot establish game FPS
or headset frame pacing. Synthetic hand timings also include target clearing
and resolve work, rather than only the cost of the hands.

Compare the same capture, GPU, resolution, MSAA, filtering and foveation in one
session, with warmup and repeated runs. Record GPU average/tail timings and
inspect images alongside timing results. Use live profiler recordings to check
sustained gameplay performance.

References: [Vulkan attachment load/store guidance](https://docs.vulkan.org/samples/latest/samples/performance/render_passes/README.html),
[OpenXR swapchain usage flags](https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html#XrSwapchainUsageFlagBits),
[Vulkan shading rate attachment](https://docs.vulkan.org/refpages/latest/refpages/source/VkRenderingFragmentShadingRateAttachmentInfoKHR.html).
