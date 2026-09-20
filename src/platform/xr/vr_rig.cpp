// The VR rig of the race (vr_rig.h).
#include "platform/xr/vr_rig.h"

#include <algorithm>
#include <cmath>

namespace gt2::vr {
namespace {

// A quaternion (x, y, z, w) as a column-major 3x3 whose columns are the rotated right / up / back axes.
void QuatToMatrix(const float q[4], float m[9]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float n = std::sqrt(x * x + y * y + z * z + w * w);
    const float s = n > 0 ? 2.0f / (n * n) : 0.0f;
    m[0] = 1 - s * (y * y + z * z);
    m[1] = s * (x * y + z * w);
    m[2] = s * (x * z - y * w);
    m[3] = s * (x * y - z * w);
    m[4] = 1 - s * (x * x + z * z);
    m[5] = s * (y * z + x * w);
    m[6] = s * (x * z + y * w);
    m[7] = s * (y * z - x * w);
    m[8] = 1 - s * (x * x + y * y);
}

void MatrixToQuat(const float m[9], float q[4]) {
    const float trace = m[0] + m[4] + m[8];
    if (trace > 0) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q[3] = 0.25f * s;
        q[0] = (m[5] - m[7]) / s;
        q[1] = (m[6] - m[2]) / s;
        q[2] = (m[1] - m[3]) / s;
    } else if (m[0] > m[4] && m[0] > m[8]) {
        const float s = std::sqrt(1.0f + m[0] - m[4] - m[8]) * 2.0f;
        q[3] = (m[5] - m[7]) / s;
        q[0] = 0.25f * s;
        q[1] = (m[3] + m[1]) / s;
        q[2] = (m[6] + m[2]) / s;
    } else if (m[4] > m[8]) {
        const float s = std::sqrt(1.0f + m[4] - m[0] - m[8]) * 2.0f;
        q[3] = (m[6] - m[2]) / s;
        q[0] = (m[3] + m[1]) / s;
        q[1] = 0.25f * s;
        q[2] = (m[7] + m[5]) / s;
    } else {
        const float s = std::sqrt(1.0f + m[8] - m[0] - m[4]) * 2.0f;
        q[3] = (m[1] - m[3]) / s;
        q[0] = (m[6] + m[2]) / s;
        q[1] = (m[7] + m[5]) / s;
        q[2] = 0.25f * s;
    }
}

// c = a . b (column-major 3x3).
void Mul3(const float a[9], const float b[9], float c[9]) {
    for (int col = 0; col < 3; col++)
        for (int row = 0; row < 3; row++) {
            float s = 0;
            for (int k = 0; k < 3; k++) s += a[k * 3 + row] * b[col * 3 + k];
            c[col * 3 + row] = s;
        }
}

void Apply3(const float m[9], const float v[3], float out[3]) {
    for (int row = 0; row < 3; row++) out[row] = m[0 * 3 + row] * v[0] + m[1 * 3 + row] * v[1] + m[2 * 3 + row] * v[2];
}

float Dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void Normalise(float q[4]) {
    const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (n > 0)
        for (int i = 0; i < 4; i++) q[i] /= n;
}

// Shortest-arc interpolation between two unit quaternions.
void Slerp(const float a[4], const float b0[4], float t, float out[4]) {
    float b[4] = {b0[0], b0[1], b0[2], b0[3]};
    float cosine = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (cosine < 0) {
        cosine = -cosine;
        for (int i = 0; i < 4; i++) b[i] = -b[i];
    }
    if (cosine > 0.9995f) { // almost equal: linear, then normalised
        for (int i = 0; i < 4; i++) out[i] = a[i] + (b[i] - a[i]) * t;
        Normalise(out);
        return;
    }
    const float angle = std::acos(std::clamp(cosine, -1.0f, 1.0f));
    const float s = std::sin(angle);
    const float wa = std::sin((1 - t) * angle) / s, wb = std::sin(t * angle) / s;
    for (int i = 0; i < 4; i++) out[i] = a[i] * wa + b[i] * wb;
    Normalise(out);
}

// c = a . b, column-major 4x4.
void Mul4(const float a[16], const float b[16], float c[16]) {
    for (int col = 0; col < 4; col++)
        for (int row = 0; row < 4; row++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a[k * 4 + row] * b[col * 4 + k];
            c[col * 4 + row] = s;
        }
}

// The view matrix of an eye whose world rotation is `r` (columns right, up, back) and whose position, relative to
// the reference origin the draw list uses, is `offset`: world-minus-refEye -> eye space (x right, y up, z back).
void ViewMatrix(const float r[9], const float offset[3], float out[16]) {
    for (int col = 0; col < 3; col++)
        for (int row = 0; row < 3; row++) out[col * 4 + row] = r[row * 3 + col]; // transpose
    // translation = -r^T . offset (the eye-space coordinates of the reference origin)
    const float d[3] = {-offset[0], -offset[1], -offset[2]};
    for (int row = 0; row < 3; row++) out[12 + row] = r[row * 3 + 0] * d[0] + r[row * 3 + 1] * d[1] + r[row * 3 + 2] * d[2];
    out[3] = out[7] = out[11] = 0;
    out[15] = 1;
}

} // namespace

void HudProjection(const Pose& head, const EyeView eyes[2], float out[2][16]) {
    float rotation[9]; QuatToMatrix(head.orientation, rotation);
    float plane[16] = {};
    for (int row = 0; row < 3; ++row) {
        plane[row] = rotation[row] * 1.28f;
        plane[4 + row] = rotation[3 + row] * -0.96f;
        plane[12 + row] = head.position[row] - rotation[6 + row] * 2.0f;
    }
    plane[15] = 1;
    for (int eye = 0; eye < 2; ++eye) {
        float er[9], view[16], projection[16], pv[16];
        QuatToMatrix(eyes[eye].pose.orientation, er);
        ViewMatrix(er, eyes[eye].pose.position, view);
        Projection(eyes[eye].fov, 0.05f, projection);
        Mul4(projection, view, pv); Mul4(pv, plane, out[eye]);
    }
}

void Projection(const Fov& fov, float nearZ, float out[16]) {
    const double tl = std::tan(double(fov.left)), tr = std::tan(double(fov.right));
    const double tu = std::tan(double(fov.up)), td = std::tan(double(fov.down));
    const double w = tr - tl, h = tu - td;
    for (int i = 0; i < 16; i++) out[i] = 0;
    out[0] = float(2.0 / w);
    out[8] = float((tr + tl) / w);
    out[5] = float(-2.0 / h);          // Vulkan clip space: y down
    out[9] = float(-(tu + td) / h);
    out[14] = nearZ;                   // reversed Z, infinite far: z_ndc = nearZ / depth
    out[11] = -1.0f;                   // clip w = -z = the distance ahead
}

namespace {

// r = a u + c v, solved exactly for a and c (the two axes need not be orthonormal - the original camera's are s16
// quantised to 4096 = 1, so assuming they were costs about 1e-4 of the field of view, a visible tenth of a pixel).
bool Solve2(const float u[3], const float v[3], const float r[3], float& a, float& c) {
    const float g11 = Dot3(u, u), g12 = Dot3(u, v), g22 = Dot3(v, v);
    const float det = g11 * g22 - g12 * g12;
    if (std::abs(det) < 1e-12f) return false;
    const float b1 = Dot3(r, u), b2 = Dot3(r, v);
    a = (b1 * g22 - b2 * g12) / det;
    c = (g11 * b2 - g12 * b1) / det;
    return true;
}

} // namespace

Fov OriginalFov(const Camera& camera) {
    // The clip matrix is [kx rows0; ky rows1; (0, 0, 0, nearZ); rows2] over world coordinates, and its first two rows
    // are combinations of the camera's own axes (docs/formats/camera.md section 3; `back` = -forward, the third
    // column of the camera matrix):
    //   rows0 = A right + B back,  rows1 = C up + D back,  rows2 = -back
    // In the eye space the rig renders in - x = right . d, y = up . d, z = back . d - that is exactly the projection
    // of the frustum tan(right) = (1 + B) / A, tan(left) = (B - 1) / A, tan(up) = S (1 - D) / 2,
    // tan(down) = -S (1 + D) / 2 with S = -2 / C.
    const float r0[3] = {camera.clip[0], camera.clip[4], camera.clip[8]};
    const float r1[3] = {camera.clip[1], camera.clip[5], camera.clip[9]};
    const float back[3] = {-camera.forward[0], -camera.forward[1], -camera.forward[2]};
    float A = 0, B = 0, C = 0, D = 0;
    Fov fov;
    if (Solve2(camera.right, back, r0, A, B) && A > 1e-6f) {
        fov.right = std::atan((1.0f + B) / A);
        fov.left = std::atan((B - 1.0f) / A);
    }
    if (Solve2(camera.up, back, r1, C, D) && C < -1e-6f) {
        const float S = -2.0f / C;
        fov.up = std::atan(S * (1.0f - D) * 0.5f);
        fov.down = std::atan(-S * (1.0f + D) * 0.5f);
    }
    return fov;
}

void Recenter::Latch(const Pose& head) {
    offset_[0] = head.position[0];
    offset_[1] = head.position[1];
    offset_[2] = head.position[2];
    float m[9];
    QuatToMatrix(head.orientation, m);
    // the head's forward axis (-back = -column 2) projected on the horizontal plane
    const float fx = -m[6], fz = -m[8];
    yaw_ = (std::abs(fx) > 1e-8f || std::abs(fz) > 1e-8f) ? std::atan2(-fx, -fz) : 0.0f;
    pending_ = false;
}

Pose Recenter::Apply(const Pose& local) const {
    const float c = std::cos(-yaw_), s = std::sin(-yaw_);
    const float d[3] = {local.position[0] - offset_[0], local.position[1] - offset_[1], local.position[2] - offset_[2]};
    Pose out;
    out.position[0] = c * d[0] + s * d[2];
    out.position[1] = d[1];
    out.position[2] = -s * d[0] + c * d[2];
    const float q[4] = {0, std::sin(-yaw_ * 0.5f), 0, std::cos(-yaw_ * 0.5f)}; // Ry(-yaw)
    const float* p = local.orientation;
    out.orientation[0] = q[3] * p[0] + q[0] * p[3] + q[1] * p[2] - q[2] * p[1];
    out.orientation[1] = q[3] * p[1] - q[0] * p[2] + q[1] * p[3] + q[2] * p[0];
    out.orientation[2] = q[3] * p[2] + q[0] * p[1] - q[1] * p[0] + q[2] * p[3];
    out.orientation[3] = q[3] * p[3] - q[0] * p[0] - q[1] * p[1] - q[2] * p[2];
    Normalise(out.orientation);
    return out;
}

View Build(const Camera& camera, const EyeView eyes[2], const Settings& settings) {
    View out;
    // --- C: the original camera's world frame (columns right, up, back) ---
    float C[9];
    for (int i = 0; i < 3; i++) {
        C[0 * 3 + i] = camera.right[i];
        C[1 * 3 + i] = camera.up[i];
        C[2 * 3 + i] = -camera.forward[i];
    }
    // --- H: the horizon lock. The levelled frame keeps the camera's yaw and drops pitch and roll; the lock is how
    // far the camera is turned towards it (0 = the original camera, 1 = level). Slerped, so the result is a rotation.
    float levelled[9];
    const float lock = std::clamp(settings.horizonLock, 0.0f, 1.0f);
    if (lock <= 0.0f) {
        for (int i = 0; i < 9; i++) levelled[i] = C[i];
    } else {
        const float fx = camera.forward[0], fz = camera.forward[2];
        float yaw = 0;
        if (std::abs(fx) > 1e-8f || std::abs(fz) > 1e-8f) yaw = std::atan2(-fx, -fz);
        else { // looking straight up or down: take the yaw from the up axis instead
            yaw = std::atan2(-camera.up[0], -camera.up[2]);
        }
        const float c = std::cos(yaw), s = std::sin(yaw);
        const float level[9] = {c, 0, -s, 0, 1, 0, s, 0, c}; // Ry(yaw): columns right, up, back
        float qc[4], ql[4], q[4];
        MatrixToQuat(C, qc);
        MatrixToQuat(level, ql);
        Slerp(qc, ql, lock, q);
        QuatToMatrix(q, levelled);
    }
    for (int i = 0; i < 9; i++) out.levelled[i] = levelled[i];

    // --- S: the seat offset in the levelled camera's axes, and the origin the head sits on ---
    float seatWorld[3];
    Apply3(levelled, settings.seat, seatWorld);
    const float origin[3] = {camera.eye[0] + seatWorld[0], camera.eye[1] + seatWorld[1], camera.eye[2] + seatWorld[2]};

    // --- the head: the runtime's eye poses, the eye separation optionally forced (the checks) ---
    Pose head[2] = {eyes[0].pose, eyes[1].pose};
    float mid[3] = {0, 0, 0};
    for (int i = 0; i < 3; i++) mid[i] = 0.5f * (head[0].position[i] + head[1].position[i]);
    float separation = 0;
    {
        const float d[3] = {head[1].position[0] - head[0].position[0], head[1].position[1] - head[0].position[1],
                            head[1].position[2] - head[0].position[2]};
        separation = std::sqrt(Dot3(d, d));
    }
    if (settings.ipd >= 0.0f && separation > 1e-6f) { // scale the eyes about the mid point to the forced separation
        const float k = settings.ipd / separation;
        for (int v = 0; v < 2; v++)
            for (int i = 0; i < 3; i++) head[v].position[i] = mid[i] + (head[v].position[i] - mid[i]) * k;
        separation = settings.ipd;
    } else if (settings.ipd >= 0.0f) {
        separation = 0;
    }
    out.ipd = separation;

    // --- the mid eye: the reference origin of the draw list and the camera of the CPU work ---
    float midWorld[3];
    {
        const float scaled[3] = {mid[0] * settings.worldScale, mid[1] * settings.worldScale, mid[2] * settings.worldScale};
        Apply3(levelled, scaled, midWorld);
    }
    for (int i = 0; i < 3; i++) out.refEye[i] = origin[i] + midWorld[i];
    float midRot[9], headMid[9];
    QuatToMatrix(head[0].orientation, headMid);
    Mul3(levelled, headMid, midRot);
    for (int i = 0; i < 3; i++) {
        out.right[i] = midRot[0 * 3 + i];
        out.up[i] = midRot[1 * 3 + i];
        out.forward[i] = -midRot[2 * 3 + i];
    }

    // --- per eye ---
    const Fov original = settings.originalFov ? OriginalFov(camera) : Fov{};
    for (int v = 0; v < 2; v++) {
        float rot[9], headRot[9];
        QuatToMatrix(head[v].orientation, headRot);
        Mul3(levelled, headRot, rot);
        float eyeWorld[3];
        {
            const float scaled[3] = {head[v].position[0] * settings.worldScale, head[v].position[1] * settings.worldScale,
                                     head[v].position[2] * settings.worldScale};
            float moved[3];
            Apply3(levelled, scaled, moved);
            for (int i = 0; i < 3; i++) eyeWorld[i] = origin[i] + moved[i];
        }
        for (int i = 0; i < 3; i++) out.eyeWorld[v][i] = eyeWorld[i];
        MatrixToQuat(rot, out.eyeQuat[v]);
        out.fov[v] = settings.originalFov ? original : eyes[v].fov;

        float projection[16];
        Projection(out.fov[v], settings.nearZ, projection);
        const float offset[3] = {eyeWorld[0] - out.refEye[0], eyeWorld[1] - out.refEye[1], eyeWorld[2] - out.refEye[2]};
        float view[16];
        ViewMatrix(rot, offset, view);
        Mul4(projection, view, out.worldVP[v]);
        float sky[16];
        const float none[3] = {0, 0, 0};
        ViewMatrix(rot, none, sky);
        sky[12] = sky[13] = sky[14] = 0; // the backdrop sits at infinity: no eye translation, no parallax
        Mul4(projection, sky, out.skyVP[v]);
    }
    out.valid = true;
    return out;
}

void LowerIntroCamera(Camera& camera, float groundY, float lowering) {
    camera.eye[1] -= std::min(std::max(lowering,0.f),std::max(0.f,camera.eye[1]-groundY-1.2f));
}

} // namespace gt2::vr
