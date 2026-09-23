#pragma once
#include "gt2view/cockpit_body.h"

namespace gt2view {
namespace cockpit_detail {

inline bool ClearForwardRay(std::span<const gt2::CarMeshVertex> body, const gt2::CarTexture& texture,
                            const std::array<uint16_t, 16>& masks, const std::array<float, 3>& eye, float verticalSlope = 0) {
    for (size_t at = 0; at + 2 < body.size(); at += 3) {
        const auto* tri = body.data() + at;
        const float* a = tri[0].pos; const float* b = tri[1].pos; const float* c = tri[2].pos;
        const float ay = a[1] + verticalSlope * a[2], eyeY = eye[1] + verticalSlope * eye[2];
        const float bx = b[0] - a[0], by = b[1] + verticalSlope * b[2] - ay;
        const float cx = c[0] - a[0], cy = c[1] + verticalSlope * c[2] - ay;
        const float determinant = bx * cy - by * cx;
        if (std::abs(determinant) < 1e-8f) continue;
        const float u = ((eye[0] - a[0]) * cy - (eyeY - ay) * cx) / determinant;
        const float v = (bx * (eyeY - ay) - by * (eye[0] - a[0])) / determinant;
        if (u < 0 || v < 0 || u + v > 1) continue;
        const float weights[] = {1 - u - v, u, v};
        float z = 0, tu = 0, tv = 0;
        for (size_t k = 0; k < 3; ++k) {
            z += tri[k].pos[2] * weights[k];
            tu += tri[k].texel[0] * weights[k]; tv += tri[k].texel[1] * weights[k];
        }
        if (z >= eye[2] - 0.005f) continue;
        if (!tri[0].textured) return false;
        const auto x = std::clamp(int(std::floor(tu)), 0, 255), y = std::clamp(int(std::floor(tv)), 0, 223);
        const auto index = texture.indices[size_t(y) * 256 + x];
        const auto palette = tri[0].palette;
        if (palette >= kCockpitGlazingPalette && palette < kCockpitGlazingPalette + masks.size()) {
            if (masks[palette - kCockpitGlazingPalette] & (1u << index)) continue;
            if (texture.paints.front().cluts[palette - kCockpitGlazingPalette][index]) return false;
        } else if (CockpitSourcePalette(palette) < masks.size()) {
            if (texture.paints.front().cluts[CockpitSourcePalette(palette)][index]) return false;
        } else return false;
    }
    return true;
}

} // namespace cockpit_detail

// Fit the whole mirror behind the authored windscreen, including its opaque
// banner. Testing the frame corners avoids a centre-only fit intersecting it.
inline void FitCockpitMirror(CockpitFit& fit, const gt2::CarModel& model) {
    gt2::CarMeshOptions options; options.wheels = false;
    const auto mesh = gt2::BuildCarMesh(model, options);
    const float y = std::min(fit.eye[1] + .13f, fit.roofFrontY - .085f);
    const float t = std::clamp((y + .035f - fit.windshieldY) /
                             std::max(.1f, fit.roofFrontY - fit.windshieldY), 0.f, 1.f);
    float z = fit.windshieldZ + (fit.roofFrontZ - fit.windshieldZ) * t;
    for (float x : {-.125f, 0.f, .125f}) for (float yy : {y-.035f,y,y+.035f}) {
        for (size_t at=0; at+2<mesh.size(); at+=3) {
            const auto* a=mesh[at].pos; const auto* b=mesh[at+1].pos; const auto* c=mesh[at+2].pos;
            const float bx=b[0]-a[0],by=b[1]-a[1],cx=c[0]-a[0],cy=c[1]-a[1];
            const float det=bx*cy-by*cx;
            if (std::abs(det)<1e-8f) continue;
            const float u=((x-a[0])*cy-(yy-a[1])*cx)/det;
            const float v=(bx*(yy-a[1])-by*(x-a[0]))/det;
            if (u<0 || v<0 || u+v>1) continue;
            const float hit=a[2]+u*(b[2]-a[2])+v*(c[2]-a[2]);
            if (hit>=fit.windshieldZ-.05f && hit<=fit.roofFrontZ+.08f) z=std::max(z,hit);
        }
    }
    fit.mirror={0,y,z+.035f};
}

// Returns whether the fitted view clears the original body, including painted screen banners.
// A failed search leaves the model-based seat unchanged. The body and its glass mask are immutable.
inline bool RefineCockpitEye(CockpitFit& fit, std::span<const gt2::CarMeshVertex> body,
                             const gt2::CarTexture& texture, const std::array<uint16_t, 16>& masks) {
    if (!fit.valid || body.empty() || texture.paints.empty() || texture.indices.size() != 256 * 224) return false;
    const auto original = fit.eye;
    const float halfWidth = std::min(fit.windshieldHalfWidth, fit.halfWidth);
    const float minX = -std::max(0.12f, halfWidth * 0.65f), maxX = -std::min(0.08f, halfWidth * 0.20f);
    const float minY = std::max({fit.windshieldY + 0.06f, fit.floorY + 0.36f, original[1] - 0.30f});
    float bestScore = -std::numeric_limits<float>::infinity();
    std::array<float, 3> best = original;
    auto clear = [&](const std::array<float, 3>& eye) {
        for (float back : {0.0f, 0.18f})
            for (float up : {0.0f, -0.02f, 0.02f}) {
                auto ray = eye; ray[1] += up; ray[2] += back;
                if (!cockpit_detail::ClearForwardRay(body, texture, masks, ray)) return false;
            }
        return true;
    };
    auto cone = [&](const std::array<float, 3>& eye, float direction) {
        int steps = 0;
        for (int step = 1; step <= 6; ++step) {
            const float slope = direction * float(step) * 0.05f;
            bool visible = true;
            for (float back : {0.0f, 0.18f}) {
                auto ray = eye; ray[2] += back;
                if (!cockpit_detail::ClearForwardRay(body, texture, masks, ray, slope)) { visible = false; break; }
            }
            if (!visible) break;
            ++steps;
        }
        return steps;
    };
    for (float dx : {0.0f, 0.04f, -0.04f, 0.08f, -0.08f}) {
        const float x = std::clamp(original[0] + dx, minX, maxX);
        float maxY = fit.roofFrontY - 0.11f;
        if (!fit.openTop) {
            const float roof = cockpit_detail::CabinSurfaceHeight(body, original[2], x);
            const float flatRoof = cockpit_detail::CabinSurfaceHeight(body, original[2] + 0.18f, x);
            if (!std::isfinite(roof) || !std::isfinite(flatRoof)) continue;
            maxY = std::min(roof, flatRoof) - 0.11f;
        }
        maxY = std::min(maxY, original[1] + 0.10f);
        if (maxY < minY) continue;
        for (int step = 0; step <= 30; ++step)
            for (int direction : {-1, 1}) {
                if (step == 0 && direction == 1) continue;
                const float y = original[1] + float(step * direction) * 0.01f;
                if (y < minY || y > maxY) continue;
                const std::array<float, 3> eye{x, y, original[2]};
                if (!clear(eye)) continue;
                const int above = cone(eye, 1.0f), below = cone(eye, -1.0f);
                const float score = float(std::min(above, below)) + 0.15f * float(above + below) -
                    0.30f * std::abs(y - original[1]) - 0.80f * std::abs(x - original[0]);
                if (score > bestScore) { best = eye; bestScore = score; }
            }
    }
    if (!std::isfinite(bestScore)) return false;
    fit.eye = best;
    return true;
}

} // namespace gt2view
