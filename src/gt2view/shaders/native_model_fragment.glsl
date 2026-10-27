layout(location=11) in float inNativeAlpha;
layout(location=12) in vec3 inNativeTint;
layout(location=13) flat in uint inNativeMaterial;
layout(location=14) flat in vec4 inNativeInfo;
layout(location=15) flat in vec4 inNativeState;
layout(location=16) flat in mat3 inNativeEnvironmentBasis;
vec4 nativeModelColor(vec4 textureColor) {
    vec4 info=inNativeInfo,state=inNativeState;
    if(state.z>.5 && !gl_FrontFacing && (inFlags & 134217728u)==0u)discard;
    int type=int(info.x);vec4 c=vec4(textureColor.rgb*inColor,textureColor.a*inNativeAlpha);
    if((inFlags & (1u|4194304u))!=0u) {
        int regs=int(state.y),tfx=(regs>>8)&3;bool tcc=(regs&1024)!=0;
        if(tfx==1)c=vec4(textureColor.rgb,tcc?textureColor.a:inNativeAlpha);
        else if(tfx>=2) {
            c.rgb+=vec3(inNativeAlpha*(128.0/255.0));
            c.a=tcc?(tfx==2?textureColor.a+inNativeAlpha:textureColor.a):inNativeAlpha;
        }
    }
#ifndef GT2_MODEL_GLASS_ONLY
    if(type==2) {
        bool course=pc.space==0u || (pc.options & 16u)!=0u;
        // DynamicEnvmapCamera (0044e660): the traced warp mesh has
        // radius/128 = sin(theta/2)/0.9 and a 15-degree capture tilt.
        // Omitting that warp reaches uncaptured directions near grazing
        // angles and cuts the highlight into black bands.
        vec2 xy=vec2(inTexel.x-.5,.5-inTexel.y)*2.0*(course?.9:1.0);
        float rho2=min(dot(xy,xy),1.0),scale=2.0*sqrt(max(1.0-rho2,0.0));
        vec3 r=vec3(xy*scale,1.0-2.0*rho2);
        vec3 warped=vec3(r.x,.965925826*r.y+.258819045*r.z,-.258819045*r.y+.965925826*r.z);
        vec3 environment=course?nativeCourseEnvironment(inNativeEnvironmentBasis*warped):pow(textureLod(paintStudio,r,0).rgb,vec3(2))*3.0;
        c=vec4(environment*inNativeTint,127.0/128.0);
    } else if(type==4)c=vec4(inNativeTint,127.0/128.0);
#endif
    if((inFlags & (1u|4194304u))==0u && type!=2 && type!=4)c.rgb*=128.0/255.0;
    float a=c.a*128.0,ref=info.w;int f=int(info.z);bool pass=true;
    if(f==0)pass=false;else if(f==2)pass=a<ref;else if(f==3)pass=a<=ref;
    else if(f==4)pass=abs(a-ref)<.5;else if(f==5)pass=a>=ref;
    else if(f==6)pass=a>ref;else if(f==7)pass=abs(a-ref)>=.5;
    if(!pass && (state.x<.5 || abs(state.x-2.0)<.5))discard;
    c.rgb=cabinColor(c.rgb);
    // Seated eye adaptation for the baked near-black upholstery. Preserve
    // texture contrast, alpha tests and exterior/studio material response.
    if((inFlags & 512u)!=0u) {
        float l=clamp(dot(c.rgb,vec3(.2126,.7152,.0722)),0.0,1.0);
        c.rgb+=vec3(.075)*pow(1.0-l,3.0);
    }
    // Preserve destination alpha for GT4's subsequent Cs*Ad+Cd pass. Windows
    // retain their coverage even when the texture itself has only binary alpha.
    return vec4(min(c.rgb,vec3(1)),clamp(c.a,0.0,1.0));
}
