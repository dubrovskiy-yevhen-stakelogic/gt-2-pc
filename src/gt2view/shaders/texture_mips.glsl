// Constrain both trilinear footprints to complete blocks inside this atlas region.
// A narrow or unaligned sprite falls back to a finer level instead of reading its neighbour.
bool sampleMip(vec2 uv, vec3 color, out vec4 result) {
    if ((pc.options & 8u) == 0u) return false;
    vec2 dx = dFdx(uv), dy = dFdy(uv);
    float xx = dot(dx, dx), yy = dot(dy, dy), xy = dot(dx, dy);
    float majorSquared = 0.5 * (xx + yy + sqrt(max((xx - yy) * (xx - yy) + 4.0 * xy * xy, 0.0)));
    float major = sqrt(max(majorSquared, 0.0));
    float minor = abs(dx.x * dy.y - dx.y * dy.x) / max(major, 0.0001);
    // Long, thin signs need their horizontal detail even when their height is
    // minified. Four taps along the major axis retain it without disabling mips.
    bool anisotropic = major > 2.0 * max(minor, 1.0);
    vec2 direction = abs(xy) > 0.0001 ? normalize(vec2(xy, majorSquared - xx)) : (xx > yy ? vec2(1,0) : vec2(0,1));
    vec2 span = dx * direction.x + dy * direction.y;
    float footprint = anisotropic ? max(minor, major * 0.25) : major;
    float lod = clamp(log2(max(footprint, 1.0)), 0.0, 8.0);
    vec2 lo = clamp(floor(inRect.xy), vec2(0), vec2(256));
    vec2 hi = clamp(ceil(inRect.zw), lo, vec2(256));
    float level = ceil(lod);
    vec2 first = lo, last = hi;
    for (int n = 0; n < 9; ++n) {
        float block = exp2(level);
        first = ceil(lo / block) * block + block * 0.5;
        last = floor(hi / block) * block - block * 0.5;
        if (all(greaterThanEqual(last, first))) break;
        level -= 1.0;
    }
    lod = min(lod, level);
    if (lod <= 0.0) return false;
    float layer = float((inCache & 65535u) - 1u);
    bool mixed = (inCache & 131072u) != 0u;
    bool stp = (inCache & 65536u) != 0u;
    if (!mixed && ((pc.stpPass == 1u && stp) || (pc.stpPass == 2u && !stp))) discard;
    if (mixed && pc.stpPass == 2u) layer += 1.0;
    vec4 c = vec4(0);
    int taps = anisotropic ? 4 : 1;
    for (int tap = 0; tap < taps; ++tap) {
        vec2 coord = clamp(uv + span * ((float(tap) + 0.5) / float(taps) - 0.5), first, last) / 256.0;
        vec4 value = textureLod(decodedPages, vec3(coord, layer), lod);
        if (mixed && pc.stpPass == 0u) value += textureLod(decodedPages, vec3(coord, layer + 1.0), lod);
        c += value / float(taps);
    }
    if (c.a <= 0.0) discard;
    vec3 rgb = c.rgb / c.a;
    if ((inFlags & 2u) == 0u) rgb = min(rgb * color * (255.0 / 128.0), vec3(1));
    float coverage = clamp(c.a, 0.0, 1.0);
    if ((pc.options & 4u) == 0u) {
        if (coverage < 0.5) discard;
        coverage = 1.0;
    }
    result = vec4(rgb, coverage);
    return true;
}
