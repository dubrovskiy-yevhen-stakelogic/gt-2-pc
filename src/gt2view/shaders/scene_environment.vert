#version 450
// Track-only reflection capture. Cache slots are resolved once on CPU when
// geometry or VRAM changes, rather than hashing every vertex in three views.
layout(location=0) in vec3 inPos;
layout(location=1) in vec2 inTexel;
layout(location=2) in vec3 inColor;
layout(location=3) in uint inPage;
layout(location=4) in uint inClut;
layout(location=5) in uint inFlags;
layout(location=6) in vec4 inRect;
layout(location=7) in uint inCode;
layout(push_constant) uniform Push { mat4 mvp; uint paint; uint brakeLit; uint stpPass; uint options; } pc;
layout(location=0) out vec2 outTexel;
layout(location=1) out vec3 outColor;
layout(location=2) flat out uint outPage;
layout(location=3) flat out uint outClut;
layout(location=4) flat out uint outFlags;
layout(location=5) noperspective out vec2 outTexelAffine;
layout(location=6) noperspective out vec3 outColorAffine;
layout(location=7) flat out vec4 outRect;
layout(location=8) out vec3 outScreen;
layout(location=9) flat out uint outCache;
layout(location=10) out vec3 outPaintView;
void main() {
    vec4 screen=pc.mvp*vec4(inPos,1);
    gl_Position=screen;
    gl_Position.z*=1.0+float((inFlags>>12)&3u)*2e-5;
    outTexel=inTexel;outColor=inColor;outPage=inPage;outClut=inClut;outFlags=inFlags;
    outTexelAffine=inTexel;outColorAffine=inColor;outRect=inRect;
    outScreen=vec3(screen.xy,screen.w);outCache=inCode;outPaintView=vec3(0,0,-1);
}
