// xrsim_test - self-test and demo client for the xrsim OpenXR runtime (tools/xrsim). Vulkan 1.3, dynamic rendering,
// multiview (one arraySize-2 swapchain) or one swapchain per eye.
//
//   xrsim_test --selftest [--out DIR] [--window] [--no-validation] [--loader PATH]
//       phase A: official loader (XR_RUNTIME_JSON -> xrsim_runtime.json), OpenXR 1.1, XR_KHR_vulkan_enable2,
//                multiview SRGB swapchain + depth layer + head-locked quad, Touch bindings, 72 Hz -> 90 Hz request,
//                script-driven poses / inputs / focus loss / runtime exit request
//       phase B: runtime DLL loaded directly (xrNegotiateLoaderRuntimeInterface), OpenXR 1.0, XR_KHR_vulkan_enable,
//                two UNORM swapchains, simple_controller bindings, 90 Hz from the script, xrRequestExitSession
//       Each phase checks the captured PNGs pixel-exactly, the per-frame CSV timing and the API results.
//   xrsim_test --run [--frames N] [--direct] [--loader PATH]   demo client for the current XRSIM_* environment
#include <windows.h>
#include <unknwn.h>

#include <vulkan/vulkan.h>

#define XR_NO_PROTOTYPES
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <openxr/openxr_platform.h>

#include "gt2formats/png_reader.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

const uint32_t kVertSpv[] =
#include "pattern.vert.inc"
    ;
const uint32_t kFragSpv[] =
#include "pattern.frag.inc"
    ;

// ------------------------------------------------------------------------------------------------ reporting
int g_pass = 0, g_fail = 0;
bool g_checks = true;

void Check(bool ok, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (!g_checks) {
        if (!ok) printf("  [warn] %s\n", buf);
        return;
    }
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", buf);
    (ok ? g_pass : g_fail)++;
}

std::string ExeDir() {
    char p[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, p, MAX_PATH);
    std::string s(p, n);
    return s.substr(0, s.find_last_of("\\/"));
}

bool FileExists(const std::string& p) { return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// ------------------------------------------------------------------------------------------------ OpenXR functions
#define XT_FNS(X)                                                                                                        \
    X(xrDestroyInstance) X(xrGetInstanceProperties) X(xrPollEvent) X(xrResultToString) X(xrGetSystem)                    \
    X(xrGetSystemProperties) X(xrEnumerateViewConfigurationViews) X(xrEnumerateEnvironmentBlendModes) X(xrCreateSession) \
    X(xrDestroySession) X(xrBeginSession) X(xrEndSession) X(xrRequestExitSession) X(xrWaitFrame) X(xrBeginFrame)         \
    X(xrEndFrame) X(xrLocateViews) X(xrEnumerateReferenceSpaces) X(xrCreateReferenceSpace) X(xrCreateActionSpace)       \
    X(xrLocateSpace) X(xrDestroySpace) X(xrEnumerateSwapchainFormats) X(xrCreateSwapchain) X(xrDestroySwapchain)         \
    X(xrEnumerateSwapchainImages) X(xrAcquireSwapchainImage) X(xrWaitSwapchainImage) X(xrReleaseSwapchainImage)          \
    X(xrStringToPath) X(xrCreateActionSet) X(xrDestroyActionSet) X(xrCreateAction) X(xrSuggestInteractionProfileBindings) \
    X(xrAttachSessionActionSets) X(xrGetCurrentInteractionProfile) X(xrSyncActions) X(xrGetActionStateBoolean)          \
    X(xrGetActionStateFloat) X(xrGetActionStateVector2f) X(xrGetActionStatePose) X(xrApplyHapticFeedback)                \
    X(xrGetVulkanGraphicsRequirements2KHR) X(xrCreateVulkanInstanceKHR) X(xrCreateVulkanDeviceKHR)                      \
    X(xrGetVulkanGraphicsDevice2KHR) X(xrGetVulkanGraphicsRequirementsKHR) X(xrGetVulkanInstanceExtensionsKHR)          \
    X(xrGetVulkanDeviceExtensionsKHR) X(xrGetVulkanGraphicsDeviceKHR) X(xrConvertTimeToWin32PerformanceCounterKHR)       \
    X(xrConvertWin32PerformanceCounterToTimeKHR) X(xrEnumerateDisplayRefreshRatesFB) X(xrGetDisplayRefreshRateFB)       \
    X(xrRequestDisplayRefreshRateFB) X(xrPerfSettingsSetPerformanceLevelEXT)

struct Xr {
    HMODULE module = nullptr;
    PFN_xrGetInstanceProcAddr gipa = nullptr;
    PFN_xrEnumerateInstanceExtensionProperties xrEnumerateInstanceExtensionProperties = nullptr;
    PFN_xrCreateInstance xrCreateInstance = nullptr;
#define XT_DECL(n) PFN_##n n = nullptr;
    XT_FNS(XT_DECL)
#undef XT_DECL
    void Load(XrInstance inst) {
#define XT_LOAD(n) gipa(inst, #n, reinterpret_cast<PFN_xrVoidFunction*>(&n));
        XT_FNS(XT_LOAD)
#undef XT_LOAD
    }
};

bool OpenLoader(Xr& xr, const std::string& path) {
    xr.module = LoadLibraryA(path.c_str());
    if (!xr.module) return false;
    xr.gipa = reinterpret_cast<PFN_xrGetInstanceProcAddr>(reinterpret_cast<void*>(GetProcAddress(xr.module, "xrGetInstanceProcAddr")));
    return xr.gipa != nullptr;
}

// Direct path: no loader; the runtime DLL is negotiated with like the loader does.
bool OpenDirect(Xr& xr, const std::string& runtimeDll) {
    xr.module = LoadLibraryA(runtimeDll.c_str());
    if (!xr.module) return false;
    auto neg = reinterpret_cast<PFN_xrNegotiateLoaderRuntimeInterface>(
        reinterpret_cast<void*>(GetProcAddress(xr.module, "xrNegotiateLoaderRuntimeInterface")));
    if (!neg) return false;
    XrNegotiateLoaderInfo li{};
    li.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
    li.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
    li.structSize = sizeof(li);
    li.minInterfaceVersion = 1;
    li.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    li.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
    li.maxApiVersion = XR_MAKE_VERSION(1, 0x3ff, 0xfff);
    XrNegotiateRuntimeRequest rr{};
    rr.structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST;
    rr.structVersion = XR_RUNTIME_INFO_STRUCT_VERSION;
    rr.structSize = sizeof(rr);
    if (neg(&li, &rr) != XR_SUCCESS || !rr.getInstanceProcAddr) return false;
    xr.gipa = rr.getInstanceProcAddr;
    return true;
}

// ------------------------------------------------------------------------------------------------ colours / math
struct RGB8 { uint8_t r, g, b; };
RGB8 ColA(int eye, int f) { return {(uint8_t)(f & 255), (uint8_t)(eye ? 200 : 50), 100}; }
RGB8 ColB(int eye, int f) { return {(uint8_t)(255 - (f & 255)), 30, (uint8_t)(eye ? 220 : 10)}; }
RGB8 ColC(int f) { return {(uint8_t)((f * 3) & 255), 128, (uint8_t)(255 - (f & 255))}; }
RGB8 ColQ(int f) { return {(uint8_t)((f * 5) & 255), 255, 7}; }
constexpr int kMargin = 64, kCell = 32;

double SrgbToLinear(uint8_t b) {
    const double c = b / 255.0;
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}
uint8_t LinearToSrgb8(double c) {
    c = std::clamp(c, 0.0, 1.0);
    const double e = c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
    return (uint8_t)std::lround(e * 255.0);
}
// value the shader / clear writes for a wanted stored byte
float ShaderValue(uint8_t b, bool srgbFormat) { return (float)(srgbFormat ? SrgbToLinear(b) : b / 255.0); }
// value a capture PNG must contain for a stored byte
uint8_t PngValue(uint8_t b, bool srgbFormat) { return srgbFormat ? b : LinearToSrgb8(b / 255.0); }

struct Q { double x, y, z, w; };
Q QMul(Q a, Q b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
Q QYawPitch(double yawDeg, double pitchDeg) {
    const double k = 3.14159265358979323846 / 360.0;
    return QMul({0, std::sin(yawDeg * k), 0, std::cos(yawDeg * k)}, {std::sin(pitchDeg * k), 0, 0, std::cos(pitchDeg * k)});
}
void Rotate(Q q, const double v[3], double out[3]) {
    const Q p = QMul(QMul(q, {v[0], v[1], v[2], 0}), {-q.x, -q.y, -q.z, q.w});
    out[0] = p.x; out[1] = p.y; out[2] = p.z;
}
bool Near(double a, double b, double eps = 1e-4) { return std::fabs(a - b) <= eps; }
bool QNear(const XrQuaternionf& a, Q b, double eps = 1e-4) {
    const double d = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w); // q and -q are the same rotation
    return Near(d, 1.0, eps);
}

// ------------------------------------------------------------------------------------------------ phases
struct Phase {
    char id = 'A';
    std::string name;
    bool loader = true, enable2 = true, multiview = true, depth = true, quad = true, touch = true;
    VkFormat color = VK_FORMAT_R8G8B8A8_SRGB;
    XrVersion api = XR_MAKE_VERSION(1, 1, 0);
    std::string script;
    int requestRefreshFrame = -1, requestExitFrame = -1;
    double hz = 72.0, hzAfter = 0.0;
    int timingFrom = 10, timingTo = 150;
    std::vector<int> captures;
    uint32_t expectW = 0, expectH = 0;
    int maxFrames = 100000;
};

struct Gfx {
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    uint32_t qf = 0;
    VkQueue q = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    bool validation = false;
};
int g_vkErrors = 0, g_vkWarnings = 0;

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT,
                                       const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++g_vkErrors;
        printf("  [vulkan validation error] %s\n", data->pMessage);
    } else if (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        if (g_vkWarnings++ < 5) printf("  [vulkan validation warning] %s\n", data->pMessage);
    }
    return VK_FALSE;
}

struct SwapchainImages {
    XrSwapchain sc = XR_NULL_HANDLE;
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    uint32_t w = 0, h = 0, layers = 1;
};

bool MakeSwapchain(Xr& xr, XrSession session, Gfx& g, SwapchainImages& out, VkFormat fmt, uint32_t w, uint32_t h, uint32_t layers,
                   bool depth) {
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = depth ? XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : (XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT);
    ci.format = fmt;
    ci.sampleCount = 1;
    ci.width = w;
    ci.height = h;
    ci.faceCount = 1;
    ci.arraySize = layers;
    ci.mipCount = 1;
    XrResult r = xr.xrCreateSwapchain(session, &ci, &out.sc);
    Check(r == XR_SUCCESS, "xrCreateSwapchain %ux%u x%u format %d -> %d", w, h, layers, (int)fmt, (int)r);
    if (r != XR_SUCCESS) return false;
    uint32_t n = 0;
    xr.xrEnumerateSwapchainImages(out.sc, 0, &n, nullptr);
    std::vector<XrSwapchainImageVulkanKHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    xr.xrEnumerateSwapchainImages(out.sc, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
    out.w = w;
    out.h = h;
    out.layers = layers;
    for (auto& im : imgs) {
        out.images.push_back(im.image);
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = im.image;
        vi.viewType = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        vi.format = fmt;
        vi.subresourceRange = {(VkImageAspectFlags)(depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, 1, 0, layers};
        VkImageView v = VK_NULL_HANDLE;
        vkCreateImageView(g.dev, &vi, nullptr, &v);
        out.views.push_back(v);
    }
    return true;
}

void DestroySwapchain(Xr& xr, Gfx& g, SwapchainImages& s) {
    for (VkImageView v : s.views) vkDestroyImageView(g.dev, v, nullptr);
    if (s.sc) xr.xrDestroySwapchain(s.sc);
    s = SwapchainImages{};
}

uint32_t Acquire(Xr& xr, SwapchainImages& s) {
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    xr.xrAcquireSwapchainImage(s.sc, &ai, &idx);
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    xr.xrWaitSwapchainImage(s.sc, &wi);
    return idx;
}
void Release(Xr& xr, SwapchainImages& s) {
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xr.xrReleaseSwapchainImage(s.sc, &ri);
}

bool CreatePipeline(Gfx& g, VkFormat color, VkFormat depth, uint32_t viewMask) {
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    mi.codeSize = sizeof(kVertSpv);
    mi.pCode = kVertSpv;
    vkCreateShaderModule(g.dev, &mi, nullptr, &vs);
    mi.codeSize = sizeof(kFragSpv);
    mi.pCode = kFragSpv;
    vkCreateShaderModule(g.dev, &mi, nullptr, &fs);
    VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 72};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &pcr;
    vkCreatePipelineLayout(g.dev, &li, nullptr, &g.layout);
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = vs;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = fs;
    st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = depth != VK_FORMAT_UNDEFINED ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = ds.depthTestEnable;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo rci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rci.viewMask = viewMask;
    rci.colorAttachmentCount = 1;
    rci.pColorAttachmentFormats = &color;
    rci.depthAttachmentFormat = depth;
    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.pNext = &rci;
    pi.stageCount = 2;
    pi.pStages = st;
    pi.pVertexInputState = &vin;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &dy;
    pi.layout = g.layout;
    const VkResult r = vkCreateGraphicsPipelines(g.dev, VK_NULL_HANDLE, 1, &pi, nullptr, &g.pipe);
    vkDestroyShaderModule(g.dev, vs, nullptr);
    vkDestroyShaderModule(g.dev, fs, nullptr);
    return r == VK_SUCCESS;
}

bool HasLayer(const char* name) {
    uint32_t n = 0;
    vkEnumerateInstanceLayerProperties(&n, nullptr);
    std::vector<VkLayerProperties> v(n);
    vkEnumerateInstanceLayerProperties(&n, v.data());
    for (auto& l : v)
        if (strcmp(l.layerName, name) == 0) return true;
    return false;
}

std::vector<std::string> SplitSpaces(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream is(s);
    for (std::string t; is >> t;) out.push_back(t);
    return out;
}

// Creates the VkInstance / VkDevice the way the phase's extension requires.
bool CreateVulkan(Xr& xr, XrInstance inst, XrSystemId sys, const Phase& P, Gfx& g, bool wantValidation) {
    g.validation = wantValidation && HasLayer("VK_LAYER_KHRONOS_validation");
    std::vector<const char*> layers, exts;
    if (g.validation) {
        layers.push_back("VK_LAYER_KHRONOS_validation");
        exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    std::vector<std::string> xrInstExts;
    if (!P.enable2) {
        uint32_t n = 0;
        xr.xrGetVulkanInstanceExtensionsKHR(inst, sys, 0, &n, nullptr);
        std::string s(n, '\0');
        xr.xrGetVulkanInstanceExtensionsKHR(inst, sys, n, &n, s.data());
        xrInstExts = SplitSpaces(s.c_str());
        for (auto& e : xrInstExts) exts.push_back(e.c_str());
    }
    VkDebugUtilsMessengerCreateInfoEXT dm{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    dm.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
    dm.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    dm.pfnUserCallback = &DebugCb;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "xrsim_test";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pNext = g.validation ? &dm : nullptr;
    ici.pApplicationInfo = &app;
    ici.enabledLayerCount = (uint32_t)layers.size();
    ici.ppEnabledLayerNames = layers.data();
    ici.enabledExtensionCount = (uint32_t)exts.size();
    ici.ppEnabledExtensionNames = exts.data();
    if (P.enable2) {
        XrVulkanInstanceCreateInfoKHR ci{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
        ci.systemId = sys;
        ci.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
        ci.vulkanCreateInfo = &ici;
        VkResult vr = VK_ERROR_UNKNOWN;
        const XrResult r = xr.xrCreateVulkanInstanceKHR(inst, &ci, &g.inst, &vr);
        Check(r == XR_SUCCESS && vr == VK_SUCCESS, "xrCreateVulkanInstanceKHR -> %d / VkResult %d", (int)r, (int)vr);
        if (r != XR_SUCCESS || vr != VK_SUCCESS) return false;
        XrVulkanGraphicsDeviceGetInfoKHR gi{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
        gi.systemId = sys;
        gi.vulkanInstance = g.inst;
        Check(xr.xrGetVulkanGraphicsDevice2KHR(inst, &gi, &g.pd) == XR_SUCCESS && g.pd, "xrGetVulkanGraphicsDevice2KHR");
    } else {
        const VkResult vr = vkCreateInstance(&ici, nullptr, &g.inst);
        Check(vr == VK_SUCCESS, "vkCreateInstance (%zu extension(s) from xrGetVulkanInstanceExtensionsKHR)", xrInstExts.size());
        if (vr != VK_SUCCESS) return false;
        Check(xr.xrGetVulkanGraphicsDeviceKHR(inst, sys, g.inst, &g.pd) == XR_SUCCESS && g.pd, "xrGetVulkanGraphicsDeviceKHR");
    }
    if (!g.pd) return false;
    if (g.validation) {
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(g.inst, "vkCreateDebugUtilsMessengerEXT"));
        if (create) create(g.inst, &dm, nullptr, &g.messenger);
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(g.pd, &props);
    printf("  GPU: %s (Vulkan %u.%u), validation %s\n", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
           VK_API_VERSION_MINOR(props.apiVersion), g.validation ? "on" : "off");
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g.pd, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qp(nq);
    vkGetPhysicalDeviceQueueFamilyProperties(g.pd, &nq, qp.data());
    g.qf = UINT32_MAX;
    for (uint32_t i = 0; i < nq && g.qf == UINT32_MAX; ++i)
        if (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) g.qf = i;
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = g.qf;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    f11.multiview = VK_TRUE;
    f11.pNext = &f13;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f11;
    std::vector<std::string> xrDevExts;
    std::vector<const char*> dexts;
    if (!P.enable2) {
        uint32_t n = 0;
        xr.xrGetVulkanDeviceExtensionsKHR(inst, sys, 0, &n, nullptr);
        std::string s(n, '\0');
        xr.xrGetVulkanDeviceExtensionsKHR(inst, sys, n, &n, s.data());
        xrDevExts = SplitSpaces(s.c_str());
        for (auto& e : xrDevExts) dexts.push_back(e.c_str());
    }
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qi;
    dci.enabledExtensionCount = (uint32_t)dexts.size();
    dci.ppEnabledExtensionNames = dexts.data();
    if (P.enable2) {
        XrVulkanDeviceCreateInfoKHR ci{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
        ci.systemId = sys;
        ci.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
        ci.vulkanPhysicalDevice = g.pd;
        ci.vulkanCreateInfo = &dci;
        VkResult vr = VK_ERROR_UNKNOWN;
        const XrResult r = xr.xrCreateVulkanDeviceKHR(inst, &ci, &g.dev, &vr);
        Check(r == XR_SUCCESS && vr == VK_SUCCESS, "xrCreateVulkanDeviceKHR -> %d / VkResult %d", (int)r, (int)vr);
        if (r != XR_SUCCESS || vr != VK_SUCCESS) return false;
    } else {
        const VkResult vr = vkCreateDevice(g.pd, &dci, nullptr, &g.dev);
        Check(vr == VK_SUCCESS, "vkCreateDevice -> %d", (int)vr);
        if (vr != VK_SUCCESS) return false;
    }
    vkGetDeviceQueue(g.dev, g.qf, 0, &g.q);
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = g.qf;
    vkCreateCommandPool(g.dev, &pci, nullptr, &g.pool);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = g.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    vkAllocateCommandBuffers(g.dev, &cai, &g.cb);
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(g.dev, &fi, nullptr, &g.fence);
    return true;
}

void DestroyVulkan(Gfx& g) {
    if (g.dev) {
        vkDeviceWaitIdle(g.dev);
        if (g.pipe) vkDestroyPipeline(g.dev, g.pipe, nullptr);
        if (g.layout) vkDestroyPipelineLayout(g.dev, g.layout, nullptr);
        if (g.fence) vkDestroyFence(g.dev, g.fence, nullptr);
        if (g.pool) vkDestroyCommandPool(g.dev, g.pool, nullptr);
        vkDestroyDevice(g.dev, nullptr);
    }
    if (g.messenger) {
        auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(g.inst, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy) destroy(g.inst, g.messenger, nullptr);
    }
    if (g.inst) vkDestroyInstance(g.inst, nullptr);
    g = Gfx{};
}

// ------------------------------------------------------------------------------------------------ verification
std::vector<std::vector<std::string>> ReadCsv(const std::string& path) {
    std::vector<std::vector<std::string>> rows;
    std::ifstream f(path);
    std::string line;
    bool header = true;
    while (std::getline(f, line)) {
        if (header) { header = false; continue; }
        std::vector<std::string> cols;
        std::stringstream ss(line);
        for (std::string c; std::getline(ss, c, ',');) cols.push_back(c);
        rows.push_back(cols);
    }
    return rows;
}

void VerifyEyePng(const std::string& path, int eye, int f, uint32_t W, uint32_t H, bool srgb) {
    gt2::PngImage img;
    try {
        img = gt2::ReadPngFile(path);
    } catch (const std::exception& e) {
        Check(false, "capture %s readable (%s)", path.c_str(), e.what());
        return;
    }
    if (img.width != (int)W || img.height != (int)H) {
        Check(false, "capture %s size %dx%d (expected %ux%u)", path.c_str(), img.width, img.height, W, H);
        return;
    }
    const RGB8 a = ColA(eye, f), b = ColB(eye, f), c = ColC(f);
    long long bad = 0;
    int fx = -1, fy = -1;
    RGB8 got{}, want{};
    for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t x = 0; x < W; ++x) {
            const bool inside = x >= (uint32_t)kMargin && x < W - kMargin && y >= (uint32_t)kMargin && y < H - kMargin;
            const RGB8 e = !inside ? c : ((((x / kCell) + (y / kCell)) & 1) ? b : a);
            const RGB8 w{PngValue(e.r, srgb), PngValue(e.g, srgb), PngValue(e.b, srgb)};
            const uint8_t* p = &img.rgba[((size_t)y * W + x) * 4];
            if (p[0] != w.r || p[1] != w.g || p[2] != w.b || p[3] != 255) {
                if (bad++ == 0) {
                    fx = (int)x;
                    fy = (int)y;
                    got = {p[0], p[1], p[2]};
                    want = w;
                }
            }
        }
    }
    const std::string file = path.substr(path.find_last_of("\\/") + 1);
    if (bad == 0) Check(true, "capture %s: %ux%u pixel-exact (eye %d, frame %d)", file.c_str(), W, H, eye, f);
    else
        Check(false, "capture %s: %lld mismatching pixels, first (%d,%d) = %u,%u,%u expected %u,%u,%u", file.c_str(), bad, fx, fy,
              got.r, got.g, got.b, want.r, want.g, want.b);
}

void VerifyQuadPng(const std::string& path, int f, uint32_t W, uint32_t H, bool srgb) {
    gt2::PngImage img;
    try {
        img = gt2::ReadPngFile(path);
    } catch (const std::exception& e) {
        Check(false, "quad capture %s readable (%s)", path.c_str(), e.what());
        return;
    }
    const RGB8 q = ColQ(f);
    const uint8_t w[4] = {PngValue(q.r, srgb), PngValue(q.g, srgb), PngValue(q.b, srgb), 255};
    long long bad = 0;
    for (size_t i = 0; i + 3 < img.rgba.size(); i += 4)
        if (memcmp(&img.rgba[i], w, 4) != 0) ++bad;
    const bool ok = img.width == (int)W && img.height == (int)H && bad == 0;
    Check(ok, "quad capture %s: %dx%d, %lld mismatching pixels", path.substr(path.find_last_of("\\/") + 1).c_str(), img.width,
          img.height, bad);
}

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
    default: return "?";
    }
}

// ------------------------------------------------------------------------------------------------ one phase
struct Options {
    bool selftest = false, window = false, validation = true, direct = false;
    std::string out, loader;
    int frames = 0;
};

void RunPhase(const Phase& P, const Options& O, const std::string& outDir) {
    printf("\n=== phase %c: %s ===\n", P.id, P.name.c_str());
    fflush(stdout);
    const std::string exeDir = ExeDir();
    if (O.selftest) {
        CreateDirectoryA(outDir.c_str(), nullptr);
        const std::string scriptPath = outDir + "\\script.txt";
        std::ofstream(scriptPath) << P.script;
        SetEnvironmentVariableA("XRSIM_SCRIPT", scriptPath.c_str());
        SetEnvironmentVariableA("XRSIM_OUT", outDir.c_str());
        SetEnvironmentVariableA("XRSIM_WINDOW", O.window ? "1" : nullptr);
        SetEnvironmentVariableA("XRSIM_REFRESH", nullptr);
        SetEnvironmentVariableA("XRSIM_RESOLUTION", nullptr);
        DeleteFileA((outDir + "\\xrsim_frames.csv").c_str());
        for (int f : P.captures)
            for (const char* suffix : {"_l0_proj_v0.png", "_l0_proj_v1.png", "_l1_quad.png"}) {
                char n[64];
                snprintf(n, sizeof(n), "\\f%06d%s", f, suffix);
                DeleteFileA((outDir + n).c_str());
            }
    }
    Xr xr;
    if (P.loader) {
        const std::string manifest = exeDir + "\\xrsim_runtime.json";
        if (O.selftest) SetEnvironmentVariableA("XR_RUNTIME_JSON", manifest.c_str());
        const std::string loader = O.loader.empty() ? exeDir + "\\openxr_loader.dll" : O.loader;
        const bool ok = OpenLoader(xr, loader);
        Check(ok, "official OpenXR loader %s loaded", loader.c_str());
        if (!ok) return;
    } else {
        const std::string dll = exeDir + "\\xrsim_runtime.dll";
        const bool ok = OpenDirect(xr, dll);
        Check(ok, "direct runtime load + xrNegotiateLoaderRuntimeInterface (%s)", dll.c_str());
        if (!ok) return;
    }
    xr.gipa(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", reinterpret_cast<PFN_xrVoidFunction*>(&xr.xrEnumerateInstanceExtensionProperties));
    xr.gipa(XR_NULL_HANDLE, "xrCreateInstance", reinterpret_cast<PFN_xrVoidFunction*>(&xr.xrCreateInstance));
    uint32_t nExt = 0;
    xr.xrEnumerateInstanceExtensionProperties(nullptr, 0, &nExt, nullptr);
    std::vector<XrExtensionProperties> extProps(nExt, {XR_TYPE_EXTENSION_PROPERTIES});
    xr.xrEnumerateInstanceExtensionProperties(nullptr, nExt, &nExt, extProps.data());
    auto hasExt = [&](const char* n) {
        for (auto& e : extProps)
            if (strcmp(e.extensionName, n) == 0) return true;
        return false;
    };
    std::vector<const char*> exts;
    const char* want[] = {P.enable2 ? XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME : XR_KHR_VULKAN_ENABLE_EXTENSION_NAME,
                          XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME, XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME,
                          XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME, XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME};
    bool allExt = true;
    for (const char* e : want) {
        allExt = allExt && hasExt(e);
        exts.push_back(e);
    }
    Check(allExt, "runtime lists the %zu required extensions (of %u)", exts.size(), nExt);

    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy(ici.applicationInfo.applicationName, "xrsim_test");
    strcpy(ici.applicationInfo.engineName, "none");
    ici.applicationInfo.apiVersion = P.api;
    ici.enabledExtensionCount = (uint32_t)exts.size();
    ici.enabledExtensionNames = exts.data();
    XrInstance inst = XR_NULL_HANDLE;
    XrResult r = xr.xrCreateInstance(&ici, &inst);
    Check(r == XR_SUCCESS, "xrCreateInstance (API %u.%u) -> %d", (unsigned)XR_VERSION_MAJOR(P.api), (unsigned)XR_VERSION_MINOR(P.api), (int)r);
    if (r != XR_SUCCESS) return;
    xr.Load(inst);
    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    xr.xrGetInstanceProperties(inst, &ip);
    Check(strcmp(ip.runtimeName, "xrsim") == 0, "active runtime is '%s'", ip.runtimeName);
    if (strcmp(ip.runtimeName, "xrsim") != 0) {
        xr.xrDestroyInstance(inst);
        return;
    }

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    XrSystemId sys = XR_NULL_SYSTEM_ID;
    if (P.id == 'A') {
        sgi.formFactor = XR_FORM_FACTOR_HANDHELD_DISPLAY;
        Check(xr.xrGetSystem(inst, &sgi, &sys) == XR_ERROR_FORM_FACTOR_UNSUPPORTED, "handheld form factor -> XR_ERROR_FORM_FACTOR_UNSUPPORTED");
        XrPath bad = XR_NULL_PATH;
        Check(xr.xrStringToPath(inst, "/user//hand", &bad) == XR_ERROR_PATH_FORMAT_INVALID, "malformed path -> XR_ERROR_PATH_FORMAT_INVALID");
        PFN_xrVoidFunction fn = nullptr;
        Check(xr.gipa(inst, "xrGetVulkanGraphicsDeviceKHR", &fn) == XR_ERROR_FUNCTION_UNSUPPORTED && !fn,
              "function of a non-enabled extension -> XR_ERROR_FUNCTION_UNSUPPORTED");
    }
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    r = xr.xrGetSystem(inst, &sgi, &sys);
    Check(r == XR_SUCCESS, "xrGetSystem(HMD)");
    uint32_t nViews = 0;
    XrViewConfigurationView vcv[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    xr.xrEnumerateViewConfigurationViews(inst, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &nViews, vcv);
    const uint32_t W = vcv[0].recommendedImageRectWidth, H = vcv[0].recommendedImageRectHeight;
    if (P.expectW) Check(nViews == 2 && W == P.expectW && H == P.expectH, "recommended view size %ux%u (expected %ux%u)", W, H, P.expectW, P.expectH);

    XrGraphicsRequirementsVulkanKHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    r = P.enable2 ? xr.xrGetVulkanGraphicsRequirements2KHR(inst, sys, &req) : xr.xrGetVulkanGraphicsRequirementsKHR(inst, sys, &req);
    Check(r == XR_SUCCESS, "graphics requirements");
    Gfx g;
    if (!CreateVulkan(xr, inst, sys, P, g, O.validation)) {
        xr.xrDestroyInstance(inst);
        DestroyVulkan(g);
        return;
    }
    XrGraphicsBindingVulkanKHR gb{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    gb.instance = g.inst;
    gb.physicalDevice = g.pd;
    gb.device = g.dev;
    gb.queueFamilyIndex = g.qf;
    gb.queueIndex = 0;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &gb;
    sci.systemId = sys;
    XrSession session = XR_NULL_HANDLE;
    r = xr.xrCreateSession(inst, &sci, &session);
    Check(r == XR_SUCCESS, "xrCreateSession -> %d", (int)r);
    if (r != XR_SUCCESS) {
        DestroyVulkan(g);
        xr.xrDestroyInstance(inst);
        return;
    }

    // ---- actions
    auto path = [&](const char* s) {
        XrPath p = XR_NULL_PATH;
        xr.xrStringToPath(inst, s, &p);
        return p;
    };
    const XrPath left = path("/user/hand/left"), right = path("/user/hand/right");
    const XrPath hands[2] = {left, right};
    XrActionSetCreateInfo asci{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy(asci.actionSetName, "gameplay");
    strcpy(asci.localizedActionSetName, "Gameplay");
    XrActionSet aset = XR_NULL_HANDLE;
    xr.xrCreateActionSet(inst, &asci, &aset);
    auto makeAction = [&](const char* name, XrActionType type, bool bothHands) {
        XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
        strcpy(ci.actionName, name);
        strcpy(ci.localizedActionName, name);
        ci.actionType = type;
        ci.countSubactionPaths = bothHands ? 2 : 0;
        ci.subactionPaths = bothHands ? hands : nullptr;
        XrAction a = XR_NULL_HANDLE;
        const XrResult rr = xr.xrCreateAction(aset, &ci, &a);
        Check(rr == XR_SUCCESS, "xrCreateAction %s", name);
        return a;
    };
    const XrAction aTrigger = makeAction("trigger", XR_ACTION_TYPE_FLOAT_INPUT, true);
    const XrAction aButton = makeAction("button", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
    const XrAction aStick = makeAction("stick", XR_ACTION_TYPE_VECTOR2F_INPUT, true);
    const XrAction aPose = makeAction("hand_pose", XR_ACTION_TYPE_POSE_INPUT, true);
    const XrAction aHaptic = makeAction("haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT, true);
    std::vector<XrActionSuggestedBinding> sb;
    const char* profile = P.touch ? "/interaction_profiles/oculus/touch_controller" : "/interaction_profiles/khr/simple_controller";
    if (P.touch) {
        sb = {{aTrigger, path("/user/hand/left/input/trigger/value")}, {aTrigger, path("/user/hand/right/input/trigger")},
              {aButton, path("/user/hand/right/input/a/click")},        {aStick, path("/user/hand/left/input/thumbstick")},
              {aStick, path("/user/hand/right/input/thumbstick")},      {aPose, path("/user/hand/left/input/grip/pose")},
              {aPose, path("/user/hand/right/input/grip/pose")},        {aHaptic, path("/user/hand/left/output/haptic")},
              {aHaptic, path("/user/hand/right/output/haptic")}};
    } else {
        sb = {{aButton, path("/user/hand/right/input/select/click")}, {aPose, path("/user/hand/left/input/aim/pose")},
              {aPose, path("/user/hand/right/input/aim/pose")},       {aHaptic, path("/user/hand/right/output/haptic")}};
    }
    XrInteractionProfileSuggestedBinding ipsb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    if (P.id == 'A') {
        XrActionSuggestedBinding bad[] = {{aButton, path("/user/hand/left/input/a/click")}};
        ipsb.interactionProfile = path("/interaction_profiles/oculus/touch_controller");
        ipsb.countSuggestedBindings = 1;
        ipsb.suggestedBindings = bad;
        Check(xr.xrSuggestInteractionProfileBindings(inst, &ipsb) == XR_ERROR_PATH_UNSUPPORTED, "binding to a left-hand 'a' -> XR_ERROR_PATH_UNSUPPORTED");
        ipsb.interactionProfile = path("/interaction_profiles/valve/index_controller");
        Check(xr.xrSuggestInteractionProfileBindings(inst, &ipsb) == XR_ERROR_PATH_UNSUPPORTED, "unsupported interaction profile -> XR_ERROR_PATH_UNSUPPORTED");
    }
    ipsb.interactionProfile = path(profile);
    ipsb.countSuggestedBindings = (uint32_t)sb.size();
    ipsb.suggestedBindings = sb.data();
    r = xr.xrSuggestInteractionProfileBindings(inst, &ipsb);
    Check(r == XR_SUCCESS, "suggest %zu bindings for %s", sb.size(), profile);
    XrSessionActionSetsAttachInfo sai{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    sai.countActionSets = 1;
    sai.actionSets = &aset;
    Check(xr.xrAttachSessionActionSets(session, &sai) == XR_SUCCESS, "xrAttachSessionActionSets");
    XrInteractionProfileState ips{XR_TYPE_INTERACTION_PROFILE_STATE};
    xr.xrGetCurrentInteractionProfile(session, right, &ips);
    Check(ips.interactionProfile == path(profile), "current interaction profile is %s", profile);

    // ---- spaces
    auto refSpace = [&](XrReferenceSpaceType t) {
        XrReferenceSpaceCreateInfo ci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        ci.referenceSpaceType = t;
        ci.poseInReferenceSpace.orientation.w = 1;
        XrSpace s = XR_NULL_HANDLE;
        xr.xrCreateReferenceSpace(session, &ci, &s);
        return s;
    };
    const XrSpace local = refSpace(XR_REFERENCE_SPACE_TYPE_LOCAL), view = refSpace(XR_REFERENCE_SPACE_TYPE_VIEW),
                  stage = refSpace(XR_REFERENCE_SPACE_TYPE_STAGE);
    const XrSpace localFloor = P.api >= XR_MAKE_VERSION(1, 1, 0) ? refSpace(XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR) : XR_NULL_HANDLE;
    XrSpace handSpace[2] = {};
    for (int h = 0; h < 2; ++h) {
        XrActionSpaceCreateInfo ci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        ci.action = aPose;
        ci.subactionPath = hands[h];
        ci.poseInActionSpace.orientation.w = 1;
        xr.xrCreateActionSpace(session, &ci, &handSpace[h]);
    }
    Check(local && view && stage && handSpace[0] && handSpace[1] && (P.api < XR_MAKE_VERSION(1, 1, 0) || localFloor),
          "reference spaces (VIEW, LOCAL, STAGE%s) and action spaces", localFloor ? ", LOCAL_FLOOR" : "");

    // ---- swapchains + pipeline
    uint32_t nFmt = 0;
    xr.xrEnumerateSwapchainFormats(session, 0, &nFmt, nullptr);
    std::vector<int64_t> fmts(nFmt);
    xr.xrEnumerateSwapchainFormats(session, nFmt, &nFmt, fmts.data());
    Check(std::find(fmts.begin(), fmts.end(), (int64_t)P.color) != fmts.end(), "swapchain format %d offered (%u formats)", (int)P.color, nFmt);
    if (P.id == 'A') {
        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        ci.format = VK_FORMAT_R8_UNORM;
        ci.sampleCount = ci.faceCount = ci.arraySize = ci.mipCount = 1;
        ci.width = ci.height = 64;
        XrSwapchain bad = XR_NULL_HANDLE;
        Check(xr.xrCreateSwapchain(session, &ci, &bad) == XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED, "unsupported format -> XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED");
    }
    const bool srgb = P.color == VK_FORMAT_R8G8B8A8_SRGB || P.color == VK_FORMAT_B8G8R8A8_SRGB;
    SwapchainImages color[2], depthSc, quadSc;
    const uint32_t QW = 256, QH = 128;
    if (P.multiview) {
        MakeSwapchain(xr, session, g, color[0], P.color, W, H, 2, false);
        if (P.depth) MakeSwapchain(xr, session, g, depthSc, VK_FORMAT_D32_SFLOAT, W, H, 2, true);
    } else {
        MakeSwapchain(xr, session, g, color[0], P.color, W, H, 1, false);
        MakeSwapchain(xr, session, g, color[1], P.color, W, H, 1, false);
    }
    if (P.quad) MakeSwapchain(xr, session, g, quadSc, VK_FORMAT_R8G8B8A8_SRGB, QW, QH, 1, false);
    Check(CreatePipeline(g, P.color, P.depth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_UNDEFINED, P.multiview ? 3u : 0u), "graphics pipeline (viewMask %u)",
          P.multiview ? 3u : 0u);

    // ---- frame loop
    std::vector<XrSessionState> states;
    int profileEvents = 0, refreshEvents = 0;
    float refreshTo = 0;
    bool running = false, done = false;
    int frame = 0;
    struct Timing { int64_t waitReturnQpc; XrTime display; XrDuration period; };
    std::vector<Timing> timing;
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    const DWORD startTick = GetTickCount();
    int notFocusedSyncs = 0, inactiveWhileUnfocused = 0;
    while (!done) {
        XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
        while (xr.xrPollEvent(inst, &ev) == XR_SUCCESS) {
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                const auto& sc = reinterpret_cast<const XrEventDataSessionStateChanged&>(ev);
                states.push_back(sc.state);
                if (sc.state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    r = xr.xrBeginSession(session, &bi);
                    Check(r == XR_SUCCESS, "xrBeginSession");
                    running = r == XR_SUCCESS;
                    if (P.id == 'A') {
                        XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO};
                        Check(xr.xrBeginFrame(session, &fbi) == XR_ERROR_CALL_ORDER_INVALID, "xrBeginFrame before xrWaitFrame -> XR_ERROR_CALL_ORDER_INVALID");
                    }
                } else if (sc.state == XR_SESSION_STATE_STOPPING) {
                    r = xr.xrEndSession(session);
                    Check(r == XR_SUCCESS, "xrEndSession on STOPPING (frame %d)", frame);
                    running = false;
                } else if (sc.state == XR_SESSION_STATE_EXITING || sc.state == XR_SESSION_STATE_LOSS_PENDING) {
                    done = true;
                }
            } else if (ev.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {
                ++profileEvents;
            } else if (ev.type == XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB) {
                ++refreshEvents;
                refreshTo = reinterpret_cast<const XrEventDataDisplayRefreshRateChangedFB&>(ev).toDisplayRefreshRate;
            }
            ev = {XR_TYPE_EVENT_DATA_BUFFER};
        }
        if (done) break;
        if (O.selftest && (GetTickCount() - startTick > 60000 || frame >= P.maxFrames)) {
            Check(false, "phase did not finish in time (frame %d)", frame);
            if (running) xr.xrRequestExitSession(session);
            if (frame >= P.maxFrames + 400 || GetTickCount() - startTick > 70000) break;
        }
        if (!running) {
            Sleep(1);
            continue;
        }
        XrFrameWaitInfo fwi{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState fs{XR_TYPE_FRAME_STATE};
        r = xr.xrWaitFrame(session, &fwi, &fs);
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (r != XR_SUCCESS) {
            Check(false, "xrWaitFrame -> %d", (int)r);
            break;
        }
        timing.push_back({now.QuadPart, fs.predictedDisplayTime, fs.predictedDisplayPeriod});
        XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO};
        r = xr.xrBeginFrame(session, &fbi);
        if (r != XR_SUCCESS) Check(false, "xrBeginFrame -> %d (frame %d)", (int)r, frame);

        // ---- input
        XrActiveActionSet aas{aset, XR_NULL_PATH};
        XrActionsSyncInfo syi{XR_TYPE_ACTIONS_SYNC_INFO};
        syi.countActiveActionSets = 1;
        syi.activeActionSets = &aas;
        const XrResult sr = xr.xrSyncActions(session, &syi);
        auto getF = [&](XrAction a, XrPath sub) {
            XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
            gi.action = a;
            gi.subactionPath = sub;
            XrActionStateFloat st{XR_TYPE_ACTION_STATE_FLOAT};
            xr.xrGetActionStateFloat(session, &gi, &st);
            return st;
        };
        auto getB = [&](XrAction a, XrPath sub) {
            XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
            gi.action = a;
            gi.subactionPath = sub;
            XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
            xr.xrGetActionStateBoolean(session, &gi, &st);
            return st;
        };
        auto getV = [&](XrAction a, XrPath sub) {
            XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
            gi.action = a;
            gi.subactionPath = sub;
            XrActionStateVector2f st{XR_TYPE_ACTION_STATE_VECTOR2F};
            xr.xrGetActionStateVector2f(session, &gi, &st);
            return st;
        };
        auto locate = [&](XrSpace s, XrSpace base) {
            XrSpaceLocation l{XR_TYPE_SPACE_LOCATION};
            xr.xrLocateSpace(s, base, fs.predictedDisplayTime, &l);
            return l;
        };
        const XrSpaceLocationFlags valid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        if (P.id == 'A' && O.selftest) {
            if (frame == 0) Check(sr == XR_SESSION_NOT_FOCUSED, "frame 0: xrSyncActions before FOCUSED -> XR_SESSION_NOT_FOCUSED");
            if (frame == 10) {
                const auto t = getF(aTrigger, right);
                const auto b = getB(aButton, XR_NULL_PATH);
                Check(sr == XR_SUCCESS && t.isActive && t.currentState == 0.0f && b.isActive && !b.currentState,
                      "frame 10: focused, trigger 0, button up");
            }
            if (frame == 30) Check(getF(aTrigger, right).changedSinceLastSync == XR_TRUE, "frame 30: trigger changedSinceLastSync");
            if (frame == 40) {
                const auto tr = getF(aTrigger, right), tl = getF(aTrigger, left), ta = getF(aTrigger, XR_NULL_PATH);
                const auto b = getB(aButton, XR_NULL_PATH);
                const auto st = getV(aStick, left);
                Check(tr.currentState == 0.75f && tl.currentState == 0.0f && ta.currentState == 0.75f,
                      "frame 40: trigger right %.3f left %.3f any %.3f (script 0.75 / 0 / 0.75)", tr.currentState, tl.currentState, ta.currentState);
                Check(b.currentState == XR_TRUE && !b.changedSinceLastSync, "frame 40: 'a' button held");
                Check(st.isActive && st.currentState.x == 0.5f && st.currentState.y == -0.25f, "frame 40: left thumbstick (%.3f, %.3f)",
                      st.currentState.x, st.currentState.y);
                XrHapticActionInfo hai{XR_TYPE_HAPTIC_ACTION_INFO};
                hai.action = aHaptic;
                hai.subactionPath = right;
                XrHapticVibration hv{XR_TYPE_HAPTIC_VIBRATION};
                hv.amplitude = 0.5f;
                hv.duration = XR_MIN_HAPTIC_DURATION;
                hv.frequency = XR_FREQUENCY_UNSPECIFIED;
                Check(xr.xrApplyHapticFeedback(session, &hai, reinterpret_cast<XrHapticBaseHeader*>(&hv)) == XR_SUCCESS, "haptic feedback accepted");
                XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
                gi.action = aTrigger;
                XrActionStateBoolean wrong{XR_TYPE_ACTION_STATE_BOOLEAN};
                Check(xr.xrGetActionStateBoolean(session, &gi, &wrong) == XR_ERROR_ACTION_TYPE_MISMATCH, "boolean read of a float action -> XR_ERROR_ACTION_TYPE_MISMATCH");
            }
            if (frame == 55) Check(getF(aTrigger, right).currentState == 0.25f, "frame 55: trigger 0.25");
            if (frame >= 80 && frame < 90) {
                notFocusedSyncs += sr == XR_SESSION_NOT_FOCUSED;
                inactiveWhileUnfocused += !getF(aTrigger, right).isActive;
            }
            if (frame == 95) Check(sr == XR_SUCCESS && getF(aTrigger, right).isActive, "frame 95: focus regained, actions active");
            if (frame == 50) {
                // head: lerp @0 (0,1.6,0) ypr 0 -> @100 (0.3,1.7,-0.5) ypr (30,10,0): halfway
                const Q q1 = QYawPitch(30, 10);
                const Q qh = [&] {
                    Q h{q1.x, q1.y, q1.z, q1.w + 1.0};
                    const double n = std::sqrt(h.x * h.x + h.y * h.y + h.z * h.z + h.w * h.w);
                    return Q{h.x / n, h.y / n, h.z / n, h.w / n};
                }();
                const auto vl = locate(view, local);
                Check((vl.locationFlags & valid) == valid && Near(vl.pose.position.x, 0.15) && Near(vl.pose.position.y, 0.05) &&
                          Near(vl.pose.position.z, -0.25) && QNear(vl.pose.orientation, qh),
                      "frame 50: VIEW in LOCAL = (%.4f, %.4f, %.4f) (expected (0.15, 0.05, -0.25), half-way rotation)", vl.pose.position.x,
                      vl.pose.position.y, vl.pose.position.z);
                const auto vs = locate(view, stage);
                Check(Near(vs.pose.position.y, 1.65), "frame 50: VIEW in STAGE y = %.4f (expected 1.65)", vs.pose.position.y);
                if (localFloor) Check(Near(locate(view, localFloor).pose.position.y, 1.65), "frame 50: VIEW in LOCAL_FLOOR y = 1.65");
                const auto hr = locate(handSpace[1], local);
                Check((hr.locationFlags & valid) == valid && Near(hr.pose.position.x, 0.2) && Near(hr.pose.position.y, -0.3) &&
                          Near(hr.pose.position.z, -0.4),
                      "frame 50: right grip in LOCAL = (%.3f, %.3f, %.3f)", hr.pose.position.x, hr.pose.position.y, hr.pose.position.z);
                XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
                vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                vli.displayTime = fs.predictedDisplayTime;
                vli.space = local;
                XrViewState vst{XR_TYPE_VIEW_STATE};
                XrView vv[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
                uint32_t nv = 0;
                xr.xrLocateViews(session, &vli, &vst, 2, &nv, vv);
                const double dx = vv[1].pose.position.x - vv[0].pose.position.x, dy = vv[1].pose.position.y - vv[0].pose.position.y,
                             dz = vv[1].pose.position.z - vv[0].pose.position.z;
                const double left0[3] = {-0.0315, 0, 0};
                double off[3];
                Rotate(qh, left0, off);
                Check(Near(std::sqrt(dx * dx + dy * dy + dz * dz), 0.063) && Near(vv[0].pose.position.x, 0.15 + off[0]) &&
                          Near(vv[0].pose.position.z, -0.25 + off[2]) && vv[0].fov.angleLeft < 0 && vv[1].fov.angleRight > 0,
                      "frame 50: xrLocateViews eye separation %.4f m, left eye offset along the head's rotated X",
                      std::sqrt(dx * dx + dy * dy + dz * dz));
                LARGE_INTEGER pc{}, nowPc;
                QueryPerformanceCounter(&nowPc);
                const XrResult cr = xr.xrConvertTimeToWin32PerformanceCounterKHR(inst, fs.predictedDisplayTime, &pc);
                const double ahead = (double)(pc.QuadPart - nowPc.QuadPart) / (double)freq.QuadPart;
                XrTime back = 0;
                xr.xrConvertWin32PerformanceCounterToTimeKHR(inst, &pc, &back);
                Check(cr == XR_SUCCESS && ahead > 0 && ahead <= 3.0 * fs.predictedDisplayPeriod / 1e9 &&
                          std::llabs(back - fs.predictedDisplayTime) <= 1000000000LL / freq.QuadPart + 1,
                      "frame 50: predictedDisplayTime is %.2f ms ahead of QueryPerformanceCounter; round trip error %lld ns", ahead * 1e3,
                      (long long)std::llabs(back - fs.predictedDisplayTime));
            }
            if (frame == 65) Check((locate(handSpace[0], local).locationFlags & valid) == 0, "frame 65: left controller untracked -> location flags 0");
            if (frame == 75) Check((locate(handSpace[0], local).locationFlags & valid) == valid, "frame 75: left controller tracked again");
            if (frame == 5) Check(xr.xrEndSession(session) == XR_ERROR_SESSION_NOT_STOPPING, "xrEndSession while FOCUSED -> XR_ERROR_SESSION_NOT_STOPPING");
        }
        if (P.id == 'B' && O.selftest) {
            if (frame == 10) Check(sr == XR_SUCCESS && getB(aButton, XR_NULL_PATH).isActive && !getB(aButton, XR_NULL_PATH).currentState,
                                   "frame 10: select (simple_controller) up");
            if (frame == 30) Check(getB(aButton, XR_NULL_PATH).currentState == XR_TRUE, "frame 30: select down (right trigger 0.8 > 0.5)");
            if (frame == 30) Check((locate(handSpace[1], local).locationFlags & valid) == valid, "frame 30: right aim pose located");
        }
        if (frame == P.requestRefreshFrame) {
            float cur = 0;
            xr.xrGetDisplayRefreshRateFB(session, &cur);
            Check(cur == (float)P.hz, "refresh rate %.1f Hz before the request", cur);
            Check(xr.xrRequestDisplayRefreshRateFB(session, 75.0f) == XR_ERROR_DISPLAY_REFRESH_RATE_UNSUPPORTED_FB, "75 Hz -> XR_ERROR_DISPLAY_REFRESH_RATE_UNSUPPORTED_FB");
            Check(xr.xrRequestDisplayRefreshRateFB(session, (float)P.hzAfter) == XR_SUCCESS, "request %.0f Hz", P.hzAfter);
            Check(xr.xrPerfSettingsSetPerformanceLevelEXT(session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, XR_PERF_SETTINGS_LEVEL_BOOST_EXT) == XR_SUCCESS,
                  "XR_EXT_performance_settings level accepted");
        }
        if (frame == P.requestExitFrame) Check(xr.xrRequestExitSession(session) == XR_SUCCESS, "xrRequestExitSession at frame %d", frame);

        // ---- render
        std::vector<XrCompositionLayerBaseHeader*> layers;
        XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        XrCompositionLayerDepthInfoKHR di[2] = {{XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR}, {XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR}};
        XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
        if (fs.shouldRender) {
            XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
            vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            vli.displayTime = fs.predictedDisplayTime;
            vli.space = local;
            XrViewState vst{XR_TYPE_VIEW_STATE};
            XrView vv[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
            uint32_t nv = 0;
            xr.xrLocateViews(session, &vli, &vst, 2, &nv, vv);
            uint32_t ci[2] = {0, 0}, dIdx = 0, qIdx = 0;
            ci[0] = Acquire(xr, color[0]);
            if (!P.multiview) ci[1] = Acquire(xr, color[1]);
            if (P.depth) dIdx = Acquire(xr, depthSc);
            if (P.quad) qIdx = Acquire(xr, quadSc);

            vkResetCommandBuffer(g.cb, 0);
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(g.cb, &bi);
            VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            vkCmdPipelineBarrier(g.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
            struct PC { float colA[2][4]; float colB[2][4]; int32_t eyeBase; int32_t cell; } pc{};
            for (int e = 0; e < 2; ++e) {
                const RGB8 a = ColA(e, frame), b = ColB(e, frame);
                const float av[4] = {ShaderValue(a.r, srgb), ShaderValue(a.g, srgb), ShaderValue(a.b, srgb), 1.0f};
                const float bv[4] = {ShaderValue(b.r, srgb), ShaderValue(b.g, srgb), ShaderValue(b.b, srgb), 1.0f};
                memcpy(pc.colA[e], av, sizeof(av));
                memcpy(pc.colB[e], bv, sizeof(bv));
            }
            pc.cell = kCell;
            const RGB8 cc = ColC(frame);
            VkClearValue clear{};
            clear.color = {{ShaderValue(cc.r, srgb), ShaderValue(cc.g, srgb), ShaderValue(cc.b, srgb), 1.0f}};
            const int passes = P.multiview ? 1 : 2;
            for (int p = 0; p < passes; ++p) {
                VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                ca.imageView = color[p].views[ci[p]];
                ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                ca.clearValue = clear;
                VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                if (P.depth) {
                    da.imageView = depthSc.views[dIdx];
                    da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                    da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                    da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                    da.clearValue.depthStencil = {1.0f, 0};
                }
                VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
                ri.renderArea = {{0, 0}, {W, H}};
                ri.layerCount = 1;
                ri.viewMask = P.multiview ? 3u : 0u;
                ri.colorAttachmentCount = 1;
                ri.pColorAttachments = &ca;
                ri.pDepthAttachment = P.depth ? &da : nullptr;
                vkCmdBeginRendering(g.cb, &ri);
                const VkViewport vpt{0, 0, (float)W, (float)H, 0, 1};
                const VkRect2D sc{{kMargin, kMargin}, {W - 2 * kMargin, H - 2 * kMargin}};
                vkCmdSetViewport(g.cb, 0, 1, &vpt);
                vkCmdSetScissor(g.cb, 0, 1, &sc);
                vkCmdBindPipeline(g.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipe);
                pc.eyeBase = p;
                vkCmdPushConstants(g.cb, g.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
                vkCmdDraw(g.cb, 3, 1, 0, 0);
                vkCmdEndRendering(g.cb);
            }
            if (P.quad) {
                const RGB8 qc = ColQ(frame);
                VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                ca.imageView = quadSc.views[qIdx];
                ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                ca.clearValue.color = {{ShaderValue(qc.r, true), ShaderValue(qc.g, true), ShaderValue(qc.b, true), 1.0f}};
                VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
                ri.renderArea = {{0, 0}, {QW, QH}};
                ri.layerCount = 1;
                ri.colorAttachmentCount = 1;
                ri.pColorAttachments = &ca;
                vkCmdBeginRendering(g.cb, &ri);
                vkCmdEndRendering(g.cb);
            }
            vkEndCommandBuffer(g.cb);
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.commandBufferCount = 1;
            si.pCommandBuffers = &g.cb;
            vkQueueSubmit(g.q, 1, &si, g.fence);
            vkWaitForFences(g.dev, 1, &g.fence, VK_TRUE, UINT64_MAX);
            vkResetFences(g.dev, 1, &g.fence);
            Release(xr, color[0]);
            if (!P.multiview) Release(xr, color[1]);
            if (P.depth) Release(xr, depthSc);
            if (P.quad) Release(xr, quadSc);

            for (int e = 0; e < 2; ++e) {
                pv[e].pose = vv[e].pose;
                pv[e].fov = vv[e].fov;
                pv[e].subImage.swapchain = P.multiview ? color[0].sc : color[e].sc;
                pv[e].subImage.imageRect = {{0, 0}, {(int32_t)W, (int32_t)H}};
                pv[e].subImage.imageArrayIndex = P.multiview ? (uint32_t)e : 0u;
                if (P.depth) {
                    di[e].subImage.swapchain = depthSc.sc;
                    di[e].subImage.imageRect = pv[e].subImage.imageRect;
                    di[e].subImage.imageArrayIndex = (uint32_t)e;
                    di[e].minDepth = 0.0f;
                    di[e].maxDepth = 1.0f;
                    di[e].nearZ = 0.05f;
                    di[e].farZ = 100.0f;
                    pv[e].next = &di[e];
                }
            }
            proj.space = local;
            proj.viewCount = 2;
            proj.views = pv;
            layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&proj));
            if (P.quad) {
                quad.space = view;
                quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                quad.subImage.swapchain = quadSc.sc;
                quad.subImage.imageRect = {{0, 0}, {(int32_t)QW, (int32_t)QH}};
                quad.pose.orientation.w = 1;
                quad.pose.position = {0.0f, -0.2f, -1.0f};
                quad.size = {0.4f, 0.2f};
                layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad));
            }
        }
        XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
        fei.displayTime = fs.predictedDisplayTime;
        fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        fei.layerCount = (uint32_t)layers.size();
        fei.layers = layers.data();
        r = xr.xrEndFrame(session, &fei);
        if (r != XR_SUCCESS) Check(false, "xrEndFrame -> %d (frame %d)", (int)r, frame);
        ++frame;
        if (!O.selftest && O.frames > 0 && frame == O.frames) xr.xrRequestExitSession(session);
    }
    printf("  %d frames\n", frame);
    fflush(stdout);

    // ---- teardown (the runtime flushes its PNG writer when the session is destroyed)
    DestroySwapchain(xr, g, color[0]);
    DestroySwapchain(xr, g, color[1]);
    DestroySwapchain(xr, g, depthSc);
    DestroySwapchain(xr, g, quadSc);
    for (XrSpace s : {local, view, stage, localFloor, handSpace[0], handSpace[1]})
        if (s) xr.xrDestroySpace(s);
    Check(xr.xrDestroySession(session) == XR_SUCCESS, "xrDestroySession");
    xr.xrDestroyActionSet(aset);
    Check(xr.xrDestroyInstance(inst) == XR_SUCCESS, "xrDestroyInstance");
    DestroyVulkan(g);
    if (g.validation || O.validation) Check(g_vkErrors == 0, "Vulkan validation: %d error(s), %d warning(s)", g_vkErrors, g_vkWarnings);
    g_vkErrors = g_vkWarnings = 0;
    if (!O.selftest) return;

    // ---- session state sequence
    std::vector<XrSessionState> expect;
    if (P.id == 'A')
        expect = {XR_SESSION_STATE_IDLE,     XR_SESSION_STATE_READY,        XR_SESSION_STATE_SYNCHRONIZED, XR_SESSION_STATE_VISIBLE,
                  XR_SESSION_STATE_FOCUSED,  XR_SESSION_STATE_VISIBLE,      XR_SESSION_STATE_FOCUSED,      XR_SESSION_STATE_VISIBLE,
                  XR_SESSION_STATE_SYNCHRONIZED, XR_SESSION_STATE_STOPPING, XR_SESSION_STATE_IDLE,         XR_SESSION_STATE_EXITING};
    else
        expect = {XR_SESSION_STATE_IDLE,    XR_SESSION_STATE_READY,        XR_SESSION_STATE_SYNCHRONIZED, XR_SESSION_STATE_VISIBLE,
                  XR_SESSION_STATE_FOCUSED, XR_SESSION_STATE_VISIBLE,      XR_SESSION_STATE_SYNCHRONIZED, XR_SESSION_STATE_STOPPING,
                  XR_SESSION_STATE_IDLE,    XR_SESSION_STATE_EXITING};
    std::string seq;
    for (XrSessionState s : states) seq += std::string(seq.empty() ? "" : " ") + StateName(s);
    Check(states == expect, "session states: %s", seq.c_str());
    Check(profileEvents == 1, "one XrEventDataInteractionProfileChanged (%d)", profileEvents);
    if (P.id == 'A') {
        Check(notFocusedSyncs == 10 && inactiveWhileUnfocused == 10, "frames 80-89 (script focus_loss): xrSyncActions -> XR_SESSION_NOT_FOCUSED, actions inactive (%d/%d)",
              notFocusedSyncs, inactiveWhileUnfocused);
        Check(refreshEvents == 1 && refreshTo == (float)P.hzAfter, "XrEventDataDisplayRefreshRateChangedFB to %.1f Hz", refreshTo);
    }

    // ---- timing: app-side (xrWaitFrame return times) and the runtime's CSV
    const int64_t period = std::llround(1e9 / P.hz);
    if ((int)timing.size() > P.timingTo) {
        const double meanMs = (double)(timing[(size_t)P.timingTo].waitReturnQpc - timing[(size_t)P.timingFrom].waitReturnQpc) /
                              (double)freq.QuadPart * 1e3 / (P.timingTo - P.timingFrom);
        double maxDev = 0;
        for (int i = P.timingFrom + 1; i <= P.timingTo; ++i) {
            const double d = (double)(timing[(size_t)i].waitReturnQpc - timing[(size_t)i - 1].waitReturnQpc) / (double)freq.QuadPart * 1e3;
            maxDev = std::max(maxDev, std::fabs(d - period / 1e6));
        }
        Check(std::fabs(meanMs - period / 1e6) <= 0.5, "%.0f Hz: mean xrWaitFrame interval %.4f ms over frames %d-%d (expected %.4f +- 0.5; max deviation %.3f ms)",
              P.hz, meanMs, P.timingFrom, P.timingTo, period / 1e6, maxDev);
        bool steps = true;
        int bad = -1;
        for (int i = 1; i <= P.timingTo; ++i)
            if (timing[(size_t)i].display - timing[(size_t)i - 1].display != period || timing[(size_t)i].period != period) {
                steps = false;
                if (bad < 0) bad = i;
            }
        Check(steps, "predictedDisplayTime steps exactly %lld ns for frames 0-%d%s", (long long)period, P.timingTo,
              steps ? "" : (" (first deviation at frame " + std::to_string(bad) + ")").c_str());
        if (P.hzAfter > 0 && P.requestRefreshFrame >= 0 && (int)timing.size() > P.requestRefreshFrame + 5) {
            const int64_t p2 = std::llround(1e9 / P.hzAfter);
            const size_t k = (size_t)P.requestRefreshFrame + 1;
            Check(timing[k].display - timing[k - 1].display == p2 && timing[k + 3].period == p2,
                  "after the refresh request: step %lld ns (expected %lld)", (long long)(timing[k].display - timing[k - 1].display), (long long)p2);
        }
    } else {
        Check(false, "not enough frames for the timing check (%zu)", timing.size());
    }
    const auto csv = ReadCsv(outDir + "\\xrsim_frames.csv");
    int csvRows = 0, csvRender = 0;
    bool csvMono = true, csvLayers = true, csvMatches = true;
    const std::string wantLayers = P.quad ? (P.depth ? "P2d|Q" : "P2|Q") : (P.depth ? "P2d" : "P2");
    int64_t prevDisplay = 0;
    for (const auto& row : csv) {
        if (row.size() < 14) continue;
        const int f = std::stoi(row[0]);
        const int64_t display = std::stoll(row[4]);
        ++csvRows;
        if (prevDisplay && display <= prevDisplay) csvMono = false;
        prevDisplay = display;
        if (f < (int)timing.size() && timing[(size_t)f].display != display) csvMatches = false;
        if (row[7] == "1") {
            ++csvRender;
            if (row[12] != wantLayers) csvLayers = false;
        }
    }
    Check(csvRows == frame && csvMono && csvMatches, "xrsim_frames.csv: %d rows (frames %d), display times strictly increasing and equal to the app's",
          csvRows, frame);
    Check(csvRender > 0 && csvLayers, "xrsim_frames.csv: %d rendered frames, layers '%s'", csvRender, wantLayers.c_str());

    // ---- captures
    for (int f : P.captures) {
        char n[64];
        for (int e = 0; e < 2; ++e) {
            snprintf(n, sizeof(n), "\\f%06d_l0_proj_v%d.png", f, e);
            VerifyEyePng(outDir + n, e, f, W, H, srgb);
        }
        if (P.quad) {
            snprintf(n, sizeof(n), "\\f%06d_l1_quad.png", f);
            VerifyQuadPng(outDir + n, f, QW, QH, true);
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    // stdout stays buffered: an unbuffered redirected stdout costs ~8 ms per line (measured) and breaks frame pacing
    setvbuf(stdout, nullptr, _IOFBF, 1 << 16);
    Options O;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--selftest") O.selftest = true;
        else if (a == "--run") O.selftest = false;
        else if (a == "--window") O.window = true;
        else if (a == "--no-validation") O.validation = false;
        else if (a == "--direct") O.direct = true;
        else if (a == "--out" && i + 1 < argc) O.out = argv[++i];
        else if (a == "--loader" && i + 1 < argc) O.loader = argv[++i];
        else if (a == "--frames" && i + 1 < argc) O.frames = atoi(argv[++i]);
        else {
            printf("usage: xrsim_test --selftest [--out DIR] [--window] [--no-validation] [--loader openxr_loader.dll]\n"
                   "       xrsim_test --run [--frames N] [--direct] [--loader openxr_loader.dll] [--no-validation]\n");
            return 2;
        }
    }
    const std::string exeDir = ExeDir();
    if (!O.selftest) {
        g_checks = false;
        Phase P;
        P.id = 'R';
        P.name = "demo client (environment as set by the caller)";
        P.loader = !O.direct;
        P.api = XR_MAKE_VERSION(1, 0, 0);
        RunPhase(P, O, "");
        return 0;
    }
    const std::string out = O.out.empty() ? exeDir + "\\xrsim_selftest" : O.out;
    CreateDirectoryA(out.c_str(), nullptr);
    const bool haveLoader = !O.loader.empty() || FileExists(exeDir + "\\openxr_loader.dll");

    Phase A;
    A.id = 'A';
    A.name = "official loader, OpenXR 1.1, XR_KHR_vulkan_enable2, multiview SRGB + depth + quad, Touch, 72 Hz";
    A.loader = haveLoader;
    A.expectW = 1680;
    A.expectH = 1760;
    A.hz = 72.0;
    A.hzAfter = 90.0;
    A.requestRefreshFrame = 160;
    A.timingFrom = 10;
    A.timingTo = 159;
    A.captures = {20, 60, 120};
    A.maxFrames = 260;
    A.script =
        "# xrsim_test phase A (generated)\n"
        "refresh 72\n"
        "@0 head pos 0 1.6 0 ypr 0 0 0\n"
        "@100 head pos 0.3 1.7 -0.5 ypr 30 10 0 lerp\n"
        "@0 right pos 0.2 1.3 -0.4\n"
        "@0 left pos -0.2 1.3 -0.4\n"
        "@30 right trigger 0.75\n"
        "@30 right a 1\n"
        "@30 left thumbstick 0.5 -0.25\n"
        "@50 right trigger 0.25\n"
        "@60 left tracked 0\n"
        "@70 left tracked 1\n"
        "@80 event focus_loss\n"
        "@90 event focus_gain\n"
        "capture 20 60 120\n"
        "@200 event exit\n";
    if (!haveLoader) printf("NOTE: no openxr_loader.dll next to the test and no --loader: phase A uses the direct runtime path\n");
    RunPhase(A, O, out + "\\phaseA");

    Phase B;
    B.id = 'B';
    B.name = "direct runtime load, OpenXR 1.0, XR_KHR_vulkan_enable, two UNORM swapchains, simple_controller, 90 Hz";
    B.loader = false;
    B.enable2 = false;
    B.multiview = false;
    B.depth = false;
    B.quad = false;
    B.touch = false;
    B.color = VK_FORMAT_B8G8R8A8_UNORM;
    B.api = XR_MAKE_VERSION(1, 0, 0);
    B.expectW = 800;
    B.expectH = 600;
    B.hz = 90.0;
    B.timingFrom = 10;
    B.timingTo = 59;
    B.requestExitFrame = 60;
    B.captures = {30};
    B.maxFrames = 120;
    B.script =
        "# xrsim_test phase B (generated)\n"
        "refresh 90\n"
        "resolution 800 600\n"
        "@20 right trigger 0.8\n"
        "capture 30\n";
    RunPhase(B, O, out + "\\phaseB");

    printf("\nxrsim self-test: %d passed, %d failed%s\n", g_pass, g_fail, haveLoader ? "" : " (loader path NOT exercised)");
    return g_fail == 0 ? 0 : 1;
}
