# Texture mipmaps

**Smooth + mipmaps** uses a 256x256 to 1x1 mip chain for decoded PS1 world textures on PC and Quest. Original filtering retains nearest sampling; 2D HUD/menu layers do not select world mips.

The GPU regenerates only dirty material layers after VRAM or palette changes. Stable frames reuse the chain. The 512-layer RGBA8 cache adds about 42.7 MiB of GPU storage, without another CPU mip staging allocation. Eight-bit-expanded RGB preserves sparse colour precision; transparent texels are black, and filtering divides premultiplied colour by coverage. Opaque and STP classes keep separate layers and drawing passes.

UV derivatives select trilinear LOD. Each footprint is clamped to complete mip blocks inside the primitive's atlas rectangle. Regions too small or unaligned use a finer level, preventing neighbouring atlas artwork from leaking in. This conservative rule can retain aliasing on very narrow sprites. MSAA alpha-to-coverage represents fence/tree coverage; without MSAA, a 0.5 coverage cutout is used. Very thin features can disappear when below the available sample coverage. Mips do not fix coplanar geometry or geometry-edge aliasing.

The render benchmark accepts `GT2_BENCH_MIPS=0` for a same-resolution/filtering A/B against level-zero bilinear sampling; unset it for the normal mip path. Compare both eyes and stationary/moving distant fences at the same MSAA, resolution and foveation. Desktop Vulkan validation and offscreen replay checks do not establish headset comfort or sustained frame rate.
