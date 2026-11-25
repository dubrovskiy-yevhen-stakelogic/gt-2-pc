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
const bool worldOnly = false;
#else
layout(constant_id = 0) const bool cachedOnly = false;
layout(constant_id = 1) const bool worldOnly = false;
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
layout(set = 0, binding = 14) uniform sampler2D generatedAtlas;
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
    float farClip;
    uint mirrorView;
    vec4 hudClip;
#ifdef GT2_WEBGL
    vec4 nativeModelRotation; // Match the vertex-stage uniform struct for WebGL linking.
};
uniform Push pc;
#else
} pc;
#endif

GT2_VARYING(0) in vec2 inTexel;
GT2_VARYING(1) in vec3 inColor;
GT2_VARYING(2) flat in uint inPage;
GT2_VARYING(3) flat in uint inClut;
#ifdef GT2_WEBGL
GT2_VARYING(4) flat in uint inFlags;
#else
GT2_VARYING(4) flat in uint inFlagsRaw;
// CPU admits only original/generated world flags to this specialization.
#define inFlags (worldOnly ? (inFlagsRaw & 0x0180ff17u) : inFlagsRaw)
#endif
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

// Every Seattle lane samples the same generated aggregate in track metres.
vec3 asphaltSample(uint base, ivec2 coord, uint side) {
    uvec2 p=uvec2(coord)&(side-1u);
    uint word=externalWord(base+p.y*side+p.x);
    return vec3(word&255u,(word>>8)&255u,(word>>16)&255u)/255.0;
}

vec3 asphaltMip(uint base, vec2 uv, int level) {
    uint side=512u>>level;
    base+=((512u*512u-side*side)*4u)/3u;
    uv=uv*(float(side)/512.0)-0.5;
    ivec2 at=ivec2(floor(uv));
    vec2 f=fract(uv);
    return mix(mix(asphaltSample(base,at,side),asphaltSample(base,at+ivec2(1,0),side),f.x),
               mix(asphaltSample(base,at+ivec2(0,1),side),asphaltSample(base,at+ivec2(1),side),f.x),f.y);
}

// Only the view behind each sash moves. The surround and central mullions stay anchored.
vec2 sashParallax(vec2 texel, vec2 size, bool tall) {
    vec3 dx=dFdx(inPaintView),dy=dFdy(inPaintView);
    vec2 tx=dFdx(inTexel),ty=dFdy(inTexel);
    vec2 nativeSize=vec2(96,tall ? 96 : 70);
    vec2 uv=texel/size*nativeSize;
    vec2 opening=tall ? vec2(6,16) : vec2(7,9);
    vec2 local;
    if(tall) {
        float bay=uv.x<34.0 ? 6.0 : uv.x<64.0 ? 35.0 : 65.0;
        local=vec2(mod(uv.x-bay,9.0),mod(uv.y-10.0,20.0));
        if(uv.x<bay || uv.x>=bay+24.0 || uv.y<10.0 || uv.y>=66.0) return texel;
    } else {
        local=mod(uv-vec2(6,5),vec2(13));
        if(uv.x<6.0 || uv.x>=91.0 || uv.y<5.0 || uv.y>=66.0) return texel;
    }
    vec2 pane=local/opening;
    vec2 edge=min(pane,1.0-pane);
    vec2 margin=min(edge,abs(pane-.5))-.045;
    if(min(margin.x,margin.y)<=0.0) return texel;
    float det=tx.x*ty.y-tx.y*ty.x;
    if(abs(det)<1e-9) return texel;
    vec3 tangent=normalize((dx*ty.y-dy*tx.y)*sign(det));
    vec3 bitangent=normalize((dy*tx.x-dx*ty.x)*sign(det));
    float front=max(abs(dot(inPaintView,cross(tangent,bitangent))),.001);
    vec2 drift=.08*vec2(dot(inPaintView,tangent),dot(inPaintView,bitangent))/front;
    return texel+clamp(drift,-margin,margin)*opening*size/nativeSize;
}

// Intersect the view ray with a finite room behind a stationary opening.
vec3 windowRoom(vec3 facade, vec2 uv, vec2 size) {
    uint dims=externalWord(inPage-2u);
    uvec2 native=uvec2(dims&65535u,dims>>16);
    vec2 p=clamp(uv/size*vec2(native),vec2(0),vec2(native)-.001);
    vec3 dx=dFdx(inPaintView),dy=dFdy(inPaintView);
    vec2 tx=dFdx(inTexel),ty=dFdy(inTexel), footprint=fwidth(p);
    float det=tx.x*ty.y-tx.y*ty.x;
    uint index=uint(p.y)*native.x+uint(p.x);
    uint maskBase=externalWord(inPage-5u), boxesBase=externalWord(inPage-4u);
    uint box;
    if(boxesBase!=0u) {
        uint packed=externalWord(maskBase+index/4u);
        uint opening=(packed>>((index%4u)*8u))&255u;
        if(opening==0u) return facade;
        box=externalWord(boxesBase+opening-1u);
    } else box=externalWord(maskBase+index);
    if(box==0u || abs(det)<1e-9) return facade;
    vec2 origin=vec2(box&255u,(box>>8)&255u), span=vec2((box>>16)&255u,box>>24);
    vec2 pane=(p-origin)/span;
    vec2 frame=min(pane,1.-pane);
    float border=min(min(frame.x,frame.y),min(abs(pane.x-.5),abs(pane.y-.5))+.014);
    float pixelSpan=max(footprint.x/span.x,footprint.y/span.y);
    float aa=clamp(pixelSpan,.008,.18);
    float glazing=smoothstep(.026,.026+aa,border)*(1.-smoothstep(.18,.70,pixelSpan));
    // Frames and subpixel openings contribute no interior. Skip the ray and
    // four room-atlas reads once their existing coverage has reached zero.
    if(glazing==0.) return facade;
    vec3 u=normalize((dx*ty.y-dy*tx.y)*sign(det));
    vec3 v=normalize((dy*tx.x-dx*ty.x)*sign(det));
    float facing=max(abs(dot(inPaintView,normalize(cross(u,v)))),.0001);
    vec2 slope=vec2(dot(inPaintView,u),dot(inPaintView,v))/facing*.72*min(span.x,span.y)/span;
    vec2 hit=(mix(vec2(0),vec2(1),greaterThanEqual(slope,vec2(0)))-pane)/
        (sign(slope+1e-12)*max(abs(slope),vec2(1e-5)));
    float depth=clamp(min(hit.x,hit.y),0.,1.);
    vec2 wall=clamp(pane+slope*depth,0.,1.);
    uint seed=uint(origin.x)*13u+uint(origin.y)*7u;
    vec2 tile=vec2(seed%2u,(seed/2u)%2u)*96.;
    vec2 at=tile+wall*94.;
    vec3 room=vec3(0);
    uint base=externalWord(inPage-3u);
    for(int k=0;k<4;++k) {
        ivec2 d=ivec2(k&1,k>>1), xy=ivec2(at)+d;
        uint c=externalWord(base+uint(xy.x+192*xy.y));
        vec2 weight=mix(1.-fract(at),fract(at),vec2(d));
        room+=vec3(c&255u,(c>>8)&255u,(c>>16)&255u)*(weight.x*weight.y/255.);
    }
    if(depth<1.) {
        vec3 plaster=mix(vec3(.38,.29,.21),vec3(.70,.57,.40),depth);
        if(hit.y<hit.x) plaster*=slope.y>0. ? .47 : 1.14;
        room=mix(plaster,room,.24);
    }
    bool night=externalWord(inPage-1u)!=0u;
    if(night) room*=seed%5u==0u ? vec3(.19,.22,.29) : vec3(1.3,1.02,.78);
    else room=room*.48+facade*.32;
    return mix(facade,room,glazing*(night?.93:.79));
}

#ifndef GT2_WEBGL
vec4 generatedLevel(uint base,ivec2 size,vec2 uv,int level) {
    uint packed=externalWord(base+uint(size.x*size.y)+uint(level));
    vec2 origin=vec2(packed&65535u,packed>>16);
    ivec2 dims=max(size>>level,ivec2(1));
    vec2 at=clamp(uv*vec2(dims),vec2(.5),vec2(dims)-.5);
    return textureLod(generatedAtlas,(origin+at)/4096.,0.);
}
vec4 generatedSample(uint base,ivec2 size,vec2 uv,float lod) {
    float last=floor(log2(float(max(size.x,size.y))));
    lod=clamp(lod,0.,last);int level=int(lod);
    vec4 c=mix(generatedLevel(base,size,uv,level),generatedLevel(base,size,uv,min(level+1,int(last))),fract(lod));
    if(c.a>1e-5)c.rgb/=c.a;
    return c;
}
#endif
vec3 asphaltAlbedo(vec3 control) {
    vec2 uv=inPaintView.xz*96.0;
    vec2 dx=dFdx(uv),dy=dFdy(uv);
    vec2 major=dot(dx,dx)>dot(dy,dy)?dx:dy;
    float minor=min(length(dx),length(dy));
    float lod=clamp(log2(max(1.0,max(minor,length(major)*.25))),0.0,8.99);
    int level=int(floor(lod));
    uint base=externalWord(inPage-1u);
    vec3 stone=vec3(0);
#ifndef GT2_WEBGL
    if(worldOnly || (inFlags&16384u)!=0u) {
        // Hardware bilinear mip tiles replace 36 incoherent storage-buffer reads.
        float hardwareLod=clamp(log2(max(1.,length(major))),0.,9.);
        stone=generatedSample(base,ivec2(512),fract(uv/512.),hardwareLod).rgb;
        stone+=(generatedSample(base,ivec2(512),fract(uv*.083/512.),max(1.,hardwareLod-2.)).rgb-vec3(105,103,100)/255.)*.36;
        float paint=smoothstep(.06,.30,max(control.r-control.b,control.r-.51));
        return mix(stone*control*(255./128.),control,paint);
    }
#endif
    for(int tap=0;tap<4;++tap) {
        vec2 at=uv+major*((float(tap)-1.5)*.25);
        stone+=mix(asphaltMip(base,at,level),asphaltMip(base,at,level+1),fract(lod))*.25;
    }
    // Larger patches of aggregate remain legible beyond the individual stones.
    stone+=(asphaltMip(base,uv*.083,clamp(level-2,1,9))-vec3(105,103,100)/255.0)*.36;
    float paint=smoothstep(.06,.30,max(control.r-control.b,control.r-.51));
    return mix(stone*control*(255.0/128.0),control,paint);
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

#include "texture_mips.glsl"

vec3 cabinColor(vec3 color) {
    if ((inFlags & 536870912u)!=0u) return (inFlags & 67108864u)!=0u ? vec3(0,1,0) : vec3(.055,.060,.067);
    if ((inFlags & 67108864u)!=0u) return gl_FrontFacing ? vec3(1,0,1) : vec3(0,1,0);
    if (!gl_FrontFacing) {
        if ((inFlags & 134217728u)!=0u) return vec3(.055,.060,.067);
    }
    return color;
}

#ifndef GT2_WEBGL
#include "paint_clearcoat.glsl"
#endif
vec3 modelColor(vec3 base,vec3 color) {
    if ((inFlags & 268435456u)==0u) return cabinColor(base*color);
    if (!gl_FrontFacing) discard;
#ifdef GT2_WEBGL
    vec3 n=normalize(color),r=reflect(vec3(0,0,-1),n);
    vec3 radiance=mix(vec3(.07),vec3(.34,.39,.47),clamp(r.y*.65+.45,0.0,1.0));
    float fresnel=.075+.78*pow(1.0-abs(n.z),5.0);
    return max(sqrt(base*base+radiance*fresnel)-base,vec3(0));
#else
    return paintClearcoat(base,color,inPaintView,pc.space==0u);
#endif
}

#ifndef GT2_WEBGL
#include "native_model_fragment.glsl"
#endif
#include "authored_glass.glsl"

void shadeScene() {
    // Draw Distance: the same far clip in the main views, split screen and the rear-view mirror.
    if (pc.farClip > 0.0 && inScreen.z > pc.farClip) discard;
    // Opaque and transparent draws share immutable geometry. Select fragments
    // before doing any texture reads, keeping early cabin depth effective.
    if ((inFlags & 32u)!=0u ? pc.stpPass!=3u : pc.stpPass==3u) discard;
#ifndef GT2_WEBGL
    if((inFlags & 64u)!=0u && (inFlags & (16u|4194304u))==0u){outColor=nativeModelColor(vec4(1));return;}
#endif
    if ((inFlags & 268435456u)!=0u && (inFlags & (16u|4194304u))==0u) {
        if (!gl_FrontFacing) discard;
#ifndef GT2_WEBGL
        outColor=vec4(stockPaintClearcoat(inColor,inPaintView,pc.space==0u || (pc.options & 16u)!=0u),1);return;
#endif
    }
#ifndef GT2_WEBGL
    if ((inFlags & 4194304u) != 0u) {
        vec2 size=vec2(inClut & 65535u,inClut >> 16);
        vec2 origin=vec2(inPage & 65535u,inPage >> 16);
        vec2 uv=fract(inTexel/size)*size;
        if ((inFlags & 8388608u)!=0u) uv.x=clamp(inTexel.x,0.5,size.x-0.5);
        if ((inFlags & 16777216u)!=0u) uv.y=clamp(inTexel.y,0.5,size.y-0.5);
        // Keep binary native cabin aperture coverage independent of smoothing.
        if ((pc.options & 1u)==0u || (inFlags & 1088u)==1088u) uv=floor(uv)+0.5;
        vec4 c=textureLod(modelAtlas,(origin+uv)/vec2(textureSize(modelAtlas,0)),0);
        if ((inFlags & 2147483648u)!=0u) {
            if (c.a<0.5 && distance(c.rgb,vec3(8,16,16)/255.0)>.025) discard;
            c.a=1.0;
        }
        if ((inFlags & 2097152u)!=0u) {
            // The authored glass mask clips the reflected image to the mirror,
            // preserving its black border instead of filling the whole quad.
            if (c.a<0.5 && distance(c.rgb,vec3(8,16,16)/255.0)>.025) discard;
            outColor=vec4(textureLod(rearView,inColor.xy,0).rgb,1);return;
        }
        if((inFlags & 64u)!=0u){outColor=nativeModelColor(c);return;}
        if ((inFlags & 32u)!=0u && c.a<.5/255.0) discard;
        if ((inFlags & (33554432u|32u))==0u && c.a<((inFlags & 1073741824u)!=0u ? .5/255.0 : .5)) discard;
        if(authoredGlazing()){outColor=authoredGlass(c);return;}
        outColor=vec4(modelColor(c.rgb,inColor),(inFlags & 32u)!=0u?c.a:1.0);return;
    }
#endif
    if ((inFlags & 2097152u) != 0u && (inFlags & 16u)==0u) {
        #ifdef GT2_WEBGL
        outColor = vec4(textureLod(rearView, vec2(inTexel.x, 1.0-inTexel.y), 0.0).rgb, 1.0);
#else
        outColor = vec4(textureLod(rearView, inTexel, 0).rgb, 1.0);
#endif
        return;
    }
    if ((inFlags & 65536u) != 0u) {
        outColor = vec4(texture(handAlbedo, inTexel).rgb * inColor, 1.0);
        return;
    }
    if (pc.hudClip.z > pc.hudClip.x && (any(lessThan(inScreen.xy / inScreen.z, pc.hudClip.xy)) || any(greaterThan(inScreen.xy / inScreen.z, pc.hudClip.zw)))) discard;
    // The option "affine": the PS1 GPU's screen-linear interpolation; otherwise perspective-correct (Vulkan's default).
    bool affine = (pc.options & 2u) != 0u;
    bool smoothTextures = (pc.options & 1u) != 0u;
    vec2 texelCoord = inTexel;
    vec3 color = inColor;
    if (affine) {
        // The PS1 GPU interpolates screen-linearly, but the original subdivides the large polygons near the camera
        // (not ported: our course polygons are whole), which bounds the warp. Bound it here the same way: the affine
        // texel at most 4 texels from the perspective-correct one (continuous, so no seams).
        #ifdef GT2_WEBGL
        texelCoord = inTexel + clamp(inTexelAffine / outAffineW - inTexel, vec2(-4.0), vec2(4.0));
        color = inColorAffine / outAffineW;
#else
        texelCoord = inTexel + clamp(inTexelAffine - inTexel, vec2(-4.0), vec2(4.0));
        color = inColorAffine;
#endif
    }
    if ((inFlags & 2048u) != 0u) {
        if (pc.mirrorView == 1u) { if (gl_FrontFacing) discard; }
        else if (pc.mirrorView == 0u) { if (!gl_FrontFacing) discard; }
    } // kCullBack: the original's NCLIP test
    if ((inFlags & 1u) == 0u) {
        if (pc.stpPass == 1u) discard; // untextured semi-transparent: blended pass only
        outColor = vec4(cabinColor(color), 1.0);
        return;
    }
    if ((inFlags & 1048576u) != 0u) {
        ivec2 lo=clamp(ivec2(floor(inRect.xy)),ivec2(0),ivec2(1023));
        ivec2 hi=clamp(max(ivec2(ceil(inRect.zw))-1,lo),lo,ivec2(1023));
        vec2 p=texelCoord-0.5;ivec2 t=ivec2(floor(p));vec2 f=fract(p);
        vec4 c=mix(mix(contourTexel(t,lo,hi,color),contourTexel(t+ivec2(1,0),lo,hi,color),f.x),
                   mix(contourTexel(t+ivec2(0,1),lo,hi,color),contourTexel(t+ivec2(1,1),lo,hi,color),f.x),f.y);
        if(c.a<0.001 || (pc.stpPass==2u && c.a<0.5)) discard;
        outColor=vec4(c.rgb/c.a,pc.stpPass==2u?1.0:c.a); return;
    }
    if ((inFlags & (8388608u | 16u)) == 8388608u) { // kNearestUi only: external textures reuse this bit for clamp-U
        ivec2 lo=clamp(ivec2(floor(inRect.xy)),ivec2(0),ivec2(1023));
        ivec2 hi=clamp(max(ivec2(ceil(inRect.zw))-1,lo),lo,ivec2(1023));
        vec4 c=uiTexel(ivec2(floor(texelCoord)),lo,hi,color);
        if(c.a<0.5) discard;
        outColor=vec4(c.rgb/c.a,1); return;
    }
    if ((inFlags & 262144u) != 0u) {
        int limit = (inFlags & 131072u) != 0u ? 1023 : 255;
        ivec2 lo = clamp(ivec2(floor(inRect.xy)), ivec2(0), ivec2(limit));
        ivec2 hi = clamp(max(ivec2(ceil(inRect.zw))-1, lo), lo, ivec2(limit));
        // Reconstructed contours are sampled at HD resolution, without a source-sized blur.
        int scale = (inFlags & 131072u) != 0u ? 4 : 1;
        int step = max(1,scale/2);
        vec2 p = (texelCoord - 0.5*float(step))/float(step);
        ivec2 base = ivec2(floor(p))*step;
        vec2 f = fract(p);
        vec4 c = mix(mix(uiTexel(base,lo,hi,color), uiTexel(base+ivec2(step,0),lo,hi,color), f.x),
                     mix(uiTexel(base+ivec2(0,step),lo,hi,color), uiTexel(base+ivec2(step,step),lo,hi,color), f.x), f.y);
        if(c.a < 0.001) discard;
        // STP draws retain their fixed PS1 blend equation; opaque UI uses alpha coverage.
        if(pc.stpPass==2u && c.a<0.5) discard;
        outColor=vec4(c.rgb/c.a, pc.stpPass==2u ? 1.0 : c.a);
        return;
    }
    if ((inFlags & 131072u) != 0u) {
        ivec2 lo=clamp(ivec2(floor(inRect.xy)),ivec2(0),ivec2(1023));
        ivec2 hi=clamp(max(ivec2(ceil(inRect.zw))-1,lo),lo,ivec2(1023));
        vec4 c=uiTexel(ivec2(floor(texelCoord)),lo,hi,color);
        if(c.a<0.5) discard;
        outColor=vec4(c.rgb/c.a,1); return;
    }
    if (!cachedOnly && (inFlags & 16u) != 0u) { // external RGBA8 image, repeat wrapping, nearest texel, alpha mask at 0.5
        ivec2 size = ivec2(int(inClut & 0xFFFFu), int(inClut >> 16));
        if ((inFlags & 64u)==0u && (inFlags & 768u)!=0u && (inFlags & 768u)!=768u)
            texelCoord=sashParallax(texelCoord,vec2(size),(inFlags & 512u)!=0u);
        vec4 c;
#ifndef GT2_WEBGL
        bool generatedFiltered=(worldOnly || (inFlags&16384u)!=0u) && smoothTextures;
        // Filtered colour/coverage replaces this read entirely except for the
        // PS1 STP class, which deliberately keeps nearest source alpha.
        c=(!generatedFiltered || (inFlags&1024u)!=0u)
            ? externalTexel(ivec2(floor(texelCoord)), size) : vec4(0);
#else
        c=externalTexel(ivec2(floor(texelCoord)), size);
#endif
#ifndef GT2_WEBGL
        if(generatedFiltered) {
            // Compute derivatives before coverage/STP divergence. Cutouts use
            // filtered coverage, not a second level-zero alpha test.
            vec2 dx=dFdx(texelCoord),dy=dFdy(texelCoord);
            float lod=log2(max(1.,max(length(dx),length(dy))));
            vec4 filtered=generatedSample(inPage,size,texelCoord/vec2(size),lod);
            c.rgb=filtered.rgb;
            if((inFlags&1024u)==0u)c.a=filtered.a;
        }
#endif
        if ((inFlags & 2147483648u)!=0u) {
            if (c.a<0.5 && distance(c.rgb,vec3(8,16,16)/255.0)>.025) discard;
            c.a=1.0;
        }
        if ((inFlags & 2097152u)!=0u) {
            if (c.a<0.5 && distance(c.rgb,vec3(8,16,16)/255.0)>.025) discard;
#ifdef GT2_WEBGL
            outColor=vec4(textureLod(rearView,vec2(inColor.x,1.0-inColor.y),0.0).rgb,1);
#else
            outColor=vec4(textureLod(rearView,inColor.xy,0).rgb,1);
#endif
            return;
        }
        // Generated world packs encode the original STP bit in the centre texel.
        // Zero is a cutout, 128 is a blended texel, 255 is an opaque texel.
        if ((inFlags & (1024u|64u))==1024u) {
            bool stp=c.a>0.49 && c.a<0.75;
            if ((pc.stpPass==1u && stp) || (pc.stpPass==2u && !stp)) discard;
        }
        bool opaque = (inFlags & 33554432u)!=0u;
        float cutoff=(inFlags & (1073741824u|32u))!=0u ? .5/255.0 : .5;
        if (!opaque && c.a < cutoff) discard;
#ifndef GT2_WEBGL
        if(!generatedFiltered)
#endif
        if (smoothTextures) { // bilinear over the covering texels (alpha-masked ones left out)
            vec2 p = texelCoord - 0.5;
            vec2 f = fract(p);
            ivec2 i0 = ivec2(floor(p));
            vec3 sum = vec3(0.0);
            float wsum = 0.0;
            for (int k = 0; k < 4; k++) {
                ivec2 d = ivec2(k & 1, k >> 1);
                float w = (d.x == 1 ? f.x : 1.0 - f.x) * (d.y == 1 ? f.y : 1.0 - f.y);
                vec4 s = externalTexel(i0 + d, size);
                if (!opaque && s.a < cutoff) continue;
                sum += w * s.rgb;
                wsum += w;
            }
            if (wsum > 0.0) c.rgb = sum / wsum;
        }
        if((inFlags & 32768u)!=0u) c.rgb=asphaltAlbedo(c.rgb);
        if(authoredGlazing()){outColor=authoredGlass(c);return;}
        vec3 shaded=modelColor(c.rgb,inColor);
        if((inFlags & 832u)==768u) shaded=windowRoom(shaded,texelCoord,vec2(size));
        outColor = vec4(shaded, (inFlags & 32u)!=0u?c.a:1.0);
        return;
    }
    uint pageX = inPage & 0xFFFFu, pageY = inPage >> 16;
    if (!cachedOnly && (inFlags & 8u) != 0u) { // overlay: a 15-bit image stored in our VRAM rows, bit 15 = pixel present
        uvec2 o = uvec2(clamp(ivec2(floor(texelCoord)), ivec2(0), ivec2(1023)));
        uint px = word(pageX + o.x, pageY + o.y);
        if ((px & 0x8000u) == 0u) discard;
        outColor = vec4(vec3(float(px & 31u), float((px >> 5) & 31u), float((px >> 10) & 31u)) / 31.0, 1.0);
        return;
    }
    if (smoothTextures && inCache != 0u && sampleMip(texelCoord, color, outColor)) return;
    uvec2 t = uvec2(clamp(ivec2(floor(texelCoord)), ivec2(0), ivec2(255)));
    if ((inFlags & 32768u) != 0u) t = uvec2(clamp(ivec2(t), ivec2(floor(inRect.xy)), ivec2(max(ceil(inRect.zw) - 1.0, floor(inRect.xy)))));
    uint clutX = inClut & 0xFFFFu, clutY = inClut >> 16;
    if ((inFlags & 4u) != 0u) {
        clutY += pc.paint;
        if (clutX == 224u && pc.brakeLit != 0u) clutX = 240u; // CLUT 14 -> 15
    }

    uint depth = (inFlags >> 8) & 3u;
    uint texel = pageTexel(t, pageX, pageY, clutX, clutY, depth);
    if (texel == 0u) discard; // PS1: colour 0x0000 is transparent
    if (pc.stpPass == 1u && (texel & 0x8000u) != 0u) discard; // semi-transparent texel: the blended pass
    if (pc.stpPass == 2u && (texel & 0x8000u) == 0u) discard; // opaque texel of a semi-transparent polygon

    vec3 rgb = rgb15(texel);
    if (smoothTextures) {
        // Smooth: the PS1 texels decoded through their CLUT first, then filtered bilinearly. The shape stays the
        // nearest texel's (transparency and the STP pass above); only texels of the same class (not transparent, same
        // STP bit) blend, and the taps stay inside the triangle's UV rectangle so that neighbouring images of the
        // page's atlas never bleed in.
        vec2 p = texelCoord - 0.5;
        vec2 f = fract(p);
        ivec2 i0 = ivec2(floor(p));
        ivec2 lo = clamp(ivec2(floor(inRect.xy)), ivec2(0), ivec2(255));
        ivec2 hi = clamp(max(ivec2(ceil(inRect.zw)) - 1, lo), ivec2(0), ivec2(255));
        if (cachedOnly || inCache != 0u) {
            // Coverage-normalized hardware bilinear. Mixed pages select
            // the layer of the nearest texel's STP class.
            vec2 uv = (clamp(p, vec2(lo), vec2(hi)) + 0.5) / 256.0;
            uint layer = (inCache & 65535u) - 1u;
            if ((inCache & 131072u) != 0u && (texel & 32768u) != 0u) ++layer;
            vec4 c = textureLod(decodedPages, vec3(uv, float(layer)), 0.0);
            if (c.a > 0.0) rgb = (c.rgb / c.a);
        } else {
        vec3 sum = vec3(0.0);
        float wsum = 0.0;
        for (int k = 0; k < 4; k++) {
            ivec2 d = ivec2(k & 1, k >> 1);
            float w = (d.x == 1 ? f.x : 1.0 - f.x) * (d.y == 1 ? f.y : 1.0 - f.y);
            uint s = pageTexel(uvec2(clamp(i0 + d, lo, hi)), pageX, pageY, clutX, clutY, depth);
            if (s == 0u || (s & 0x8000u) != (texel & 0x8000u)) continue;
            sum += w * rgb15(s);
            wsum += w;
        }
        if (wsum > 0.0) rgb = sum / wsum;
        }
    }
    if ((inFlags & 16384u) != 0u) { // kIntegerModulate (2D menus): the GPU's integer (texel * c) >> 7 on 5-bit texels
        vec3 t5 = vec3(float(texel & 31u), float((texel >> 5) & 31u), float((texel >> 10) & 31u));
        vec3 c8 = floor(color * 255.0 + 0.5);
        rgb = min(floor(t5 * c8 / 128.0), vec3(31.0)) / 31.0;
    } else if ((inFlags & 2u) == 0u) rgb = min(rgb * color * (255.0 / 128.0), vec3(1.0)); // 0x80 = neutral
    outColor = vec4(rgb, 1.0);
}

void main() {
    // kSpaceMenuCarTexture: the glued menu/showroom car target, alpha preserved for compositing over 2D.
    if (pc.space == 5u) { outColor = texture(rearView, inTexel); return; }
    shadeScene();
    // The mirror is a reflected camera, not a translucent material. Car coverage
    // alpha would otherwise make the body see-through in the mirror target.
    if (pc.mirrorView == 2u) outColor.a = 1.0;
    // Apply diagnostic colour after all coverage tests, including rim alpha.
    if ((inFlags & 128u) != 0u) outColor.rgb = vec3(1,0,1);
}
