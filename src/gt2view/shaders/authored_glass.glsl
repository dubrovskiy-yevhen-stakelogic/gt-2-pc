// Reflection + alpha blend identifies the exterior authored glazing attributes.
// Source alpha still clips the original glass silhouette. Cabin uses its old range.
bool authoredGlazing() { return (inFlags & (268435456u|32u))==(268435456u|32u); }
#ifndef GT2_WEBGL
vec3 glassCourseEnvironment(vec3 direction) {
    if(nativeViews.enabled.x<.5)return textureLod(paintCourse,direction,0).rgb;
    float best=-1e30;vec2 uv=vec2(0);int face=-1;
    for(int i=0;i<3;++i) {
        vec4 p=nativeViews.camera[i]*vec4(direction,0);
        if(p.w>best){best=p.w;uv=.5+.5*p.xy/max(p.w,1e-12);face=i;}
    }
    vec4 local=vec4(0);
    if(best>0.0 && all(greaterThanEqual(uv,vec2(0))) && all(lessThanEqual(uv,vec2(1))))
        local=textureLod(nativeEnvironment,vec3(uv,float(face)),0);
    // Unlike additive paint clearcoat, alpha-blended glass needs the sky's
    // actual brightness. Coverage prevents sky leaking through buildings.
    vec3 sky=nativeViews.stockSky.rgb*nativeViews.stockSky.a*mix(.18,1.0,smoothstep(-.1,.25,direction.y));
    return local.rgb+sky*(1.0-local.a);
}
#endif
vec4 authoredGlass(vec4 base) {
    vec3 n=normalize(inColor),v=normalize(inPaintView);
    float grazing=pow(1.0-clamp(abs(dot(n,v)),0.0,1.0),5.0);
    vec3 r=reflect(v,n);
#ifdef GT2_WEBGL
    vec3 environment=mix(vec3(.10,.11,.12),vec3(.43,.49,.56),smoothstep(-.15,.45,r.y));
#else
    vec3 environment=pc.space==0u ? glassCourseEnvironment(r) : textureLod(paintStudio,r,.85).rgb;
#endif
    vec3 tint=vec3(.12,.17,.18);
    // Bounded opacity: the interior remains visible even at grazing angles.
    float alpha=clamp(base.a*(2.2+2.1*grazing),0.0,.49);
    return vec4(mix(tint,environment,.58+.25*grazing),alpha);
}
