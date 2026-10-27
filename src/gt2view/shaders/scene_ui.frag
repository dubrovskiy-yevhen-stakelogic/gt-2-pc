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
#ifndef GT2_WEBGL
#extension GL_GOOGLE_include_directive : require
#endif
#ifdef GT2_WEBGL
const bool cachedOnly = false;
#else
layout(constant_id = 0) const bool cachedOnly = false;
#endif

// PS1-style texturing: the whole 1024-wide VRAM image lives in a buffer of 16-bit words and
// polygons address it by page + CLUT, exactly like the original GPU. Rows 0-511 mirror the
// console VRAM (course textures); rows 512+ are ours (car texture and its paint CLUTs).
#ifdef GT2_WEBGL
uniform highp usampler2D vramImage;
uniform highp usampler2D externalImage;
in float outAffineW;
uint externalWord(uint i) { return texelFetch(externalImage, ivec2(int(i & 4095u), int(i >> 12)), 0).r; }
#else
layout(std430, set = 0, binding = 0) readonly buffer Vram { uint words[]; } vram;

// External RGBA8 images (mod meshes): flag 16; page = first texel, clut = width | height << 16.
layout(std430, set = 0, binding = 1) readonly buffer External {
    uint texels[];
} ext;

uint externalWord(uint i) { return ext.texels[i]; }
layout(set = 0, binding = 7) uniform sampler2D modelAtlas;
#endif

#ifdef GT2_WEBGL
struct Push {
#else
layout(push_constant) uniform Push {
#endif
    mat4 mvp;
    uint paint;
    uint brakeLit;
    uint stpPass; // DrawItem::stpPass
    uint options; // 1 smooth textures, 2 affine mapping (scene items only; 0 = the PS1's nearest texel, perspective-correct)
    uint space;
    uint eye;
    uvec2 reserved;
    vec4 hudClip;
#ifdef GT2_WEBGL
};
uniform Push pc;
#else
} pc;
#endif

GT2_VARYING(0) in vec2 inTexel;
GT2_VARYING(1) in vec3 inColor;
GT2_VARYING(2) flat in uint inPage;
GT2_VARYING(3) flat in uint inClut;
GT2_VARYING(4) flat in uint inFlags;
GT2_VARYING(5) noperspective in vec2 inTexelAffine;
GT2_VARYING(6) noperspective in vec3 inColorAffine;
GT2_VARYING(7) flat in vec4 inRect; // UV rectangle of the triangle (texel units)

GT2_VARYING(8) in vec3 inScreen;
layout(location = 0) out vec4 outColor;
GT2_VARYING(9) flat in uint inCache;
GT2_VARYING(10) in vec3 inPaintView;
#ifdef GT2_WEBGL
uniform sampler2DArray decodedPages;
#else
layout(set = 0, binding = 3) uniform sampler2DArray decodedPages;
#endif
#ifdef GT2_WEBGL
uniform sampler2D handAlbedo;
#else
layout(set = 0, binding = 5) uniform sampler2D handAlbedo;
#endif
#ifdef GT2_WEBGL
uniform sampler2D rearView;
#else
layout(set = 0, binding = 6) uniform sampler2D rearView;
#endif

uint word(uint x, uint y) {
#ifdef GT2_WEBGL
    uint at = y * 1024u + x;
    return texelFetch(vramImage, ivec2(int(at & 1023u), int(at >> 10)), 0).r;
#else
    return vram.words[y * 1024u + x];
#endif
}

// One 16-bit texel of a texture page: 4-bit / 8-bit through the CLUT, or direct 15-bit.
uint pageTexel(uvec2 t, uint pageX, uint pageY, uint clutX, uint clutY, uint depth) {
    if (cachedOnly || inCache != 0u) {
        int layer = int((inCache & 65535u) - 1u);
        uvec4 c = uvec4(round(texelFetch(decodedPages, ivec3(t, layer), 0) * 255.0));
        uint stp = (inCache & 65536u) >> 1;
        if ((inCache & 131072u) != 0u && c.a == 0u) {
            c = uvec4(round(texelFetch(decodedPages, ivec3(t, layer + 1), 0) * 255.0));
            stp = 32768u;
        }
        return c.a == 0u ? 0u : (c.r >> 3) | ((c.g >> 3) << 5) | ((c.b >> 3) << 10) | stp;
    }
    if (depth == 0u) {
        uint w = word(pageX + t.x / 4u, pageY + t.y);
        return word(clutX + ((w >> ((t.x & 3u) * 4u)) & 15u), clutY);
    }
    if (depth == 1u) {
        uint w = word(pageX + t.x / 2u, pageY + t.y);
        return word(clutX + ((w >> ((t.x & 1u) * 8u)) & 255u), clutY);
    }
    return word(pageX + t.x, pageY + t.y);
}

vec3 rgb15(uint texel) { return vec3(float(texel & 31u), float((texel >> 5) & 31u), float((texel >> 10) & 31u)) / 31.0; }

vec4 externalTexel(ivec2 t, ivec2 size) {
    if ((inFlags & 8388608u)!=0u) t.x=clamp(t.x,0,size.x-1);
    if ((inFlags & 16777216u)!=0u) t.y=clamp(t.y,0,size.y-1);
    t = ((t % size) + size) % size;
    uint px = externalWord(inPage + uint(t.y) * uint(size.x) + uint(t.x));
    return vec4(float(px & 255u), float((px >> 8) & 255u), float((px >> 16) & 255u), float(px >> 24)) / 255.0;
}

vec4 uiColour(uint px, vec3 color) {
    if (px==0u || (pc.stpPass==1u && (px&32768u)!=0u) || (pc.stpPass==2u && (px&32768u)==0u)) return vec4(0);
    vec3 rgb = rgb15(px);
    if ((inFlags & 16384u) != 0u) rgb=min(floor(rgb*31.0*floor(color*255.0+0.5)/128.0),vec3(31))/31.0;
    else if ((inFlags & 2u) == 0u) rgb=min(rgb*color*(255.0/128.0),vec3(1));
    return vec4(rgb,1);
}

vec4 uiTexel(ivec2 t, ivec2 lo, ivec2 hi, vec3 color) {
    t = clamp(t, lo, hi);
    uint px;
    if ((inFlags & 131072u) != 0u) {
        uint address = uint(t.y)*1024u+uint(t.x);
        uint index = (externalWord(inPage+address/4u) >> ((address&3u)*8u)) & 255u;
        px = word((inClut&65535u)+index, inClut>>16);
    } else {
        px = pageTexel(uvec2(t), inPage&65535u, inPage>>16, inClut&65535u, inClut>>16, (inFlags>>8)&3u);
    }
    return uiColour(px,color);
}

vec4 contourTexel(ivec2 t,ivec2 lo,ivec2 hi,vec3 color) {
    t=clamp(t,lo,hi);
    uint address=uint(t.y)*1024u+uint(t.x);
    uint pair=(externalWord(inPage+address/2u)>>((address&1u)*16u))&65535u;
    vec4 c=uiColour(word((inClut&65535u)+(pair&15u),inClut>>16),color);
    if((pair>>8)!=0u) c=mix(c,uiColour(word((inClut&65535u)+((pair>>4)&15u),inClut>>16),color),float(pair>>8)/255.0);
    return c;
}

// Mono menu primitives only. Preserve the general shader's palette, STP,
// contour and bilinear equations; exclude the unrelated model/mip branches.
void main() {
    vec3 color=inColor;
    if((inFlags & 2048u)!=0u && !gl_FrontFacing) discard;
    if((inFlags & 1u)==0u){
        if(pc.stpPass==1u) discard;
        outColor=vec4(color,1);return;
    }
    if((inFlags & 1048576u)!=0u){
        ivec2 lo=clamp(ivec2(floor(inRect.xy)),ivec2(0),ivec2(1023));
        ivec2 hi=clamp(max(ivec2(ceil(inRect.zw))-1,lo),lo,ivec2(1023));
        vec2 p=inTexel-0.5;ivec2 t=ivec2(floor(p));vec2 f=fract(p);
        vec4 c=mix(mix(contourTexel(t,lo,hi,color),contourTexel(t+ivec2(1,0),lo,hi,color),f.x),
                   mix(contourTexel(t+ivec2(0,1),lo,hi,color),contourTexel(t+ivec2(1,1),lo,hi,color),f.x),f.y);
        if(c.a<0.001 || (pc.stpPass==2u && c.a<0.5)) discard;
        outColor=vec4(c.rgb/c.a,pc.stpPass==2u?1.0:c.a);return;
    }
    if((inFlags & 262144u)!=0u){
        int limit=(inFlags & 131072u)!=0u?1023:255;
        ivec2 lo=clamp(ivec2(floor(inRect.xy)),ivec2(0),ivec2(limit));
        ivec2 hi=clamp(max(ivec2(ceil(inRect.zw))-1,lo),lo,ivec2(limit));
        int scale=(inFlags & 131072u)!=0u?4:1;
        int step=max(1,scale/2);
        vec2 p=(inTexel-0.5*float(step))/float(step);
        ivec2 base=ivec2(floor(p))*step;vec2 f=fract(p);
        vec4 c=mix(mix(uiTexel(base,lo,hi,color),uiTexel(base+ivec2(step,0),lo,hi,color),f.x),
                   mix(uiTexel(base+ivec2(0,step),lo,hi,color),uiTexel(base+ivec2(step,step),lo,hi,color),f.x),f.y);
        if(c.a<0.001 || (pc.stpPass==2u && c.a<0.5)) discard;
        outColor=vec4(c.rgb/c.a,pc.stpPass==2u?1.0:c.a);return;
    }
    if((inFlags & 131072u)!=0u){
        ivec2 lo=clamp(ivec2(floor(inRect.xy)),ivec2(0),ivec2(1023));
        ivec2 hi=clamp(max(ivec2(ceil(inRect.zw))-1,lo),lo,ivec2(1023));
        vec4 c=uiTexel(ivec2(floor(inTexel)),lo,hi,color);
        if(c.a<0.5)discard;
        outColor=vec4(c.rgb/c.a,1);return;
    }
    if((inFlags & 8u)!=0u){
        uvec2 t=uvec2(clamp(ivec2(floor(inTexel)),ivec2(0),ivec2(1023)));
        uint px=word((inPage & 65535u)+t.x,(inPage>>16)+t.y);
        if((px & 32768u)==0u)discard;
        outColor=vec4(rgb15(px),1);return;
    }
    uvec2 t=uvec2(clamp(ivec2(floor(inTexel)),ivec2(0),ivec2(255)));
    if((inFlags & 32768u)!=0u)t=uvec2(clamp(ivec2(t),ivec2(floor(inRect.xy)),ivec2(max(ceil(inRect.zw)-1.0,floor(inRect.xy)))));
    uint px=pageTexel(t,inPage & 65535u,inPage>>16,inClut & 65535u,inClut>>16,(inFlags>>8)&3u);
    vec4 c=uiColour(px,color);
    if(c.a==0.0)discard;
    outColor=c;
}
