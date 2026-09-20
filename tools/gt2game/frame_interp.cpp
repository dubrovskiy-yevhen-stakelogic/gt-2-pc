// High frame rates for the race (see frame_interp.h).
#include "frame_interp.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <cmath>

namespace gt2game {

namespace {

using V3 = std::array<double, 3>;

double Dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V3 Sub(const V3& a, const V3& b, double k) { return {a[0] - b[0] * k, a[1] - b[1] * k, a[2] - b[2] * k}; }
V3 Normalised(const V3& v) {
    const double l = std::sqrt(Dot(v, v));
    return l > 1e-12 ? V3{v[0] / l, v[1] / l, v[2] / l} : v;
}

// Columns c0, c1, c2 of a rotation lerped between two s16 (4096 = 1) matrices, then Gram-Schmidt in the order
// `first`, `second`, third (the remaining column keeps its sign relative to the others: the handedness of the input).
std::array<V3, 3> LerpRotation(const int16_t a[3][3], const int16_t b[3][3], double t, int first, int second) {
    std::array<V3, 3> c{};
    for (int col = 0; col < 3; col++)
        for (int row = 0; row < 3; row++) c[size_t(col)][size_t(row)] = (double(a[row][col]) + (double(b[row][col]) - double(a[row][col])) * t) / 4096.0;
    const int third = 3 - first - second;
    V3& f = c[size_t(first)];
    V3& s = c[size_t(second)];
    V3& r = c[size_t(third)];
    f = Normalised(f);
    s = Normalised(Sub(s, f, Dot(s, f)));
    r = Normalised(Sub(Sub(r, f, Dot(r, f)), s, Dot(r, s)));
    return c;
}

} // namespace

void InterpolatedModelMatrix(const gt2::sim::CarPose& a, const gt2::sim::CarPose& b, float t, float* m) {
    int16_t ra[3][3], rb[3][3];
    for (size_t row = 0; row < 3; row++)
        for (size_t col = 0; col < 3; col++) {
            ra[row][col] = a.rotation[row][col];
            rb[row][col] = b.rotation[row][col];
        }
    const std::array<V3, 3> c = LerpRotation(ra, rb, t, 2, 1); // forward axis first, then up
    for (int col = 0; col < 3; col++)
        for (int row = 0; row < 3; row++) m[col * 4 + row] = float(c[size_t(col)][size_t(row)]);
    m[3] = m[7] = m[11] = 0;
    for (size_t i = 0; i < 3; i++) m[12 + i] = float((double(a.worldPosition[i]) + (double(b.worldPosition[i]) - double(a.worldPosition[i])) * t) / 65536.0);
    m[15] = 1;
}

bool PoseJump(const gt2::sim::CarPose& a, const gt2::sim::CarPose& b) {
    double d2 = 0;
    for (size_t i = 0; i < 3; i++) {
        const double d = (double(b.worldPosition[i]) - double(a.worldPosition[i])) / 65536.0;
        d2 += d * d;
    }
    return d2 > 20.0 * 20.0;
}

gt2::sim::CarPose InterpolatedPoseRounded(const gt2::sim::CarPose& a, const gt2::sim::CarPose& b, float t) {
    gt2::sim::CarPose p = b;
    for (size_t i = 0; i < 3; i++) p.worldPosition[i] = LerpInt(a.worldPosition[i], b.worldPosition[i], t);
    return p;
}

bool CameraCut(const gt2::camera::RaceCamera& a, const gt2::camera::RaceCamera& b) {
    if (a.position != b.position || a.lookBack != b.lookBack || a.target != b.target || a.replayMode != b.replayMode || a.onboardView != b.onboardView ||
        a.replayFlags != b.replayFlags || a.hideTarget != b.hideTarget)
        return true;
    double d2 = 0;
    for (size_t i = 0; i < 3; i++) {
        const double d = (double(b.view.t[i]) - double(a.view.t[i])) / 65536.0;
        d2 += d * d;
    }
    if (d2 > 25.0 * 25.0) return true;
    double dot = 0; // the back axes (column 2)
    for (size_t i = 0; i < 3; i++) dot += double(a.view.m[i][2]) * double(b.view.m[i][2]) / (4096.0 * 4096.0);
    return dot < 0.5;
}

gt2::camera::CameraProjection InterpolatedProjection(const gt2::camera::RaceCamera& a, const gt2::camera::RaceCamera& b, float t) {
    // As 0x8007B374 / ProjectionOf: P = [[sx, 0, cx], [0, -sy, -cy], [0, 0, -4096]] of b's window, V = the view axes.
    gt2::camera::CameraProjection p = gt2::camera::ProjectionOf(b);
    const std::array<V3, 3> c = LerpRotation(a.view.m, b.view.m, t, 2, 1); // columns right, up, back: back first, then up
    const int32_t w = b.rectW, h = b.rectH;
    const double sx = b.spanX ? double(int32_t(w << 12) / b.spanX) : 4096.0, sy = b.spanY ? double(int32_t(h << 12) / b.spanY) : 4096.0;
    const double cx = double((int32_t(b.centreX) * int32_t(sx)) >> 12), cy = double((int32_t(b.centreY) * int32_t(sy)) >> 12);
    const double P[3][3] = {{sx, 0, cx}, {0, -sy, -cy}, {0, 0, -4096.0}};
    double eye[3];
    for (size_t i = 0; i < 3; i++) eye[i] = (double(a.view.t[i]) + (double(b.view.t[i]) - double(a.view.t[i])) * double(t)) / 65536.0;
    for (int r = 0; r < 3; r++) {
        double tr = 0;
        for (int j = 0; j < 3; j++) { // (P V^T)[r][j] = sum_k P[r][k] V[j][k], V[j][k] = column k, component j
            double s = 0;
            for (int k = 0; k < 3; k++) s += P[r][k] * c[size_t(k)][size_t(j)];
            p.rows[r][j] = float(s / 4096.0);
            tr -= s / 4096.0 * eye[j];
        }
        p.rows[r][3] = float(tr);
    }
    p.H = float(double(a.H) + (double(b.H) - double(a.H)) * double(t));
    for (size_t i = 0; i < 3; i++) {
        p.eye[i] = float(eye[i]);
        p.right[i] = float(c[0][i]);
        p.up[i] = float(c[1][i]);
        p.forward[i] = float(-c[2][i]);
    }
    return p;
}

void ClipMatrixOf(const gt2::camera::CameraProjection& p, float aspect, float zNear, float out[16]) {
    const float kx = p.H / (p.halfHeight * aspect), ky = p.H / p.halfHeight;
    for (int col = 0; col < 4; col++) {
        out[col * 4 + 0] = kx * p.rows[0][col];
        out[col * 4 + 1] = ky * p.rows[1][col];
        out[col * 4 + 2] = col == 3 ? zNear : 0.0f;
        out[col * 4 + 3] = p.rows[2][col];
    }
}

gt2view::SmokePool InterpolatedSmoke(const gt2view::SmokePool& a, const gt2view::SmokePool& b, float t) {
    gt2view::SmokePool out = b;
    std::bitset<gt2view::SmokePool::kCapacity> activeA;
    for (int k = a.activeHead, guard = 0; k >= 0 && k < gt2view::SmokePool::kCapacity && guard < gt2view::SmokePool::kCapacity; k = a.records[size_t(k)].next, guard++)
        activeA.set(size_t(k));
    std::bitset<gt2view::SmokePool::kCapacity> activeB, same;
    for (int k = b.activeHead, guard = 0; k >= 0 && k < gt2view::SmokePool::kCapacity && guard < gt2view::SmokePool::kCapacity; k = b.records[size_t(k)].next, guard++) {
        activeB.set(size_t(k));
        const gt2view::SmokeParticle& pa = a.records[size_t(k)];
        const gt2view::SmokeParticle& pb = b.records[size_t(k)];
        // A sprite spawned by the last step is not in `a` (and has size 0: not drawn yet); one spawned by the step before
        // is in both and grows from size 0.
        if (!activeA.test(size_t(k)) || pa.kind != pb.kind || pa.angle != pb.angle) continue;
        same.set(size_t(k));
        gt2view::SmokeParticle& o = out.records[size_t(k)];
        for (size_t i = 0; i < 3; i++) o.position[i] = LerpInt(pa.position[i], pb.position[i], t);
        o.size = int16_t(LerpInt(pa.size, pb.size, t));
        o.intensity = int16_t(LerpInt(pa.intensity, pb.intensity, t));
    }
    // The sprites that faded out in the last step (0x80016978 unlinked them): drawn fading from `a` towards the negative
    // intensity that removed them, rising and growing like the update does. A slot the step reused holds a new sprite of
    // size 0 (not drawn yet), so the fading one may take its place for drawing.
    for (int k = a.activeHead, guard = 0; k >= 0 && k < gt2view::SmokePool::kCapacity && guard < gt2view::SmokePool::kCapacity; k = a.records[size_t(k)].next, guard++) {
        if (same.test(size_t(k))) continue;
        const gt2view::SmokeParticle& pa = a.records[size_t(k)];
        const int32_t intensity = LerpInt(pa.intensity, int32_t(pa.intensity) - int32_t(pa.fade), t);
        if (intensity < 0 || pa.size == 0) continue;
        gt2view::SmokeParticle o = pa;
        o.intensity = int16_t(intensity);
        o.size = int16_t(LerpInt(pa.size, int32_t(pa.size) + b.growth, t));
        o.position[1] = LerpInt(pa.position[1], int32_t(uint32_t(pa.position[1]) + uint32_t(b.rise)), t);
        if (activeB.test(size_t(k))) {
            o.next = out.records[size_t(k)].next; // keep the list
        } else {
            o.next = out.activeHead;
            out.activeHead = int16_t(k);
        }
        out.records[size_t(k)] = o;
    }
    return out;
}

FrameLog::FrameLog(const std::string& csvPath, double interval) : interval_(interval) {
    if (!csvPath.empty()) {
        csv_ = std::fopen(csvPath.c_str(), "w");
        if (csv_) std::fprintf(csv_, "time_ms,frame_ms,extra,alpha,sim_steps,build_ms,present_ms\n");
        else std::printf("frame log: cannot write %s\n", csvPath.c_str());
    }
    Reset();
}

FrameLog::~FrameLog() {
    if (csv_) std::fclose(csv_);
}

void FrameLog::Reset() {
    start_ = windowStart_ = Clock::now();
    haveLast_ = false;
    frames_ = extras_ = timed_ = 0;
    maxFrame_ = sumFrame_ = 0;
    sumBuild_ = sumPresent_ = 0; workSamples_ = 0; sumGpu_ = maxGpu_ = 0; gpuSamples_ = 0;
    stepsAtWindow_ = -1;
}

void FrameLog::Presented(bool extra, double alpha, int simSteps, double buildMs, double presentMs, double gpuMs) {
    const Clock::time_point now = Clock::now();
    if (stepsAtWindow_ < 0) stepsAtWindow_ = simSteps;
    const double frameMs = haveLast_ ? std::chrono::duration<double, std::milli>(now - last_).count() : 0.0;
    if (haveLast_) {
        sumFrame_ += frameMs;
        maxFrame_ = std::max(maxFrame_, frameMs);
        timed_++;
    }
    last_ = now;
    haveLast_ = true;
    frames_++;
    if (extra) extras_++;
    if (gpuMs >= 0) { sumGpu_ += gpuMs; maxGpu_ = std::max(maxGpu_, gpuMs); ++gpuSamples_; }
    if (buildMs >= 0 && presentMs >= 0) { sumBuild_ += buildMs; sumPresent_ += presentMs; ++workSamples_; }
    if (csv_)
        std::fprintf(csv_, "%.3f,%.3f,%d,%.3f,%d,%.3f,%.3f\n", std::chrono::duration<double, std::milli>(now - start_).count(), frameMs, extra ? 1 : 0, alpha, simSteps,
                     buildMs, presentMs);
    const double elapsed = std::chrono::duration<double>(now - windowStart_).count();
    if (elapsed >= interval_) {
        const int intervals = timed_ > 0 ? timed_ : 1;
        std::printf("render: %.1f fps (%d frames, %d in-between) over %.1f s, frame time avg %.2f ms max %.2f ms; simulation %.1f steps/s\n", frames_ / elapsed,
                    frames_, extras_, elapsed, sumFrame_ / intervals, maxFrame_, (simSteps - stepsAtWindow_) / elapsed);
        if (workSamples_) std::printf("render work: CPU build %.3f ms; renderer + submission %.3f ms (includes GPU wait)\n", sumBuild_ / workSamples_, sumPresent_ / workSamples_);
        if (gpuSamples_) std::printf("render GPU: average %.3f ms; max %.3f ms\n", sumGpu_ / gpuSamples_, maxGpu_);
        sumBuild_ = sumPresent_ = 0; workSamples_ = 0; sumGpu_ = maxGpu_ = 0; gpuSamples_ = 0;
        windowStart_ = now;
        frames_ = extras_ = timed_ = 0;
        sumFrame_ = maxFrame_ = 0;
        haveLast_ = true;
        stepsAtWindow_ = simSteps;
    }
}

} // namespace gt2game
