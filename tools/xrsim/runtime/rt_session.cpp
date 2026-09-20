// xrsim - session state machine, frame pacing (xrWaitFrame / xrBeginFrame / xrEndFrame), layers, spaces, views,
// refresh rate (XR_FB_display_refresh_rate), performance settings (XR_EXT_performance_settings), per-frame CSV.
#include "rt_api.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstring>

namespace xs {

namespace {

const char* StateName(XrSessionState s) {
    switch (s) {
    case XR_SESSION_STATE_IDLE: return "IDLE";
    case XR_SESSION_STATE_READY: return "READY";
    case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
    case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
    case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
    case XR_SESSION_STATE_STOPPING: return "STOPPING";
    case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
    case XR_SESSION_STATE_EXITING: return "EXITING";
    default: return "UNKNOWN";
    }
}

int64_t PeriodOf(double hz) { return (int64_t)std::llround(1e9 / hz); }

void ApplyRefresh(Session* s, float hz) {
    if (std::fabs(hz - s->refreshHz) < 0.001f) return;
    const float from = s->refreshHz;
    const int64_t oldP = s->periodNs;
    s->refreshHz = hz;
    s->periodNs = PeriodOf(hz);
    if (s->nextDisplay != 0) s->nextDisplay = s->nextDisplay - oldP + s->periodNs;
    Log("refresh rate %.2f -> %.2f Hz (period %lld ns)", from, hz, (long long)s->periodNs);
    if (s->inst->Has(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME)) {
        XrEventDataDisplayRefreshRateChangedFB ev{XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB};
        ev.fromDisplayRefreshRate = from;
        ev.toDisplayRefreshRate = hz;
        s->inst->PushEvent(&ev, sizeof(ev));
    }
}

std::string Fmt(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

std::string PoseCsv(const Pose& p) {
    return Fmt("%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f", p.p.x, p.p.y, p.p.z, p.q.x, p.q.y, p.q.z, p.q.w);
}

void WriteRow(Session* s, const FrameRec& r) {
    std::string row = Fmt("%lld,%lld,%lld,%.4f,%lld,%lld,%lld,%d,%s,%lld,%lld,%s,%s,%d,", (long long)r.frame,
                          (long long)r.waitCall, (long long)r.waitReturn, (double)(r.waitReturn - r.waitCall) / 1e6,
                          (long long)r.display, (long long)r.period, (long long)r.skipped, r.shouldRender ? 1 : 0,
                          StateName(r.state), (long long)r.begin, (long long)r.end, r.result.c_str(),
                          r.layers.empty() ? "-" : r.layers.c_str(), r.captured ? 1 : 0);
    row += PoseCsv(r.head);
    for (int v = 0; v < 2; ++v) row += "," + (r.haveViews ? PoseCsv(r.views[v]) : std::string(",,,,,,"));
    s->inst->out.Csv(row);
}

bool SpaceWorld(Session* s, Space* sp, XrTime t, Pose& out) {
    const Config& c = s->inst->cfg;
    Pose origin;
    if (sp->isAction) {
        bool active = false;
        if (!InputPoseForAction(s, sp->action, sp->subPath, t, origin, active) || !active) return false;
    } else {
        switch (sp->refType) {
        case XR_REFERENCE_SPACE_TYPE_VIEW: origin = s->SnapshotAt(t).head; break;
        case XR_REFERENCE_SPACE_TYPE_LOCAL: origin.p = {0, c.localHeight, 0}; break;
        default: break; // STAGE and LOCAL_FLOOR share the floor origin (LOCAL is above it)
        }
    }
    out = Mul(origin, sp->offset);
    return true;
}

bool RefSpaceSupported(Session* s, XrReferenceSpaceType t) {
    if (t == XR_REFERENCE_SPACE_TYPE_VIEW || t == XR_REFERENCE_SPACE_TYPE_LOCAL || t == XR_REFERENCE_SPACE_TYPE_STAGE) return true;
    return t == XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR && (s->inst->Is11() || s->inst->Has(XR_EXT_LOCAL_FLOOR_EXTENSION_NAME));
}

XrResult CheckSubImage(Session* s, const XrSwapchainSubImage& si, Swapchain*& out) {
    out = GetSwapchain(si.swapchain);
    if (!out || out->session != s) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "layer swapchain");
    if (out->lastReleased < 0) return XS_FAIL(XR_ERROR_LAYER_INVALID, "swapchain image never released");
    if (si.imageArrayIndex >= out->ci.arraySize) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "imageArrayIndex %u", si.imageArrayIndex);
    const XrRect2Di& r = si.imageRect;
    if (r.offset.x < 0 || r.offset.y < 0 || r.extent.width <= 0 || r.extent.height <= 0 ||
        (int64_t)r.offset.x + r.extent.width > (int64_t)out->ci.width || (int64_t)r.offset.y + r.extent.height > (int64_t)out->ci.height)
        return XS_FAIL(XR_ERROR_SWAPCHAIN_RECT_INVALID, "rect %d,%d %dx%d in %ux%u", r.offset.x, r.offset.y, r.extent.width,
                       r.extent.height, out->ci.width, out->ci.height);
    return XR_SUCCESS;
}

const char* kCsvHeader =
    "frame,wait_call_ns,wait_return_ns,wait_ms,display_ns,period_ns,skipped,should_render,state,begin_ns,end_ns,result,layers,"
    "captured,head_px,head_py,head_pz,head_qx,head_qy,head_qz,head_qw,v0_px,v0_py,v0_pz,v0_qx,v0_qy,v0_qz,v0_qw,v1_px,v1_py,"
    "v1_pz,v1_qx,v1_qy,v1_qz,v1_qw";

} // namespace

// ================================================================================================ Session helpers
const Snapshot& Session::SnapshotAt(XrTime t) const {
    if (history.empty()) return cur;
    for (auto it = history.rbegin(); it != history.rend(); ++it)
        if (it->time <= t) return *it;
    return history.front();
}

void Session::SetState(XrSessionState s) {
    Log("session state %s -> %s", StateName(state), StateName(s));
    state = s;
    XrEventDataSessionStateChanged ev{XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED};
    ev.session = (XrSession)handle;
    ev.state = s;
    ev.time = NowXr();
    inst->PushEvent(&ev, sizeof(ev));
}

void Session::UpdateState() {
    if (!running) return;
    auto rank = [](XrSessionState st) {
        switch (st) {
        case XR_SESSION_STATE_SYNCHRONIZED: return 1;
        case XR_SESSION_STATE_VISIBLE: return 2;
        case XR_SESSION_STATE_FOCUSED: return 3;
        default: return 0; // READY
        }
    };
    for (int guard = 0; guard < 8; ++guard) {
        if (state == XR_SESSION_STATE_STOPPING) return;
        if (exitPending) {
            if (state == XR_SESSION_STATE_FOCUSED) SetState(XR_SESSION_STATE_VISIBLE);
            else if (state == XR_SESSION_STATE_VISIBLE) SetState(XR_SESSION_STATE_SYNCHRONIZED);
            else if (state == XR_SESSION_STATE_SYNCHRONIZED) SetState(XR_SESSION_STATE_STOPPING);
            else SetState(XR_SESSION_STATE_SYNCHRONIZED); // READY
            continue;
        }
        if (!synced) return;
        const XrSessionState want = userVisible ? (userFocus ? XR_SESSION_STATE_FOCUSED : XR_SESSION_STATE_VISIBLE) : XR_SESSION_STATE_SYNCHRONIZED;
        const int r = rank(state), w = rank(want);
        if (r == w) return;
        static const XrSessionState kByRank[] = {XR_SESSION_STATE_READY, XR_SESSION_STATE_SYNCHRONIZED, XR_SESSION_STATE_VISIBLE,
                                                 XR_SESSION_STATE_FOCUSED};
        SetState(kByRank[r < w ? r + 1 : r - 1]);
    }
}

void DestroySessionObject(Session* s) {
    s->running = false;
    s->frameCv.notify_all();
    for (auto& [n, rec] : s->frames) {
        rec.result = "not_ended";
        WriteRow(s, rec);
    }
    s->frames.clear();
    while (!s->spaces.empty()) {
        Space* sp = s->spaces.back();
        s->spaces.pop_back();
        Unregister(sp->handle);
    }
    while (!s->swapchains.empty()) DestroySwapchainObject(s->swapchains.back());
    if (s->mirror) s->mirror->Stop();
    VkSessionShutdown(s);
    s->inst->out.Flush();
    // drop queued events of this session
    auto& ev = s->inst->events;
    ev.erase(std::remove_if(ev.begin(), ev.end(),
                            [&](const XrEventDataBuffer& b) {
                                if (b.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
                                    return reinterpret_cast<const XrEventDataSessionStateChanged&>(b).session == (XrSession)s->handle;
                                if (b.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED)
                                    return reinterpret_cast<const XrEventDataInteractionProfileChanged&>(b).session == (XrSession)s->handle;
                                return false;
                            }),
             ev.end());
    s->inst->session = nullptr;
    Log("session destroyed (%lld frames)", (long long)s->nextFrame);
    Unregister(s->handle);
}

// ================================================================================================ session
XrResult XRAPI_CALL xrCreateSession(XrInstance instance, const XrSessionCreateInfo* ci, XrSession* session) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Instance* inst = GetInstance(instance);
    if (!inst) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "instance");
    if (!ci || !session || ci->type != XR_TYPE_SESSION_CREATE_INFO) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "createInfo");
    if (ci->systemId != kSystemId) return XS_FAIL(XR_ERROR_SYSTEM_INVALID, "systemId");
    if (ci->createFlags != 0) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "createFlags");
    const XrGraphicsBindingVulkanKHR* binding = nullptr;
    for (auto* n = reinterpret_cast<const XrBaseInStructure*>(ci->next); n; n = n->next)
        if (n->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) binding = reinterpret_cast<const XrGraphicsBindingVulkanKHR*>(n);
    if (!binding) return XS_FAIL(XR_ERROR_GRAPHICS_DEVICE_INVALID, "no XrGraphicsBindingVulkanKHR in the chain (xrsim is Vulkan only)");
    if (!inst->Has(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) && !inst->Has(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME))
        return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "Vulkan binding without XR_KHR_vulkan_enable(2)");
    if (!inst->reqCalled) return XS_FAIL(XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING, "call xrGetVulkanGraphicsRequirements(2)KHR first");
    if (inst->session) return XS_FAIL(XR_ERROR_LIMIT_REACHED, "one session at a time");

    auto s = std::make_unique<Session>();
    s->inst = inst;
    std::string err;
    if (!VkSessionInit(s.get(), binding, err)) return XS_FAIL(XR_ERROR_GRAPHICS_DEVICE_INVALID, "%s", err.c_str());
    s->refreshHz = (float)inst->cfg.refreshHz;
    s->periodNs = PeriodOf(inst->cfg.refreshHz);
    s->cur.head = inst->cfg.defaultHead;
    s->cur.hand[0] = inst->cfg.defaultHand[0];
    s->cur.hand[1] = inst->cfg.defaultHand[1];
    if (inst->script.loaded) inst->script.Evaluate(0, 0.0, s->cur, nullptr);
    if (inst->cfg.window) s->mirror = std::make_unique<Mirror>();
    Session* sp = s.get();
    *session = (XrSession)Register(std::move(s));
    inst->session = sp;
    if (!inst->csvHeaderWritten) {
        inst->out.Csv(kCsvHeader);
        inst->csvHeaderWritten = true;
    }
    Log("xrCreateSession: device %p queue family %u index %u, %zu swapchain formats", (void*)sp->device, sp->queueFamily,
        sp->queueIndex, sp->formats.size());
    sp->SetState(XR_SESSION_STATE_IDLE);
    sp->SetState(XR_SESSION_STATE_READY);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrDestroySession(XrSession session) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Session* s = GetSession(session);
    if (!s) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "session");
    DestroySessionObject(s);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrBeginSession(XrSession session, const XrSessionBeginInfo* info) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Session* s = GetSession(session);
    if (!s) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "session");
    if (!info || info->type != XR_TYPE_SESSION_BEGIN_INFO) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "beginInfo");
    if (info->primaryViewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
        if (info->primaryViewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MONO ||
            info->primaryViewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_WITH_FOVEATED_INSET)
            return XS_FAIL(XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED, "%d", (int)info->primaryViewConfigurationType);
        return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "view configuration %d", (int)info->primaryViewConfigurationType);
    }
    if (s->running) return XS_FAIL(XR_ERROR_SESSION_RUNNING, "already running");
    if (s->state != XR_SESSION_STATE_READY) return XS_FAIL(XR_ERROR_SESSION_NOT_READY, "state %s", StateName(s->state));
    s->running = true;
    s->synced = false;
    s->exitPending = false;
    s->nextDisplay = 0;
    s->waitedNotBegun.clear();
    s->begunFrame = -1;
    Log("xrBeginSession");
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrEndSession(XrSession session) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Session* s = GetSession(session);
    if (!s) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "session");
    if (!s->running) return XS_FAIL(XR_ERROR_SESSION_NOT_RUNNING, "not running");
    if (s->state != XR_SESSION_STATE_STOPPING) return XS_FAIL(XR_ERROR_SESSION_NOT_STOPPING, "state %s", StateName(s->state));
    s->running = false;
    s->waitedNotBegun.clear();
    s->begunFrame = -1;
    s->frameCv.notify_all();
    for (auto& [n, rec] : s->frames) {
        rec.result = "not_ended";
        WriteRow(s, rec);
    }
    s->frames.clear();
    s->inst->out.Flush();
    Log("xrEndSession");
    s->SetState(XR_SESSION_STATE_IDLE);
    if (s->exitPending) s->SetState(XR_SESSION_STATE_EXITING);
    else s->SetState(XR_SESSION_STATE_READY);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrRequestExitSession(XrSession session) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Session* s = GetSession(session);
    if (!s) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "session");
    if (!s->running) return XS_FAIL(XR_ERROR_SESSION_NOT_RUNNING, "not running");
    Log("xrRequestExitSession");
    s->exitPending = true;
    s->UpdateState();
    return XR_SUCCESS;
}

// ================================================================================================ frame loop
XrResult XRAPI_CALL xrWaitFrame(XrSession session, const XrFrameWaitInfo* info, XrFrameState* frameState) {
    std::unique_lock<std::recursive_mutex> lk(g_lock);
    Session* s = GetSession(session);
    if (!s) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "session");
    if (info && info->type != XR_TYPE_FRAME_WAIT_INFO) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "frameWaitInfo.type");
    if (!frameState || frameState->type != XR_TYPE_FRAME_STATE) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "frameState");
    if (!s->running) return XS_FAIL(XR_ERROR_SESSION_NOT_RUNNING, "not running");
    const XrTime callT = NowXr();
    if (!s->waitedNotBegun.empty()) {
        if (s->lastWaitThread == GetCurrentThreadId())
            return XS_FAIL(XR_ERROR_CALL_ORDER_INVALID, "xrWaitFrame called twice on one thread without xrBeginFrame (would deadlock)");
        const uint64_t h = s->handle;
        s->frameCv.wait(lk, [&] {
            Session* x = GetSession((XrSession)h);
            return !x || !x->running || x->waitedNotBegun.empty();
        });
        s = GetSession((XrSession)h);
        if (!s) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "session destroyed while waiting");
        if (!s->running) return XS_FAIL(XR_ERROR_SESSION_NOT_RUNNING, "session ended while waiting");
    }
    if (s->nextDisplay == 0) s->nextDisplay = callT + s->periodNs;
    XrTime display = s->nextDisplay;
    int64_t skipped = 0;
    const XrTime now = NowXr();
    while (now >= display) {  // the app missed this display time: the frame goes to the next refresh
        display += s->periodNs;
        ++skipped;
    }
    const XrTime wake = display - s->periodNs;
    s->nextDisplay = display + s->periodNs;
    s->lastWaitThread = GetCurrentThreadId();
    const int64_t n = s->nextFrame++;
    s->waitedNotBegun.push_back(n);
    if (now < wake) {
        const uint64_t h = s->handle;
        lk.unlock();
        SleepUntil(wake);
        lk.lock();
        s = GetSession((XrSession)h);
        if (!s) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "session destroyed while waiting");
    }
    if (s->firstDisplay == 0) s->firstDisplay = display;

    // simulated device state for this frame
    Instance* inst = s->inst;
    Snapshot snap;
    snap.frame = n;
    snap.time = display;
    snap.head = inst->cfg.defaultHead;
    snap.hand[0] = inst->cfg.defaultHand[0];
    snap.hand[1] = inst->cfg.defaultHand[1];
    std::vector<ScriptShot> fired;
    if (inst->script.loaded) inst->script.Evaluate(n, (double)(display - s->firstDisplay) / 1e9, snap, &fired);
    s->history.push_back(snap);
    while (s->history.size() > 512) s->history.pop_front();
    s->cur = snap;
    for (const ScriptShot& sh : fired) {
        if (sh.kind == ScriptShot::Capture) {
            s->captureFrames.insert(n);
        } else if (sh.kind == ScriptShot::Refresh) {
            ApplyRefresh(s, (float)sh.value);
        } else {
            Log("frame %lld: script event %s (line %d)", (long long)n, sh.arg.c_str(), sh.line);
            if (sh.arg == "focus_loss") s->userFocus = false;
            else if (sh.arg == "focus_gain") s->userFocus = true;
            else if (sh.arg == "hide") s->userVisible = false;
            else if (sh.arg == "show") s->userVisible = true;
            else if (sh.arg == "exit") s->exitPending = true;
        }
    }
    if (s->mirror && s->mirror->closeRequested() && !s->exitPending) {
        Log("mirror window closed: requesting exit");
        s->exitPending = true;
    }
    s->UpdateState();

    FrameRec rec;
    rec.frame = n;
    rec.waitCall = callT;
    rec.waitReturn = NowXr();
    rec.display = display;
    rec.period = s->periodNs;
    rec.skipped = skipped;
    rec.state = s->state;
    rec.shouldRender = s->state == XR_SESSION_STATE_VISIBLE || s->state == XR_SESSION_STATE_FOCUSED;
    rec.head = snap.head;
    s->frames[n] = rec;
    if (skipped) Log("frame %lld: app late, %lld refresh interval(s) skipped", (long long)n, (long long)skipped);

    frameState->predictedDisplayTime = display;
    frameState->predictedDisplayPeriod = s->periodNs;
    frameState->shouldRender = rec.shouldRender ? XR_TRUE : XR_FALSE;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrBeginFrame(XrSession session, const XrFrameBeginInfo* info) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Session* s = GetSession(session);
    if (!s) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "session");
    if (info && info->type != XR_TYPE_FRAME_BEGIN_INFO) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "frameBeginInfo.type");
    if (!s->running) return XS_FAIL(XR_ERROR_SESSION_NOT_RUNNING, "not running");
    if (s->waitedNotBegun.empty()) return XS_FAIL(XR_ERROR_CALL_ORDER_INVALID, "xrBeginFrame without a preceding xrWaitFrame");
    XrResult r = XR_SUCCESS;
    if (s->begunFrame >= 0) {
        auto it = s->frames.find(s->begunFrame);
        if (it != s->frames.end()) {
            it->second.result = "discarded";
            WriteRow(s, it->second);
            s->frames.erase(it);
        }
        r = XR_FRAME_DISCARDED;
    }
    s->begunFrame = s->waitedNotBegun.front();
    s->waitedNotBegun.pop_front();
    s->frameCv.notify_all();
    auto it = s->frames.find(s->begunFrame);
    if (it != s->frames.end()) it->second.begin = NowXr();
    return r;
}

XrResult XRAPI_CALL xrEndFrame(XrSession session, const XrFrameEndInfo* info) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Session* s = GetSession(session);
    if (!s) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "session");
    if (!info || info->type != XR_TYPE_FRAME_END_INFO) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "frameEndInfo");
    if (!s->running) return XS_FAIL(XR_ERROR_SESSION_NOT_RUNNING, "not running");
    if (s->begunFrame < 0) return XS_FAIL(XR_ERROR_CALL_ORDER_INVALID, "xrEndFrame without xrBeginFrame");
    if (info->displayTime <= 0) return XS_FAIL(XR_ERROR_TIME_INVALID, "displayTime %lld", (long long)info->displayTime);
    if (info->environmentBlendMode != XR_ENVIRONMENT_BLEND_MODE_OPAQUE) {
        if (info->environmentBlendMode == XR_ENVIRONMENT_BLEND_MODE_ADDITIVE || info->environmentBlendMode == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND)
            return XS_FAIL(XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED, "%d", (int)info->environmentBlendMode);
        return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "environmentBlendMode %d", (int)info->environmentBlendMode);
    }
    if (info->layerCount > XR_MIN_COMPOSITION_LAYERS_SUPPORTED) return XS_FAIL(XR_ERROR_LAYER_LIMIT_EXCEEDED, "%u layers", info->layerCount);
    if (info->layerCount && !info->layers) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "layers == NULL");

    const int64_t n = s->begunFrame;
    const bool capture = s->captureFrames.count(n) != 0;
    std::vector<LayerView> views;           // capture list
    const LayerView* mirrorL = nullptr;
    const LayerView* mirrorR = nullptr;
    std::string layers;
    Pose submitted[2];
    bool haveViews = false;
    char name[96];
    views.reserve(info->layerCount * 2);
    for (uint32_t li = 0; li < info->layerCount; ++li) {
        const XrCompositionLayerBaseHeader* L = info->layers[li];
        if (!L) return XS_FAIL(XR_ERROR_LAYER_INVALID, "layers[%u] == NULL", li);
        Space* space = GetSpace(L->space);
        if (!space || space->session != s) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "layers[%u].space", li);
        if (!layers.empty()) layers += "|";
        if (L->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            const auto* P = reinterpret_cast<const XrCompositionLayerProjection*>(L);
            if (P->viewCount != 2 || !P->views) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "projection viewCount %u (stereo needs 2)", P->viewCount);
            bool depth = false;
            for (uint32_t v = 0; v < 2; ++v) {
                const XrCompositionLayerProjectionView& pv = P->views[v];
                if (pv.type != XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "views[%u].type", v);
                if (!IsUnitQuat(pv.pose.orientation)) return XS_FAIL(XR_ERROR_POSE_INVALID, "layer %u view %u pose", li, v);
                Swapchain* sc = nullptr;
                XrResult r = CheckSubImage(s, pv.subImage, sc);
                if (XR_FAILED(r)) return r;
                if (sc->fmt->pix == FmtInfo::Depth) return XS_FAIL(XR_ERROR_LAYER_INVALID, "projection view uses a depth swapchain");
                for (auto* e = reinterpret_cast<const XrBaseInStructure*>(pv.next); e; e = e->next) {
                    if (e->type != XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR) continue;
                    if (!s->inst->Has(XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME)) {
                        Log("xrEndFrame: XrCompositionLayerDepthInfoKHR ignored (XR_KHR_composition_layer_depth not enabled)");
                        continue;
                    }
                    const auto* d = reinterpret_cast<const XrCompositionLayerDepthInfoKHR*>(e);
                    Swapchain* dsc = nullptr;
                    r = CheckSubImage(s, d->subImage, dsc);
                    if (XR_FAILED(r)) return r;
                    if (dsc->fmt->pix != FmtInfo::Depth) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "depth info references a color swapchain");
                    if (!(d->minDepth >= 0 && d->minDepth <= 1 && d->maxDepth >= 0 && d->maxDepth <= 1 && d->minDepth < d->maxDepth) ||
                        d->nearZ == d->farZ)
                        return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "depth range %.3f..%.3f near %.3f far %.3f", d->minDepth,
                                       d->maxDepth, d->nearZ, d->farZ);
                    depth = true;
                }
                submitted[v] = FromXr(pv.pose);
                snprintf(name, sizeof(name), "f%06lld_l%u_proj_v%u.png", (long long)n, li, v);
                views.push_back({sc, (uint32_t)sc->lastReleased, pv.subImage.imageArrayIndex, pv.subImage.imageRect, name,
                                 (P->layerFlags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT) == 0});
            }
            if (!haveViews) {
                mirrorL = &views[views.size() - 2];
                mirrorR = &views[views.size() - 1];
            }
            haveViews = true;
            layers += depth ? "P2d" : "P2";
        } else if (L->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
            const auto* Q = reinterpret_cast<const XrCompositionLayerQuad*>(L);
            if (!IsUnitQuat(Q->pose.orientation)) return XS_FAIL(XR_ERROR_POSE_INVALID, "quad layer %u pose", li);
            if (Q->eyeVisibility > XR_EYE_VISIBILITY_RIGHT) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "eyeVisibility %d", (int)Q->eyeVisibility);
            Swapchain* sc = nullptr;
            const XrResult r = CheckSubImage(s, Q->subImage, sc);
            if (XR_FAILED(r)) return r;
            if (sc->fmt->pix == FmtInfo::Depth) return XS_FAIL(XR_ERROR_LAYER_INVALID, "quad layer uses a depth swapchain");
            snprintf(name, sizeof(name), "f%06lld_l%u_quad.png", (long long)n, li);
            views.push_back({sc, (uint32_t)sc->lastReleased, Q->subImage.imageArrayIndex, Q->subImage.imageRect, name, false});
            layers += "Q";
        } else {
            return XS_FAIL(XR_ERROR_LAYER_INVALID, "layer %u type %d not supported", li, (int)L->type);
        }
    }

    auto it = s->frames.find(n);
    FrameRec rec;
    if (it != s->frames.end()) rec = it->second;
    rec.frame = n;
    rec.end = NowXr();
    rec.result = "ok";
    rec.layers = layers;
    rec.haveViews = haveViews;
    rec.views[0] = submitted[0];
    rec.views[1] = submitted[1];
    if (capture) {
        if (s->inst->out.enabled() && !views.empty()) {
            CaptureViews(s, views);
            rec.captured = true;
        } else {
            Log("frame %lld: capture requested but %s", (long long)n, s->inst->out.enabled() ? "no layers were submitted" : "XRSIM_OUT is not set");
        }
        s->captureFrames.erase(n);
    }
    if (s->mirror && mirrorL) MirrorViews(s, mirrorL, mirrorR);
    WriteRow(s, rec);
    if (it != s->frames.end()) s->frames.erase(it);
    s->begunFrame = -1;
    if (!s->synced) {
        s->synced = true;
        s->UpdateState();
    }
    return XR_SUCCESS;
}

// ================================================================================================ views
XrResult XRAPI_CALL xrLocateViews(XrSession session, const XrViewLocateInfo* info, XrViewState* viewState, uint32_t cap,
                                  uint32_t* count, XrView* views) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Session* s = GetSession(session);
    if (!s) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "session");
    if (!info || info->type != XR_TYPE_VIEW_LOCATE_INFO || !viewState || viewState->type != XR_TYPE_VIEW_STATE || !count)
        return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "arguments");
    if (info->viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
        return XS_FAIL(XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED, "%d", (int)info->viewConfigurationType);
    if (info->displayTime <= 0) return XS_FAIL(XR_ERROR_TIME_INVALID, "displayTime %lld", (long long)info->displayTime);
    Space* base = GetSpace(info->space);
    if (!base || base->session != s) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "space");
    *count = 2;
    if (cap == 0) return XR_SUCCESS;
    if (cap < 2) return XR_ERROR_SIZE_INSUFFICIENT;
    if (!views) return XR_ERROR_VALIDATION_FAILURE;
    for (int i = 0; i < 2; ++i)
        if (views[i].type != XR_TYPE_VIEW) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "views[%d].type", i);
    Pose baseW;
    const bool ok = SpaceWorld(s, base, info->displayTime, baseW);
    const Pose head = s->SnapshotAt(info->displayTime).head;
    const Config& c = s->inst->cfg;
    const double d2r = 3.14159265358979323846 / 180.0;
    for (int i = 0; i < 2; ++i) {
        Pose eye;
        eye.p = {(i == 0 ? -0.5 : 0.5) * c.ipd, 0, 0};
        views[i].pose = ToXr(ok ? Mul(Inverse(baseW), Mul(head, eye)) : Pose{});
        if (i == 0) views[i].fov = {(float)(c.fov[0] * d2r), (float)(c.fov[1] * d2r), (float)(c.fov[2] * d2r), (float)(c.fov[3] * d2r)};
        else views[i].fov = {(float)(-c.fov[1] * d2r), (float)(-c.fov[0] * d2r), (float)(c.fov[2] * d2r), (float)(c.fov[3] * d2r)};
    }
    viewState->viewStateFlags = ok ? (XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT |
                                      XR_VIEW_STATE_ORIENTATION_TRACKED_BIT | XR_VIEW_STATE_POSITION_TRACKED_BIT)
                                   : 0;
    return XR_SUCCESS;
}

// ================================================================================================ spaces
XrResult XRAPI_CALL xrEnumerateReferenceSpaces(XrSession session, uint32_t cap, uint32_t* count, XrReferenceSpaceType* spaces) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Session* s = GetSession(session);
    if (!s) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "session");
    std::vector<XrReferenceSpaceType> list = {XR_REFERENCE_SPACE_TYPE_VIEW, XR_REFERENCE_SPACE_TYPE_LOCAL, XR_REFERENCE_SPACE_TYPE_STAGE};
    if (RefSpaceSupported(s, XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR)) list.push_back(XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR);
    return TwoCall(cap, count, spaces, list);
}

XrResult XRAPI_CALL xrCreateReferenceSpace(XrSession session, const XrReferenceSpaceCreateInfo* ci, XrSpace* space) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Session* s = GetSession(session);
    if (!s) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "session");
    if (!ci || !space || ci->type != XR_TYPE_REFERENCE_SPACE_CREATE_INFO) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "createInfo");
    if (!RefSpaceSupported(s, ci->referenceSpaceType)) return XS_FAIL(XR_ERROR_REFERENCE_SPACE_UNSUPPORTED, "%d", (int)ci->referenceSpaceType);
    if (!IsUnitQuat(ci->poseInReferenceSpace.orientation)) return XS_FAIL(XR_ERROR_POSE_INVALID, "poseInReferenceSpace");
    auto sp = std::make_unique<Space>();
    sp->session = s;
    sp->refType = ci->referenceSpaceType;
    sp->offset = FromXr(ci->poseInReferenceSpace);
    s->spaces.push_back(sp.get());
    *space = (XrSpace)Register(std::move(sp));
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrGetReferenceSpaceBoundsRect(XrSession session, XrReferenceSpaceType type, XrExtent2Df* bounds) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Session* s = GetSession(session);
    if (!s) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "session");
    if (!bounds) return XR_ERROR_VALIDATION_FAILURE;
    if (!RefSpaceSupported(s, type)) return XS_FAIL(XR_ERROR_REFERENCE_SPACE_UNSUPPORTED, "%d", (int)type);
    if (type == XR_REFERENCE_SPACE_TYPE_STAGE) {
        *bounds = {(float)s->inst->cfg.stageW, (float)s->inst->cfg.stageD};
        return XR_SUCCESS;
    }
    *bounds = {0, 0};
    return XR_SPACE_BOUNDS_UNAVAILABLE;
}

XrResult XRAPI_CALL xrCreateActionSpace(XrSession session, const XrActionSpaceCreateInfo* ci, XrSpace* space) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Session* s = GetSession(session);
    if (!s) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "session");
    if (!ci || !space || ci->type != XR_TYPE_ACTION_SPACE_CREATE_INFO) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "createInfo");
    Action* a = GetAction(ci->action);
    if (!a || a->set->inst != s->inst) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "action");
    if (a->type != XR_ACTION_TYPE_POSE_INPUT) return XS_FAIL(XR_ERROR_ACTION_TYPE_MISMATCH, "action '%s' is not a pose action", a->name.c_str());
    if (ci->subactionPath != XR_NULL_PATH && a->SlotOf(ci->subactionPath) < 0)
        return XS_FAIL(XR_ERROR_PATH_UNSUPPORTED, "subactionPath not declared for '%s'", a->name.c_str());
    if (!IsUnitQuat(ci->poseInActionSpace.orientation)) return XS_FAIL(XR_ERROR_POSE_INVALID, "poseInActionSpace");
    auto sp = std::make_unique<Space>();
    sp->session = s;
    sp->isAction = true;
    sp->action = a;
    sp->subPath = ci->subactionPath;
    sp->offset = FromXr(ci->poseInActionSpace);
    s->spaces.push_back(sp.get());
    *space = (XrSpace)Register(std::move(sp));
    return XR_SUCCESS;
}

namespace {
XrSpaceLocationFlags LocateOne(Session* s, Space* sp, Space* base, XrTime t, XrPosef& pose) {
    Pose a, b;
    if (!SpaceWorld(s, sp, t, a) || !SpaceWorld(s, base, t, b)) {
        pose = ToXr(Pose{});
        return 0;
    }
    pose = ToXr(Mul(Inverse(b), a));
    return XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT |
           XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
}
} // namespace

XrResult XRAPI_CALL xrLocateSpace(XrSpace space, XrSpace baseSpace, XrTime time, XrSpaceLocation* location) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Space* sp = GetSpace(space);
    Space* base = GetSpace(baseSpace);
    if (!sp || !base) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "space");
    if (sp->session != base->session) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "spaces of different sessions");
    if (!location || location->type != XR_TYPE_SPACE_LOCATION) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "location");
    if (time <= 0) return XS_FAIL(XR_ERROR_TIME_INVALID, "time %lld", (long long)time);
    location->locationFlags = LocateOne(sp->session, sp, base, time, location->pose);
    for (auto* n = reinterpret_cast<XrBaseOutStructure*>(location->next); n; n = n->next) {
        if (n->type == XR_TYPE_SPACE_VELOCITY) {
            auto* v = reinterpret_cast<XrSpaceVelocity*>(n);
            v->velocityFlags = 0; // velocities are not simulated
            v->linearVelocity = {0, 0, 0};
            v->angularVelocity = {0, 0, 0};
        }
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrLocateSpaces(XrSession session, const XrSpacesLocateInfo* info, XrSpaceLocations* locs) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Session* s = GetSession(session);
    if (!s) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "session");
    if (!info || info->type != XR_TYPE_SPACES_LOCATE_INFO || !locs || locs->type != XR_TYPE_SPACE_LOCATIONS)
        return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "arguments");
    if (info->spaceCount == 0 || !info->spaces || locs->locationCount != info->spaceCount || !locs->locations)
        return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "counts");
    if (info->time <= 0) return XS_FAIL(XR_ERROR_TIME_INVALID, "time");
    Space* base = GetSpace(info->baseSpace);
    if (!base || base->session != s) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "baseSpace");
    for (uint32_t i = 0; i < info->spaceCount; ++i) {
        Space* sp = GetSpace(info->spaces[i]);
        if (!sp || sp->session != s) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "spaces[%u]", i);
    }
    XrSpaceVelocities* vel = nullptr;
    for (auto* n = reinterpret_cast<XrBaseOutStructure*>(locs->next); n; n = n->next)
        if (n->type == XR_TYPE_SPACE_VELOCITIES) vel = reinterpret_cast<XrSpaceVelocities*>(n);
    if (vel && (vel->velocityCount != info->spaceCount || !vel->velocities)) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "velocities");
    for (uint32_t i = 0; i < info->spaceCount; ++i) {
        locs->locations[i].locationFlags = LocateOne(s, GetSpace(info->spaces[i]), base, info->time, locs->locations[i].pose);
        if (vel) vel->velocities[i] = XrSpaceVelocityData{0, {0, 0, 0}, {0, 0, 0}};
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrDestroySpace(XrSpace space) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Space* sp = GetSpace(space);
    if (!sp) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "space");
    auto& v = sp->session->spaces;
    v.erase(std::remove(v.begin(), v.end(), sp), v.end());
    Unregister(sp->handle);
    return XR_SUCCESS;
}

// ================================================================================================ refresh rate / performance
XrResult XRAPI_CALL xrEnumerateDisplayRefreshRatesFB(XrSession session, uint32_t cap, uint32_t* count, float* rates) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Session* s = GetSession(session);
    if (!s) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "session");
    return TwoCall(cap, count, rates, s->inst->cfg.refreshRates);
}

XrResult XRAPI_CALL xrGetDisplayRefreshRateFB(XrSession session, float* rate) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Session* s = GetSession(session);
    if (!s) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "session");
    if (!rate) return XR_ERROR_VALIDATION_FAILURE;
    *rate = s->refreshHz;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrRequestDisplayRefreshRateFB(XrSession session, float rate) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Session* s = GetSession(session);
    if (!s) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "session");
    if (rate == 0.0f) rate = (float)s->inst->cfg.refreshHz;
    bool ok = false;
    for (float r : s->inst->cfg.refreshRates) ok = ok || std::fabs(r - rate) < 0.01f;
    if (!ok) return XS_FAIL(XR_ERROR_DISPLAY_REFRESH_RATE_UNSUPPORTED_FB, "%.3f Hz", rate);
    ApplyRefresh(s, rate);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrPerfSettingsSetPerformanceLevelEXT(XrSession session, XrPerfSettingsDomainEXT domain, XrPerfSettingsLevelEXT level) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Session* s = GetSession(session);
    if (!s) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "session");
    if (domain != XR_PERF_SETTINGS_DOMAIN_CPU_EXT && domain != XR_PERF_SETTINGS_DOMAIN_GPU_EXT)
        return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "domain %d", (int)domain);
    if (level != XR_PERF_SETTINGS_LEVEL_POWER_SAVINGS_EXT && level != XR_PERF_SETTINGS_LEVEL_SUSTAINED_LOW_EXT &&
        level != XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT && level != XR_PERF_SETTINGS_LEVEL_BOOST_EXT)
        return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "level %d", (int)level);
    Log("performance level %s = %d (recorded; no effect in the simulator)", domain == XR_PERF_SETTINGS_DOMAIN_CPU_EXT ? "CPU" : "GPU", (int)level);
    return XR_SUCCESS;
}

} // namespace xs
