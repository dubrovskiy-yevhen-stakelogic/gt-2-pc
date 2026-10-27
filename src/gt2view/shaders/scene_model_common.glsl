// Mono imported models do not use the PS1 palette/VRAM interpreter. Keep the
// same atlas, alpha masks and clearcoat, without its dependent buffer reads.
layout(push_constant) uniform Push {
    mat4 mvp; uint paint; uint brakeLit; uint stpPass; uint options;
    uint space; uint eye; uvec2 reserved; vec4 hudClip;
} pc;
layout(location=0) in vec2 inTexel;
layout(location=1) in vec3 inColor;
layout(location=2) flat in uint inPage;
layout(location=3) flat in uint inClut;
layout(location=4) flat in uint inFlags;
layout(location=8) in vec3 inScreen;
layout(location=10) in vec3 inPaintView;
layout(set=0,binding=6) uniform sampler2D rearView;
layout(set=0,binding=7) uniform sampler2D modelAtlas;
layout(location=0) out vec4 outColor;
#ifndef GT2_MODEL_GLASS_ONLY
#include "paint_clearcoat.glsl"
#endif
vec3 cabinColor(vec3 color) {
    if ((inFlags & 536870912u)!=0u) return (inFlags & 67108864u)!=0u ? vec3(0,1,0) : vec3(.055,.060,.067);
    if ((inFlags & 67108864u)!=0u) return gl_FrontFacing ? vec3(1,0,1) : vec3(0,1,0);
    if (!gl_FrontFacing && (inFlags & 134217728u)!=0u) return vec3(.055,.060,.067);
    return color;
}
#include "native_model_fragment.glsl"
void main() {
    // Stereo HUD mirrors clip in their own plane; framebuffer scissors cannot
    // follow its per-eye projection. Match the general and cached car shaders.
    if(pc.hudClip.z>pc.hudClip.x && (any(lessThan(inScreen.xy/inScreen.z,pc.hudClip.xy)) ||
       any(greaterThan(inScreen.xy/inScreen.z,pc.hudClip.zw))))discard;
    // Opaque and transparent draws share immutable geometry. Select fragments
    // before doing any texture reads, keeping early cabin depth effective.
    if ((inFlags & 32u)!=0u ? pc.stpPass!=3u : pc.stpPass==3u) discard;
    if ((inFlags & 4194304u)==0u) {
        if((inFlags & 64u)!=0u){outColor=nativeModelColor(vec4(1));return;}
        outColor=vec4(cabinColor(inColor),1);return;
    }
    vec2 size=vec2(inClut & 65535u,inClut >> 16);
    vec2 origin=vec2(inPage & 65535u,inPage >> 16);
    vec2 uv=fract(inTexel/size)*size;
    if ((inFlags & 8388608u)!=0u) uv.x=clamp(inTexel.x,0.5,size.x-0.5);
    if ((inFlags & 16777216u)!=0u) uv.y=clamp(inTexel.y,0.5,size.y-0.5);
    if ((pc.options & 1u)==0u) uv=floor(uv)+0.5;
    vec4 c=textureLod(modelAtlas,(origin+uv)/vec2(textureSize(modelAtlas,0)),0);
    if ((inFlags & 2147483648u)!=0u) {
        if (c.a<0.5 && distance(c.rgb,vec3(8,16,16)/255.0)>.025) discard;
        c.a=1.0;
    }
    if ((inFlags & 2097152u)!=0u) {
        if (c.a<0.5 && distance(c.rgb,vec3(8,16,16)/255.0)>.025) discard;
        outColor=vec4(textureLod(rearView,inColor.xy,0).rgb,1);return;
    }
    if((inFlags & 64u)!=0u){outColor=nativeModelColor(c);return;}
    if ((inFlags & 32u)!=0u && c.a<.5/255.0) discard;
        if ((inFlags & (33554432u|32u))==0u && c.a<((inFlags & 1073741824u)!=0u ? .5/255.0 : .5)) discard;
#ifdef GT2_MODEL_GLASS_ONLY
    outColor=vec4(cabinColor(c.rgb*inColor),c.a);
#else
    if ((inFlags & 268435456u)!=0u) {
        if (!gl_FrontFacing) discard;
        outColor=vec4(paintClearcoat(c.rgb,inColor,inPaintView,pc.space==0u),1);
    } else outColor=vec4(cabinColor(c.rgb*inColor),(inFlags & 32u)!=0u?c.a:1.0);
#endif
}
