// The OpenXR session of the game (xr_session.h; docs/research/vr_port_plan.md, M1).
//
// Order of the start-up, which is the order the specification requires: load the loader -> instance with the
// extensions the runtime offers -> system (HMD) -> graphics requirements -> VkInstance and VkDevice through
// xrCreateVulkanInstanceKHR / xrCreateVulkanDeviceKHR -> session on that Vulkan binding -> reference spaces ->
// the quad swapchain. Everything the runtime does not offer (display refresh rate, performance settings, the
// STAGE space) is optional and only logged; XR_KHR_vulkan_enable2 is not.
#ifdef _WIN32
#include <windows.h>
#include <unknwn.h> // XR_USE_PLATFORM_WIN32 declares structures that name IUnknown
#endif
#ifdef __ANDROID__
#include <jni.h> // XR_USE_PLATFORM_ANDROID declares structures that name JavaVM / jobject
#include <time.h>
#endif

#include <vulkan/vulkan.h>

#ifdef _WIN32
#define XR_NO_PROTOTYPES // Windows: the loader is a DLL opened at run time, every function comes from xrGetInstanceProcAddr
#define XR_USE_PLATFORM_WIN32
#endif
#ifdef __ANDROID__
// Android: the Khronos loader is a library of the APK (openxr_loader_for_android), linked like any other - only
// xrGetInstanceProcAddr is taken from it directly, everything else still goes through the table below.
#define XR_USE_PLATFORM_ANDROID
#define XR_USE_TIMESPEC
#endif
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "platform/xr/xr_session.h"
#include "gt2view/shading_rate.h"
#ifdef __ANDROID__
#include "platform/xr/xr_actions.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace gt2::xr {
namespace {

using Clock = std::chrono::steady_clock;

// The functions the session uses; every one is fetched from the runtime through xrGetInstanceProcAddr.
#define GT2XR_FUNCTIONS(X)                                                                                              \
    X(xrDestroyInstance) X(xrGetInstanceProperties) X(xrPollEvent) X(xrResultToString) X(xrGetSystem)                    \
    X(xrGetSystemProperties) X(xrEnumerateViewConfigurationViews) X(xrCreateSession) X(xrDestroySession)                 \
    X(xrBeginSession) X(xrEndSession) X(xrRequestExitSession) X(xrWaitFrame) X(xrBeginFrame) X(xrEndFrame)               \
    X(xrLocateViews) X(xrEnumerateReferenceSpaces) X(xrCreateReferenceSpace) X(xrLocateSpace) X(xrDestroySpace)          \
    X(xrEnumerateSwapchainFormats) X(xrCreateSwapchain) X(xrDestroySwapchain) X(xrEnumerateSwapchainImages)              \
    X(xrAcquireSwapchainImage) X(xrWaitSwapchainImage) X(xrReleaseSwapchainImage)

#define GT2XR_OPTIONAL_FUNCTIONS(X)                                                                                     \
    X(xrGetVulkanGraphicsRequirements2KHR) X(xrCreateVulkanInstanceKHR) X(xrCreateVulkanDeviceKHR)                       \
    X(xrGetVulkanGraphicsDevice2KHR) X(xrEnumerateDisplayRefreshRatesFB) X(xrGetDisplayRefreshRateFB)                    \
    X(xrRequestDisplayRefreshRateFB) X(xrPerfSettingsSetPerformanceLevelEXT)

struct Api {
    PFN_xrGetInstanceProcAddr gipa = nullptr;
    PFN_xrEnumerateInstanceExtensionProperties xrEnumerateInstanceExtensionProperties = nullptr;
    PFN_xrCreateInstance xrCreateInstance = nullptr;
#define GT2XR_DECL(n) PFN_##n n = nullptr;
    GT2XR_FUNCTIONS(GT2XR_DECL)
    GT2XR_OPTIONAL_FUNCTIONS(GT2XR_DECL)
#ifdef _WIN32
    PFN_xrConvertTimeToWin32PerformanceCounterKHR xrConvertTimeToWin32PerformanceCounterKHR = nullptr;
#endif
#ifdef __ANDROID__
    // The same service on Android: XrTime -> CLOCK_MONOTONIC, which is what std::chrono::steady_clock reads there.
    PFN_xrConvertTimeToTimespecTimeKHR xrConvertTimeToTimespecTimeKHR = nullptr;
#endif
#undef GT2XR_DECL
};

std::string ResultText(const Api& api, XrInstance instance, XrResult r) {
    char buf[XR_MAX_RESULT_STRING_SIZE] = {};
    if (api.xrResultToString && instance != XR_NULL_HANDLE && api.xrResultToString(instance, r, buf) == XR_SUCCESS) return buf;
    return std::to_string(int(r));
}

// GT2_VK_VALIDATION=1: the Khronos validation layer's messages on the console (development aid, off by default).
VKAPI_ATTR VkBool32 VKAPI_CALL ValidationMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                                 const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT))
        std::printf("vulkan %s: %s\n", (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ? "error" : "warning",
                    data && data->pMessage ? data->pMessage : "?");
    return VK_FALSE;
}

XrPosef IdentityPose() {
    XrPosef p{};
    p.orientation.w = 1.0f;
    return p;
}

} // namespace

struct Session::Impl {
#ifdef _WIN32
    HMODULE loader = nullptr;
#else
    void* loader = nullptr;
#endif
    Api api;
#ifdef __ANDROID__
    std::unique_ptr<ControllerActions> controls;
#endif
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace localSpace = XR_NULL_HANDLE, viewSpace = XR_NULL_HANDLE, stageSpace = XR_NULL_HANDLE;
    XrSwapchain quad = XR_NULL_HANDLE;
    std::vector<VkImage> quadImages;
    XrFrameState frameState{XR_TYPE_FRAME_STATE};
    XrPosef quadPose = IdentityPose();
    uint32_t quadIndex = 0;
    bool quadAcquired = false;

    // The stereo projection layer (M2): one colour swapchain of arraySize 2 and, when the runtime offers
    // XR_KHR_composition_layer_depth, a depth swapchain of the same shape.
    XrSwapchain stereo = XR_NULL_HANDLE, stereoDepth = XR_NULL_HANDLE;
    std::vector<VkImage> stereoImages, stereoDepthImages;
    uint32_t stereoIndex = 0, stereoDepthIndex = 0;
    bool stereoAcquired = false;
    VkImage lastDirectImage = VK_NULL_HANDLE;

    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;

    // XrTime (nanoseconds of the performance counter on Windows) -> the steady clock.
    Clock::time_point steadyEpoch{};
    long long qpcFrequency = 0;

    void Check(XrResult r, const char* what) const {
        if (XR_FAILED(r)) throw std::runtime_error(std::string("OpenXR: ") + what + " failed (" + ResultText(api, instance, r) + ")");
    }
};

namespace {

// The loader: the copy next to the executable first (CMake puts the build the xrsim tools use there), then whatever
// the system has. Nothing is downloaded and nothing is searched for outside these two places.
#ifdef _WIN32
HMODULE LoadLoader(std::string& tried) {
    std::vector<std::string> candidates;
    char exePath[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    if (n > 0) {
        std::string dir(exePath, n);
        const size_t slash = dir.find_last_of("\\/");
        if (slash != std::string::npos) candidates.push_back(dir.substr(0, slash + 1) + "openxr_loader.dll");
    }
#ifdef GT2_OPENXR_LOADER_PATH
    candidates.push_back(GT2_OPENXR_LOADER_PATH);
#endif
    candidates.push_back("openxr_loader.dll");
    for (const std::string& c : candidates) {
        if (HMODULE m = LoadLibraryA(c.c_str())) return m;
        tried += (tried.empty() ? "" : ", ") + c;
    }
    return nullptr;
}
#endif

} // namespace

const char* Session::StateName() const {
    switch (XrSessionState(state_)) {
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

Session::Session(const SessionOptions& options) : impl_(std::make_unique<Impl>()), options_(options) {
    Impl& s = *impl_;
#ifdef _WIN32
    std::string tried;
    s.loader = LoadLoader(tried);
    if (!s.loader) throw std::runtime_error("no OpenXR loader (tried " + tried + "): --vr needs an openxr_loader.dll next to the executable");
    s.api.gipa = reinterpret_cast<PFN_xrGetInstanceProcAddr>(reinterpret_cast<void*>(GetProcAddress(s.loader, "xrGetInstanceProcAddr")));
    if (!s.api.gipa) throw std::runtime_error("the OpenXR loader has no xrGetInstanceProcAddr");
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    s.qpcFrequency = freq.QuadPart;
    { // the steady clock's epoch in performance-counter ticks (both are the same counter on Windows)
        LARGE_INTEGER counter;
        QueryPerformanceCounter(&counter);
        const Clock::time_point now = Clock::now();
        const auto ticks = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(double(counter.QuadPart) / double(s.qpcFrequency)));
        s.steadyEpoch = now - ticks;
    }
#elif defined(__ANDROID__)
    // The loader is linked into the APK, but on Android it must be initialised with the application's Java objects
    // before anything else is asked of it (XR_KHR_loader_init): that is how it finds the installed runtime through
    // the system's broker.
    if (!options_.androidVm || !options_.androidActivity)
        throw std::runtime_error("OpenXR (Android): the session needs the activity's JavaVM and its instance (android_main sets SessionOptions::androidVm / androidActivity)");
    {
        PFN_xrInitializeLoaderKHR initializeLoader = nullptr;
        const XrResult got = xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", reinterpret_cast<PFN_xrVoidFunction*>(&initializeLoader));
        if (XR_FAILED(got) || !initializeLoader)
            throw std::runtime_error("OpenXR (Android): the loader has no xrInitializeLoaderKHR (result " + std::to_string(int(got)) + ")");
        XrLoaderInitInfoAndroidKHR init{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
        init.applicationVM = options_.androidVm;
        init.applicationContext = options_.androidActivity;
        const XrResult r0 = initializeLoader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&init));
        if (XR_FAILED(r0)) throw std::runtime_error("OpenXR (Android): xrInitializeLoaderKHR failed (" + std::to_string(int(r0)) + ")");
    }
    s.api.gipa = &xrGetInstanceProcAddr;
#else
    throw std::runtime_error("the OpenXR session is implemented for Windows and Android only");
#endif

    s.api.gipa(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties",
               reinterpret_cast<PFN_xrVoidFunction*>(&s.api.xrEnumerateInstanceExtensionProperties));
    s.api.gipa(XR_NULL_HANDLE, "xrCreateInstance", reinterpret_cast<PFN_xrVoidFunction*>(&s.api.xrCreateInstance));
    if (!s.api.xrEnumerateInstanceExtensionProperties || !s.api.xrCreateInstance)
        throw std::runtime_error("the OpenXR loader found no runtime (set XR_RUNTIME_JSON, or install a runtime)");

    uint32_t extCount = 0;
    XrResult r = s.api.xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr);
    if (XR_FAILED(r)) throw std::runtime_error("OpenXR: no runtime available (xrEnumerateInstanceExtensionProperties -> " + std::to_string(int(r)) + ")");
    std::vector<XrExtensionProperties> props(extCount, {XR_TYPE_EXTENSION_PROPERTIES});
    s.api.xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, props.data());
    auto has = [&](const char* name) {
        for (const XrExtensionProperties& p : props)
            if (std::strcmp(p.extensionName, name) == 0) return true;
        return false;
    };
    if (!has(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME))
        throw std::runtime_error("the OpenXR runtime does not offer " XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME " (Vulkan is the only backend of this game)");
    std::vector<const char*> extensions{XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
#ifdef _WIN32
    info_.timeConversion = has(XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME);
    if (info_.timeConversion) extensions.push_back(XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME);
#endif
#ifdef __ANDROID__
    info_.timeConversion = has(XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME);
    if (info_.timeConversion) extensions.push_back(XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME);
    const bool androidCreateInstance = has(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME);
    if (androidCreateInstance) extensions.push_back(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME);
#endif
    info_.refreshRateExtension = has(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    if (info_.refreshRateExtension) extensions.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    info_.performanceExtension = has(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
    if (info_.performanceExtension) extensions.push_back(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
    info_.depthLayer = has(XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME); // M2: the stereo layer's depth image
    if (info_.depthLayer) extensions.push_back(XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME);

    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
#ifdef __ANDROID__
    XrInstanceCreateInfoAndroidKHR androidInfo{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    androidInfo.applicationVM = options_.androidVm;
    androidInfo.applicationActivity = options_.androidActivity;
    if (androidCreateInstance) ici.next = &androidInfo;
#endif
    std::snprintf(ici.applicationInfo.applicationName, sizeof(ici.applicationInfo.applicationName), "%s", options_.appName.c_str());
    std::snprintf(ici.applicationInfo.engineName, sizeof(ici.applicationInfo.engineName), "gt2pc");
    ici.applicationInfo.applicationVersion = 1;
    ici.applicationInfo.engineVersion = 1;
    ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ici.enabledExtensionCount = uint32_t(extensions.size());
    ici.enabledExtensionNames = extensions.data();
    r = s.api.xrCreateInstance(&ici, &s.instance);
    if (XR_FAILED(r)) throw std::runtime_error("OpenXR: xrCreateInstance failed (" + std::to_string(int(r)) + ")");

#define GT2XR_LOAD(n) s.api.gipa(s.instance, #n, reinterpret_cast<PFN_xrVoidFunction*>(&s.api.n));
    GT2XR_FUNCTIONS(GT2XR_LOAD)
    GT2XR_OPTIONAL_FUNCTIONS(GT2XR_LOAD)
#ifdef _WIN32
    if (info_.timeConversion) GT2XR_LOAD(xrConvertTimeToWin32PerformanceCounterKHR)
#endif
#ifdef __ANDROID__
    if (info_.timeConversion) GT2XR_LOAD(xrConvertTimeToTimespecTimeKHR)
#endif
#undef GT2XR_LOAD

    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    if (s.api.xrGetInstanceProperties(s.instance, &ip) == XR_SUCCESS) info_.runtimeName = ip.runtimeName;

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    r = s.api.xrGetSystem(s.instance, &sgi, &s.system);
    if (XR_FAILED(r)) throw std::runtime_error("OpenXR: no head-mounted display (xrGetSystem -> " + ResultText(s.api, s.instance, r) + ")");
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    if (s.api.xrGetSystemProperties(s.instance, s.system, &sp) == XR_SUCCESS) info_.systemName = sp.systemName;

    uint32_t viewCount = 0;
    XrViewConfigurationView views[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    s.Check(s.api.xrEnumerateViewConfigurationViews(s.instance, s.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &viewCount, views),
            "xrEnumerateViewConfigurationViews");
    if (viewCount != 2) throw std::runtime_error("OpenXR: the primary stereo view configuration has " + std::to_string(viewCount) + " views");
    info_.viewWidth = views[0].recommendedImageRectWidth;
    info_.viewHeight = views[0].recommendedImageRectHeight;

    // --- Vulkan through the runtime (XR_KHR_vulkan_enable2) ---
    if (!s.api.xrGetVulkanGraphicsRequirements2KHR || !s.api.xrCreateVulkanInstanceKHR || !s.api.xrCreateVulkanDeviceKHR ||
        !s.api.xrGetVulkanGraphicsDevice2KHR)
        throw std::runtime_error("OpenXR: the runtime enabled " XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME " but does not provide its functions");
    XrGraphicsRequirementsVulkan2KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    s.Check(s.api.xrGetVulkanGraphicsRequirements2KHR(s.instance, s.system, &requirements), "xrGetVulkanGraphicsRequirements2KHR");

    std::vector<const char*> layers;
    // The desktop mirror (tools/gt2game/game_window_xr.cpp) presents the same image in a window of its own, which
    // needs a surface on the runtime's instance. The runtime merges these with whatever it needs itself.
    std::vector<const char*> instanceExtensions;
#ifdef _WIN32
    if (options_.mirrorWindow) {
        instanceExtensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
        instanceExtensions.push_back("VK_KHR_win32_surface");
    }
#endif
    if (options_.vulkanValidation) {
        layers.push_back("VK_LAYER_KHRONOS_validation");
        instanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = options_.appName.c_str();
    appInfo.pEngineName = "gt2pc";
    appInfo.apiVersion = VK_API_VERSION_1_3; // the renderer needs dynamic rendering
    VkInstanceCreateInfo vici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    vici.pApplicationInfo = &appInfo;
    vici.enabledLayerCount = uint32_t(layers.size());
    vici.ppEnabledLayerNames = layers.data();
    vici.enabledExtensionCount = uint32_t(instanceExtensions.size());
    vici.ppEnabledExtensionNames = instanceExtensions.data();
    XrVulkanInstanceCreateInfoKHR xvi{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    xvi.systemId = s.system;
    xvi.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
    xvi.vulkanCreateInfo = &vici;
    VkResult vr = VK_ERROR_UNKNOWN;
    s.Check(s.api.xrCreateVulkanInstanceKHR(s.instance, &xvi, &vkInstance_, &vr), "xrCreateVulkanInstanceKHR");
    if (vr != VK_SUCCESS) throw std::runtime_error("OpenXR: the runtime could not create a Vulkan instance (VkResult " + std::to_string(int(vr)) + ")");

    if (options_.vulkanValidation) {
        VkDebugUtilsMessengerCreateInfoEXT dmi{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        dmi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
        dmi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        dmi.pfnUserCallback = &ValidationMessage;
        if (auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(vkInstance_, "vkCreateDebugUtilsMessengerEXT")))
            create(vkInstance_, &dmi, nullptr, &s.messenger);
        std::printf("xr: Vulkan validation layer on\n");
    }

    XrVulkanGraphicsDeviceGetInfoKHR gdi{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    gdi.systemId = s.system;
    gdi.vulkanInstance = vkInstance_;
    s.Check(s.api.xrGetVulkanGraphicsDevice2KHR(s.instance, &gdi, &vkPhysical_), "xrGetVulkanGraphicsDevice2KHR");
    if (!vkPhysical_) throw std::runtime_error("OpenXR: the runtime named no Vulkan physical device");

    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(vkPhysical_, &families, nullptr);
    std::vector<VkQueueFamilyProperties> familyProps(families);
    vkGetPhysicalDeviceQueueFamilyProperties(vkPhysical_, &families, familyProps.data());
    vkQueueFamily_ = UINT32_MAX;
    for (uint32_t i = 0; i < families && vkQueueFamily_ == UINT32_MAX; i++)
        if (familyProps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) vkQueueFamily_ = i;
    if (vkQueueFamily_ == UINT32_MAX) throw std::runtime_error("OpenXR: the runtime's GPU has no graphics queue");

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = vkQueueFamily_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    f11.multiview = VK_TRUE; // M2's stereo array target
    f11.pNext = &f13;
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &f11;
    std::vector<const char*> deviceExtensions;
    if (options_.mirrorWindow) deviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    VkPhysicalDeviceFragmentShadingRateFeaturesKHR shadingRate{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
#ifdef __ANDROID__
    info_.shadingRate = gt2view::ShadingRateFeatures(vkPhysical_, shadingRate);
#endif
    if (info_.shadingRate) {
        deviceExtensions.push_back(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);
        f13.pNext = &shadingRate;
    }
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &features;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = uint32_t(deviceExtensions.size());
    dci.ppEnabledExtensionNames = deviceExtensions.data();
    XrVulkanDeviceCreateInfoKHR xdi{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    xdi.systemId = s.system;
    xdi.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
    xdi.vulkanPhysicalDevice = vkPhysical_;
    xdi.vulkanCreateInfo = &dci;
    vr = VK_ERROR_UNKNOWN;
    s.Check(s.api.xrCreateVulkanDeviceKHR(s.instance, &xdi, &vkDevice_, &vr), "xrCreateVulkanDeviceKHR");
    if (vr != VK_SUCCESS) throw std::runtime_error("OpenXR: the runtime could not create a Vulkan device (VkResult " + std::to_string(int(vr)) + ")");
    vkGetDeviceQueue(vkDevice_, vkQueueFamily_, 0, &vkQueue_);

    // --- session ---
    XrGraphicsBindingVulkan2KHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
    binding.instance = vkInstance_;
    binding.physicalDevice = vkPhysical_;
    binding.device = vkDevice_;
    binding.queueFamilyIndex = vkQueueFamily_;
    binding.queueIndex = 0;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = s.system;
    s.Check(s.api.xrCreateSession(s.instance, &sci, &s.session), "xrCreateSession");
#ifdef __ANDROID__
    s.controls = std::make_unique<ControllerActions>(s.instance, s.session, s.api.gipa);
#endif

    // --- reference spaces: LOCAL is the app's world, VIEW the head, STAGE the floor when the runtime has one ---
    uint32_t spaceCount = 0;
    s.api.xrEnumerateReferenceSpaces(s.session, 0, &spaceCount, nullptr);
    std::vector<XrReferenceSpaceType> spaces(spaceCount);
    s.api.xrEnumerateReferenceSpaces(s.session, spaceCount, &spaceCount, spaces.data());
    auto hasSpace = [&](XrReferenceSpaceType t) { return std::find(spaces.begin(), spaces.end(), t) != spaces.end(); };
    XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsci.poseInReferenceSpace = IdentityPose();
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    s.Check(s.api.xrCreateReferenceSpace(s.session, &rsci, &s.localSpace), "xrCreateReferenceSpace(LOCAL)");
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    s.Check(s.api.xrCreateReferenceSpace(s.session, &rsci, &s.viewSpace), "xrCreateReferenceSpace(VIEW)");
    if (hasSpace(XR_REFERENCE_SPACE_TYPE_STAGE)) {
        rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
        info_.stageSpace = s.api.xrCreateReferenceSpace(s.session, &rsci, &s.stageSpace) == XR_SUCCESS;
    }

    // --- the cinema quad's swapchain: an sRGB format so that the bytes the game draws reach the compositor unchanged
    // (the frame is rendered into the *_UNORM twin and copied, see RenderFormat) ---
    uint32_t formatCount = 0;
    s.api.xrEnumerateSwapchainFormats(s.session, 0, &formatCount, nullptr);
    std::vector<int64_t> formats(formatCount);
    s.api.xrEnumerateSwapchainFormats(s.session, formatCount, &formatCount, formats.data());
    auto hasFormat = [&](VkFormat f) { return std::find(formats.begin(), formats.end(), int64_t(f)) != formats.end(); };
    if (hasFormat(VK_FORMAT_R8G8B8A8_SRGB)) {
        quadFormat_ = VK_FORMAT_R8G8B8A8_SRGB;
        renderFormat_ = VK_FORMAT_R8G8B8A8_UNORM;
    } else if (hasFormat(VK_FORMAT_B8G8R8A8_SRGB)) {
        quadFormat_ = VK_FORMAT_B8G8R8A8_SRGB;
        renderFormat_ = VK_FORMAT_B8G8R8A8_UNORM;
    } else if (hasFormat(VK_FORMAT_R8G8B8A8_UNORM)) {
        quadFormat_ = renderFormat_ = VK_FORMAT_R8G8B8A8_UNORM;
    } else if (hasFormat(VK_FORMAT_B8G8R8A8_UNORM)) {
        quadFormat_ = renderFormat_ = VK_FORMAT_B8G8R8A8_UNORM;
    } else {
        throw std::runtime_error("OpenXR: the runtime offers no 8-bit RGBA swapchain format");
    }
    XrSwapchainCreateInfo scci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    scci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    scci.format = int64_t(quadFormat_);
    scci.sampleCount = 1;
    scci.width = options_.quadWidth;
    scci.height = options_.quadHeight;
    scci.faceCount = 1;
    scci.arraySize = 1;
    scci.mipCount = 1;
    s.Check(s.api.xrCreateSwapchain(s.session, &scci, &s.quad), "xrCreateSwapchain(cinema quad)");
    uint32_t imageCount = 0;
    s.api.xrEnumerateSwapchainImages(s.quad, 0, &imageCount, nullptr);
    std::vector<XrSwapchainImageVulkan2KHR> images(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
    s.Check(s.api.xrEnumerateSwapchainImages(s.quad, imageCount, &imageCount, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),
            "xrEnumerateSwapchainImages");
    for (const XrSwapchainImageVulkan2KHR& image : images) s.quadImages.push_back(image.image);

    // --- the copy of a rendered frame into the compositor's image ---
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = vkQueueFamily_;
    if (vkCreateCommandPool(vkDevice_, &pci, nullptr, &s.pool) != VK_SUCCESS) throw std::runtime_error("OpenXR: vkCreateCommandPool failed");
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = s.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    vkAllocateCommandBuffers(vkDevice_, &cai, &s.cmd);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(vkDevice_, &fci, nullptr, &s.fence);

    // --- refresh rate and performance level (optional extensions) ---
    if (info_.refreshRateExtension && s.api.xrEnumerateDisplayRefreshRatesFB) {
        uint32_t rateCount = 0;
        s.api.xrEnumerateDisplayRefreshRatesFB(s.session, 0, &rateCount, nullptr);
        std::vector<float> rates(rateCount);
        s.api.xrEnumerateDisplayRefreshRatesFB(s.session, rateCount, &rateCount, rates.data());
        for (float f : rates) info_.refreshRates.push_back(double(f));
        if (options_.refreshHz > 0 && s.api.xrRequestDisplayRefreshRateFB) {
            const XrResult rr = s.api.xrRequestDisplayRefreshRateFB(s.session, options_.refreshHz);
            if (XR_FAILED(rr)) std::printf("xr: %.1f Hz refused by the runtime (%s)\n", double(options_.refreshHz), ResultText(s.api, s.instance, rr).c_str());
        }
        float current = 0;
        if (s.api.xrGetDisplayRefreshRateFB && s.api.xrGetDisplayRefreshRateFB(s.session, &current) == XR_SUCCESS) info_.refreshHz = double(current);
    }

    if (info_.refreshHz > 0) period_ = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / info_.refreshHz));
    lastDisplay_ = Clock::now();
}

Session::~Session() {
    Impl& s = *impl_;
    if (vkDevice_) vkDeviceWaitIdle(vkDevice_);
    if (s.quad && s.api.xrDestroySwapchain) s.api.xrDestroySwapchain(s.quad);
    if (s.stereo && s.api.xrDestroySwapchain) s.api.xrDestroySwapchain(s.stereo);
    if (s.stereoDepth && s.api.xrDestroySwapchain) s.api.xrDestroySwapchain(s.stereoDepth);
    for (XrSpace space : {s.localSpace, s.viewSpace, s.stageSpace})
        if (space && s.api.xrDestroySpace) s.api.xrDestroySpace(space);
    #ifdef __ANDROID__
    s.controls.reset();
    #endif
    if (s.session && s.api.xrDestroySession) s.api.xrDestroySession(s.session);
    if (vkDevice_) {
        if (s.fence) vkDestroyFence(vkDevice_, s.fence, nullptr);
        if (s.pool) vkDestroyCommandPool(vkDevice_, s.pool, nullptr);
        vkDestroyDevice(vkDevice_, nullptr);
    }
    if (s.messenger) {
        if (auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(vkInstance_, "vkDestroyDebugUtilsMessengerEXT")))
            destroy(vkInstance_, s.messenger, nullptr);
    }
    if (vkInstance_) vkDestroyInstance(vkInstance_, nullptr);
    if (s.instance && s.api.xrDestroyInstance) s.api.xrDestroyInstance(s.instance);
#ifdef _WIN32
    if (s.loader) FreeLibrary(s.loader);
#endif
}

bool Session::PollEvents() {
    Impl& s = *impl_;
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (s.api.xrPollEvent(s.instance, &event) == XR_SUCCESS) {
        switch (event.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            const auto& changed = reinterpret_cast<const XrEventDataSessionStateChanged&>(event);
            state_ = int(changed.state);
            std::printf("xr: session %s\n", StateName());
            if (changed.state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                s.Check(s.api.xrBeginSession(s.session, &bi), "xrBeginSession");
                running_ = true;
                if (info_.performanceExtension && s.api.xrPerfSettingsSetPerformanceLevelEXT) {
                    const auto cpu = s.api.xrPerfSettingsSetPerformanceLevelEXT(s.session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT,
                        XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
#ifdef __ANDROID__
                    const auto gpuLevel = XR_PERF_SETTINGS_LEVEL_BOOST_EXT;
#else
                    const auto gpuLevel = XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT;
#endif
                    const auto gpu = s.api.xrPerfSettingsSetPerformanceLevelEXT(s.session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, gpuLevel);
                    std::printf("xr: CPU sustained-high: %s; GPU level %d: %s (runtime controls clocks)\n",
                        ResultText(s.api, s.instance, cpu).c_str(), int(gpuLevel), ResultText(s.api, s.instance, gpu).c_str());
                    if (XR_FAILED(gpu) && gpuLevel == XR_PERF_SETTINGS_LEVEL_BOOST_EXT) {
                        const auto fallback = s.api.xrPerfSettingsSetPerformanceLevelEXT(s.session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT,
                            XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
                        std::printf("xr: GPU sustained-high fallback: %s\n", ResultText(s.api, s.instance, fallback).c_str());
                    }
                }
                frameOpen_ = false;
            } else if (changed.state == XR_SESSION_STATE_STOPPING) {
                running_ = false;
                frameOpen_ = false;
                s.Check(s.api.xrEndSession(s.session), "xrEndSession");
            } else if (changed.state == XR_SESSION_STATE_EXITING || changed.state == XR_SESSION_STATE_LOSS_PENDING) {
                running_ = false;
                quit_ = true;
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            quit_ = true;
            running_ = false;
            break;
        case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
            quadLatched_ = false; // the runtime recentered: the cinema quad is placed again in front of the head
            recenter_ = true;     // ... and the VR rig latches its origin again (vr_rig.h Recenter)
            break;
        case XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB: {
            const auto& refresh = reinterpret_cast<const XrEventDataDisplayRefreshRateChangedFB&>(event);
            info_.refreshHz = double(refresh.toDisplayRefreshRate);
            if (info_.refreshHz > 0) period_ = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / info_.refreshHz));
            std::printf("xr: display refresh rate %.1f Hz\n", info_.refreshHz);
            break;
        }
        default: break;
        }
        event = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
    }
    return !quit_;
}

void Session::RequestExit() {
    Impl& s = *impl_;
    if (running_ && s.api.xrRequestExitSession) s.api.xrRequestExitSession(s.session);
    else quit_ = true;
}

bool Session::BeginFrame() {
    Impl& s = *impl_;
    if (!running_ || quit_) return false;
    s.frameState = XrFrameState{XR_TYPE_FRAME_STATE};
    XrFrameWaitInfo fwi{XR_TYPE_FRAME_WAIT_INFO};
    const XrResult waited = s.api.xrWaitFrame(s.session, &fwi, &s.frameState);
    if (XR_FAILED(waited)) {
        if (waited == XR_ERROR_SESSION_NOT_RUNNING) return false;
        s.Check(waited, "xrWaitFrame");
    }
    XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO};
    const XrResult began = s.api.xrBeginFrame(s.session, &fbi);
    if (XR_FAILED(began)) {
        if (began == XR_ERROR_SESSION_NOT_RUNNING) return false;
        s.Check(began, "xrBeginFrame");
    }
    frameOpen_ = true;
    shouldRender_ = s.frameState.shouldRender == XR_TRUE;

    // The display time of this frame on the steady clock, and the runtime's period: the game's frame pacing
    // (game_window.h VBlankTiming) runs on them.
    Clock::time_point display = Clock::now();
#ifdef _WIN32
    if (s.api.xrConvertTimeToWin32PerformanceCounterKHR) {
        LARGE_INTEGER counter{};
        if (s.api.xrConvertTimeToWin32PerformanceCounterKHR(s.instance, s.frameState.predictedDisplayTime, &counter) == XR_SUCCESS && s.qpcFrequency)
            display = s.steadyEpoch + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(double(counter.QuadPart) / double(s.qpcFrequency)));
    }
#endif
#ifdef __ANDROID__
    // CLOCK_MONOTONIC is what std::chrono::steady_clock reads on Android, so the conversion is exact.
    if (s.api.xrConvertTimeToTimespecTimeKHR) {
        timespec ts{};
        if (s.api.xrConvertTimeToTimespecTimeKHR(s.instance, s.frameState.predictedDisplayTime, &ts) == XR_SUCCESS)
            display = Clock::time_point(std::chrono::duration_cast<Clock::duration>(std::chrono::seconds(ts.tv_sec) + std::chrono::nanoseconds(ts.tv_nsec)));
    }
#endif
    lastDisplay_ = display;
    if (s.frameState.predictedDisplayPeriod > 0)
        period_ = std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(s.frameState.predictedDisplayPeriod));
    return true;
}

void Session::SubmitFrame(VkImage image) {
    Impl& s = *impl_;
    if (!frameOpen_) return;
    std::vector<XrCompositionLayerBaseHeader*> layers;
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    if (shouldRender_ && image != VK_NULL_HANDLE) {
        // The anchor of the cinema screen (VC's theater mode): the yaw the head had when the screen appeared, 2 m
        // ahead of it and at its height. Latched once, so the screen stays where it is when the player looks around.
        if (!quadLatched_) {
            XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
            if (s.api.xrLocateSpace(s.viewSpace, s.localSpace, s.frameState.predictedDisplayTime, &head) == XR_SUCCESS &&
                (head.locationFlags & (XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) ==
                    (XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
                const XrQuaternionf& q = head.pose.orientation;
                // the head's forward axis (-Z) projected on the horizontal plane -> the yaw of the screen
                const float fx = -2.0f * (q.x * q.z + q.w * q.y);
                const float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
                const float yaw = std::atan2(-fx, -fz); // Ry(yaw) . (0, 0, -1) = the horizontal forward axis; 0 = down -Z
                s.quadPose.orientation = {0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
                s.quadPose.position = {head.pose.position.x + std::sin(yaw) * -options_.quadDistance, head.pose.position.y,
                                       head.pose.position.z - std::cos(yaw) * options_.quadDistance};
                quadLatched_ = true;
                std::printf("xr: cinema screen at (%.2f, %.2f, %.2f), yaw %.1f deg\n", double(s.quadPose.position.x), double(s.quadPose.position.y),
                            double(s.quadPose.position.z), double(yaw) * 180.0 / 3.14159265358979323846);
            }
        }
        if (CopyToQuad(image)) {
            quad.layerFlags = 0;
            quad.space = s.localSpace;
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH; // mono: M2 replaces this with the stereo projection layer
            quad.subImage.swapchain = s.quad;
            quad.subImage.imageRect = {{0, 0}, {int32_t(options_.quadWidth), int32_t(options_.quadHeight)}};
            quad.subImage.imageArrayIndex = 0;
            quad.pose = s.quadPose;
            quad.size = {options_.quadMetres, options_.quadMetres * float(options_.quadHeight) / float(options_.quadWidth)};
            layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad));
        }
    }
    XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
    fei.displayTime = s.frameState.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount = uint32_t(layers.size());
    fei.layers = layers.data();
    s.Check(s.api.xrEndFrame(s.session, &fei), "xrEndFrame");
    frameOpen_ = false;
    frameIndex_++;
}

// The rendered frame into the compositor's image: a plain copy, so that the bytes the game drew are what the runtime
// shows (and what its capture writes). The caller has already waited for the rendering to finish.
bool Session::CopyToQuad(VkImage src) {
    Impl& s = *impl_;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(s.api.xrAcquireSwapchainImage(s.quad, &ai, &s.quadIndex))) return false;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(s.api.xrWaitSwapchainImage(s.quad, &wi))) return false;
    VkImage dst = s.quadImages[s.quadIndex];

    vkResetCommandBuffer(s.cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(s.cmd, &bi);
    auto barrier = [&](VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess, VkAccessFlags dstAccess) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = srcAccess;
        b.dstAccessMask = dstAccess;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    barrier(dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {options_.quadWidth, options_.quadHeight, 1};
    vkCmdCopyImage(s.cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    barrier(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT);
    vkEndCommandBuffer(s.cmd);

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s.cmd;
    vkResetFences(vkDevice_, 1, &s.fence);
    vkQueueSubmit(vkQueue_, 1, &si, s.fence);
    vkWaitForFences(vkDevice_, 1, &s.fence, VK_TRUE, UINT64_MAX);

    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    return s.api.xrReleaseSwapchainImage(s.quad, &ri) == XR_SUCCESS;
}

bool Session::TakeRecenter() {
    const bool r = recenter_;
    recenter_ = false;
    return r;
}

bool Session::SetRefreshRate(float hz) {
    auto& s = *impl_;
    if (!info_.refreshRateExtension || !s.api.xrRequestDisplayRefreshRateFB) return false;
    bool offered = false;
    for (double rate : info_.refreshRates) if (std::abs(rate - hz) < 0.1) offered = true;
    if (!offered) return false;
    return XR_SUCCEEDED(s.api.xrRequestDisplayRefreshRateFB(s.session, hz));
}

void Session::CreateStereoSwapchains(uint32_t width, uint32_t height, bool direct) {
    Impl& s = *impl_;
    if (stereoReady_) return;
    if (width == 0 || height == 0) throw std::runtime_error("OpenXR: the stereo swapchain needs a non-empty size");
    uint32_t formatCount = 0;
    s.api.xrEnumerateSwapchainFormats(s.session, 0, &formatCount, nullptr);
    std::vector<int64_t> formats(formatCount);
    s.api.xrEnumerateSwapchainFormats(s.session, formatCount, &formatCount, formats.data());
    auto hasFormat = [&](VkFormat f) { return std::find(formats.begin(), formats.end(), int64_t(f)) != formats.end(); };
    // As for the quad: an sRGB swapchain whose *_UNORM twin the game renders into, so that the bytes the game drew
    // are exactly the bytes the compositor (and the runtime's capture) sees.
    if (hasFormat(VK_FORMAT_R8G8B8A8_SRGB)) {
        stereoFormat_ = VK_FORMAT_R8G8B8A8_SRGB;
        stereoRenderFormat_ = VK_FORMAT_R8G8B8A8_UNORM;
    } else if (hasFormat(VK_FORMAT_B8G8R8A8_SRGB)) {
        stereoFormat_ = VK_FORMAT_B8G8R8A8_SRGB;
        stereoRenderFormat_ = VK_FORMAT_B8G8R8A8_UNORM;
    } else if (hasFormat(VK_FORMAT_R8G8B8A8_UNORM)) {
        stereoFormat_ = stereoRenderFormat_ = VK_FORMAT_R8G8B8A8_UNORM;
    } else if (hasFormat(VK_FORMAT_B8G8R8A8_UNORM)) {
        stereoFormat_ = stereoRenderFormat_ = VK_FORMAT_B8G8R8A8_UNORM;
    } else {
        throw std::runtime_error("OpenXR: the runtime offers no 8-bit RGBA swapchain format");
    }
    XrSwapchainCreateInfo scci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    scci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    scci.format = int64_t(stereoFormat_);
    scci.sampleCount = 1;
    scci.width = width;
    scci.height = height;
    scci.faceCount = 1;
    scci.arraySize = 2; // one array layer per eye
    scci.mipCount = 1;
    if (direct) scci.usageFlags |= XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT;
    XrResult created = s.api.xrCreateSwapchain(s.session, &scci, &s.stereo);
    directStereo_ = direct && XR_SUCCEEDED(created);
    if (direct && XR_FAILED(created)) {
        std::printf("xr: direct stereo unavailable (%s), using copy path\n", ResultText(s.api, s.instance, created).c_str());
        scci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        created = s.api.xrCreateSwapchain(s.session, &scci, &s.stereo);
    }
    s.Check(created, "xrCreateSwapchain(stereo colour)");
    std::printf("xr: stereo submission: %s\n", directStereo_ ? "direct UNORM rendering into mutable sRGB swapchain (MSAA)" : "copy");
    uint32_t imageCount = 0;
    s.api.xrEnumerateSwapchainImages(s.stereo, 0, &imageCount, nullptr);
    std::vector<XrSwapchainImageVulkan2KHR> images(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
    s.Check(s.api.xrEnumerateSwapchainImages(s.stereo, imageCount, &imageCount, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),
            "xrEnumerateSwapchainImages(stereo)");
    for (const XrSwapchainImageVulkan2KHR& image : images) s.stereoImages.push_back(image.image);

    if (info_.depthLayer && hasFormat(VK_FORMAT_D32_SFLOAT)) {
        stereoDepthFormat_ = VK_FORMAT_D32_SFLOAT;
        XrSwapchainCreateInfo dci = scci;
        dci.usageFlags = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        dci.format = int64_t(stereoDepthFormat_);
        if (XR_SUCCEEDED(s.api.xrCreateSwapchain(s.session, &dci, &s.stereoDepth))) {
            uint32_t n = 0;
            s.api.xrEnumerateSwapchainImages(s.stereoDepth, 0, &n, nullptr);
            std::vector<XrSwapchainImageVulkan2KHR> depthImages(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
            if (XR_SUCCEEDED(s.api.xrEnumerateSwapchainImages(s.stereoDepth, n, &n,
                                                              reinterpret_cast<XrSwapchainImageBaseHeader*>(depthImages.data())))) {
                for (const XrSwapchainImageVulkan2KHR& image : depthImages) s.stereoDepthImages.push_back(image.image);
                stereoDepth_ = &s.stereoDepth;
            }
        }
    }
    stereoWidth_ = width;
    stereoHeight_ = height;
    stereoReady_ = true;
}

VkImage Session::AcquireStereoRenderImage() {
    auto& s = *impl_;
    if (!directStereo_ || !frameOpen_ || !shouldRender_) return VK_NULL_HANDLE;
    if (!s.stereoAcquired) {
        XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        s.Check(s.api.xrAcquireSwapchainImage(s.stereo, &acquire, &s.stereoIndex), "acquire direct stereo image");
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; wait.timeout = XR_INFINITE_DURATION;
        s.Check(s.api.xrWaitSwapchainImage(s.stereo, &wait), "wait direct stereo image");
        s.stereoAcquired = true;
    }
    return s.stereoImages.at(s.stereoIndex);
}

bool Session::LocateViews(vr::Pose& head, vr::EyeView eyes[2]) {
    Impl& s = *impl_;
    if (!frameOpen_) return false;
    XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
    vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    vli.displayTime = s.frameState.predictedDisplayTime;
    vli.space = s.localSpace;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    uint32_t count = 0;
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    if (XR_FAILED(s.api.xrLocateViews(s.session, &vli, &vs, 2, &count, views)) || count != 2) return false;
    constexpr XrViewStateFlags kValid = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
    if ((vs.viewStateFlags & kValid) != kValid) return false;
    for (int v = 0; v < 2; v++) {
        eyes[v].pose.position[0] = views[v].pose.position.x;
        eyes[v].pose.position[1] = views[v].pose.position.y;
        eyes[v].pose.position[2] = views[v].pose.position.z;
        eyes[v].pose.orientation[0] = views[v].pose.orientation.x;
        eyes[v].pose.orientation[1] = views[v].pose.orientation.y;
        eyes[v].pose.orientation[2] = views[v].pose.orientation.z;
        eyes[v].pose.orientation[3] = views[v].pose.orientation.w;
        eyes[v].fov.left = views[v].fov.angleLeft;
        eyes[v].fov.right = views[v].fov.angleRight;
        eyes[v].fov.up = views[v].fov.angleUp;
        eyes[v].fov.down = views[v].fov.angleDown;
    }
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (s.api.xrLocateSpace(s.viewSpace, s.localSpace, s.frameState.predictedDisplayTime, &loc) == XR_SUCCESS &&
        (loc.locationFlags & (XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) ==
            (XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
        head.position[0] = loc.pose.position.x;
        head.position[1] = loc.pose.position.y;
        head.position[2] = loc.pose.position.z;
        head.orientation[0] = loc.pose.orientation.x;
        head.orientation[1] = loc.pose.orientation.y;
        head.orientation[2] = loc.pose.orientation.z;
        head.orientation[3] = loc.pose.orientation.w;
    } else { // no head pose: the mid point of the two eyes is close enough for the rig's origin
        for (int i = 0; i < 3; i++) head.position[i] = 0.5f * (eyes[0].pose.position[i] + eyes[1].pose.position[i]);
        for (int i = 0; i < 4; i++) head.orientation[i] = eyes[0].pose.orientation[i];
    }
    return true;
}

void Session::SubmitStereoFrame(VkImage color, VkImage depth, const vr::EyeView eyes[2], float nearZ) {
    Impl& s = *impl_;
    if (!frameOpen_) return;
    std::vector<XrCompositionLayerBaseHeader*> layers;
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView projectionViews[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                           {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerDepthInfoKHR depthInfo[2] = {{XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR},
                                                   {XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR}};
    const bool withDepth = depth != VK_NULL_HANDLE && stereoDepth_ != nullptr;
    bool ready = false;
    if (s.stereoAcquired) {
        // Caller has submitted GPU writes on the session queue, ending in COLOR_ATTACHMENT_OPTIMAL.
        if (color != s.stereoImages.at(s.stereoIndex) || withDepth) throw std::runtime_error("mismatched direct stereo submission");
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        s.Check(s.api.xrReleaseSwapchainImage(s.stereo, &release), "release direct stereo image");
        s.stereoAcquired = false; s.lastDirectImage = color; ready = true;
    } else if (shouldRender_ && stereoReady_ && color != VK_NULL_HANDLE) {
        // Idle periods reuse the last released image without touching runtime-owned memory.
        if (color == s.lastDirectImage && !withDepth) ready = true;
        else { ready = CopyToStereo(color, withDepth ? depth : VK_NULL_HANDLE); s.lastDirectImage = VK_NULL_HANDLE; }
    }
    if (shouldRender_ && ready) {
        for (int v = 0; v < 2; v++) {
            projectionViews[v].pose.position = {eyes[v].pose.position[0], eyes[v].pose.position[1], eyes[v].pose.position[2]};
            projectionViews[v].pose.orientation = {eyes[v].pose.orientation[0], eyes[v].pose.orientation[1], eyes[v].pose.orientation[2],
                                                   eyes[v].pose.orientation[3]};
            projectionViews[v].fov = {eyes[v].fov.left, eyes[v].fov.right, eyes[v].fov.up, eyes[v].fov.down};
            projectionViews[v].subImage.swapchain = s.stereo;
            projectionViews[v].subImage.imageRect = {{0, 0}, {int32_t(stereoWidth_), int32_t(stereoHeight_)}};
            projectionViews[v].subImage.imageArrayIndex = uint32_t(v);
            if (withDepth) {
                // Reversed Z with an infinite far plane: the buffer holds nearZ / distance, so 0 is infinitely far
                // (nearZ of the layer) and 1 is the near plane (farZ of the layer).
                depthInfo[v].subImage.swapchain = s.stereoDepth;
                depthInfo[v].subImage.imageRect = projectionViews[v].subImage.imageRect;
                depthInfo[v].subImage.imageArrayIndex = uint32_t(v);
                depthInfo[v].minDepth = 0.0f;
                depthInfo[v].maxDepth = 1.0f;
                depthInfo[v].nearZ = std::numeric_limits<float>::infinity();
                depthInfo[v].farZ = nearZ;
                projectionViews[v].next = &depthInfo[v];
            }
        }
        projection.layerFlags = 0;
        projection.space = s.localSpace;
        projection.viewCount = 2;
        projection.views = projectionViews;
        layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&projection));
    }
    XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
    fei.displayTime = s.frameState.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount = uint32_t(layers.size());
    fei.layers = layers.data();
    s.Check(s.api.xrEndFrame(s.session, &fei), "xrEndFrame");
    frameOpen_ = false;
    frameIndex_++;
}

// The two-layer images into the projection layer's swapchain images: a plain copy, like the quad's.
bool Session::CopyToStereo(VkImage color, VkImage depth) {
    Impl& s = *impl_;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(s.api.xrAcquireSwapchainImage(s.stereo, &ai, &s.stereoIndex))) return false;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(s.api.xrWaitSwapchainImage(s.stereo, &wi))) return false;
    const bool withDepth = depth != VK_NULL_HANDLE && s.stereoDepth != XR_NULL_HANDLE;
    if (withDepth) {
        if (XR_FAILED(s.api.xrAcquireSwapchainImage(s.stereoDepth, &ai, &s.stereoDepthIndex)) ||
            XR_FAILED(s.api.xrWaitSwapchainImage(s.stereoDepth, &wi)))
            return false;
    }

    vkResetCommandBuffer(s.cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(s.cmd, &bi);
    auto barrier = [&](VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess,
                       VkAccessFlags dstAccess) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = srcAccess;
        b.dstAccessMask = dstAccess;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {aspect, 0, 1, 0, 2};
        vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    auto copy = [&](VkImage src, VkImage dst, VkImageAspectFlags aspect) {
        VkImageCopy region{};
        region.srcSubresource = {aspect, 0, 0, 2};
        region.dstSubresource = {aspect, 0, 0, 2};
        region.extent = {stereoWidth_, stereoHeight_, 1};
        vkCmdCopyImage(s.cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    };
    VkImage dstColor = s.stereoImages[s.stereoIndex];
    barrier(dstColor, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT);
    copy(color, dstColor, VK_IMAGE_ASPECT_COLOR_BIT);
    barrier(dstColor, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT);
    if (withDepth) {
        VkImage dstDepth = s.stereoDepthImages[s.stereoDepthIndex];
        barrier(dstDepth, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT);
        copy(depth, dstDepth, VK_IMAGE_ASPECT_DEPTH_BIT);
        barrier(dstDepth, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
    }
    vkEndCommandBuffer(s.cmd);

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s.cmd;
    vkResetFences(vkDevice_, 1, &s.fence);
    vkQueueSubmit(vkQueue_, 1, &si, s.fence);
    vkWaitForFences(vkDevice_, 1, &s.fence, VK_TRUE, UINT64_MAX);

    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    const bool released = s.api.xrReleaseSwapchainImage(s.stereo, &ri) == XR_SUCCESS;
    if (withDepth) s.api.xrReleaseSwapchainImage(s.stereoDepth, &ri);
    return released;
}

} // namespace gt2::xr

#ifdef __ANDROID__
bool gt2::xr::Session::ReadController(input::Ps1PadFrame& pad, float vibration) {
    return impl_->controls->Poll(pad, vibration, running_ && State() == 5);
}
gt2::vr::TrackedControllers gt2::xr::Session::ControllerTracking() {
    if (!running_ || State() != 5) return {};
    return impl_->controls->Locate(impl_->localSpace, impl_->frameState.predictedDisplayTime);
}
#endif
