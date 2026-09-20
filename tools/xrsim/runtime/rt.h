// xrsim - OpenXR simulator runtime for automated testing of OpenXR apps without a headset (development tool).
// Shared declarations of the runtime DLL. Own code; the OpenXR headers are Khronos (Apache-2.0 OR MIT).
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <unknwn.h> // IUnknown, used by openxr_platform.h's Win32 section

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#ifndef XR_NO_PROTOTYPES
#define XR_NO_PROTOTYPES
#endif
#ifndef XR_USE_PLATFORM_WIN32
#define XR_USE_PLATFORM_WIN32
#endif
#ifndef XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_GRAPHICS_API_VULKAN
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace xs {

// ------------------------------------------------------------------------------------------------ math
struct Vec3 { double x = 0, y = 0, z = 0; };
struct Quat { double x = 0, y = 0, z = 0, w = 1; };
struct Pose { Quat q; Vec3 p; };

Quat Mul(const Quat& a, const Quat& b);
Vec3 Rotate(const Quat& q, const Vec3& v);
Pose Mul(const Pose& a, const Pose& b); // a * b: b expressed in a's frame -> parent frame
Pose Inverse(const Pose& a);
Quat Normalize(const Quat& q);
Quat Slerp(Quat a, const Quat& b, double t);
// Degrees. yaw about +Y (positive turns left), pitch about +X (positive looks up), roll about +Z; q = yaw * pitch * roll.
Quat FromYawPitchRoll(double yawDeg, double pitchDeg, double rollDeg);
XrPosef ToXr(const Pose& p);
Pose FromXr(const XrPosef& p);
bool IsUnitQuat(const XrQuaternionf& q);

// ------------------------------------------------------------------------------------------------ time
XrTime NowXr();                           // monotonic nanoseconds (QueryPerformanceCounter based)
int64_t QpcFrequency();
XrTime QpcToXr(int64_t qpc);
int64_t XrToQpc(XrTime t);
void SleepUntil(XrTime target);           // high-resolution waitable timer + short spin

// ------------------------------------------------------------------------------------------------ logging
// Text log: <XRSIM_OUT>/xrsim_runtime.log (when XRSIM_OUT is set), stderr when XRSIM_VERBOSE=1, OutputDebugString.
void Log(const char* fmt, ...);
void OpenLogFile(const std::string& path);
void CloseLogFile();
bool Verbose();
const char* ResultName(XrResult r);
XrResult FailImpl(XrResult r, const char* func, const char* fmt, ...);
#define XS_FAIL(r, ...) ::xs::FailImpl((r), __func__, __VA_ARGS__)

// ------------------------------------------------------------------------------------------------ simulated input
enum Comp : int {
    C_TRIGGER, C_TRIGGER_TOUCH, C_SQUEEZE, C_STICK_X, C_STICK_Y, C_STICK_CLICK, C_STICK_TOUCH, C_THUMBREST_TOUCH,
    C_X, C_X_TOUCH, C_Y, C_Y_TOUCH, C_A, C_A_TOUCH, C_B, C_B_TOUCH, C_MENU, C_SYSTEM, C_COUNT
};
const char* CompScriptName(int c);

// The simulated device state of one frame (evaluated from the script at xrWaitFrame).
struct Snapshot {
    int64_t frame = -1;
    XrTime time = 0;              // predictedDisplayTime of that frame
    Pose head;                    // in STAGE space
    Pose hand[2];                 // grip pose of left (0) / right (1) controller in STAGE space
    bool handTracked[2] = {true, true};
    float comp[2][C_COUNT] = {};
};

// ------------------------------------------------------------------------------------------------ configuration
struct Config {
    double refreshHz = 72.0;
    std::vector<float> refreshRates = {72.0f, 80.0f, 90.0f, 120.0f};
    uint32_t width = 1680, height = 1760;  // recommended per-eye image size
    uint32_t maxSamples = 4;
    double ipd = 0.063;
    // left-eye field of view in degrees (right eye mirrored): left, right, up, down (angles, left/down negative)
    double fov[4] = {-50.0, 43.0, 45.0, -52.0};
    double localHeight = 1.6;              // LOCAL origin height above the STAGE floor
    double stageW = 2.0, stageD = 2.0;     // STAGE bounds (meters)
    Pose defaultHead, defaultHand[2];
    std::string scriptPath, outDir;
    bool window = false;
    int gpuIndex = -1;
    Config();
};

// ------------------------------------------------------------------------------------------------ script
struct ScriptKey {
    bool isTime = false;
    int64_t frame = 0;
    double t = 0;
    bool lerp = false;
    std::vector<double> v;
    int line = 0;
    bool reached = false;
    int64_t reachedFrame = 0;
    double reachedT = 0;
};
struct ScriptShot {
    enum Kind { Event, Capture, Refresh } kind = Event;
    bool isTime = false;
    int64_t frame = 0;
    double t = 0;
    std::string arg;
    double value = 0;
    bool fired = false;
    int line = 0;
};
class Script {
public:
    bool Load(const std::string& path, Config& cfg, std::string& err);
    // Evaluates the channels at (frame, seconds since frame 0) into snap; appends the one-shot commands reached now
    // to *fired (not fired / not consumed when fired == nullptr).
    void Evaluate(int64_t frame, double t, Snapshot& snap, std::vector<ScriptShot>* fired);
    bool loaded = false;

private:
    struct Channel { std::vector<ScriptKey> keys; };
    std::map<std::string, Channel> channels_;
    std::vector<ScriptShot> shots_;
    bool ParseLine(const std::string& line, int lineNo, Config& cfg, std::string& err);
};

// ------------------------------------------------------------------------------------------------ outputs
// Background PNG writer + per-frame CSV.
class Output {
public:
    ~Output();
    bool Open(const std::string& dir);
    bool enabled() const { return !dir_.empty(); }
    const std::string& dir() const { return dir_; }
    void Csv(const std::string& row);
    // raw: w * h texels of fmt (tightly packed); converted to display RGBA8 and written as PNG on the worker thread
    void QueuePng(std::string path, int w, int h, std::vector<uint8_t> raw, const struct FmtInfo* fmt, bool opaque);
    void Flush();      // waits until all queued PNGs are written
    void Close();
    int pngWritten() const { return written_; }

private:
    struct Job { std::string path; int w, h; std::vector<uint8_t> raw; const struct FmtInfo* fmt; bool opaque; };
    std::string dir_;
    FILE* csv_ = nullptr;
    std::thread worker_;
    std::mutex m_;
    std::condition_variable cv_, idle_;
    std::deque<Job> jobs_;
    bool busy_ = false, stop_ = false;
    int written_ = 0;
    void Run();
};

// Desktop mirror window (both eyes side by side), own thread.
class Mirror {
public:
    ~Mirror();
    void Start(int eyeW, int eyeH);
    void Stop();
    // rgba: (2*eyeW) x eyeH, RGBA8 sRGB-encoded
    void Present(const uint8_t* rgba, int w, int h);
    bool closeRequested() const { return closeRequested_; }
    int eyeW = 0, eyeH = 0;

private:
    std::thread thread_;
    std::mutex m_;
    std::vector<uint8_t> bgra_;
    int w_ = 0, h_ = 0;
    HWND hwnd_ = nullptr;
    volatile bool closeRequested_ = false, stop_ = false;
    void Run();
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
};

// ------------------------------------------------------------------------------------------------ objects
enum class Kind { Instance = 1, Session, Space, Swapchain, ActionSet, Action };
struct Object {
    Kind kind;
    uint64_t handle = 0;
    explicit Object(Kind k) : kind(k) {}
    virtual ~Object() = default;
};

struct Session;
struct ActionSet;
struct Action;

enum SrcKind { SK_BOOL, SK_FLOAT, SK_VEC2, SK_POSE, SK_HAPTIC };
struct Binding {        // one suggested binding, resolved against the profile's component list
    Action* action;
    XrPath path;        // resolved input / output source path (identifier included)
    int hand;           // 0 left, 1 right
    SrcKind kind;
    int comp;           // Comp index, virtual component, or pose kind
};

struct Instance : Object {
    Instance() : Object(Kind::Instance) {}
    XrVersion apiVersion = 0;
    std::set<std::string> exts;
    std::string appName, engineName;
    Config cfg;
    Script script;
    Output out;
    bool reqCalled = false;              // xrGetVulkanGraphicsRequirements(2)KHR called
    bool csvHeaderWritten = false;
    PFN_vkGetInstanceProcAddr appGipa = nullptr;
    VkInstance lastVkInstance = VK_NULL_HANDLE;
    VkPhysicalDevice chosenPhys = VK_NULL_HANDLE;
    std::deque<XrEventDataBuffer> events;
    Session* session = nullptr;
    std::vector<std::string> pathStr;    // path id - 1 -> string
    std::unordered_map<std::string, XrPath> pathIds;
    std::vector<ActionSet*> actionSets;
    std::map<XrPath, std::vector<Binding>> suggested;   // interaction profile path -> bindings
    bool Has(const char* ext) const { return exts.count(ext) != 0; }
    bool Is11() const { return XR_VERSION_MINOR(apiVersion) >= 1; }
    XrPath GetPath(const std::string& s); // interns
    std::string PathString(XrPath p) const;
    void PushEvent(const void* ev, size_t size);
};

struct VkFns {
#define XS_VK_INSTANCE_FNS(X) \
    X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceFormatProperties) X(vkGetPhysicalDeviceImageFormatProperties) X(vkGetDeviceProcAddr)
#define XS_VK_DEVICE_FNS(X) \
    X(vkGetDeviceQueue) X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory) \
    X(vkFreeMemory) X(vkBindImageMemory) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) \
    X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkInvalidateMappedMemoryRanges) X(vkCreateCommandPool) \
    X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkFreeCommandBuffers) X(vkBeginCommandBuffer) \
    X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdCopyImageToBuffer) X(vkCmdBlitImage) \
    X(vkQueueSubmit) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) X(vkGetFenceStatus) \
    X(vkDeviceWaitIdle)
#define XS_DECL(n) PFN_##n n = nullptr;
    XS_VK_INSTANCE_FNS(XS_DECL)
    XS_VK_DEVICE_FNS(XS_DECL)
#undef XS_DECL
};

struct FmtInfo {
    VkFormat format;
    enum Pix { RGBA8_SRGB, BGRA8_SRGB, RGBA8_UNORM, BGRA8_UNORM, RGBA16F, A2B10G10R10, Depth } pix;
    uint32_t bpp;
    VkImageAspectFlags aspect;
};
const FmtInfo* FindFmt(VkFormat f);
// Display-referred RGBA8 of w x h texels: *_SRGB copied, linear formats encoded with the sRGB transfer function.
std::vector<uint8_t> ToDisplayRgba(const FmtInfo* fmt, const uint8_t* src, uint32_t w, uint32_t h, bool opaque);

struct MirrorSlot {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory imageMem = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory bufferMem = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool pending = false;
};

struct Swapchain;
struct Space;

struct FrameRec {
    int64_t frame = 0;
    XrTime waitCall = 0, waitReturn = 0, display = 0, period = 0, begin = 0, end = 0;
    bool shouldRender = false;
    int64_t skipped = 0;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    std::string result, layers;
    Pose head;
    bool haveViews = false;
    Pose views[2];
    bool captured = false;
};

struct Session : Object {
    Session() : Object(Kind::Session) {}
    Instance* inst = nullptr;
    // Vulkan binding
    VkInstance vkInstance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queueFamily = 0, queueIndex = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkFns vk;
    VkPhysicalDeviceMemoryProperties memProps{};
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    std::vector<VkFormat> formats;
    // state machine
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool running = false, synced = false, userVisible = true, userFocus = true, exitPending = false;
    XrViewConfigurationType viewConfig = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    // frame loop
    int64_t nextFrame = 0;                   // index of the next xrWaitFrame
    std::deque<int64_t> waitedNotBegun;
    int64_t begunFrame = -1;
    DWORD lastWaitThread = 0;
    std::condition_variable_any frameCv;
    XrTime nextDisplay = 0, firstDisplay = 0;
    int64_t periodNs = 0;
    float refreshHz = 72.0f;
    std::map<int64_t, FrameRec> frames;
    std::deque<Snapshot> history;           // most recent last
    Snapshot cur;
    std::set<int64_t> captureFrames;
    int64_t haptics = 0;
    std::string perfLevels = "";
    // children
    std::vector<Space*> spaces;
    std::vector<Swapchain*> swapchains;
    // input
    bool attached = false;
    XrPath profile = XR_NULL_PATH;
    std::vector<ActionSet*> attachedSets;
    // mirror
    std::unique_ptr<Mirror> mirror;
    MirrorSlot mirrorSlots[2];
    int mirrorNext = 0;
    bool mirrorOk = false, mirrorTried = false;

    void SetState(XrSessionState s);
    void UpdateState();                      // walks toward the desired state, one event per step
    const Snapshot& SnapshotAt(XrTime t) const;
};

struct Swapchain : Object {
    Swapchain() : Object(Kind::Swapchain) {}
    Session* session = nullptr;
    XrSwapchainCreateInfo ci{};
    const FmtInfo* fmt = nullptr;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    std::vector<VkImage> images;
    std::vector<VkDeviceMemory> mems;
    std::deque<uint32_t> acquired;
    bool frontWaited = false;
    uint32_t nextIndex = 0;
    int64_t lastReleased = -1;
    bool everAcquired = false;
};

struct Space : Object {
    Space() : Object(Kind::Space) {}
    Session* session = nullptr;
    bool isAction = false;
    XrReferenceSpaceType refType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    Action* action = nullptr;
    XrPath subPath = XR_NULL_PATH;
    Pose offset;
};

struct ActionSet : Object {
    ActionSet() : Object(Kind::ActionSet) {}
    Instance* inst = nullptr;
    std::string name, loc;
    uint32_t priority = 0;
    bool attached = false;
    std::vector<Action*> actions;
};

struct ActionValue {
    bool b = false;
    float f = 0, x = 0, y = 0;
    bool active = false, changed = false;
    XrTime lastChange = 0;
};

struct Action : Object {
    Action() : Object(Kind::Action) {}
    ActionSet* set = nullptr;
    std::string name, loc;
    XrActionType type = XR_ACTION_TYPE_BOOLEAN_INPUT;
    std::vector<XrPath> subPaths;
    std::vector<ActionValue> vals;         // [0] = no subaction path, [i + 1] = subPaths[i]
    int SlotOf(XrPath p) const;            // -1 when the path was not declared
};

// ------------------------------------------------------------------------------------------------ registry
extern std::recursive_mutex g_lock;
uint64_t Register(std::unique_ptr<Object> obj);
Object* LookupAny(uint64_t h);
void Unregister(uint64_t h);
template <class T, Kind K> T* Lookup(uint64_t h) {
    Object* o = LookupAny(h);
    return (o && o->kind == K) ? static_cast<T*>(o) : nullptr;
}
inline Instance* GetInstance(XrInstance h) { return Lookup<Instance, Kind::Instance>((uint64_t)h); }
inline Session* GetSession(XrSession h) { return Lookup<Session, Kind::Session>((uint64_t)h); }
inline Space* GetSpace(XrSpace h) { return Lookup<Space, Kind::Space>((uint64_t)h); }
inline Swapchain* GetSwapchain(XrSwapchain h) { return Lookup<Swapchain, Kind::Swapchain>((uint64_t)h); }
inline ActionSet* GetActionSet(XrActionSet h) { return Lookup<ActionSet, Kind::ActionSet>((uint64_t)h); }
inline Action* GetAction(XrAction h) { return Lookup<Action, Kind::Action>((uint64_t)h); }

constexpr XrSystemId kSystemId = 1;

// Two-call idiom helper.
template <class T>
XrResult TwoCall(uint32_t capacity, uint32_t* countOut, T* out, const std::vector<T>& items) {
    if (!countOut) return XR_ERROR_VALIDATION_FAILURE;
    *countOut = (uint32_t)items.size();
    if (capacity == 0) return XR_SUCCESS;
    if (capacity < items.size()) return XR_ERROR_SIZE_INSUFFICIENT;
    if (!out) return XR_ERROR_VALIDATION_FAILURE;
    for (size_t i = 0; i < items.size(); ++i) out[i] = items[i];
    return XR_SUCCESS;
}
XrResult TwoCallString(uint32_t capacity, uint32_t* countOut, char* out, const std::string& s);

// ------------------------------------------------------------------------------------------------ cross-module
void DestroySessionObject(Session* s);        // rt_session.cpp
void DestroySwapchainObject(Swapchain* sc);   // rt_vulkan.cpp
void DestroyActionSetObject(ActionSet* as);   // rt_input.cpp
bool VkSessionInit(Session* s, const XrGraphicsBindingVulkanKHR* b, std::string& err);  // rt_vulkan.cpp
void VkSessionShutdown(Session* s);
struct LayerView {                            // one captured / mirrored image region
    Swapchain* sc;
    uint32_t image, arrayIndex;
    XrRect2Di rect;
    std::string name;                         // capture file name (without directory)
    bool forceOpaque;
};
void CaptureViews(Session* s, const std::vector<LayerView>& views);   // synchronous readback + PNG jobs
void MirrorViews(Session* s, const LayerView* left, const LayerView* right);
void InputOnAttach(Session* s);                                        // rt_input.cpp
bool InputPoseForAction(Session* s, Action* a, XrPath subPath, XrTime t, Pose& out, bool& active); // rt_input.cpp

// Function table entry points (defined in the module that implements them).
PFN_xrVoidFunction FindFunction(Instance* inst, const char* name, XrResult& res);

} // namespace xs
