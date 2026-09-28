#version 450
#ifdef GT2_WEBGL
precision highp float;
precision highp int;
precision highp sampler2D;
precision highp sampler2DArray;
#define GT2_VARYING(n)
#define noperspective
#else
#define GT2_VARYING(n) layout(location = n)
#endif

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inTexel;   // texel coordinates inside the texture page
layout(location = 2) in vec3 inColor;   // polygon colour / 255
layout(location = 3) in uint inPage;    // pageX | pageY << 16   (VRAM words)
layout(location = 4) in uint inClut;    // clutX | clutY << 16
layout(location = 5) in uint inFlags;   // 1 textured, 2 raw texture, 4 car (paint/brake aware), bits 8-9 colour depth
layout(location = 6) in vec4 inRect;    // UV rectangle of the vertex's triangle (min u, min v, max u, max v)

#ifdef GT2_WEBGL
struct Push {
#else
layout(push_constant) uniform Push {
#endif
    mat4 mvp;
    uint paint;
    uint brakeLit;
    uint stpPass;
    uint options; // 1 smooth textures, 2 affine mapping (scene items only; vk_scene_renderer.h RenderOptions)
#ifdef GT2_WEBGL
    uint space;
    uint eye;
    uvec2 reserved;
    vec4 hudClip;
#endif
#ifdef GT2_WEBGL
};
uniform Push pc;
#else
} pc;
#endif

GT2_VARYING(0) out vec2 outTexel;
GT2_VARYING(1) out vec3 outColor;
GT2_VARYING(2) flat out uint outPage;
GT2_VARYING(3) flat out uint outClut;
GT2_VARYING(4) flat out uint outFlags;
// The same texel / colour interpolated linearly in screen space, like the PS1 GPU (the affine option).
GT2_VARYING(5) noperspective out vec2 outTexelAffine;
GT2_VARYING(6) noperspective out vec3 outColorAffine;
GT2_VARYING(7) flat out vec4 outRect;
GT2_VARYING(8) out vec3 outScreen;

GT2_VARYING(9) flat out uint outCache;
#ifdef GT2_WEBGL
uniform highp usampler2D materialTable;
out float outAffineW;
uvec4 materialAt(uint i) { return texelFetch(materialTable, ivec2(int(i & 255u), int(i >> 8)), 0); }
#else
layout(std430, set = 0, binding = 4) readonly buffer MaterialTable { uvec4 entries[]; } materials;
uvec4 materialAt(uint i) { return materials.entries[i]; }
#endif
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
        uvec4 e = materialAt(at);
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
#ifdef GT2_WEBGL
    // Reproduce noperspective interpolation in GLSL ES 3.00.
    outTexelAffine *= gl_Position.w;
    outColorAffine *= gl_Position.w;
    outAffineW = gl_Position.w;
    // Shared builders produce Vulkan clip coordinates (Y down, Z in [0,1]).
    gl_Position.y = -gl_Position.y;
    gl_Position.z = 2.0 * gl_Position.z - gl_Position.w;
#endif
    outRect = inRect;
    vec4 screen = pc.mvp * vec4(inPos, 1.0);
    outScreen = vec3(screen.xy, screen.w);
}
