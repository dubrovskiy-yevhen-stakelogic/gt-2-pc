// xrsim - Vulkan interop (XR_KHR_vulkan_enable / XR_KHR_vulkan_enable2), swapchains with images the runtime creates
// on the application's VkDevice, capture readback (-> PNG) and the mirror-window readback.
#include "rt_api.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace xs {

namespace {

PFN_vkGetInstanceProcAddr SystemGipa() {
    static PFN_vkGetInstanceProcAddr fn = [] {
        HMODULE m = LoadLibraryA("vulkan-1.dll");
        return m ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(reinterpret_cast<void*>(GetProcAddress(m, "vkGetInstanceProcAddr"))) : nullptr;
    }();
    return fn;
}
PFN_vkGetInstanceProcAddr InstGipa(Instance* inst) { return inst->appGipa ? inst->appGipa : SystemGipa(); }

const FmtInfo kFormats[] = {
    {VK_FORMAT_R8G8B8A8_SRGB, FmtInfo::RGBA8_SRGB, 4, VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_B8G8R8A8_SRGB, FmtInfo::BGRA8_SRGB, 4, VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R8G8B8A8_UNORM, FmtInfo::RGBA8_UNORM, 4, VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_B8G8R8A8_UNORM, FmtInfo::BGRA8_UNORM, 4, VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_R16G16B16A16_SFLOAT, FmtInfo::RGBA16F, 8, VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_A2B10G10R10_UNORM_PACK32, FmtInfo::A2B10G10R10, 4, VK_IMAGE_ASPECT_COLOR_BIT},
    {VK_FORMAT_D32_SFLOAT, FmtInfo::Depth, 4, VK_IMAGE_ASPECT_DEPTH_BIT},
    {VK_FORMAT_D24_UNORM_S8_UINT, FmtInfo::Depth, 4, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT},
    {VK_FORMAT_D16_UNORM, FmtInfo::Depth, 2, VK_IMAGE_ASPECT_DEPTH_BIT},
    {VK_FORMAT_D32_SFLOAT_S8_UINT, FmtInfo::Depth, 8, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT},
};

int FindMemType(Session* s, uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags prefer = 0) {
    int fallback = -1;
    for (uint32_t i = 0; i < s->memProps.memoryTypeCount; ++i) {
        if (!(bits & (1u << i))) continue;
        const VkMemoryPropertyFlags f = s->memProps.memoryTypes[i].propertyFlags;
        if ((f & want) != want) continue;
        if ((f & prefer) == prefer) return (int)i;
        if (fallback < 0) fallback = (int)i;
    }
    return fallback;
}

VkImageLayout LayoutForUsage(XrSwapchainUsageFlags u) {
    if (u & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    if (u & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    if (u & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT) return VK_IMAGE_LAYOUT_GENERAL;
    if (u & XR_SWAPCHAIN_USAGE_SAMPLED_BIT) return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (u & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    return VK_IMAGE_LAYOUT_GENERAL;
}

VkCommandBuffer BeginCmd(Session* s) {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = s->cmdPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (s->vk.vkAllocateCommandBuffers(s->device, &ai, &cb) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    s->vk.vkBeginCommandBuffer(cb, &bi);
    return cb;
}

bool SubmitWait(Session* s, VkCommandBuffer cb) {
    s->vk.vkEndCommandBuffer(cb);
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    s->vk.vkCreateFence(s->device, &fi, nullptr, &fence);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    VkResult r = s->vk.vkQueueSubmit(s->queue, 1, &si, fence);
    if (r == VK_SUCCESS) r = s->vk.vkWaitForFences(s->device, 1, &fence, VK_TRUE, 10ull * 1000 * 1000 * 1000);
    s->vk.vkDestroyFence(s->device, fence, nullptr);
    s->vk.vkFreeCommandBuffers(s->device, s->cmdPool, 1, &cb);
    if (r != VK_SUCCESS) Log("runtime GPU submission failed: VkResult %d", (int)r);
    return r == VK_SUCCESS;
}

void ImageBarrier(Session* s, VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, uint32_t layer, uint32_t layers, uint32_t mips,
                  VkImageLayout from, VkImageLayout to, VkAccessFlags srcA, VkAccessFlags dstA, VkPipelineStageFlags srcS,
                  VkPipelineStageFlags dstS) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = srcA;
    b.dstAccessMask = dstA;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {aspect, 0, mips, layer, layers};
    s->vk.vkCmdPipelineBarrier(cb, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// Swapchain image -> TRANSFER_SRC and back, around `record`.
template <class F>
void WithTransferSrc(Session* s, VkCommandBuffer cb, Swapchain* sc, uint32_t image, uint32_t layer, F record) {
    ImageBarrier(s, cb, sc->images[image], sc->fmt->aspect, layer, 1, 1, sc->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    record();
    ImageBarrier(s, cb, sc->images[image], sc->fmt->aspect, layer, 1, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sc->layout, 0,
                 VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
}

struct HostBuffer {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void* map = nullptr;
    bool coherent = true;
};
bool CreateHostBuffer(Session* s, VkDeviceSize size, HostBuffer& hb) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (s->vk.vkCreateBuffer(s->device, &bi, nullptr, &hb.buf) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    s->vk.vkGetBufferMemoryRequirements(s->device, hb.buf, &mr);
    const int mt = FindMemType(s, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                               VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt < 0) return false;
    hb.coherent = (s->memProps.memoryTypes[mt].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = (uint32_t)mt;
    if (s->vk.vkAllocateMemory(s->device, &ai, nullptr, &hb.mem) != VK_SUCCESS) return false;
    s->vk.vkBindBufferMemory(s->device, hb.buf, hb.mem, 0);
    return s->vk.vkMapMemory(s->device, hb.mem, 0, VK_WHOLE_SIZE, 0, &hb.map) == VK_SUCCESS;
}
void DestroyHostBuffer(Session* s, HostBuffer& hb) {
    if (hb.map) s->vk.vkUnmapMemory(s->device, hb.mem);
    if (hb.buf) s->vk.vkDestroyBuffer(s->device, hb.buf, nullptr);
    if (hb.mem) s->vk.vkFreeMemory(s->device, hb.mem, nullptr);
    hb = HostBuffer{};
}
void Invalidate(Session* s, const HostBuffer& hb) {
    if (hb.coherent) return;
    VkMappedMemoryRange r{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    r.memory = hb.mem;
    r.size = VK_WHOLE_SIZE;
    s->vk.vkInvalidateMappedMemoryRanges(s->device, 1, &r);
}

// ---------------------------------------------------------------------------------------------- pixel conversion
double Oetf(double c) { // linear -> sRGB transfer function (IEC 61966-2-1)
    c = std::clamp(c, 0.0, 1.0);
    return c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
}
uint8_t Enc(double linear) { return (uint8_t)std::lround(Oetf(linear) * 255.0); }
const uint8_t* EncLut() {
    static uint8_t lut[256];
    static bool init = [] {
        for (int i = 0; i < 256; ++i) lut[i] = Enc(i / 255.0);
        return true;
    }();
    (void)init;
    return lut;
}
float HalfToFloat(uint16_t h) {
    const uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1f, man = h & 0x3ff;
    float v;
    if (exp == 0) v = std::ldexp((float)man, -24);
    else if (exp == 31) v = man ? 0.0f : 65504.0f; // NaN -> 0, Inf -> max
    else v = std::ldexp((float)(man | 0x400), (int)exp - 25);
    return sign ? -v : v;
}

// Converts `w x h` texels of `fmt` to display-referred RGBA8 (sRGB-encoded): *_SRGB formats are copied, linear
// formats (UNORM, SFLOAT, 10-bit) are encoded with the sRGB transfer function, as a compositor would show them.
} // namespace
std::vector<uint8_t> ToDisplayRgba(const FmtInfo* fmt, const uint8_t* src, uint32_t w, uint32_t h, bool opaque) {
    std::vector<uint8_t> out((size_t)w * h * 4);
    const uint8_t* lut = EncLut();
    const size_t n = (size_t)w * h;
    for (size_t i = 0; i < n; ++i) {
        uint8_t* o = &out[i * 4];
        switch (fmt->pix) {
        case FmtInfo::RGBA8_SRGB: memcpy(o, src + i * 4, 4); break;
        case FmtInfo::BGRA8_SRGB: o[0] = src[i * 4 + 2]; o[1] = src[i * 4 + 1]; o[2] = src[i * 4 + 0]; o[3] = src[i * 4 + 3]; break;
        case FmtInfo::RGBA8_UNORM: o[0] = lut[src[i * 4]]; o[1] = lut[src[i * 4 + 1]]; o[2] = lut[src[i * 4 + 2]]; o[3] = src[i * 4 + 3]; break;
        case FmtInfo::BGRA8_UNORM: o[0] = lut[src[i * 4 + 2]]; o[1] = lut[src[i * 4 + 1]]; o[2] = lut[src[i * 4]]; o[3] = src[i * 4 + 3]; break;
        case FmtInfo::RGBA16F: {
            uint16_t hv[4];
            memcpy(hv, src + i * 8, 8);
            for (int c = 0; c < 3; ++c) o[c] = Enc(HalfToFloat(hv[c]));
            o[3] = (uint8_t)std::lround(std::clamp((double)HalfToFloat(hv[3]), 0.0, 1.0) * 255.0);
            break;
        }
        case FmtInfo::A2B10G10R10: {
            uint32_t p;
            memcpy(&p, src + i * 4, 4);
            o[0] = Enc((p & 0x3ff) / 1023.0);
            o[1] = Enc(((p >> 10) & 0x3ff) / 1023.0);
            o[2] = Enc(((p >> 20) & 0x3ff) / 1023.0);
            o[3] = (uint8_t)((p >> 30) * 85);
            break;
        }
        default: break;
        }
        if (opaque) o[3] = 255;
    }
    return out;
}
namespace {

void ChooseDevice(Instance* inst, VkInstance vkInst, VkPhysicalDevice* out, XrResult& r) {
    r = XR_SUCCESS;
    PFN_vkGetInstanceProcAddr gipa = InstGipa(inst);
    auto enumPD = gipa ? reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(vkInst, "vkEnumeratePhysicalDevices")) : nullptr;
    auto props = gipa ? reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(vkInst, "vkGetPhysicalDeviceProperties")) : nullptr;
    if (!enumPD || !props) {
        r = XS_FAIL(XR_ERROR_RUNTIME_FAILURE, "cannot load vkEnumeratePhysicalDevices");
        return;
    }
    uint32_t n = 0;
    enumPD(vkInst, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    if (n) enumPD(vkInst, &n, pds.data());
    if (pds.empty()) {
        r = XS_FAIL(XR_ERROR_RUNTIME_FAILURE, "no Vulkan physical devices");
        return;
    }
    int pick = -1;
    if (inst->cfg.gpuIndex >= 0 && inst->cfg.gpuIndex < (int)pds.size()) pick = inst->cfg.gpuIndex;
    for (size_t i = 0; pick < 0 && i < pds.size(); ++i) {
        VkPhysicalDeviceProperties p;
        props(pds[i], &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) pick = (int)i;
    }
    if (pick < 0) pick = 0;
    VkPhysicalDeviceProperties p;
    props(pds[(size_t)pick], &p);
    Log("Vulkan physical device %d: %s (API %u.%u.%u)", pick, p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
        VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion));
    inst->chosenPhys = pds[(size_t)pick];
    inst->lastVkInstance = vkInst;
    *out = inst->chosenPhys;
}

XrResult GraphicsRequirements(XrInstance instance, XrSystemId systemId, XrGraphicsRequirementsVulkanKHR* req) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Instance* inst = GetInstance(instance);
    if (!inst) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "instance");
    if (systemId != kSystemId) return XS_FAIL(XR_ERROR_SYSTEM_INVALID, "systemId");
    if (!req || req->type != XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "graphicsRequirements");
    req->minApiVersionSupported = XR_MAKE_VERSION(1, 0, 0);
    req->maxApiVersionSupported = XR_MAKE_VERSION(1, 4, 0);
    inst->reqCalled = true;
    return XR_SUCCESS;
}

} // namespace

const FmtInfo* FindFmt(VkFormat f) {
    for (const FmtInfo& i : kFormats)
        if (i.format == f) return &i;
    return nullptr;
}

// ================================================================================================ graphics binding
XrResult XRAPI_CALL xrGetVulkanGraphicsRequirementsKHR(XrInstance instance, XrSystemId systemId, XrGraphicsRequirementsVulkanKHR* req) {
    return GraphicsRequirements(instance, systemId, req);
}
XrResult XRAPI_CALL xrGetVulkanGraphicsRequirements2KHR(XrInstance instance, XrSystemId systemId, XrGraphicsRequirementsVulkanKHR* req) {
    return GraphicsRequirements(instance, systemId, req);
}

XrResult XRAPI_CALL xrGetVulkanInstanceExtensionsKHR(XrInstance instance, XrSystemId systemId, uint32_t cap, uint32_t* count, char* buffer) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    if (!GetInstance(instance)) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "instance");
    if (systemId != kSystemId) return XS_FAIL(XR_ERROR_SYSTEM_INVALID, "systemId");
    return TwoCallString(cap, count, buffer, ""); // the simulator needs no extra Vulkan instance extensions
}

XrResult XRAPI_CALL xrGetVulkanDeviceExtensionsKHR(XrInstance instance, XrSystemId systemId, uint32_t cap, uint32_t* count, char* buffer) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    if (!GetInstance(instance)) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "instance");
    if (systemId != kSystemId) return XS_FAIL(XR_ERROR_SYSTEM_INVALID, "systemId");
    return TwoCallString(cap, count, buffer, ""); // ... nor device extensions
}

XrResult XRAPI_CALL xrGetVulkanGraphicsDeviceKHR(XrInstance instance, XrSystemId systemId, VkInstance vkInstance, VkPhysicalDevice* phys) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Instance* inst = GetInstance(instance);
    if (!inst) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "instance");
    if (systemId != kSystemId) return XS_FAIL(XR_ERROR_SYSTEM_INVALID, "systemId");
    if (!vkInstance || !phys) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "null VkInstance / output");
    XrResult r;
    ChooseDevice(inst, vkInstance, phys, r);
    return r;
}

XrResult XRAPI_CALL xrGetVulkanGraphicsDevice2KHR(XrInstance instance, const XrVulkanGraphicsDeviceGetInfoKHR* info, VkPhysicalDevice* phys) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Instance* inst = GetInstance(instance);
    if (!inst) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "instance");
    if (!info || info->type != XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR || !phys || !info->vulkanInstance)
        return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "getInfo");
    if (info->systemId != kSystemId) return XS_FAIL(XR_ERROR_SYSTEM_INVALID, "systemId");
    XrResult r;
    ChooseDevice(inst, info->vulkanInstance, phys, r);
    return r;
}

XrResult XRAPI_CALL xrCreateVulkanInstanceKHR(XrInstance instance, const XrVulkanInstanceCreateInfoKHR* ci, VkInstance* vkInstance, VkResult* vkResult) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Instance* inst = GetInstance(instance);
    if (!inst) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "instance");
    if (!ci || ci->type != XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR || !vkInstance || !vkResult || !ci->pfnGetInstanceProcAddr ||
        !ci->vulkanCreateInfo)
        return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "createInfo");
    if (ci->systemId != kSystemId) return XS_FAIL(XR_ERROR_SYSTEM_INVALID, "systemId");
    if (ci->createFlags != 0) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "createFlags");
    auto create = reinterpret_cast<PFN_vkCreateInstance>(ci->pfnGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!create) return XS_FAIL(XR_ERROR_RUNTIME_FAILURE, "vkCreateInstance not available");
    *vkResult = create(ci->vulkanCreateInfo, ci->vulkanAllocator, vkInstance);
    if (*vkResult == VK_SUCCESS) {
        inst->appGipa = ci->pfnGetInstanceProcAddr;
        inst->lastVkInstance = *vkInstance;
    }
    Log("xrCreateVulkanInstanceKHR: VkResult %d", (int)*vkResult);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrCreateVulkanDeviceKHR(XrInstance instance, const XrVulkanDeviceCreateInfoKHR* ci, VkDevice* vkDevice, VkResult* vkResult) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Instance* inst = GetInstance(instance);
    if (!inst) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "instance");
    if (!ci || ci->type != XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR || !vkDevice || !vkResult || !ci->pfnGetInstanceProcAddr ||
        !ci->vulkanCreateInfo || !ci->vulkanPhysicalDevice)
        return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "createInfo");
    if (ci->systemId != kSystemId) return XS_FAIL(XR_ERROR_SYSTEM_INVALID, "systemId");
    if (ci->createFlags != 0) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "createFlags");
    if (!inst->lastVkInstance) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "call xrGetVulkanGraphicsDevice2KHR first");
    if (inst->chosenPhys && ci->vulkanPhysicalDevice != inst->chosenPhys)
        return XS_FAIL(XR_ERROR_GRAPHICS_DEVICE_INVALID, "physical device differs from xrGetVulkanGraphicsDevice2KHR");
    auto create = reinterpret_cast<PFN_vkCreateDevice>(ci->pfnGetInstanceProcAddr(inst->lastVkInstance, "vkCreateDevice"));
    if (!create) return XS_FAIL(XR_ERROR_RUNTIME_FAILURE, "vkCreateDevice not available");
    *vkResult = create(ci->vulkanPhysicalDevice, ci->vulkanCreateInfo, ci->vulkanAllocator, vkDevice);
    Log("xrCreateVulkanDeviceKHR: VkResult %d", (int)*vkResult);
    return XR_SUCCESS;
}

bool VkSessionInit(Session* s, const XrGraphicsBindingVulkanKHR* b, std::string& err) {
    Instance* inst = s->inst;
    if (!b->instance || !b->physicalDevice || !b->device) {
        err = "XrGraphicsBindingVulkanKHR has null handles";
        return false;
    }
    if (inst->chosenPhys && inst->chosenPhys != b->physicalDevice) {
        err = "physicalDevice differs from the one returned by xrGetVulkanGraphicsDevice(2)KHR";
        return false;
    }
    PFN_vkGetInstanceProcAddr gipa = InstGipa(inst);
    if (!gipa) {
        err = "no vkGetInstanceProcAddr (vulkan-1.dll not found)";
        return false;
    }
    s->vkInstance = b->instance;
    s->phys = b->physicalDevice;
    s->device = b->device;
    s->queueFamily = b->queueFamilyIndex;
    s->queueIndex = b->queueIndex;
#define XS_LOADI(n) s->vk.n = reinterpret_cast<PFN_##n>(gipa(b->instance, #n)); if (!s->vk.n) { err = "cannot load " #n; return false; }
    XS_VK_INSTANCE_FNS(XS_LOADI)
#undef XS_LOADI
#define XS_LOADD(n) s->vk.n = reinterpret_cast<PFN_##n>(s->vk.vkGetDeviceProcAddr(b->device, #n)); if (!s->vk.n) { err = "cannot load " #n; return false; }
    XS_VK_DEVICE_FNS(XS_LOADD)
#undef XS_LOADD
    s->vk.vkGetDeviceQueue(s->device, s->queueFamily, s->queueIndex, &s->queue);
    if (!s->queue) {
        err = "vkGetDeviceQueue returned no queue";
        return false;
    }
    s->vk.vkGetPhysicalDeviceMemoryProperties(s->phys, &s->memProps);
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = s->queueFamily;
    if (s->vk.vkCreateCommandPool(s->device, &pi, nullptr, &s->cmdPool) != VK_SUCCESS) {
        err = "vkCreateCommandPool failed";
        return false;
    }
    for (const FmtInfo& f : kFormats) {
        VkFormatProperties fp;
        s->vk.vkGetPhysicalDeviceFormatProperties(s->phys, f.format, &fp);
        const VkFormatFeatureFlags need = f.pix == FmtInfo::Depth ? VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT
                                                                   : (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT);
        if ((fp.optimalTilingFeatures & need) == need) s->formats.push_back(f.format);
    }
    return true;
}

void VkSessionShutdown(Session* s) {
    if (!s->device) return;
    for (MirrorSlot& m : s->mirrorSlots) {
        if (m.pending) s->vk.vkWaitForFences(s->device, 1, &m.fence, VK_TRUE, 5ull * 1000 * 1000 * 1000);
        if (m.mapped) s->vk.vkUnmapMemory(s->device, m.bufferMem);
        if (m.buffer) s->vk.vkDestroyBuffer(s->device, m.buffer, nullptr);
        if (m.bufferMem) s->vk.vkFreeMemory(s->device, m.bufferMem, nullptr);
        if (m.image) s->vk.vkDestroyImage(s->device, m.image, nullptr);
        if (m.imageMem) s->vk.vkFreeMemory(s->device, m.imageMem, nullptr);
        if (m.fence) s->vk.vkDestroyFence(s->device, m.fence, nullptr);
        if (m.cmd) s->vk.vkFreeCommandBuffers(s->device, s->cmdPool, 1, &m.cmd);
        m = MirrorSlot{};
    }
    if (s->cmdPool) s->vk.vkDestroyCommandPool(s->device, s->cmdPool, nullptr);
    s->cmdPool = VK_NULL_HANDLE;
    s->device = VK_NULL_HANDLE;
}

// ================================================================================================ swapchains
XrResult XRAPI_CALL xrEnumerateSwapchainFormats(XrSession session, uint32_t cap, uint32_t* count, int64_t* formats) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Session* s = GetSession(session);
    if (!s) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "session");
    std::vector<int64_t> list;
    for (VkFormat f : s->formats) list.push_back((int64_t)f);
    return TwoCall(cap, count, formats, list);
}

XrResult XRAPI_CALL xrCreateSwapchain(XrSession session, const XrSwapchainCreateInfo* ci, XrSwapchain* swapchain) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Session* s = GetSession(session);
    if (!s) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "session");
    if (!ci || !swapchain || ci->type != XR_TYPE_SWAPCHAIN_CREATE_INFO) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "createInfo");
    const XrSwapchainCreateFlags knownFlags = XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT | XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
    if (ci->createFlags & ~knownFlags) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "createFlags 0x%llx", (unsigned long long)ci->createFlags);
    if (ci->createFlags & XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT) return XS_FAIL(XR_ERROR_FEATURE_UNSUPPORTED, "protected content");
    const XrSwapchainUsageFlags knownUsage = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                             XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT |
                                             XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                                             XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT | XR_SWAPCHAIN_USAGE_INPUT_ATTACHMENT_BIT_KHR;
    if (ci->usageFlags & ~knownUsage) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "usageFlags 0x%llx", (unsigned long long)ci->usageFlags);
    const FmtInfo* fmt = FindFmt((VkFormat)ci->format);
    if (!fmt || std::find(s->formats.begin(), s->formats.end(), (VkFormat)ci->format) == s->formats.end())
        return XS_FAIL(XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED, "VkFormat %lld", (long long)ci->format);
    const bool depth = fmt->pix == FmtInfo::Depth;
    if (depth && (ci->usageFlags & (XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT)))
        return XS_FAIL(XR_ERROR_FEATURE_UNSUPPORTED, "color / storage usage on a depth format");
    if (!depth && (ci->usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))
        return XS_FAIL(XR_ERROR_FEATURE_UNSUPPORTED, "depth usage on a color format");
    if (ci->width == 0 || ci->height == 0 || ci->width > 8192 || ci->height > 8192)
        return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "size %ux%u", ci->width, ci->height);
    if (ci->faceCount != 1) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "faceCount %u (cube swapchains are not supported)", ci->faceCount);
    if (ci->arraySize == 0 || ci->mipCount == 0) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "arraySize / mipCount 0");
    if (ci->sampleCount == 0 || (ci->sampleCount & (ci->sampleCount - 1)) || ci->sampleCount > s->inst->cfg.maxSamples)
        return XS_FAIL(XR_ERROR_FEATURE_UNSUPPORTED, "sampleCount %u", ci->sampleCount);

    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT; // capture / mirror readback
    if (ci->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (ci->usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (ci->usageFlags & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (ci->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (ci->usageFlags & XR_SWAPCHAIN_USAGE_SAMPLED_BIT) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (ci->usageFlags & XR_SWAPCHAIN_USAGE_INPUT_ATTACHMENT_BIT_KHR) usage |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    const VkImageCreateFlags vflags = (ci->usageFlags & XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT) ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
    VkImageFormatProperties ifp;
    if (s->vk.vkGetPhysicalDeviceImageFormatProperties(s->phys, fmt->format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, vflags, &ifp) != VK_SUCCESS ||
        ifp.maxArrayLayers < ci->arraySize || ifp.maxMipLevels < ci->mipCount || !(ifp.sampleCounts & ci->sampleCount) ||
        ifp.maxExtent.width < ci->width || ifp.maxExtent.height < ci->height)
        return XS_FAIL(XR_ERROR_FEATURE_UNSUPPORTED, "the device does not support this image (format %d usage 0x%x)", (int)fmt->format, usage);

    auto sc = std::make_unique<Swapchain>();
    sc->session = s;
    sc->ci = *ci;
    sc->ci.next = nullptr;
    sc->fmt = fmt;
    sc->layout = LayoutForUsage(ci->usageFlags);
    const uint32_t count = (ci->createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) ? 1 : 3;
    auto cleanup = [&] {
        for (VkImage im : sc->images) s->vk.vkDestroyImage(s->device, im, nullptr);
        for (VkDeviceMemory m : sc->mems) s->vk.vkFreeMemory(s->device, m, nullptr);
    };
    for (uint32_t i = 0; i < count; ++i) {
        VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ic.flags = vflags;
        ic.imageType = VK_IMAGE_TYPE_2D;
        ic.format = fmt->format;
        ic.extent = {ci->width, ci->height, 1};
        ic.mipLevels = ci->mipCount;
        ic.arrayLayers = ci->arraySize;
        ic.samples = (VkSampleCountFlagBits)ci->sampleCount;
        ic.tiling = VK_IMAGE_TILING_OPTIMAL;
        ic.usage = usage;
        ic.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImage img = VK_NULL_HANDLE;
        if (s->vk.vkCreateImage(s->device, &ic, nullptr, &img) != VK_SUCCESS) {
            cleanup();
            return XS_FAIL(XR_ERROR_RUNTIME_FAILURE, "vkCreateImage failed");
        }
        sc->images.push_back(img);
        VkMemoryRequirements mr;
        s->vk.vkGetImageMemoryRequirements(s->device, img, &mr);
        const int mt = FindMemType(s, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = (uint32_t)std::max(mt, 0);
        VkDeviceMemory mem = VK_NULL_HANDLE;
        if (mt < 0 || s->vk.vkAllocateMemory(s->device, &ai, nullptr, &mem) != VK_SUCCESS) {
            cleanup();
            return XS_FAIL(XR_ERROR_OUT_OF_MEMORY, "vkAllocateMemory failed (%llu bytes)", (unsigned long long)mr.size);
        }
        sc->mems.push_back(mem);
        s->vk.vkBindImageMemory(s->device, img, mem, 0);
    }
    VkCommandBuffer cb = BeginCmd(s);
    for (VkImage img : sc->images)
        ImageBarrier(s, cb, img, fmt->aspect, 0, ci->arraySize, ci->mipCount, VK_IMAGE_LAYOUT_UNDEFINED, sc->layout, 0,
                     VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    if (!cb || !SubmitWait(s, cb)) {
        cleanup();
        return XS_FAIL(XR_ERROR_RUNTIME_FAILURE, "initial layout transition failed");
    }
    Swapchain* p = sc.get();
    s->swapchains.push_back(p);
    *swapchain = (XrSwapchain)Register(std::move(sc));
    Log("xrCreateSwapchain: %ux%u x%u format %d usage 0x%llx samples %u mips %u images %u layout %d", ci->width, ci->height,
        ci->arraySize, (int)fmt->format, (unsigned long long)ci->usageFlags, ci->sampleCount, ci->mipCount, count, (int)p->layout);
    return XR_SUCCESS;
}

void DestroySwapchainObject(Swapchain* sc) {
    Session* s = sc->session;
    for (MirrorSlot& m : s->mirrorSlots)
        if (m.pending) s->vk.vkWaitForFences(s->device, 1, &m.fence, VK_TRUE, 5ull * 1000 * 1000 * 1000);
    for (VkImage im : sc->images) s->vk.vkDestroyImage(s->device, im, nullptr);
    for (VkDeviceMemory m : sc->mems) s->vk.vkFreeMemory(s->device, m, nullptr);
    auto& v = s->swapchains;
    v.erase(std::remove(v.begin(), v.end(), sc), v.end());
    Unregister(sc->handle);
}

XrResult XRAPI_CALL xrDestroySwapchain(XrSwapchain swapchain) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Swapchain* sc = GetSwapchain(swapchain);
    if (!sc) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "swapchain");
    DestroySwapchainObject(sc);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrEnumerateSwapchainImages(XrSwapchain swapchain, uint32_t cap, uint32_t* count, XrSwapchainImageBaseHeader* images) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Swapchain* sc = GetSwapchain(swapchain);
    if (!sc) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "swapchain");
    if (!count) return XR_ERROR_VALIDATION_FAILURE;
    *count = (uint32_t)sc->images.size();
    if (cap == 0) return XR_SUCCESS;
    if (cap < sc->images.size()) return XR_ERROR_SIZE_INSUFFICIENT;
    if (!images || images[0].type != XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "images[].type");
    auto* vi = reinterpret_cast<XrSwapchainImageVulkanKHR*>(images);
    for (size_t i = 0; i < sc->images.size(); ++i) {
        if (vi[i].type != XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "images[%zu].type", i);
        vi[i].image = sc->images[i];
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrAcquireSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageAcquireInfo* info, uint32_t* index) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Swapchain* sc = GetSwapchain(swapchain);
    if (!sc) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "swapchain");
    if ((info && info->type != XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO) || !index) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "arguments");
    if ((sc->ci.createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) && sc->everAcquired)
        return XS_FAIL(XR_ERROR_CALL_ORDER_INVALID, "static swapchain image acquired twice");
    if (sc->acquired.size() >= sc->images.size()) return XS_FAIL(XR_ERROR_CALL_ORDER_INVALID, "all images already acquired");
    *index = sc->nextIndex;
    sc->acquired.push_back(sc->nextIndex);
    sc->nextIndex = (sc->nextIndex + 1) % (uint32_t)sc->images.size();
    sc->everAcquired = true;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrWaitSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageWaitInfo* info) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Swapchain* sc = GetSwapchain(swapchain);
    if (!sc) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "swapchain");
    if (!info || info->type != XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "waitInfo");
    if (sc->acquired.empty() || sc->frontWaited) return XS_FAIL(XR_ERROR_CALL_ORDER_INVALID, "no acquired image to wait on");
    sc->frontWaited = true; // runtime reads are synchronous: the image is always ready
    return XR_SUCCESS;
}

XrResult XRAPI_CALL xrReleaseSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo* info) {
    std::lock_guard<std::recursive_mutex> lk(g_lock);
    Swapchain* sc = GetSwapchain(swapchain);
    if (!sc) return XS_FAIL(XR_ERROR_HANDLE_INVALID, "swapchain");
    if (info && info->type != XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO) return XS_FAIL(XR_ERROR_VALIDATION_FAILURE, "releaseInfo");
    if (sc->acquired.empty() || !sc->frontWaited) return XS_FAIL(XR_ERROR_CALL_ORDER_INVALID, "no waited image to release");
    sc->lastReleased = sc->acquired.front();
    sc->acquired.pop_front();
    sc->frontWaited = false;
    return XR_SUCCESS;
}

// ================================================================================================ capture
void CaptureViews(Session* s, const std::vector<LayerView>& views) {
    struct Region { const LayerView* v; VkDeviceSize offset; };
    std::vector<Region> regions;
    VkDeviceSize total = 0;
    for (const LayerView& v : views) {
        if (v.sc->ci.sampleCount != 1) {
            Log("capture %s skipped: multisampled swapchain", v.name.c_str());
            continue;
        }
        regions.push_back({&v, total});
        total += (VkDeviceSize)v.rect.extent.width * v.rect.extent.height * v.sc->fmt->bpp;
        total = (total + 15) & ~VkDeviceSize(15);
    }
    if (regions.empty()) return;
    HostBuffer hb;
    if (!CreateHostBuffer(s, total, hb)) {
        Log("capture: cannot allocate a %llu-byte readback buffer", (unsigned long long)total);
        DestroyHostBuffer(s, hb);
        return;
    }
    VkCommandBuffer cb = BeginCmd(s);
    for (const Region& r : regions) {
        const LayerView& v = *r.v;
        WithTransferSrc(s, cb, v.sc, v.image, v.arrayIndex, [&] {
            VkBufferImageCopy c{};
            c.bufferOffset = r.offset;
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, v.arrayIndex, 1};
            c.imageOffset = {v.rect.offset.x, v.rect.offset.y, 0};
            c.imageExtent = {(uint32_t)v.rect.extent.width, (uint32_t)v.rect.extent.height, 1};
            s->vk.vkCmdCopyImageToBuffer(cb, v.sc->images[v.image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, hb.buf, 1, &c);
        });
    }
    VkBufferMemoryBarrier bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.buffer = hb.buf;
    bb.size = VK_WHOLE_SIZE;
    s->vk.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &bb, 0, nullptr);
    if (SubmitWait(s, cb)) {
        Invalidate(s, hb);
        for (const Region& r : regions) {
            const LayerView& v = *r.v;
            const uint32_t w = (uint32_t)v.rect.extent.width, h = (uint32_t)v.rect.extent.height;
            const uint8_t* src = static_cast<const uint8_t*>(hb.map) + r.offset;
            std::vector<uint8_t> raw(src, src + (size_t)w * h * v.sc->fmt->bpp);
            s->inst->out.QueuePng(s->inst->out.dir() + "\\" + v.name, (int)w, (int)h, std::move(raw), v.sc->fmt, v.forceOpaque);
        }
        Log("captured %zu image(s)", regions.size());
    }
    DestroyHostBuffer(s, hb);
}

// ================================================================================================ mirror
namespace {
bool SetupMirror(Session* s, const LayerView* l) {
    const int maxEye = 640;
    int ew = std::min<int>(l->rect.extent.width, maxEye);
    int eh = (int)((int64_t)ew * l->rect.extent.height / l->rect.extent.width);
    if (eh > 720) {
        eh = 720;
        ew = (int)((int64_t)eh * l->rect.extent.width / l->rect.extent.height);
    }
    ew = std::max(ew, 16);
    eh = std::max(eh, 16);
    VkFormatProperties fp;
    s->vk.vkGetPhysicalDeviceFormatProperties(s->phys, VK_FORMAT_R8G8B8A8_SRGB, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
        Log("mirror disabled: R8G8B8A8_SRGB is not a blit destination on this device");
        return false;
    }
    for (MirrorSlot& m : s->mirrorSlots) {
        VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ic.imageType = VK_IMAGE_TYPE_2D;
        ic.format = VK_FORMAT_R8G8B8A8_SRGB;
        ic.extent = {(uint32_t)ew * 2, (uint32_t)eh, 1};
        ic.mipLevels = 1;
        ic.arrayLayers = 1;
        ic.samples = VK_SAMPLE_COUNT_1_BIT;
        ic.tiling = VK_IMAGE_TILING_OPTIMAL;
        ic.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (s->vk.vkCreateImage(s->device, &ic, nullptr, &m.image) != VK_SUCCESS) return false;
        VkMemoryRequirements mr;
        s->vk.vkGetImageMemoryRequirements(s->device, m.image, &mr);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = mr.size;
        const int mt = FindMemType(s, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mt < 0) return false;
        ai.memoryTypeIndex = (uint32_t)mt;
        if (s->vk.vkAllocateMemory(s->device, &ai, nullptr, &m.imageMem) != VK_SUCCESS) return false;
        s->vk.vkBindImageMemory(s->device, m.image, m.imageMem, 0);
        HostBuffer hb;
        if (!CreateHostBuffer(s, (VkDeviceSize)ew * 2 * eh * 4, hb)) return false;
        m.buffer = hb.buf;
        m.bufferMem = hb.mem;
        m.mapped = hb.map;
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ca.commandPool = s->cmdPool;
        ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ca.commandBufferCount = 1;
        if (s->vk.vkAllocateCommandBuffers(s->device, &ca, &m.cmd) != VK_SUCCESS) return false;
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (s->vk.vkCreateFence(s->device, &fi, nullptr, &m.fence) != VK_SUCCESS) return false;
    }
    s->mirror->Start(ew, eh);
    return true;
}
} // namespace

void MirrorViews(Session* s, const LayerView* left, const LayerView* right) {
    if (!s->mirrorOk) {
        if (s->mirrorTried) return;
        s->mirrorTried = true;
        s->mirrorOk = SetupMirror(s, left);
        if (!s->mirrorOk) {
            Log("mirror window disabled (setup failed)");
            s->mirror.reset();
            return;
        }
    }
    const int ew = s->mirror->eyeW, eh = s->mirror->eyeH;
    // present finished readbacks (oldest first), never block the frame
    for (int k = 0; k < 2; ++k) {
        MirrorSlot& m = s->mirrorSlots[(s->mirrorNext + k) % 2];
        if (m.pending && s->vk.vkGetFenceStatus(s->device, m.fence) == VK_SUCCESS) {
            m.pending = false;
            VkMappedMemoryRange mr{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
            mr.memory = m.bufferMem;
            mr.size = VK_WHOLE_SIZE;
            s->vk.vkInvalidateMappedMemoryRanges(s->device, 1, &mr);
            s->mirror->Present(static_cast<const uint8_t*>(m.mapped), ew * 2, eh);
        }
    }
    MirrorSlot& m = s->mirrorSlots[s->mirrorNext];
    if (m.pending) return; // GPU still busy with it: skip this frame's mirror update
    s->mirrorNext = (s->mirrorNext + 1) % 2;
    s->vk.vkResetFences(s->device, 1, &m.fence);
    s->vk.vkResetCommandBuffer(m.cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    s->vk.vkBeginCommandBuffer(m.cmd, &bi);
    ImageBarrier(s, m.cmd, m.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    const LayerView* eyes[2] = {left, right};
    for (int e = 0; e < 2; ++e) {
        const LayerView& v = *eyes[e];
        VkFormatProperties fp;
        s->vk.vkGetPhysicalDeviceFormatProperties(s->phys, v.sc->fmt->format, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT)) continue;
        const VkFilter filter = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        WithTransferSrc(s, m.cmd, v.sc, v.image, v.arrayIndex, [&] {
            VkImageBlit b{};
            b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, v.arrayIndex, 1};
            b.srcOffsets[0] = {v.rect.offset.x, v.rect.offset.y, 0};
            b.srcOffsets[1] = {v.rect.offset.x + v.rect.extent.width, v.rect.offset.y + v.rect.extent.height, 1};
            b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            b.dstOffsets[0] = {e * ew, 0, 0};
            b.dstOffsets[1] = {e * ew + ew, eh, 1};
            s->vk.vkCmdBlitImage(m.cmd, v.sc->images[v.image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 1, &b, filter);
        });
    }
    ImageBarrier(s, m.cmd, m.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {(uint32_t)ew * 2, (uint32_t)eh, 1};
    s->vk.vkCmdCopyImageToBuffer(m.cmd, m.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m.buffer, 1, &c);
    VkBufferMemoryBarrier bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.buffer = m.buffer;
    bb.size = VK_WHOLE_SIZE;
    s->vk.vkCmdPipelineBarrier(m.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &bb, 0, nullptr);
    s->vk.vkEndCommandBuffer(m.cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &m.cmd;
    if (s->vk.vkQueueSubmit(s->queue, 1, &si, m.fence) == VK_SUCCESS) m.pending = true;
}

} // namespace xs
