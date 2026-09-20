// xrsim - automation script (XRSIM_SCRIPT): parser and per-frame evaluation. Format: docs/research/xrsim.md.
#include "rt.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

namespace xs {

namespace {
const char* const kCompNames[C_COUNT] = {
    "trigger", "trigger_touch", "squeeze", "thumbstick_x", "thumbstick_y", "thumbstick_click", "thumbstick_touch",
    "thumbrest_touch", "x", "x_touch", "y", "y_touch", "a", "a_touch", "b", "b_touch", "menu", "system"};

bool Num(const std::string& s, double& v) {
    char* end = nullptr;
    v = strtod(s.c_str(), &end);
    return end && end != s.c_str() && *end == 0 && std::isfinite(v);
}

std::vector<double> PoseVec(const Pose& p) { return {p.p.x, p.p.y, p.p.z, p.q.x, p.q.y, p.q.z, p.q.w}; }
Pose VecPose(const std::vector<double>& v) {
    Pose p;
    p.p = {v[0], v[1], v[2]};
    p.q = Normalize({v[3], v[4], v[5], v[6]});
    return p;
}
} // namespace

const char* CompScriptName(int c) { return (c >= 0 && c < C_COUNT) ? kCompNames[c] : "?"; }

bool Script::Load(const std::string& path, Config& cfg, std::string& err) {
    std::ifstream f(path);
    if (!f) {
        err = "cannot open " + path;
        return false;
    }
    std::string line;
    int no = 0;
    while (std::getline(f, line)) {
        ++no;
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        if (!ParseLine(line, no, cfg, err)) {
            err = "line " + std::to_string(no) + ": " + err;
            return false;
        }
    }
    loaded = true;
    Log("script %s: %zu channels, %zu one-shot commands", path.c_str(), channels_.size(), shots_.size());
    return true;
}

bool Script::ParseLine(const std::string& text, int lineNo, Config& cfg, std::string& err) {
    std::istringstream is(text);
    std::vector<std::string> tok;
    for (std::string t; is >> t;) {
        std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return (char)tolower(c); });
        tok.push_back(t);
    }
    if (tok.empty()) return true;

    bool timed = false, isTime = false;
    int64_t frame = 0;
    double t = 0;
    size_t i = 0;
    if (tok[0][0] == '@') {
        std::string k = tok[0].substr(1);
        timed = true;
        double v = 0;
        if (!k.empty() && k.back() == 's') {
            k.pop_back();
            if (!Num(k, v) || v < 0) { err = "bad time key " + tok[0]; return false; }
            isTime = true;
            t = v;
        } else {
            if (!Num(k, v) || v < 0 || v != std::floor(v)) { err = "bad frame key " + tok[0]; return false; }
            frame = (int64_t)v;
        }
        i = 1;
        if (tok.size() < 2) { err = "missing command after key"; return false; }
    }
    const std::string cmd = tok[i];
    std::vector<std::string> args(tok.begin() + (long long)i + 1, tok.end());
    auto nums = [&](size_t from, size_t n, std::vector<double>& out) {
        out.clear();
        for (size_t k = 0; k < n; ++k) {
            double v = 0;
            if (from + k >= args.size() || !Num(args[from + k], v)) return false;
            out.push_back(v);
        }
        return true;
    };
    auto addShot = [&](ScriptShot::Kind kind, const std::string& arg, double value) {
        ScriptShot s;
        s.kind = kind;
        s.isTime = isTime;
        s.frame = frame;
        s.t = t;
        s.arg = arg;
        s.value = value;
        s.line = lineNo;
        shots_.push_back(s);
    };
    auto addKey = [&](const std::string& ch, std::vector<double> v, bool lerp) -> bool {
        Channel& c = channels_[ch];
        if (!c.keys.empty()) {
            const ScriptKey& p = c.keys.back();
            if (p.isTime == isTime && (isTime ? t < p.t : frame < p.frame)) {
                err = "keys of channel '" + ch + "' must be in chronological order";
                return false;
            }
        }
        ScriptKey k;
        k.isTime = isTime;
        k.frame = frame;
        k.t = t;
        k.lerp = lerp;
        k.v = std::move(v);
        k.line = lineNo;
        c.keys.push_back(std::move(k));
        return true;
    };

    std::vector<double> v;
    // ---- configuration (header only)
    if (cmd == "resolution" || cmd == "ipd" || cmd == "fov" || cmd == "local_height" || cmd == "stage" || cmd == "gpu") {
        if (timed) { err = cmd + " is a header directive (no @key)"; return false; }
        if (cmd == "resolution") {
            if (!nums(0, 2, v) || v[0] < 1 || v[1] < 1 || v[0] > 8192 || v[1] > 8192) { err = "resolution W H"; return false; }
            cfg.width = (uint32_t)v[0];
            cfg.height = (uint32_t)v[1];
        } else if (cmd == "ipd") {
            if (!nums(0, 1, v) || v[0] < 0 || v[0] > 0.2) { err = "ipd <meters>"; return false; }
            cfg.ipd = v[0];
        } else if (cmd == "fov") {
            if (!nums(0, 4, v) || v[0] >= v[1] || v[3] >= v[2]) { err = "fov <left> <right> <up> <down> (degrees)"; return false; }
            for (int k = 0; k < 4; ++k) cfg.fov[k] = v[k];
        } else if (cmd == "local_height") {
            if (!nums(0, 1, v)) { err = "local_height <meters>"; return false; }
            cfg.localHeight = v[0];
        } else if (cmd == "stage") {
            if (!nums(0, 2, v) || v[0] <= 0 || v[1] <= 0) { err = "stage <width> <depth>"; return false; }
            cfg.stageW = v[0];
            cfg.stageD = v[1];
        } else {
            if (!nums(0, 1, v)) { err = "gpu <index>"; return false; }
            cfg.gpuIndex = (int)v[0];
        }
        return true;
    }
    if (cmd == "refresh") {
        if (!nums(0, 1, v) || v[0] < 1 || v[0] > 1000) { err = "refresh <hz>"; return false; }
        if (!timed) cfg.refreshHz = v[0];
        else addShot(ScriptShot::Refresh, "", v[0]);
        return true;
    }
    if (cmd == "capture") {
        if (timed) {
            if (!args.empty()) { err = "@key capture takes no arguments"; return false; }
            addShot(ScriptShot::Capture, "", 0);
            return true;
        }
        if (args.empty()) { err = "capture <frame> [frame...]"; return false; }
        for (const std::string& a : args) {
            double f = 0;
            if (!Num(a, f) || f < 0 || f != std::floor(f)) { err = "bad capture frame " + a; return false; }
            frame = (int64_t)f;
            addShot(ScriptShot::Capture, "", 0);
        }
        return true;
    }
    if (cmd == "event") {
        static const char* const kEvents[] = {"focus_loss", "focus_gain", "hide", "show", "exit"};
        bool ok = args.size() == 1;
        if (ok) {
            ok = false;
            for (const char* e : kEvents) ok = ok || args[0] == e;
        }
        if (!ok) { err = "event focus_loss|focus_gain|hide|show|exit"; return false; }
        addShot(ScriptShot::Event, args[0], 0);
        return true;
    }
    if (cmd == "head" || cmd == "left" || cmd == "right") {
        const bool isHead = cmd == "head";
        const int hand = cmd == "left" ? 0 : 1;
        if (args.empty()) { err = "missing arguments"; return false; }
        bool lerp = false;
        if (args.back() == "lerp") {
            lerp = true;
            args.pop_back();
        }
        // pose command
        if (!args.empty() && (args[0] == "pos" || args[0] == "rot" || args[0] == "ypr")) {
            Channel& c = channels_[cmd];
            std::vector<double> pv = c.keys.empty() ? PoseVec(isHead ? cfg.defaultHead : cfg.defaultHand[hand]) : c.keys.back().v;
            size_t k = 0;
            while (k < args.size()) {
                if (args[k] == "pos") {
                    if (!nums(k + 1, 3, v)) { err = "pos x y z"; return false; }
                    pv[0] = v[0]; pv[1] = v[1]; pv[2] = v[2];
                    k += 4;
                } else if (args[k] == "rot") {
                    if (!nums(k + 1, 4, v)) { err = "rot qx qy qz qw"; return false; }
                    const Quat q = Normalize({v[0], v[1], v[2], v[3]});
                    pv[3] = q.x; pv[4] = q.y; pv[5] = q.z; pv[6] = q.w;
                    k += 5;
                } else if (args[k] == "ypr") {
                    if (!nums(k + 1, 3, v)) { err = "ypr yaw pitch roll (degrees)"; return false; }
                    const Quat q = FromYawPitchRoll(v[0], v[1], v[2]);
                    pv[3] = q.x; pv[4] = q.y; pv[5] = q.z; pv[6] = q.w;
                    k += 4;
                } else {
                    err = "unexpected '" + args[k] + "'";
                    return false;
                }
            }
            return addKey(cmd, pv, lerp);
        }
        if (isHead) { err = "head pos|rot|ypr ..."; return false; }
        const std::string comp = args[0];
        if (comp == "tracked") {
            if (!nums(1, 1, v) || args.size() != 2) { err = "tracked 0|1"; return false; }
            return addKey(cmd + ".tracked", v, false);
        }
        if (comp == "thumbstick") {
            if (!nums(1, 2, v) || args.size() != 3) { err = "thumbstick x y"; return false; }
            return addKey(cmd + ".thumbstick", v, lerp);
        }
        for (int c = 0; c < C_COUNT; ++c) {
            if (comp != kCompNames[c] || c == C_STICK_X || c == C_STICK_Y) continue;
            if (!nums(1, 1, v) || args.size() != 2) { err = comp + " <value>"; return false; }
            if ((hand == 0 && (c == C_A || c == C_A_TOUCH || c == C_B || c == C_B_TOUCH || c == C_SYSTEM)) ||
                (hand == 1 && (c == C_X || c == C_X_TOUCH || c == C_Y || c == C_Y_TOUCH || c == C_MENU))) {
                err = comp + " is not on the " + cmd + " Touch controller";
                return false;
            }
            return addKey(cmd + "." + comp, v, lerp);
        }
        err = "unknown component '" + comp + "'";
        return false;
    }
    err = "unknown command '" + cmd + "'";
    return false;
}

void Script::Evaluate(int64_t frame, double t, Snapshot& snap, std::vector<ScriptShot>* fired) {
    auto reached = [&](const ScriptKey& k) { return k.isTime ? t >= k.t - 1e-9 : frame >= k.frame; };
    for (auto& [name, ch] : channels_) {
        int last = -1;
        for (size_t i = 0; i < ch.keys.size(); ++i) {
            ScriptKey& k = ch.keys[i];
            if (!reached(k)) break;
            if (!k.reached) {
                k.reached = true;
                k.reachedFrame = frame;
                k.reachedT = t;
            }
            last = (int)i;
        }
        if (last < 0) continue;
        std::vector<double> val = ch.keys[(size_t)last].v;
        bool isPose = val.size() == 7;
        if ((size_t)last + 1 < ch.keys.size() && ch.keys[(size_t)last + 1].lerp) {
            const ScriptKey& a = ch.keys[(size_t)last];
            const ScriptKey& b = ch.keys[(size_t)last + 1];
            double f = 0;
            if (b.isTime) {
                const double start = a.isTime ? a.t : a.reachedT;
                f = b.t > start ? (t - start) / (b.t - start) : 1.0;
            } else {
                const double start = a.isTime ? (double)a.reachedFrame : (double)a.frame;
                f = (double)b.frame > start ? ((double)frame - start) / ((double)b.frame - start) : 1.0;
            }
            f = std::clamp(f, 0.0, 1.0);
            if (isPose) {
                const Pose pa = VecPose(a.v), pb = VecPose(b.v);
                Pose r;
                r.p = {pa.p.x + (pb.p.x - pa.p.x) * f, pa.p.y + (pb.p.y - pa.p.y) * f, pa.p.z + (pb.p.z - pa.p.z) * f};
                r.q = Slerp(pa.q, pb.q, f);
                val = PoseVec(r);
            } else {
                for (size_t k = 0; k < val.size() && k < b.v.size(); ++k) val[k] = a.v[k] + (b.v[k] - a.v[k]) * f;
            }
        }
        if (name == "head") snap.head = VecPose(val);
        else if (name == "left") snap.hand[0] = VecPose(val);
        else if (name == "right") snap.hand[1] = VecPose(val);
        else {
            const int hand = name.compare(0, 5, "left.") == 0 ? 0 : 1;
            const std::string comp = name.substr(hand == 0 ? 5 : 6);
            if (comp == "tracked") snap.handTracked[hand] = val[0] != 0;
            else if (comp == "thumbstick") {
                snap.comp[hand][C_STICK_X] = (float)val[0];
                snap.comp[hand][C_STICK_Y] = (float)val[1];
            } else {
                for (int c = 0; c < C_COUNT; ++c)
                    if (comp == kCompNames[c]) snap.comp[hand][c] = (float)val[0];
            }
        }
    }
    // derived touch states: a pressed / deflected control is also touched
    for (int h = 0; h < 2; ++h) {
        float* c = snap.comp[h];
        auto touch = [&](int t0, bool on) { if (on) c[t0] = 1.0f; };
        touch(C_TRIGGER_TOUCH, c[C_TRIGGER] > 0);
        touch(C_STICK_TOUCH, c[C_STICK_X] != 0 || c[C_STICK_Y] != 0 || c[C_STICK_CLICK] != 0);
        touch(C_X_TOUCH, c[C_X] != 0);
        touch(C_Y_TOUCH, c[C_Y] != 0);
        touch(C_A_TOUCH, c[C_A] != 0);
        touch(C_B_TOUCH, c[C_B] != 0);
    }
    if (!fired) return;
    for (ScriptShot& s : shots_) {
        if (s.fired) continue;
        if (s.isTime ? t >= s.t - 1e-9 : frame >= s.frame) {
            s.fired = true;
            fired->push_back(s);
        }
    }
}

} // namespace xs
