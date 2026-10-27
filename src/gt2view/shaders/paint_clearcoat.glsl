// Shared by cached and uncached models; colour contains the authored smooth normal.
layout(set=0,binding=8) uniform samplerCube paintStudio;
layout(set=0,binding=9) uniform samplerCube paintCourse;
layout(set=0,binding=12) uniform sampler2DArray nativeEnvironment;
layout(set=0,binding=13) uniform NativeEnvironmentViews {mat4 camera[3];vec4 enabled;vec4 lightDirection;vec4 lightAmbient;vec4 lightDiffuse;vec4 lightSpecular;vec4 stockSky;} nativeViews;
vec3 nativeCourseEnvironment(vec3 direction) {
    if(nativeViews.enabled.x<.5)return textureLod(paintCourse,direction,0).rgb;
    float best=-1e30;vec2 uv=vec2(0);int face=-1;
    for(int i=0;i<3;++i) {
        vec4 p=nativeViews.camera[i]*vec4(direction,0);
        if(p.w>best){best=p.w;uv=.5+.5*p.xy/max(p.w,1e-12);face=i;}
    }
    if(best<=0.0 || any(lessThan(uv,vec2(0))) || any(greaterThan(uv,vec2(1))))return vec3(0);
    return textureLod(nativeEnvironment,vec3(uv,float(face)),0).rgb;
}
// GT4 captures nearby scenery on black. GT2's baked diffuse body needs the
// distant sky too. Coverage alpha distinguishes a real black roof/wall from
// empty background, so the sky cannot shine through covered scenery.
vec3 stockCourseEnvironment(vec3 direction) {
    if(nativeViews.enabled.x<.5)return textureLod(paintCourse,direction,0).rgb;
    float best=-1e30;vec2 uv=vec2(0);int face=-1;
    for(int i=0;i<3;++i){
        vec4 p=nativeViews.camera[i]*vec4(direction,0);
        if(p.w>best){best=p.w;uv=.5+.5*p.xy/max(p.w,1e-12);face=i;}
    }
    vec4 local=vec4(0);
    if(best>0.0 && all(greaterThanEqual(uv,vec2(0))) && all(lessThanEqual(uv,vec2(1))))
        local=textureLod(nativeEnvironment,vec3(uv,float(face)),0);
    float horizon=mix(.12,1.0,smoothstep(-.1,.25,direction.y));
    // Distant fill has no local detail. Bound its chroma and energy so a
    // blue sky cannot recolour dark green paint through the additive pass.
    vec3 skyColor=nativeViews.stockSky.rgb;
    float luminance=dot(skyColor,vec3(.2126,.7152,.0722));
    vec3 sky=mix(vec3(luminance),skyColor,.12)*.28*nativeViews.stockSky.a*horizon;
    return local.rgb+sky*(1.0-local.a);
}
// GT2's original body already contains baked diffuse shading. Its separate
// additive environment pass needs visible coverage even at normal incidence;
// the previous clearcoat multiplier fell to 3.1% there. Native GT4 is unchanged.
vec3 stockPaintClearcoat(vec3 normal,vec3 incident,bool course) {
    vec3 n=normalize(normal),v=normalize(incident),r=reflect(v,n);
    vec3 sampleColor=course?stockCourseEnvironment(r):textureLod(paintStudio,r,.85).rgb;
    vec3 radiance=course?sampleColor:sampleColor*sampleColor*3.0;
    float grazing=1.0-clamp(abs(dot(n,v)),0.0,1.0);
    return radiance*(.38+.22*grazing*grazing);
}
vec3 paintClearcoat(vec3 base,vec3 normal,vec3 incident,bool course) {
    vec3 n=normalize(normal),v=normalize(incident);
    vec3 r=reflect(v,n);
    // A small, filtered probe gives continuous highlights instead of polygon bands.
    vec3 sampleColor=course?nativeCourseEnvironment(r):textureLod(paintStudio,r,.85).rgb;
    vec3 radiance=course?sampleColor:sampleColor*sampleColor*3.0;
    return .125*radiance*(1.0+.75*min(dot(n,v),0.0));
}
