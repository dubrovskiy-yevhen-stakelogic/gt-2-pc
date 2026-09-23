#version 450
#extension GL_GOOGLE_include_directive : require
// Fully cached draws. External, overlay and uncached materials use scene.frag.
layout(constant_id = 0) const bool cachedHud = false;
layout(push_constant) uniform Push {
    mat4 mvp; uint paint; uint brakeLit; uint stpPass; uint options;
    uint space; uint eye; uvec2 reserved; vec4 hudClip;
} pc;
layout(location = 0) in vec2 inTexel;
layout(location = 1) in vec3 inColor;
layout(location = 4) flat in uint inFlags;
layout(location = 5) noperspective in vec2 inTexelAffine;
layout(location = 6) noperspective in vec3 inColorAffine;
layout(location = 7) flat in vec4 inRect;
layout(location = 8) in vec3 inScreen;
layout(location = 9) flat in uint inCache;
layout(set = 0, binding = 3) uniform sampler2DArray decodedPages;
layout(set = 0, binding = 5) uniform sampler2D handAlbedo;
layout(set = 0, binding = 6) uniform sampler2D rearView;
layout(location = 0) out vec4 outColor;
#include "texture_mips.glsl"

void main() {
    if ((inFlags & 2097152u) != 0u) {
        outColor = vec4(textureLod(rearView, inTexel, 0).rgb, 1.0);
        return;
    }
    if ((inFlags & 65536u) != 0u) {
        outColor = vec4(texture(handAlbedo, inTexel).rgb * inColor, 1.0);
        return;
    }
    if (cachedHud && pc.hudClip.z > pc.hudClip.x && (any(lessThan(inScreen.xy / inScreen.z, pc.hudClip.xy)) || any(greaterThan(inScreen.xy / inScreen.z, pc.hudClip.zw)))) discard;
    if ((inFlags & 2048u) != 0u && !gl_FrontFacing) discard;
    bool affine = (pc.options & 2u) != 0u;
    vec2 uv = affine ? inTexel + clamp(inTexelAffine - inTexel, vec2(-4), vec2(4)) : inTexel;
    vec3 color = affine ? inColorAffine : inColor;
    if ((inFlags & 1u) == 0u) {
        if (pc.stpPass == 1u) discard;
        outColor = vec4(color, 1); return;
    }
    if ((pc.options & 1u) != 0u && sampleMip(uv, color, outColor)) return;
    int layer = int((inCache & 65535u) - 1u);
    ivec2 nearest = clamp(ivec2(floor(uv)), ivec2(0), ivec2(255));
    if (cachedHud && (inFlags & 32768u) != 0u) nearest = clamp(nearest, ivec2(floor(inRect.xy)), ivec2(max(ceil(inRect.zw) - 1, floor(inRect.xy))));
    vec4 texel = texelFetch(decodedPages, ivec3(nearest, layer), 0);
    bool stp = (inCache & 65536u) != 0u;
    if ((inCache & 131072u) != 0u && texel.a == 0.0) {
        texel = texelFetch(decodedPages, ivec3(nearest, ++layer), 0); stp = true;
    }
    if (texel.a == 0.0 || (pc.stpPass == 1u && stp) || (pc.stpPass == 2u && !stp)) discard;
    vec3 rgb = floor(texel.rgb * 255.0 / 8.0) / 31.0;
    if ((pc.options & 1u) != 0u) {
        vec2 lo = clamp(floor(inRect.xy), vec2(0), vec2(255));
        vec2 hi = clamp(max(ceil(inRect.zw) - 1, lo), vec2(0), vec2(255));
        vec2 filteredUV = (clamp(uv - 0.5, lo, hi) + 0.5) / 256.0;
        vec4 filtered = textureLod(decodedPages, vec3(filteredUV, float(layer)), 0);
        if (filtered.a > 0) rgb = filtered.rgb / filtered.a;
    }
    if ((inFlags & 16384u) != 0u) {
        vec3 t5 = floor(texel.rgb * 255.0 / 8.0);
        rgb = min(floor(t5 * floor(color * 255.0 + 0.5) / 128.0), vec3(31)) / 31.0;
    } else if ((inFlags & 2u) == 0u) rgb = min(rgb * color * (255.0 / 128.0), vec3(1));
    outColor = vec4(rgb, 1);
}
