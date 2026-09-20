// xrsim - loader negotiation, function table, instance / system / path / event entry points, math, time, logging.
#include "rt_api.h"

#include <openxr/openxr_reflection.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstring>

namespace xs {

// ================================================================================================ math
Quat Mul(const Quat& a, const Quat& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
Vec3 Rotate(const Quat& q, const Vec3& v) {
    const Quat p{v.x, v.y, v.z, 0};
    const Quat c{-q.x, -q.y, -q.z, q.w};
    const Quat r = Mul(Mul(q, p), c);
    return {r.x, r.y, r.z};
}
Pose Mul(const Pose& a, const Pose& b) {
    Pose r;
    r.q = Normalize(Mul(a.q, b.q));
    const Vec3 t = Rotate(a.q, b.p);
    r.p = {a.p.x + t.x, a.p.y + t.y, a.p.z + t.z};
    return r;
}
Pose Inverse(const Pose& a) {
    Pose r;
    r.q = {-a.q.x, -a.q.y, -a.q.z, a.q.w};
    const Vec3 t = Rotate(r.q, a.p);
    r.p = {-t.x, -t.y, -t.z};
    return r;
}
Quat Normalize(const Quat& q) {
    const double n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (n <= 0) return {};
    return {q.x / n, q.y / n, q.z / n, q.w / n};
}
Quat Slerp(Quat a, const Quat& b, double t) {
    double d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0) { a = {-a.x, -a.y, -a.z, -a.w}; d = -d; }
    double wa, wb;
    if (d > 0.9995) {
        wa = 1 - t;
        wb = t;
    } else {
        const double th = std::acos(d), s = std::sin(th);
        wa = std::sin((1 - t) * th) / s;
        wb = std::sin(t * th) / s;
    }
    return Normalize({wa * a.x + wb * b.x, wa * a.y + wb * b.y, wa * a.z + wb * b.z, wa * a.w + wb * b.w});
}
Quat FromYawPitchRoll(double yawDeg, double pitchDeg, double rollDeg) {
    const double k = 3.14159265358979323846 / 360.0; // half angle in radians
    const Quat qy{0, std::sin(yawDeg * k), 0, std::cos(yawDeg * k)};
    const Quat qp{std::sin(pitchDeg * k), 0, 0, std::cos(pitchDeg * k)};
    const Quat qr{0, 0, std::sin(rollDeg * k), std::cos(rollDeg * k)};
    return Normalize(Mul(Mul(qy, qp), qr));
}
XrPosef ToXr(const Pose& p) {
    XrPosef r;
    r.orientation = {(float)p.q.x, (float)p.q.y, (float)p.q.z, (float)p.q.w};
    r.position = {(float)p.p.x, (float)p.p.y, (float)p.p.z};
    return r;
}
Pose FromXr(const XrPosef& p) {
    Pose r;
    r.q = Normalize({p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w});
    r.p = {p.position.x, p.position.y, p.position.z};
    return r;
}
bool IsUnitQuat(const XrQuaternionf& q) {
    const double n = (double)q.x * q.x + (double)q.y * q.y + (double)q.z * q.z + (double)q.w * q.w;
    return std::fabs(n - 1.0) < 0.01; // same tolerance as the conformance tests' "normalized" check
}

// ================================================================================================ time
int64_t QpcFrequency() {
    static const int64_t f = [] { LARGE_INTEGER li; QueryPerformanceFrequency(&li); return (int64_t)li.QuadPart; }();
    return f;
}
XrTime QpcToXr(int64_t qpc) {
    const int64_t f = QpcFrequency();
    return (qpc / f) * 1000000000LL + ((qpc % f) * 1000000000LL) / f;
}
int64_t XrToQpc(XrTime t) {
    const int64_t f = QpcFrequency();
    return (t / 1000000000LL) * f + ((t % 1000000000LL) * f) / 1000000000LL;
}
XrTime NowXr() {
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return QpcToXr(li.QuadPart);
}
void SleepUntil(XrTime target) {
    static thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    for (;;) {
        const XrTime now = NowXr();
        const XrTime left = target - now;
        if (left <= 0) return;
        if (left > 1500000 && timer) {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)((left - 1000000) / 100); // relative, 100 ns units
            if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) {
                WaitForSingleObject(timer, INFINITE);
                continue;
            }
        }
        if (left > 200000) SwitchToThread(); else YieldProcessor();
    }
}

// ================================================================================================ logging
namespace {
std::mutex g_logMutex;
FILE* g_logFile = nullptr;
}
bool Verbose() {
    static const bool v = [] {
        char buf[8] = {};
        return GetEnvironmentVariableA("XRSIM_VERBOSE", buf, sizeof(buf)) > 0 && buf[0] == '1';
    }();
    return v;
}
void OpenLogFile(const std::string& path) {
    std::lock_guard<std::mutex> lk(g_logMutex);
    if (g_logFile) fclose(g_logFile);
    g_logFile = fopen(path.c_str(), "w");
}
void CloseLogFile() {
    std::lock_guard<std::mutex> lk(g_logMutex);
    if (g_logFile) fclose(g_logFile);
    g_logFile = nullptr;
}
void Log(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lk(g_logMutex);
    const double t = (double)NowXr() / 1e9;
    if (g_logFile) {
        fprintf(g_logFile, "[%.6f] %s\n", t, buf);
        fflush(g_logFile);
    }
    if (Verbose()) fprintf(stderr, "[xrsim] %s\n", buf);
    std::string d = std::string("[xrsim] ") + buf + "\n";
    OutputDebugStringA(d.c_str());
}
const char* ResultName(XrResult r) {
    switch (r) {
#define XS_RES(name, val) case name: return #name;
        XR_LIST_ENUM_XrResult(XS_RES)
#undef XS_RES
    default: return nullptr;
    }
}
XrResult FailImpl(XrResult r, const char* func, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    const char* n = ResultName(r);
    Log("%s -> %s: %s", func, n ? n : "?", buf);
    return r;
}
XrResult TwoCallString(uint32_t capacity, uint32_t* countOut, char* out, const std::string& s) {
    if (!countOut) return XR_ERROR_VALIDATION_FAILURE;
    *countOut = (uint32_t)s.size() + 1;
    if (capacity == 0) return XR_SUCCESS;
    if (capacity < s.size() + 1) return XR_ERROR_SIZE_INSUFFICIENT;
    if (!out) return XR_ERROR_VALIDATION_FAILURE;
    memcpy(out, s.c_str(), s.size() + 1);
    return XR_SUCCESS;
}

// ================================================================================================ registry
std::recursive_mutex g_lock;
namespace {
std::unordered_map<uint64_t, std::unique_ptr<Object>> g_objects;
uint64_t g_nextHandle = 0x1000;
Instance* g_instance = nullptr;
}
uint64_t Register(std::unique_ptr<Object> obj) {
    const uint64_t h = g_nextHandle++;
    obj->handle = h;
    g_objects[h] = std::move(obj);
    return h;
}
Object* LookupAny(uint64_t h) {
    auto it = g_objects.find(h);
    return it == g_objects.end() ? nullptr : it->second.get();
}
void Unregister(uint64_t h) { g_objects.erase(h); }

Config::Config() {
    defaultHead.p = {0, 1.6, 0};
    defaultHand[0].p = {-0.2, 1.3, -0.35};
    defaultHand[1].p = {0.2, 1.3, -0.35};
}

XrPath Instance::GetPath(const std::string& s) {
    auto it = pathIds.find(s);
    if (it != pathIds.end()) return it->second;
    pathStr.push_back(s);
    const XrPath p = (XrPath)pathStr.size();
    pathIds[s] = p;
    return p;
}
std::string Instance::PathString(XrPath p) const {
    if (p == XR_NULL_PATH || p > pathStr.size()) return {};
    return pathStr[(size_t)p - 1];
}
void Instance::PushEvent(const void* ev, size_t size) {
    XrEventDataBuffer b{};
    memcpy(&b, ev, std::min(size, sizeof(b)));
    events.push_back(b);
}

// ================================================================================================ extensions
namespace {
struct ExtDef { const char* name; uint32_t version; };
const ExtDef kExtensions[] = {
    {XR_KHR_VULKAN_ENABLE_EXTENSION_NAME, XR_KHR_vulkan_enable_SPEC_VERSION},
    {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME, XR_KHR_vulkan_enable2_SPEC_VERSION},
    {XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME, XR_KHR_composition_layer_depth_SPEC_VERSION},
    {XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME, XR_KHR_win32_convert_performance_counter_time_SPEC_VERSION},
    {XR_KHR_LOCATE_SPACES_EXTENSION_NAME, XR_KHR_locate_spaces_SPEC_VERSION},
    {XR_EXT_LOCAL_FLOOR_EXTENSION_NAME, XR_EXT_local_floor_SPEC_VERSION},
    {XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME, XR_EXT_performance_settings_SPEC_VERSION},
    {XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME, XR_FB_display_refresh_rate_SPEC_VERSION},
};

struct FnDef { const char* name; PFN_xrVoidFunction fn; const char* ext; }; // ext: nullptr = core 1.0, "1.1" = core 1.1
#define XS_CORE(n) {#n, (PFN_xrVoidFunction)&n, nullptr}
#define XS_CORE11(n) {#n, (PFN_xrVoidFunction)&n, "1.1"}
#define XS_EXT(n, e) {#n, (PFN_xrVoidFunction)&n, e}
const FnDef kFunctions[] = {
    XS_CORE(xrGetInstanceProcAddr), XS_CORE(xrEnumerateApiLayerProperties), XS_CORE(xrEnumerateInstanceExtensionProperties),
    XS_CORE(xrCreateInstance), XS_CORE(xrDestroyInstance), XS_CORE(xrGetInstanceProperties), XS_CORE(xrPollEvent),
    XS_CORE(xrResultToString), XS_CORE(xrStructureTypeToString), XS_CORE(xrGetSystem), XS_CORE(xrGetSystemProperties),
    XS_CORE(xrEnumerateEnvironmentBlendModes), XS_CORE(xrCreateSession), XS_CORE(xrDestroySession),
    XS_CORE(xrEnumerateReferenceSpaces), XS_CORE(xrCreateReferenceSpace), XS_CORE(xrGetReferenceSpaceBoundsRect),
    XS_CORE(xrCreateActionSpace), XS_CORE(xrLocateSpace), XS_CORE(xrDestroySpace), XS_CORE(xrEnumerateViewConfigurations),
    XS_CORE(xrGetViewConfigurationProperties), XS_CORE(xrEnumerateViewConfigurationViews),
    XS_CORE(xrEnumerateSwapchainFormats), XS_CORE(xrCreateSwapchain), XS_CORE(xrDestroySwapchain),
    XS_CORE(xrEnumerateSwapchainImages), XS_CORE(xrAcquireSwapchainImage), XS_CORE(xrWaitSwapchainImage),
    XS_CORE(xrReleaseSwapchainImage), XS_CORE(xrBeginSession), XS_CORE(xrEndSession), XS_CORE(xrRequestExitSession),
    XS_CORE(xrWaitFrame), XS_CORE(xrBeginFrame), XS_CORE(xrEndFrame), XS_CORE(xrLocateViews), XS_CORE(xrStringToPath),
    XS_CORE(xrPathToString), XS_CORE(xrCreateActionSet), XS_CORE(xrDestroyActionSet), XS_CORE(xrCreateAction),
    XS_CORE(xrDestroyAction), XS_CORE(xrSuggestInteractionProfileBindings), XS_CORE(xrAttachSessionActionSets),
    XS_CORE(xrGetCurrentInteractionProfile), XS_CORE(xrGetActionStateBoolean), XS_CORE(xrGetActionStateFloat),
    XS_CORE(xrGetActionStateVector2f), XS_CORE(xrGetActionStatePose), XS_CORE(xrSyncActions),
    XS_CORE(xrEnumerateBoundSourcesForAction), XS_CORE(xrGetInputSourceLocalizedName), XS_CORE(xrApplyHapticFeedback),
    XS_CORE(xrStopHapticFeedback),
    XS_CORE11(xrLocateSpaces),
    {"xrLocateSpacesKHR", (PFN_xrVoidFunction)&xrLocateSpaces, XR_KHR_LOCATE_SPACES_EXTENSION_NAME},
    XS_EXT(xrGetVulkanInstanceExtensionsKHR, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME),
    XS_EXT(xrGetVulkanDeviceExtensionsKHR, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME),
    XS_EXT(xrGetVulkanGraphicsDeviceKHR, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME),
    XS_EXT(xrGetVulkanGraphicsRequirementsKHR, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME),
    XS_EXT(xrCreateVulkanInstanceKHR, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME),
    XS_EXT(xrCreateVulkanDeviceKHR, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME),
    XS_EXT(xrGetVulkanGraphicsDevice2KHR, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME),
    XS_EXT(xrGetVulkanGraphicsRequirements2KHR, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME),
    XS_EXT(xrConvertWin32PerformanceCounterToTimeKHR, XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME),
    XS_EXT(xrConvertTimeToWin32PerformanceCounterKHR, XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME),
    XS_EXT(xrEnumerateDisplayRefreshRatesFB, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME),
    XS_EXT(xrGetDisplayRefreshRateFB, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME),
    XS_EXT(xrRequestDisplayRefreshRateFB, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME),
    XS_EXT(xrPerfSettingsSetPerformanceLevelEXT, XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME),
};
#undef XS_CORE
#undef XS_CORE11
#undef XS_EXT

std::string GetEnv(const char* name) {
    char buf[4096];
    const DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    return (n > 0 && n < sizeof(buf)) ? std::string(buf, n) : std::string();
}
} // namespace

PFN_xrVoidFunction FindFunction(Instance* inst, const char* name, XrResult& res) {
    for (const FnDef& f : kFunctions) {
        if (strcmp(f.name, name) != 0) continue;
        if (!inst) {
            if (strcmp(name, "xrEnumerateInstanceExtensionProperties") == 0 || strcmp(name, "xrEnumerateApiLayerProperties") == 0 ||
                strcmp(name, "xrCreateInstance") == 0) {
                res = XR_SUCCESS;
                return f.fn;
            }
            res = XR_ERROR_HANDLE_INVALID;
            return nullptr;
        }
        if (f.ext && strcmp(f.ext, "1.1") == 0) {
            if (!inst->Is11()) { res = XR_ERROR_FUNCTION_UNSUPPORTED; return nullptr; }
        } else if (f.ext && !inst->Has(f.ext)) {
            res = XR_ERROR_FUNCTION_UNSUPPORTED;
            return nullptr;
        }
        res = XR_SUCCESS;
        return f.fn;
    }
    res = XR_ERROR_FUNCTION_UNSUPPORTED;
    return nullptr;
}

// ================================================================================================ entry points
XrResult XRAPI_CALL xrGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    if (!name || !function) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "null name / function");
    *function = nullptr;
    Instance* inst = nullptr;
    if (instance != XR_NULL_HANDLE) {
        inst = GetInstance(instance);
        if (!inst) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "bad instance for %s", name);
    }
    XrResult r = XR_SUCCESS;
    *function = FindFunction(inst, name, r);
    if (r != XR_SUCCESS && Verbose()) Log("xrGetInstanceProcAddr(%s) -> %s", name, ResultName(r));
    return r;
}

XrResult XRAPI_CALL xrEnumerateApiLayerProperties(uint32_t cap, uint32_t* count, XrApiLayerProperties* props) {
    return TwoCall<XrApiLayerProperties>(cap, count, props, {});
}

XrResult XRAPI_CALL xrEnumerateInstanceExtensionProperties(const char* layerName, uint32_t cap, uint32_t* count,
                                                           XrExtensionProperties* props) {
    if (layerName && layerName[0]) return XS_FAIL(XR_ERROR_API_LAYER_NOT_PRESENT, "layer %s", layerName);
    if (!count) return XR_ERROR_VALIDATION_FAILURE;
    const uint32_t n = (uint32_t)(sizeof(kExtensions) / sizeof(kExtensions[0]));
    *count = n;
    if (cap == 0) return XR_SUCCESS;
    if (cap < n) return XR_ERROR_SIZE_INSUFFICIENT;
    if (!props) return XR_ERROR_VALIDATION_FAILURE;
    for (uint32_t i = 0; i < n; ++i) {
        if (props[i].type != XR_TYPE_EXTENSION_PROPERTIES) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "props[%u].type", i);
        strncpy(props[i].extensionName, kExtensions[i].name, XR_MAX_EXTENSION_NAME_SIZE - 1);
        props[i].extensionName[XR_MAX_EXTENSION_NAME_SIZE - 1] = 0;
        props[i].extensionVersion = kExtensions[i].version;
    }
    return XR_SUCCESS;
}

namespace {
bool ParseDouble(const std::string& s, double& v) {
    char* end = nullptr;
    v = strtod(s.c_str(), &end);
    return end && end != s.c_str() && *end == 0;
}
} // namespace

XrResult XRAPI_CALL xrCreateInstance(const XrInstanceCreateInfo* ci, XrInstance* instance) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    if (!ci || !instance) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "null argument");
    if (ci->type != XR_TYPE_INSTANCE_CREATE_INFO) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "createInfo.type");
    if (ci->createFlags != 0) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "createFlags must be 0");
    const XrApplicationInfo& ai = ci->applicationInfo;
    if (strnlen(ai.applicationName, XR_MAX_APPLICATION_NAME_SIZE) == 0) return XS_FAIL(XR_ERROR_NAME_INVALID, "empty applicationName");
    if (strnlen(ai.applicationName, XR_MAX_APPLICATION_NAME_SIZE) >= XR_MAX_APPLICATION_NAME_SIZE ||
        strnlen(ai.engineName, XR_MAX_ENGINE_NAME_SIZE) >= XR_MAX_ENGINE_NAME_SIZE)
        return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "unterminated application / engine name");
    if (XR_VERSION_MAJOR(ai.apiVersion) != 1 || XR_VERSION_MINOR(ai.apiVersion) > 1)
        return XS_FAIL(XR_ERROR_API_VERSION_UNSUPPORTED, "apiVersion %u.%u", (unsigned)XR_VERSION_MAJOR(ai.apiVersion),
                       (unsigned)XR_VERSION_MINOR(ai.apiVersion));
    if (ci->enabledApiLayerCount > 0) return XS_FAIL(XR_ERROR_API_LAYER_NOT_PRESENT, "the runtime has no API layers");
    for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i) {
        const char* e = ci->enabledExtensionNames ? ci->enabledExtensionNames[i] : nullptr;
        bool ok = false;
        for (const ExtDef& d : kExtensions) ok = ok || (e && strcmp(d.name, e) == 0);
        if (!ok) return XS_FAIL(XR_ERROR_EXTENSION_NOT_PRESENT, "extension %s", e ? e : "(null)");
    }
    if (g_instance) return XS_FAIL(XR_ERROR_LIMIT_REACHED, "xrsim supports one XrInstance at a time");

    auto inst = std::make_unique<Instance>();
    inst->apiVersion = ai.apiVersion;
    inst->appName = ai.applicationName;
    inst->engineName = ai.engineName;
    for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i) inst->exts.insert(ci->enabledExtensionNames[i]);

    Config& cfg = inst->cfg;
    cfg.scriptPath = GetEnv("XRSIM_SCRIPT");
    cfg.outDir = GetEnv("XRSIM_OUT");
    cfg.window = GetEnv("XRSIM_WINDOW") == "1";
    if (!cfg.outDir.empty()) {
        if (!inst->out.Open(cfg.outDir)) return XS_FAIL(XR_ERROR_RUNTIME_FAILURE, "cannot create XRSIM_OUT %s", cfg.outDir.c_str());
        OpenLogFile(cfg.outDir + "\\xrsim_runtime.log");
    }
    if (!cfg.scriptPath.empty()) {
        std::string err;
        if (!inst->script.Load(cfg.scriptPath, cfg, err)) {
            Log("script error: %s", err.c_str());
            inst->out.Close();
            CloseLogFile();
            return XS_FAIL(XR_ERROR_RUNTIME_FAILURE, "XRSIM_SCRIPT %s: %s", cfg.scriptPath.c_str(), err.c_str());
        }
    }
    // environment overrides the script header
    double d = 0;
    if (ParseDouble(GetEnv("XRSIM_REFRESH"), d)) cfg.refreshHz = d;
    const std::string res = GetEnv("XRSIM_RESOLUTION");
    unsigned rw = 0, rh = 0;
    if (!res.empty() && sscanf(res.c_str(), "%ux%u", &rw, &rh) == 2 && rw > 0 && rh > 0) { cfg.width = rw; cfg.height = rh; }
    if (ParseDouble(GetEnv("XRSIM_GPU"), d)) cfg.gpuIndex = (int)d;
    bool rateOk = false;
    for (float r : cfg.refreshRates) rateOk = rateOk || std::fabs(r - cfg.refreshHz) < 0.01;
    if (!rateOk) cfg.refreshRates.push_back((float)cfg.refreshHz);
    std::sort(cfg.refreshRates.begin(), cfg.refreshRates.end());

    std::string extList;
    for (const auto& e : inst->exts) extList += " " + e;
    Log("xrCreateInstance: app '%s' engine '%s' api %u.%u.%u, extensions:%s", inst->appName.c_str(), inst->engineName.c_str(),
        (unsigned)XR_VERSION_MAJOR(ai.apiVersion), (unsigned)XR_VERSION_MINOR(ai.apiVersion), (unsigned)XR_VERSION_PATCH(ai.apiVersion),
        extList.c_str());
    Log("config: %.2f Hz, %ux%u per eye, ipd %.4f, script '%s', out '%s', window %d", cfg.refreshHz, cfg.width, cfg.height, cfg.ipd,
        cfg.scriptPath.c_str(), cfg.outDir.c_str(), cfg.window ? 1 : 0);
    g_instance = inst.get();
    *instance = (XrInstance)Register(std::move(inst));
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrDestroyInstance(XrInstance instance) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Instance* inst = GetInstance(instance);
    if (!inst) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "instance");
    if (inst->session) DestroySessionObject(inst->session);
    while (!inst->actionSets.empty()) DestroyActionSetObject(inst->actionSets.back());
    inst->out.Close();
    Log("xrDestroyInstance: done");
    CloseLogFile();
    g_instance = nullptr;
    Unregister(inst->handle);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrGetInstanceProperties(XrInstance instance, XrInstanceProperties* props) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    if (!GetInstance(instance)) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "instance");
    if (!props || props->type != XR_TYPE_INSTANCE_PROPERTIES) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "props");
    props->runtimeVersion = XR_MAKE_VERSION(0, 1, 0);
    strncpy(props->runtimeName, "xrsim", XR_MAX_RUNTIME_NAME_SIZE);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrPollEvent(XrInstance instance, XrEventDataBuffer* ev) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Instance* inst = GetInstance(instance);
    if (!inst) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "instance");
    if (!ev || ev->type != XR_TYPE_EVENT_DATA_BUFFER) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "eventData");
    if (inst->events.empty()) return XR_EVENT_UNAVAILABLE;
    *ev = inst->events.front();
    inst->events.pop_front();
    if (ev->type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
        const auto* sc = reinterpret_cast<const XrEventDataSessionStateChanged*>(ev);
        Log("xrPollEvent: session state %d", (int)sc->state);
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrResultToString(XrInstance instance, XrResult value, char buffer[XR_MAX_RESULT_STRING_SIZE]) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    if (!GetInstance(instance)) return XR_ERROR_HANDLE_INVALID;
    if (!buffer) return XR_ERROR_VALIDATION_FAILURE;
    const char* n = ResultName(value);
    if (n) snprintf(buffer, XR_MAX_RESULT_STRING_SIZE, "%s", n);
    else snprintf(buffer, XR_MAX_RESULT_STRING_SIZE, value < 0 ? "XR_UNKNOWN_FAILURE_%d" : "XR_UNKNOWN_SUCCESS_%d", (int)value);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrStructureTypeToString(XrInstance instance, XrStructureType value, char buffer[XR_MAX_STRUCTURE_NAME_SIZE]) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    if (!GetInstance(instance)) return XR_ERROR_HANDLE_INVALID;
    if (!buffer) return XR_ERROR_VALIDATION_FAILURE;
    const char* n = nullptr;
    switch (value) {
#define XS_ST(name, val) case name: n = #name; break;
        XR_LIST_ENUM_XrStructureType(XS_ST)
#undef XS_ST
    default: break;
    }
    if (n) snprintf(buffer, XR_MAX_STRUCTURE_NAME_SIZE, "%s", n);
    else snprintf(buffer, XR_MAX_STRUCTURE_NAME_SIZE, "XR_UNKNOWN_STRUCTURE_TYPE_%d", (int)value);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrGetSystem(XrInstance instance, const XrSystemGetInfo* info, XrSystemId* systemId) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    if (!GetInstance(instance)) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "instance");
    if (!info || !systemId || info->type != XR_TYPE_SYSTEM_GET_INFO) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "getInfo");
    if (info->formFactor == XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY) {
        *systemId = kSystemId;
        return XR_SUCCESS;
    }
    if (info->formFactor == XR_FORM_FACTOR_HANDHELD_DISPLAY) return XS_FAIL(XR_ERROR_FORM_FACTOR_UNSUPPORTED, "handheld");
    return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "formFactor %d", (int)info->formFactor);
}

XrResult XRAPI_CALL xrGetSystemProperties(XrInstance instance, XrSystemId systemId, XrSystemProperties* props) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    if (!GetInstance(instance)) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "instance");
    if (systemId != kSystemId) return XS_FAIL(XR_ERROR_SYSTEM_INVALID, "systemId");
    if (!props || props->type != XR_TYPE_SYSTEM_PROPERTIES) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "props");
    props->systemId = kSystemId;
    props->vendorId = 0x5853; // "XS"
    strncpy(props->systemName, "xrsim simulated HMD", XR_MAX_SYSTEM_NAME_SIZE);
    props->graphicsProperties.maxSwapchainImageWidth = 8192;
    props->graphicsProperties.maxSwapchainImageHeight = 8192;
    props->graphicsProperties.maxLayerCount = XR_MIN_COMPOSITION_LAYERS_SUPPORTED;
    props->trackingProperties.orientationTracking = XR_TRUE;
    props->trackingProperties.positionTracking = XR_TRUE;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrEnumerateEnvironmentBlendModes(XrInstance instance, XrSystemId systemId, XrViewConfigurationType vct,
                                                     uint32_t cap, uint32_t* count, XrEnvironmentBlendMode* modes) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    if (!GetInstance(instance)) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "instance");
    if (systemId != kSystemId) return XS_FAIL(XR_ERROR_SYSTEM_INVALID, "systemId");
    if (vct != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) return XS_FAIL(XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED, "%d", (int)vct);
    return TwoCall<XrEnvironmentBlendMode>(cap, count, modes, {XR_ENVIRONMENT_BLEND_MODE_OPAQUE});
}

XrResult XRAPI_CALL xrEnumerateViewConfigurations(XrInstance instance, XrSystemId systemId, uint32_t cap, uint32_t* count,
                                                  XrViewConfigurationType* types) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    if (!GetInstance(instance)) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "instance");
    if (systemId != kSystemId) return XS_FAIL(XR_ERROR_SYSTEM_INVALID, "systemId");
    return TwoCall<XrViewConfigurationType>(cap, count, types, {XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO});
}

XrResult XRAPI_CALL xrGetViewConfigurationProperties(XrInstance instance, XrSystemId systemId, XrViewConfigurationType vct,
                                                     XrViewConfigurationProperties* props) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    if (!GetInstance(instance)) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "instance");
    if (systemId != kSystemId) return XS_FAIL(XR_ERROR_SYSTEM_INVALID, "systemId");
    if (!props || props->type != XR_TYPE_VIEW_CONFIGURATION_PROPERTIES) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "props");
    if (vct != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) return XS_FAIL(XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED, "%d", (int)vct);
    props->viewConfigurationType = vct;
    props->fovMutable = XR_TRUE;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrEnumerateViewConfigurationViews(XrInstance instance, XrSystemId systemId, XrViewConfigurationType vct,
                                                      uint32_t cap, uint32_t* count, XrViewConfigurationView* views) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Instance* inst = GetInstance(instance);
    if (!inst) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "instance");
    if (systemId != kSystemId) return XS_FAIL(XR_ERROR_SYSTEM_INVALID, "systemId");
    if (vct != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) return XS_FAIL(XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED, "%d", (int)vct);
    if (!count) return XR_ERROR_VALIDATION_FAILURE;
    *count = 2;
    if (cap == 0) return XR_SUCCESS;
    if (cap < 2) return XR_ERROR_SIZE_INSUFFICIENT;
    if (!views) return XR_ERROR_VALIDATION_FAILURE;
    for (int i = 0; i < 2; ++i) {
        if (views[i].type != XR_TYPE_VIEW_CONFIGURATION_VIEW) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "views[%d].type", i);
        views[i].recommendedImageRectWidth = inst->cfg.width;
        views[i].recommendedImageRectHeight = inst->cfg.height;
        views[i].maxImageRectWidth = std::min<uint32_t>(8192, inst->cfg.width * 2);
        views[i].maxImageRectHeight = std::min<uint32_t>(8192, inst->cfg.height * 2);
        views[i].recommendedSwapchainSampleCount = 1;
        views[i].maxSwapchainSampleCount = inst->cfg.maxSamples;
    }
    return XR_SUCCESS;
}

namespace {
bool ValidPathString(const char* s) {
    const size_t n = strnlen(s, XR_MAX_PATH_LENGTH);
    if (n == 0 || n >= XR_MAX_PATH_LENGTH || s[0] != '/' || s[n - 1] == '/') return false;
    size_t compStart = 1;
    for (size_t i = 1; i <= n; ++i) {
        if (i == n || s[i] == '/') {
            const size_t len = i - compStart;
            if (len == 0) return false;
            bool dots = true;
            for (size_t k = compStart; k < i; ++k) dots = dots && s[k] == '.';
            if (dots) return false; // "." / ".." / "..." components
            compStart = i + 1;
            continue;
        }
        const char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')) return false;
    }
    return true;
}
} // namespace

XrResult XRAPI_CALL xrStringToPath(XrInstance instance, const char* pathString, XrPath* path) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Instance* inst = GetInstance(instance);
    if (!inst) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "instance");
    if (!pathString || !path) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "null argument");
    if (!ValidPathString(pathString)) return XS_FAIL(XR_ERROR_PATH_FORMAT_INVALID, "'%s'", pathString);
    *path = inst->GetPath(pathString);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrPathToString(XrInstance instance, XrPath path, uint32_t cap, uint32_t* count, char* buffer) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Instance* inst = GetInstance(instance);
    if (!inst) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "instance");
    if (path == XR_NULL_PATH || path > inst->pathStr.size()) return XS_FAIL(XR_ERROR_PATH_INVALID, "path %llu", (unsigned long long)path);
    return TwoCallString(cap, count, buffer, inst->PathString(path));
}

XrResult XRAPI_CALL xrConvertWin32PerformanceCounterToTimeKHR(XrInstance instance, const LARGE_INTEGER* pc, XrTime* time) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    if (!GetInstance(instance)) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "instance");
    if (!pc || !time) return XR_ERROR_VALIDATION_FAILURE;
    if (pc->QuadPart <= 0) return XS_FAIL(XR_ERROR_TIME_INVALID, "counter %lld", (long long)pc->QuadPart);
    *time = QpcToXr(pc->QuadPart);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrConvertTimeToWin32PerformanceCounterKHR(XrInstance instance, XrTime time, LARGE_INTEGER* pc) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    if (!GetInstance(instance)) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "instance");
    if (!pc) return XR_ERROR_VALIDATION_FAILURE;
    if (time <= 0) return XS_FAIL(XR_ERROR_TIME_INVALID, "time %lld", (long long)time);
    pc->QuadPart = XrToQpc(time);
    return XR_SUCCESS;
}

} // namespace xs

// ================================================================================================ loader interface
extern "C" __declspec(dllexport) XrResult XRAPI_CALL xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo* loaderInfo,
                                                                                    XrNegotiateRuntimeRequest* runtimeRequest) {
    using namespace xs;
    if (!loaderInfo || !runtimeRequest) return XR_ERROR_INITIALIZATION_FAILED;
    if (loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO || loaderInfo->structVersion != XR_LOADER_INFO_STRUCT_VERSION ||
        loaderInfo->structSize != sizeof(XrNegotiateLoaderInfo))
        return XS_FAIL(XR_ERROR_INITIALIZATION_FAILED, "bad XrNegotiateLoaderInfo");
    if (runtimeRequest->structType != XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST ||
        runtimeRequest->structVersion != XR_RUNTIME_INFO_STRUCT_VERSION || runtimeRequest->structSize != sizeof(XrNegotiateRuntimeRequest))
        return XS_FAIL(XR_ERROR_INITIALIZATION_FAILED, "bad XrNegotiateRuntimeRequest");
    if (loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_RUNTIME_VERSION || loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_RUNTIME_VERSION)
        return XS_FAIL(XR_ERROR_INITIALIZATION_FAILED, "loader interface %u..%u", loaderInfo->minInterfaceVersion, loaderInfo->maxInterfaceVersion);
    if (XR_VERSION_MAJOR(loaderInfo->minApiVersion) > 1 || XR_VERSION_MAJOR(loaderInfo->maxApiVersion) < 1)
        return XS_FAIL(XR_ERROR_INITIALIZATION_FAILED, "loader API range");
    runtimeRequest->runtimeInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    runtimeRequest->runtimeApiVersion = XR_CURRENT_API_VERSION;
    runtimeRequest->getInstanceProcAddr = &xs::xrGetInstanceProcAddr;
    return XR_SUCCESS;
}
