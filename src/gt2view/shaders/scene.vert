#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inTexel;   // texel coordinates inside the texture page
layout(location = 2) in vec3 inColor;   // polygon colour / 255
layout(location = 3) in uint inPage;    // pageX | pageY << 16   (VRAM words)
layout(location = 4) in uint inClut;    // clutX | clutY << 16
layout(location = 5) in uint inFlags;   // 1 textured, 2 raw texture, 4 car (paint/brake aware), bits 8-9 colour depth
layout(location = 6) in vec4 inRect;    // UV rectangle of the vertex's triangle (min u, min v, max u, max v)

layout(push_constant) uniform Push {
    mat4 mvp;
    uint paint;
    uint brakeLit;
    uint stpPass;
    uint options; // 1 smooth textures, 2 affine mapping (scene items only; vk_scene_renderer.h RenderOptions)
} pc;

layout(location = 0) out vec2 outTexel;
layout(location = 1) out vec3 outColor;
layout(location = 2) flat out uint outPage;
layout(location = 3) flat out uint outClut;
layout(location = 4) flat out uint outFlags;
// The same texel / colour interpolated linearly in screen space, like the PS1 GPU (the affine option).
layout(location = 5) noperspective out vec2 outTexelAffine;
layout(location = 6) noperspective out vec3 outColorAffine;
layout(location = 7) flat out vec4 outRect;
layout(location = 8) out vec3 outScreen;

layout(location = 9) flat out uint outCache;
layout(std430, set = 0, binding = 4) readonly buffer MaterialTable { uvec4 entries[]; } materials;
uint cachedPage() {
    if ((inFlags & 1u) == 0u || (inFlags & (24u | 65536u | 131072u | 262144u)) != 0u) return 0u;
    uint clut = inClut;
    if ((inFlags & 4u) != 0u) {
        clut += pc.paint << 16;
        if ((clut & 65535u) == 224u && pc.brakeLit != 0u) clut += 16u;
    }
    uint key = clut | (((inFlags >> 8) & 3u) << 28);
    uint at = ((inPage * 73856093u) ^ (key * 19349663u)) & 4095u;
    for (uint i = 0u; i < 16u; ++i, at = (at + 1u) & 4095u) {
        uvec4 e = materials.entries[at];
        if (e.w == 0u) break;
        if (e.x == inPage && e.y == key) return e.z;
    }
    return 0u;
}
void main() {
    outCache = cachedPage();
    gl_Position = pc.mvp * vec4(inPos, 1.0);
    // Ordering-table tier (flags bits 12-13, 0 = none): reversed Z, so a larger clip z is nearer. 2e-5 of the
    // distance per tier (0.6 mm at 30 m) keeps coplanar decals above their surface and nothing else.
    gl_Position.z *= 1.0 + float((inFlags >> 12) & 3u) * 2e-5;
    outTexel = inTexel;
    outColor = inColor;
    outPage = inPage;
    outClut = inClut;
    outFlags = inFlags;
    outTexelAffine = inTexel;
    outColorAffine = inColor;
    outRect = inRect;
    vec4 screen = pc.mvp * vec4(inPos, 1.0);
    outScreen = vec3(screen.xy, screen.w);
}
