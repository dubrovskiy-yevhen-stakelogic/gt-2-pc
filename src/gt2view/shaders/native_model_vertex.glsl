layout(set=0,binding=13) uniform NativeEnvironmentViews {mat4 camera[3];vec4 enabled;vec4 lightDirection;vec4 lightAmbient;vec4 lightDiffuse;vec4 lightSpecular;vec4 stockSky;} nativeViews;
// CAR4 VU routines reconstructed in the user's gt4-pc renderer. No generic PBR
// tint or guessed whole-body reflection weight participates in these passes.
layout(std430,set=0,binding=10) readonly buffer NativeVertices { vec4 entries[]; } nativeVertices;
layout(std430,set=0,binding=11) readonly buffer NativeMaterials { vec4 rows[]; } nativeMaterials;
layout(location=11) out float outNativeAlpha;
layout(location=12) out vec3 outNativeTint;
layout(location=13) flat out uint outNativeMaterial;
layout(location=14) flat out vec4 outNativeInfo;
layout(location=15) flat out vec4 outNativeState;
layout(location=16) flat out mat3 outNativeEnvironmentBasis;
vec3 nativeUnit(vec3 v,vec3 fallback) {float q=dot(v,v);return q>1e-12?v*inversesqrt(q):fallback;}
mat3 nativeProjectedBasis(mat4 m) {
    vec3 f=nativeUnit(vec3(m[0].w,m[1].w,m[2].w),vec3(0,0,1));
    vec3 x=vec3(m[0].x,m[1].x,m[2].x),y=vec3(m[0].y,m[1].y,m[2].y);
    x=nativeUnit(x-f*dot(x,f),vec3(1,0,0));
    y=nativeUnit(y-f*dot(y,f),vec3(0,1,0));
    return transpose(mat3(x,-y,-f));
}
vec3 nativeProjectedNormal(vec3 n,mat4 m) {return nativeProjectedBasis(m)*n;}
vec3 nativeProjectedRay(vec3 p,mat4 m) {
    vec3 f=vec3(m[0].w,m[1].w,m[2].w);float f2=dot(f,f);
    if(f2<1e-12)return vec3(0,0,-1); // Orthographic audit/menu planes.
    vec3 x=vec3(m[0].x,m[1].x,m[2].x),y=vec3(m[0].y,m[1].y,m[2].y);
    float sx=dot(x,f)/f2,sy=dot(y,f)/f2;
    vec4 clip=m*vec4(p,1);
    return vec3((clip.x-sx*clip.w)/max(length(x-sx*f),1e-12),
                -(clip.y-sy*clip.w)/max(length(y-sy*f),1e-12),-clip.w/sqrt(f2));
}
mat3 nativeModelBasis() {
    vec4 q=pc.nativeModelRotation;vec3 x=q.xyz;
    mat3 crossX=mat3(0,x.z,-x.y,-x.z,0,x.x,x.y,-x.x,0);
    return mat3(1)+2.0*q.w*crossX+2.0*crossX*crossX;
}
void nativeVertex(vec3 normal,vec3 incident,vec3 reflectionIncident,mat3 sphereBasis,mat3 environmentBasis) {
    vec4 a=nativeVertices.entries[gl_VertexIndex-1228800];
    uint packed=floatBitsToUint(a.w),m=packed & 65535u;
    outNativeEnvironmentBasis=environmentBasis;
    outNativeMaterial=m;outNativeAlpha=float(packed>>16)*(2.0/65535.0);
    outNativeTint=inColor;
    vec4 info=nativeMaterials.rows[m*6u+4u];int type=int(info.x);
    outNativeInfo=info;outNativeState=nativeMaterials.rows[m*6u+5u];
    vec3 n=nativeUnit(normal,vec3(0,1,0));
    bool course=pc.space==0u || (pc.options & 16u)!=0u;
    // Studio softboxes point down from above the camera. Race light is in
    // world space; only the mono path transforms it into its drawing camera.
    vec3 L=course?nativeViews.lightDirection.xyz:vec3(-.35,.85,.4);
    if(course && pc.space!=0u)L=nativeProjectedNormal(transpose(nativeModelBasis())*L,pc.mvp);
    L=nativeUnit(L,vec3(0,1,0));
    vec3 A=course?nativeViews.lightAmbient.rgb:vec3(.58);
    vec3 D=course?nativeViews.lightDiffuse.rgb:vec3(.65);
    vec3 S=course?nativeViews.lightSpecular.rgb:vec3(.55);

    if(type==1) {
        vec3 E=nativeUnit(incident,vec3(0,0,1));
        vec3 ambient=nativeMaterials.rows[m*6u].rgb;
        vec3 diffuse=nativeMaterials.rows[m*6u+1u].rgb;
        vec3 specular=nativeMaterials.rows[m*6u+2u].rgb;
        vec3 emission=nativeMaterials.rows[m*6u+3u].rgb;
        vec3 col=emission+A*ambient+D*diffuse*max(dot(L,n),0.0);
        if(any(greaterThan(abs(specular),vec3(0)))) {
            float h=max(dot(n,nativeUnit(L+E,L)),0.0),p=h*h;p*=p;
            if(info.y>=6.0)p*=p;if(info.y>=12.0)p*=p;
            col+=S*specular*p;
        }
        // The GS light block uses 255 for untextured and 128 for textured draws.
        if((inFlags & (1u|4194304u))==0u)col*=255.0/128.0;
        outColor=min(col,vec3(255.0/128.0));outNativeAlpha=127.0/128.0;
    } else if(type==2 || type==4) {
        // GT4's VU routines evaluate reflection and its weight per vertex.
        // Interpolate the authored sphere coordinates, not a per-pixel normal.
        vec3 v=nativeUnit(reflectionIncident,vec3(0,0,-1)),r=reflect(v,n);
        outNativeAlpha=127.0/128.0;
        if(type==2) {
            // CORE US1.01 VU 0x10a8 / 0049f1f8: reflect the vertex-to-eye
            // ray, then project in the DRAWING camera frame. The inverse
            // frame is retained for sampling the world-space course capture.
            r=sphereBasis*r;
            vec3 sphere=r+vec3(0,0,1);
            float q=.5*inversesqrt(max(dot(sphere,sphere),1e-20));
            outTexel=vec2(.5+r.x*q,.5-r.y*q);
            outNativeTint*=1.0+.75*min(dot(v,n),0.0);
        } else {
            float d=max(dot(L,r),0.0),p=d*d;p*=p;p*=p;
            if(info.y>=12.0)p*=p;
            outNativeTint=nativeMaterials.rows[m*6u+2u].rgb*S*p;
        }
    }
}
