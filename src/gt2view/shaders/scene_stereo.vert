#version 450
// The stereo variant of scene.vert (docs/research/vr_port_plan.md, M2). The draw list is built once, in the space
// "world - refEye" (DrawItem::mvp), and this shader applies the eye's own view-projection:
//   space 0 (kWorld)  worldVP[v] . mvp . pos
//   space 1 (kSky)    skyVP[v]   . mvp . pos   - the eye translation is removed, so the backdrop sits at infinity
//                                                and is identical in both eyes (no parallax)
//   space 2 (kScreen) head-relative HUD plane projected through each asymmetric eye frustum
// Compiled twice: with GT2_MULTIVIEW the eye is gl_ViewIndex (one pass, VkRenderingInfo::viewMask), without it the
// push constant `eye` (two passes, one per array layer). Both must produce the same image.
#ifdef GT2_MULTIVIEW
#extension GL_EXT_multiview : require
#endif

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
    uint space;   // DrawItem::space
    uint eye;     // the two-pass path's array layer
} pc;

layout(set = 0, binding = 2) uniform Views {
    mat4 worldVP[2];
    mat4 hudVP[2];
    mat4 skyVP[2];
} views;

layout(location = 0) out vec2 outTexel;
layout(location = 1) out vec3 outColor;
#ifndef GT2_CACHED
layout(location = 2) flat out uint outPage;
layout(location = 3) flat out uint outClut;
#endif
layout(location = 4) flat out uint outFlags;
// The same texel / colour interpolated linearly in screen space, like the PS1 GPU (the affine option).
layout(location = 5) noperspective out vec2 outTexelAffine;
layout(location = 6) noperspective out vec3 outColorAffine;
layout(location = 7) flat out vec4 outRect;
layout(location = 8) out vec3 outScreen;

layout(location = 9) flat out uint outCache;
layout(std430, set = 0, binding = 4) readonly buffer MaterialTable { uvec4 entries[]; } materials;
uint cachedPage() {
    if ((inFlags & 1u) == 0u || (inFlags & (24u | 65536u)) != 0u) return 0u;
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
#ifdef GT2_MULTIVIEW
    int v = gl_ViewIndex;
#else
    int v = int(pc.eye);
#endif
    vec4 p = pc.mvp * vec4(inPos, 1.0);
    if (pc.space == 0u) gl_Position = views.worldVP[v] * p;
    else if (pc.space == 1u) gl_Position = views.skyVP[v] * p;
    else {
        // Preserve homogeneous W: mirror geometry behind its camera must still
        // clip before rasterization instead of expanding across the HUD plane.
        gl_Position = views.hudVP[v] * vec4(p.xy, 0.0, p.w);
        float w = abs(p.w) > 1e-7 ? p.w : (p.w < 0.0 ? -1e-7 : 1e-7);
        gl_Position.z = (p.z / w) * gl_Position.w;
    }
    // Ordering-table tier (flags bits 12-13, 0 = none): reversed Z, so a larger clip z is nearer. 2e-5 of the
    // distance per tier (0.6 mm at 30 m) keeps coplanar decals above their surface and nothing else.
    gl_Position.z *= 1.0 + float((inFlags >> 12) & 3u) * 2e-5;
    outTexel = inTexel;
    outColor = inColor;
#ifndef GT2_CACHED
    outPage = inPage;
    outClut = inClut;
#endif
    outFlags = inFlags;
    outTexelAffine = inTexel;
    outColorAffine = inColor;
    outRect = inRect;
    vec4 screen = pc.mvp * vec4(inPos, 1.0);
    outScreen = vec3(screen.xy, screen.w);
}
