#version 450
#extension GL_GOOGLE_include_directive : require
// The stereo variant of scene.vert (docs/research/vr_port_plan.md). The draw list is built once, in the space
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
    uvec2 reserved;
    vec4 hudClip;
    vec4 nativeModelRotation;
} pc;

layout(set = 0, binding = 2) uniform Views {
    mat4 worldVP[2];
    mat4 hudVP[2];
    mat4 skyVP[2];
} views;

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
layout(location = 10) out vec3 outPaintView;
layout(std430, set = 0, binding = 4) readonly buffer MaterialTable { uvec4 entries[]; } materials;
uint cachedPage() {
    if((inFlags & 64u)!=0u)return 0u;
    if ((inFlags & 1u) == 0u || (inFlags & (24u | 65536u | 131072u | 262144u)) != 0u) return 0u;
    uint clut = inClut;
    if ((inFlags & 4u) != 0u) {
        clut += pc.paint << 16;
        if ((clut & 65535u) == 224u && pc.brakeLit != 0u) clut += 16u;
    }
    uint key = clut | (((inFlags >> 8) & 3u) << 28);
    // Must match DecodedTextureCache::Bucket, including the high CLUT row bits.
    uint at = (inPage * 73856093u) ^ (key * 19349663u);
    at ^= at >> 16;
    at *= 0x7feb352du;
    at = (at ^ (at >> 15)) & 4095u;
    for (uint i = 0u; i < 16u; ++i, at = (at + 1u) & 4095u) {
        uvec4 e = materials.entries[at];
        if (e.w == 0u) break;
        if (e.x == inPage && e.y == key) return e.z;
    }
    return 0u;
}
#ifndef GT2_CACHED
#include "native_model_vertex.glsl"
#endif
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
    outPaintView=vec3(0,0,-1);
#ifndef GT2_CACHED
    outNativeAlpha=1.0;outNativeTint=vec3(1);outNativeMaterial=0u;outNativeInfo=vec4(0);outNativeState=vec4(0);outNativeEnvironmentBasis=mat3(1);
    if((inFlags & 64u)!=0u) {
        vec3 sourceNormal=nativeVertices.entries[gl_VertexIndex-1228800].xyz;
        if(pc.space==0u) {
            mat3 basis=nativeProjectedBasis(views.worldVP[v]);
            vec3 ray=transpose(basis)*nativeProjectedRay(p.xyz,views.worldVP[v]);
            nativeVertex(mat3(pc.mvp)*sourceNormal,-ray,ray,basis,transpose(basis));
        }
        else {
            vec3 ray=nativeProjectedRay(inPos,pc.mvp);
            nativeVertex(nativeProjectedNormal(sourceNormal,pc.mvp),-ray,ray,mat3(1),nativeModelBasis()*transpose(nativeProjectedBasis(pc.mvp)));
        }
    } else
#endif
    if ((inFlags & 268435456u)!=0u) {
        mat3 basis=mat3(pc.mvp);
        // Remove projection/scale magnitude; only orientation drives the sheen.
        vec3 right=normalize(vec3(basis[0].x,basis[1].x,basis[2].x));
        vec3 down=normalize(vec3(basis[0].y,basis[1].y,basis[2].y));
        vec3 forward=normalize(cross(right,down));
        if(pc.space==0u) {
            // Both eyes share world normals and the reference-eye ray. Asymmetric
            // projection and eye translation affect visibility, never the material.
            outColor=basis*inColor;outPaintView=p.xyz;
        } else outColor=vec3(dot(right,inColor),-dot(down,inColor),-dot(forward,inColor));
    }
    outPage = inPage;
    outClut = inClut;
    outFlags = inFlags;
    outTexelAffine = inTexel;
    outColorAffine = inColor;
    outRect = inRect;
    vec4 screen = pc.mvp * vec4(inPos, 1.0);
    outScreen = vec3(screen.xy, screen.w);
}
