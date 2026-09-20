#version 450
layout(constant_id = 0) const bool cachedOnly = false;

// PS1-style texturing: the whole 1024-wide VRAM image lives in a buffer of 16-bit words and
// polygons address it by page + CLUT, exactly like the original GPU. Rows 0-511 mirror the
// console VRAM (course textures); rows 512+ are ours (car texture and its paint CLUTs).
layout(std430, set = 0, binding = 0) readonly buffer Vram { uint words[]; } vram;

// External RGBA8 images (mod meshes): flag 16; page = first texel, clut = width | height << 16.
layout(std430, set = 0, binding = 1) readonly buffer External {
    uint texels[];
} ext;

layout(push_constant) uniform Push {
    mat4 mvp;
    uint paint;
    uint brakeLit;
    uint stpPass; // DrawItem::stpPass
    uint options; // 1 smooth textures, 2 affine mapping (scene items only; 0 = the PS1's nearest texel, perspective-correct)
    uint space;
    uint eye;
    uvec2 reserved;
    vec4 hudClip;
} pc;

layout(location = 0) in vec2 inTexel;
layout(location = 1) in vec3 inColor;
layout(location = 2) flat in uint inPage;
layout(location = 3) flat in uint inClut;
layout(location = 4) flat in uint inFlags;
layout(location = 5) noperspective in vec2 inTexelAffine;
layout(location = 6) noperspective in vec3 inColorAffine;
layout(location = 7) flat in vec4 inRect; // UV rectangle of the triangle (texel units)

layout(location = 8) in vec3 inScreen;
layout(location = 0) out vec4 outColor;
layout(location = 9) flat in uint inCache;
layout(set = 0, binding = 3) uniform sampler2DArray decodedPages;
layout(set = 0, binding = 5) uniform sampler2D handAlbedo;

uint word(uint x, uint y) { return vram.words[y * 1024u + x]; }

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
        return c.a == 0u ? 0u : c.r | (c.g << 5) | (c.b << 10) | stp;
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
    t = ((t % size) + size) % size;
    uint px = ext.texels[inPage + uint(t.y) * uint(size.x) + uint(t.x)];
    return vec4(float(px & 255u), float((px >> 8) & 255u), float((px >> 16) & 255u), float(px >> 24)) / 255.0;
}

void main() {
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
        texelCoord = inTexel + clamp(inTexelAffine - inTexel, vec2(-4.0), vec2(4.0));
        color = inColorAffine;
    }
    if ((inFlags & 2048u) != 0u && !gl_FrontFacing) discard; // kCullBack: the original's NCLIP test
    if ((inFlags & 1u) == 0u) {
        if (pc.stpPass == 1u) discard; // untextured semi-transparent: blended pass only
        outColor = vec4(color, 1.0);
        return;
    }
    if (!cachedOnly && (inFlags & 16u) != 0u) { // external RGBA8 image, repeat wrapping, nearest texel, alpha mask at 0.5
        ivec2 size = ivec2(int(inClut & 0xFFFFu), int(inClut >> 16));
        vec4 c = externalTexel(ivec2(floor(texelCoord)), size);
        if (c.a < 0.5) discard;
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
                if (s.a < 0.5) continue;
                sum += w * s.rgb;
                wsum += w;
            }
            if (wsum > 0.0) c.rgb = sum / wsum;
        }
        outColor = vec4(c.rgb * color, 1.0);
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
    uvec2 t = uvec2(clamp(ivec2(floor(texelCoord)), ivec2(0), ivec2(255)));
    if ((inFlags & 32768u) != 0u) t = uvec2(clamp(ivec2(t), ivec2(floor(inRect.xy)), ivec2(max(ceil(inRect.zw) - 1, floor(inRect.xy)))));
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
            // Coverage-normalized hardware bilinear: RGB contains the original 5-bit
            // channels. Mixed pages select the layer of the nearest texel's STP class.
            vec2 uv = (clamp(p, vec2(lo), vec2(hi)) + 0.5) / 256.0;
            uint layer = (inCache & 65535u) - 1u;
            if ((inCache & 131072u) != 0u && (texel & 32768u) != 0u) ++layer;
            vec4 c = textureLod(decodedPages, vec3(uv, float(layer)), 0.0);
            if (c.a > 0.0) rgb = (c.rgb / c.a) * (255.0 / 31.0);
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
