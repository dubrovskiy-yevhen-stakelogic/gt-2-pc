#include "gt2view/vk_scene_renderer.h"
#include "gt2view/shading_rate.h"
#include "gt2view/surface_format.h"
#include "gt2view/paint_environment.h"
#include "gt2view/title_view.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <stdexcept>

#include "gt2export/png_writer.h"

namespace gt2view {
namespace {

const uint32_t kEnvironmentVertSpv[] =
#include "scene_environment.vert.inc"
    ;
const uint32_t kVertSpv[] =
#include "scene.vert.inc"
    ;
const uint32_t kCachedBlendFragSpv[] =
#include "scene_cached_blend.frag.inc"
    ;
const uint32_t kCachedFragSpv[] =
#include "scene_cached.frag.inc"
    ;
const uint32_t kFragSpv[] =
#include "scene.frag.inc"
    ;
const uint32_t kModelFragSpv[] =
#include "scene_model.frag.inc"
    ;
const uint32_t kModelDiffuseFragSpv[] =
#include "scene_model_diffuse.frag.inc"
    ;
const uint32_t kModelGlassFragSpv[] =
#include "scene_model_glass.frag.inc"
    ;
const uint32_t kUiFragSpv[] =
#include "scene_ui.frag.inc"
    ;
// The stereo variants of the vertex shader (docs/research/vr_port_plan.md): the eye from gl_ViewIndex (one
// multiview pass) or from the eye push constant (one pass per array layer).
const uint32_t kStereoVertSpv[] =
#include "scene_stereo.vert.inc"
    ;
const uint32_t kStereoMultiviewVertSpv[] =
#include "scene_stereo_mv.vert.inc"
    ;

const uint32_t kCachedStereoVertSpv[] =
#include "scene_stereo_cached.vert.inc"
    ;
const uint32_t kCachedStereoMultiviewVertSpv[] =
#include "scene_stereo_cached_mv.vert.inc"
    ;

void Check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string("Vulkan: ") + what + " failed (" + std::to_string(r) + ")");
}

constexpr VkFormat kDepthFormat = VkSceneRenderer::kSceneDepthFormat;

void Transition(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                VkAccessFlags srcAccess, VkAccessFlags dstAccess, VkPipelineStageFlags srcStage,
                VkPipelineStageFlags dstStage, uint32_t layers = 1) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, 1, 0, layers};
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

} // namespace

VkSceneRenderer::VkSceneRenderer(VkContext& context) : VkSceneRenderer(context, VkExtent2D{0, 0}, VK_FORMAT_UNDEFINED) {}

VkSceneRenderer::VkSceneRenderer(VkContext& context, VkExtent2D offscreenExtent, VkFormat offscreenFormat)
    : context_(context), surface_(context.Surface()), physical_(context.PhysicalDevice()), device_(context.Device()), queueFamily_(context.QueueFamily()),
      queue_(context.Queue()) {
    cmdBeginRendering_ = reinterpret_cast<PFN_vkCmdBeginRendering>(vkGetDeviceProcAddr(device_, "vkCmdBeginRendering"));
    cmdEndRendering_ = reinterpret_cast<PFN_vkCmdEndRendering>(vkGetDeviceProcAddr(device_, "vkCmdEndRendering"));
    if (!cmdBeginRendering_) cmdBeginRendering_ = reinterpret_cast<PFN_vkCmdBeginRendering>(vkGetDeviceProcAddr(device_, "vkCmdBeginRenderingKHR"));
    if (!cmdEndRendering_) cmdEndRendering_ = reinterpret_cast<PFN_vkCmdEndRendering>(vkGetDeviceProcAddr(device_, "vkCmdEndRenderingKHR"));
    if (!cmdBeginRendering_ || !cmdEndRendering_) throw std::runtime_error("Vulkan dynamic rendering entry points unavailable");
    if (context_.ShadingRate()) {
        cmdSetShadingRate_ = reinterpret_cast<PFN_vkCmdSetFragmentShadingRateKHR>(vkGetDeviceProcAddr(device_, "vkCmdSetFragmentShadingRateKHR"));
        const auto rates = reinterpret_cast<PFN_vkGetPhysicalDeviceFragmentShadingRatesKHR>(vkGetInstanceProcAddr(context_.Instance(), "vkGetPhysicalDeviceFragmentShadingRatesKHR"));
        if (rates) {
            uint32_t count = 0;
            if (rates(physical_, &count, nullptr) == VK_SUCCESS) {
                std::vector<VkPhysicalDeviceFragmentShadingRateKHR> supported(count, {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_KHR});
                if (rates(physical_, &count, supported.data()) == VK_SUCCESS)
                    for (const auto& rate : supported)
                        if (rate.fragmentSize.width == 2 && rate.fragmentSize.height == 2) softParticleRateSamples_ |= rate.sampleCounts;
            }
        }
        std::printf("soft-particle 2x2 shading sample mask: %u\n", unsigned(softParticleRateSamples_));
    }
    offscreen_ = offscreenExtent.width != 0 && offscreenExtent.height != 0;
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queueFamily_;
    Check(vkCreateCommandPool(device_, &pci, nullptr, &pool_), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    Check(vkAllocateCommandBuffers(device_, &cai, &cmd_), "vkAllocateCommandBuffers");

    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    Check(vkCreateFence(device_, &fci, nullptr, &fence_), "vkCreateFence");
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical_, &properties);
    cacheMenuTextures_ = properties.vendorID == 0x106bu; // Apple: avoid dependent VRAM-buffer reads in menu fragments.
    if (const char* cache = std::getenv("GT2_CACHE_MENU_TEXTURES")) cacheMenuTextures_ = std::strcmp(cache, "1") == 0;
    if (cacheMenuTextures_) std::printf("graphics: hardware palette textures enabled for menus\n");
    cachedMenuShader_ = cacheMenuTextures_;
    if (const char* shader = std::getenv("GT2_CACHE_MENU_SHADER")) cachedMenuShader_ = cacheMenuTextures_ && std::strcmp(shader, "1") == 0;
    if (cachedMenuShader_) std::printf("graphics: texture-only shader enabled for mono menu cars\n");
    uint32_t queueCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &queueCount, nullptr);
    std::vector<VkQueueFamilyProperties> queues(queueCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &queueCount, queues.data());
    timestampBits_ = queues.at(queueFamily_).timestampValidBits;
    timestampNs_ = properties.limits.timestampPeriod;
    if (timestampBits_) {
        VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        query.queryType = VK_QUERY_TYPE_TIMESTAMP; query.queryCount = 4;
        Check(vkCreateQueryPool(device_, &query, nullptr, &gpuQueries_), "vkCreateQueryPool");
    }
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    Check(vkCreateSemaphore(device_, &sci, nullptr, &acquired_), "vkCreateSemaphore");
    Check(vkCreateSemaphore(device_, &sci, nullptr, &rendered_), "vkCreateSemaphore");

    vertexBuffer_ = CreateBuffer(sizeof(SceneVertex) * kMaxVertices, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    environmentCodes_=CreateBuffer(sizeof(uint32_t)*kMaxVertices,VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    rectBuffer_ = CreateBuffer(sizeof(float) * 4 * kMaxVertices, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    std::memset(rectBuffer_.mapped, 0, static_cast<size_t>(rectBuffer_.size));
    textureBuffer_ = CreateBuffer(sizeof(uint32_t) * kVramWidth * kVramRows, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::memset(textureBuffer_.mapped, 0, static_cast<size_t>(textureBuffer_.size));
    vramShadow_.resize(size_t(kVramRows) * kVramWidth);
    decodedTable_ = CreateBuffer(sizeof(decoded_.table), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::memset(decodedTable_.mapped, 0, sizeof(decoded_.table));
    // A tiny valid array descriptor until the first cached stereo scene needs storage.
    decodedImage_ = CreateImage({1, 1}, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 2);
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR; sampler.maxLod = 8.0f;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    Check(vkCreateSampler(device_, &sampler, nullptr, &decodedSampler_), "vkCreateSampler(decoded)");
    handImage_ = CreateImage({1, 1}, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    handUpload_ = CreateBuffer(4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    modelImage_ = CreateImage({1,1}, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    modelUpload_=CreateBuffer(4,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    *static_cast<uint32_t*>(modelUpload_.mapped)=0xffffffffu;
    *static_cast<uint32_t*>(handUpload_.mapped) = 0xffffffffu; handPending_ = true;
    externalBuffer_ = CreateBuffer(sizeof(uint32_t) * kExternalTexels, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::memset(externalBuffer_.mapped, 0, static_cast<size_t>(externalBuffer_.size));
    viewBuffer_ = CreateBuffer(sizeof(StereoViews), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT); // the stereo eyes
    std::memset(viewBuffer_.mapped, 0, static_cast<size_t>(viewBuffer_.size));
    nativeVertexBuffer_=CreateBuffer(size_t(8)*kModelSetVertexStride*sizeof(NativeModelVertex),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    nativeMaterialBuffer_=CreateBuffer(size_t(8)*kNativeMaterialsPerSlot*sizeof(NativeModelMaterial),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::memset(nativeVertexBuffer_.mapped,0,size_t(nativeVertexBuffer_.size));
    std::memset(nativeMaterialBuffer_.mapped,0,size_t(nativeMaterialBuffer_.size));

    if (offscreen_) CreateOffscreen(offscreenExtent, offscreenFormat);
    else CreateSwapchain();
    mirrorColor_ = CreateImage(kMirrorExtent, colorFormat_, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    mirrorDepth_ = CreateImage(kMirrorExtent, kDepthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
        VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    const auto cubeUsage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    paintStudio_=CreateImage({kPaintEnvironmentSize,kPaintEnvironmentSize},VK_FORMAT_R8G8B8A8_UNORM,cubeUsage,VK_SAMPLE_COUNT_1_BIT,VK_IMAGE_ASPECT_COLOR_BIT,6,kPaintEnvironmentMips,true);
    paintCourse_=CreateImage({kPaintEnvironmentSize,kPaintEnvironmentSize},colorFormat_,cubeUsage|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,VK_SAMPLE_COUNT_1_BIT,VK_IMAGE_ASPECT_COLOR_BIT,6,kPaintEnvironmentMips,true);
    nativeEnvironment_=CreateImage({kNativeEnvironmentWidth,kNativeEnvironmentHeight},colorFormat_,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,VK_SAMPLE_COUNT_1_BIT,VK_IMAGE_ASPECT_COLOR_BIT,3);
    nativeEnvironmentDepth_=CreateImage({kNativeEnvironmentWidth,kNativeEnvironmentHeight},kDepthFormat,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,VK_SAMPLE_COUNT_1_BIT,VK_IMAGE_ASPECT_DEPTH_BIT);
    nativeEnvironmentViews_=CreateBuffer(3*64+16+sizeof(NativeSceneLighting),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    std::memset(nativeEnvironmentViews_.mapped,0,size_t(nativeEnvironmentViews_.size));
    SetNativeSceneLighting(NativeSceneLighting{});
    const auto studio=PaintStudioCube();paintUpload_=CreateBuffer(studio.size()*4,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    std::memcpy(paintUpload_.mapped,studio.data(),studio.size()*4);
    CreatePipeline();
}

VkSceneRenderer::~VkSceneRenderer() {
    if (!device_) return;
    vkDeviceWaitIdle(device_);
    DestroyPipelineSet(pipelines_);
    DestroyPipelineSet(modelPipelines_);
    DestroyPipelineSet(modelCullPipelines_);
    DestroyPipelineSet(stereoModelPipelines_);
    DestroyPipelineSet(stereoModelCullPipelines_);
    DestroyPipelineSet(cachedMenuPipelines_);
    DestroyPipelineSet(environmentPipelines_);
    DestroyPipelineSet(uiPipelines_);
    DestroyPipelineSet(msaaPipelines_);
    DestroyPipelineSet(msaaModelPipelines_);DestroyPipelineSet(msaaModelCullPipelines_);
    DestroyPipelineSet(stereoPipelines_);
    DestroyPipelineSet(cachedPipelines_);
    DestroyPipelineSet(cachedHudPipelines_);
    DestroyStereoTarget();
    DestroyImage(mirrorColor_); DestroyImage(mirrorDepth_);
    DestroyImage(paintStudio_);DestroyImage(paintCourse_);DestroyBuffer(paintUpload_);DestroyBuffer(paintProbeUpload_);
    DestroyImage(nativeEnvironment_);DestroyImage(nativeEnvironmentDepth_);DestroyBuffer(nativeEnvironmentViews_);
    vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
    vkDestroyDescriptorPool(device_, descPool_, nullptr);
    vkDestroyDescriptorSetLayout(device_, setLayout_, nullptr);
    DestroySwapchain();
    DestroyBuffer(vertexBuffer_);
    DestroyBuffer(environmentCodes_);
    DestroyBuffer(rectBuffer_);
    DestroyBuffer(decodedUpload_); DestroyBuffer(decodedTable_);
    if (decodedSampler_) vkDestroySampler(device_, decodedSampler_, nullptr);
    DestroyImage(decodedImage_); DestroyImage(handImage_); DestroyBuffer(handUpload_);
    DestroyImage(modelImage_); DestroyBuffer(modelUpload_);
    DestroyBuffer(textureBuffer_);
    DestroyBuffer(externalBuffer_);
    DestroyBuffer(viewBuffer_);
    DestroyBuffer(nativeVertexBuffer_);DestroyBuffer(nativeMaterialBuffer_);
    DestroyBuffer(readback_);
    vkDestroySemaphore(device_, acquired_, nullptr);
    vkDestroySemaphore(device_, rendered_, nullptr);
    if (gpuQueries_) vkDestroyQueryPool(device_, gpuQueries_, nullptr);
    vkDestroyFence(device_, fence_, nullptr);
    vkDestroyCommandPool(device_, pool_, nullptr);
    // The instance, the surface and the device belong to the VkContext.
}

uint32_t VkSceneRenderer::FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags flags) const {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(physical_, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & flags) == flags) return i;
    throw std::runtime_error("no suitable Vulkan memory type");
}

VkSceneRenderer::Buffer VkSceneRenderer::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage) {
    Buffer b;
    b.size = size;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = usage;
    Check(vkCreateBuffer(device_, &bci, nullptr, &b.buffer), "vkCreateBuffer");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, b.buffer, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    Check(vkAllocateMemory(device_, &mai, nullptr, &b.memory), "vkAllocateMemory");
    Check(vkBindBufferMemory(device_, b.buffer, b.memory, 0), "vkBindBufferMemory");
    Check(vkMapMemory(device_, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped), "vkMapMemory");
    return b;
}

void VkSceneRenderer::DestroyBuffer(Buffer& b) {
    if (b.buffer) vkDestroyBuffer(device_, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(device_, b.memory, nullptr);
    b = {};
}

void VkSceneRenderer::CreateSwapchain() {
    VkSurfaceCapabilitiesKHR caps;
    Check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps), "surface capabilities");
    extent_ = caps.currentExtent;
    if (extent_.width == 0xFFFFFFFFu) context_.WindowExtent(extent_);
    if (extent_.width == 0 || extent_.height == 0) return; // minimized

    uint32_t fn = 0;
    Check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &fn, nullptr), "surface formats");
    std::vector<VkSurfaceFormatKHR> formats(fn);
    Check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &fn, formats.data()), "surface formats");
    formats.resize(fn);
    const VkSurfaceFormatKHR chosen = ChooseDisplaySurfaceFormat(formats);
    colorFormat_ = chosen.format;

    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = surface_;
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical_, &properties);
    const uint32_t preferredImages = properties.vendorID == 0x106bu ? 3u : 2u;
    sci.minImageCount = std::max(caps.minImageCount, preferredImages);
    if (caps.maxImageCount) sci.minImageCount = std::min(sci.minImageCount, caps.maxImageCount);
    sci.imageFormat = chosen.format;
    sci.imageColorSpace = chosen.colorSpace;
    sci.imageExtent = extent_;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    // The scene target is blitted into the swapchain image (RenderOptions); harmless for the direct path.
    if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) sci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    // vsync: FIFO (the display's refresh); off: IMMEDIATE (no wait for the compositor; may tear) or MAILBOX.
    presentMode_ = VK_PRESENT_MODE_FIFO_KHR;
    if (!options_.vsync) {
        uint32_t pn = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &pn, nullptr);
        std::vector<VkPresentModeKHR> modes(pn);
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &pn, modes.data());
        for (VkPresentModeKHR m : {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR})
            if (presentMode_ == VK_PRESENT_MODE_FIFO_KHR && std::find(modes.begin(), modes.end(), m) != modes.end()) presentMode_ = m;
    }
    sci.presentMode = presentMode_;
    // Without vsync the compositor may hold two images (shown + queued): more images keep the acquire from waiting for it.
    if (presentMode_ != VK_PRESENT_MODE_FIFO_KHR) sci.minImageCount = std::max(sci.minImageCount, std::min(4u, caps.maxImageCount ? caps.maxImageCount : 4u));
    {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(physical_, colorFormat_, &fp);
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        blitLinear_ = (fp.optimalTilingFeatures & need) == need;
    }
    sci.clipped = VK_TRUE;
    Check(vkCreateSwapchainKHR(device_, &sci, nullptr, &swapchain_), "vkCreateSwapchainKHR");
    std::printf("graphics: swapchain %ux%u, format %d, colorspace %d, present %d, requested images %u\n",
        extent_.width, extent_.height, int(chosen.format), int(chosen.colorSpace), int(presentMode_), sci.minImageCount);

    uint32_t n = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &n, nullptr);
    images_.resize(n);
    vkGetSwapchainImagesKHR(device_, swapchain_, &n, images_.data());
    views_.resize(n);
    for (uint32_t i = 0; i < n; i++) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = images_[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = colorFormat_;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        Check(vkCreateImageView(device_, &vci, nullptr, &views_[i]), "vkCreateImageView");
    }

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = kDepthFormat;
    ici.extent = {extent_.width, extent_.height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    Check(vkCreateImage(device_, &ici, nullptr, &depthImage_), "vkCreateImage(depth)");
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device_, depthImage_, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    Check(vkAllocateMemory(device_, &mai, nullptr, &depthMemory_), "vkAllocateMemory(depth)");
    Check(vkBindImageMemory(device_, depthImage_, depthMemory_, 0), "vkBindImageMemory");
    VkImageViewCreateInfo dvi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    dvi.image = depthImage_;
    dvi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    dvi.format = kDepthFormat;
    dvi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    Check(vkCreateImageView(device_, &dvi, nullptr, &depthView_), "vkCreateImageView(depth)");
    CreateSceneTargets();
}

// The XR path (docs/research/vr_port_plan.md): one colour image instead of a window swapchain. Everything else -
// the pipelines, the scene targets of the graphics options, the screenshots - works exactly as on the window, because
// the frame is recorded through the same RenderTarget; Draw leaves the image in TRANSFER_SRC_OPTIMAL so that the
// session can copy it into the compositor's quad swapchain image.
void VkSceneRenderer::CreateOffscreen(VkExtent2D extent, VkFormat format) {
    extent_ = extent;
    colorFormat_ = format;
    offscreenTarget_ = CreateImage(extent_, colorFormat_,
                                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                   VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    images_.assign(1, offscreenTarget_.image);
    views_.assign(1, offscreenTarget_.view);
    const Image depth = CreateImage(extent_, kDepthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    depthImage_ = depth.image;
    depthMemory_ = depth.memory;
    depthView_ = depth.view;
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(physical_, colorFormat_, &fp);
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    blitLinear_ = (fp.optimalTilingFeatures & need) == need;
    CreateSceneTargets();
}

void VkSceneRenderer::WaitFrame() {
    if (device_ && fence_) Check(vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    if (gpuQueryPending_) {
        uint64_t ticks[4]{};
        if (vkGetQueryPoolResults(device_, gpuQueries_, 0, 4, sizeof(ticks), ticks, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            const uint64_t mask = timestampBits_ >= 64 ? ~uint64_t(0) : (uint64_t(1) << timestampBits_) - 1;
            gpuMs_ = double((ticks[1] - ticks[0]) & mask) * timestampNs_ / 1e6;
            environmentGpuMs_=double((ticks[3]-ticks[2]) & mask)*timestampNs_/1e6;
        }
        gpuQueryPending_ = false;
    }
}

VkSampleCountFlagBits VkSceneRenderer::ClampSamples(uint32_t requested) const {
    if (!physical_ || requested <= 1) return VK_SAMPLE_COUNT_1_BIT;
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical_, &props);
    const VkSampleCountFlags ok = props.limits.framebufferColorSampleCounts & props.limits.framebufferDepthSampleCounts;
    for (uint32_t n = 8; n >= 2; n >>= 1)
        if (n <= requested && (ok & n)) return VkSampleCountFlagBits(n);
    return VK_SAMPLE_COUNT_1_BIT;
}

VkSceneRenderer::Image VkSceneRenderer::CreateImage(VkExtent2D extent, VkFormat format, VkImageUsageFlags usage, VkSampleCountFlagBits samples,
                                                    VkImageAspectFlags aspect, uint32_t layers, uint32_t mipLevels,bool cube) {
    Image img;
    img.layers = layers; img.mipLevels = mipLevels;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.flags=cube?VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT:0;
    ici.format = format;
    ici.extent = {extent.width, extent.height, 1};
    ici.mipLevels = mipLevels;
    ici.arrayLayers = layers;
    ici.samples = samples;
    ici.usage = usage;
    Check(vkCreateImage(device_, &ici, nullptr, &img.image), "vkCreateImage(scene)");
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device_, img.image, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) {
        VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(physical_, &mp);
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
            if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT)) {
                mai.memoryTypeIndex = i; break;
            }
    }
    Check(vkAllocateMemory(device_, &mai, nullptr, &img.memory), "vkAllocateMemory(scene)");
    Check(vkBindImageMemory(device_, img.image, img.memory, 0), "vkBindImageMemory(scene)");
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = img.image;
    vci.viewType = cube?VK_IMAGE_VIEW_TYPE_CUBE:layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {aspect, 0, mipLevels, 0, layers};
    Check(vkCreateImageView(device_, &vci, nullptr, &img.view), "vkCreateImageView(scene)");
    if (layers > 1) // one view per eye: the two-pass stereo path renders into each array layer on its own
        for (uint32_t l = 0; l < layers && l < 6; l++) {
            vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vci.subresourceRange = {aspect, 0, 1, l, 1};
            Check(vkCreateImageView(device_, &vci, nullptr, &img.layer[l]), "vkCreateImageView(eye)");
        }
    return img;
}

void VkSceneRenderer::DestroyImage(Image& img) {
    for (VkImageView& v : img.layer)
        if (v) vkDestroyImageView(device_, v, nullptr);
    if (img.view) vkDestroyImageView(device_, img.view, nullptr);
    if (img.image && img.ownsImage) vkDestroyImage(device_, img.image, nullptr);
    if (img.memory) vkFreeMemory(device_, img.memory, nullptr);
    img = {};
}

void VkSceneRenderer::CreateSceneTargets() {
    DestroySceneTargets();
    const VkSampleCountFlagBits samples = ClampSamples(options_.msaa);
    const float scale = std::clamp(options_.sceneScale, 0.25f, 4.0f);
    if (!options_.sceneWidth && !options_.sceneHeight && scale == 1.0f && samples == VK_SAMPLE_COUNT_1_BIT) return; // the direct path: everything into the swapchain image
    if (extent_.width == 0 || extent_.height == 0) return;
    sceneExtent_ = {std::max(1u, uint32_t(std::lround(float(extent_.width) * scale))), std::max(1u, uint32_t(std::lround(float(extent_.height) * scale)))};
    if (options_.sceneWidth && options_.sceneHeight) sceneExtent_ = {options_.sceneWidth, options_.sceneHeight};
    sceneColor_ = CreateImage(sceneExtent_, colorFormat_, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SAMPLE_COUNT_1_BIT,
                              VK_IMAGE_ASPECT_COLOR_BIT);
    if (samples != VK_SAMPLE_COUNT_1_BIT)
        sceneMsaa_ = CreateImage(sceneExtent_, colorFormat_, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, samples, VK_IMAGE_ASPECT_COLOR_BIT);
    sceneDepth_ = CreateImage(sceneExtent_, kDepthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, samples, VK_IMAGE_ASPECT_DEPTH_BIT);
    if (samples != msaaSamples_) {
        DestroyPipelineSet(msaaPipelines_);
    DestroyPipelineSet(msaaModelPipelines_);DestroyPipelineSet(msaaModelCullPipelines_);
        msaaSamples_ = samples;
        if (samples != VK_SAMPLE_COUNT_1_BIT) {
            CreatePipelineSet(samples, msaaPipelines_);
            CreatePipelineSet(samples,msaaModelPipelines_,false,0,false,false,true);
            CreatePipelineSet(samples,msaaModelCullPipelines_,false,0,false,false,true,false,true);
        }
    }
    sceneTargets_ = true;
}

void VkSceneRenderer::DestroySceneTargets() {
    DestroyImage(sceneColor_);
    DestroyImage(sceneMsaa_);
    DestroyImage(sceneDepth_);
    sceneTargets_ = false;
}

void VkSceneRenderer::SetOptions(const RenderOptions& options) {
    if (options == options_) return;
    vkDeviceWaitIdle(device_);
    const bool presentChanged = options.vsync != options_.vsync && !offscreen_; // no presentation of our own in the XR path
    options_ = options;
    if (presentChanged) {
        DestroySwapchain(); // recreated with the new present mode by the next Draw
        return;
    }
    if (swapchain_ || offscreen_) CreateSceneTargets();
    // The stereo target carries its own MSAA images and pipelines: rebuild them when the sample count changed.
    if (stereoReady_ && ClampSamples(options_.msaa) != stereoSamples_) CreateStereoTarget(stereoExtent_, stereoFormat_, stereoMultiview_);
}

void VkSceneRenderer::DestroySwapchain() {
    DestroySceneTargets();
    if (offscreen_) DestroyImage(offscreenTarget_); // views_[0] is its view
    else
        for (VkImageView v : views_) vkDestroyImageView(device_, v, nullptr);
    views_.clear();
    images_.clear();
    if (depthView_) vkDestroyImageView(device_, depthView_, nullptr);
    if (depthImage_) vkDestroyImage(device_, depthImage_, nullptr);
    if (depthMemory_) vkFreeMemory(device_, depthMemory_, nullptr);
    depthView_ = VK_NULL_HANDLE;
    depthImage_ = VK_NULL_HANDLE;
    depthMemory_ = VK_NULL_HANDLE;
    if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

void VkSceneRenderer::CreatePipeline() {
    // Binding 0: the PS1 VRAM words; binding 1: the external RGBA8 texture store (kExternalTexture); binding 2: the
    // two eyes of a stereo frame (only scene_stereo.vert reads it, the desktop shaders ignore it).
    VkDescriptorSetLayoutBinding bindings[14]{};
    for (uint32_t i = 0; i < 2; i++) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    bindings[3] = {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[4] = {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr};
    bindings[5] = {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[6] = {6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[7] = {7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[8] = {8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[9] = {9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[10]={10,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT,nullptr};
    bindings[11]={11,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
    bindings[12]={12,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
    bindings[13]={13,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
    lci.bindingCount = 14;
    lci.pBindings = bindings;
    Check(vkCreateDescriptorSetLayout(device_, &lci, nullptr, &setLayout_), "vkCreateDescriptorSetLayout");

    const VkDescriptorPoolSize poolSizes[3] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 10}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 14}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 2;
    dpi.poolSizeCount = 3;
    dpi.pPoolSizes = poolSizes;
    Check(vkCreateDescriptorPool(device_, &dpi, nullptr, &descPool_), "vkCreateDescriptorPool");
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = descPool_;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &setLayout_;
    Check(vkAllocateDescriptorSets(device_, &dai, &descSet_), "vkAllocateDescriptorSets");
    Check(vkAllocateDescriptorSets(device_, &dai, &mirrorSourceSet_), "vkAllocateDescriptorSets(mirror)");
    const VkDescriptorBufferInfo dbi[3] = {{textureBuffer_.buffer, 0, VK_WHOLE_SIZE}, {externalBuffer_.buffer, 0, VK_WHOLE_SIZE},
                                           {viewBuffer_.buffer, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet writes[14]{};
    for (uint32_t i = 0; i < 3; i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = descSet_;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[i].pBufferInfo = &dbi[i];
    }
    const VkDescriptorImageInfo decodedInfo{decodedSampler_, decodedImage_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorBufferInfo tableInfo{decodedTable_.buffer, 0, VK_WHOLE_SIZE};
    writes[3] = writes[0]; writes[3].dstBinding = 3; writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[3].pBufferInfo = nullptr; writes[3].pImageInfo = &decodedInfo;
    writes[4] = writes[1]; writes[4].dstBinding = 4; writes[4].pBufferInfo = &tableInfo;
    const VkDescriptorImageInfo handInfo{decodedSampler_, handImage_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    writes[5] = writes[3]; writes[5].dstBinding = 5; writes[5].pImageInfo = &handInfo;
    const VkDescriptorImageInfo mirrorInfo{decodedSampler_, mirrorColor_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    writes[6] = writes[3]; writes[6].dstBinding = 6; writes[6].pImageInfo = &mirrorInfo;
    const VkDescriptorImageInfo modelInfo{decodedSampler_,modelImage_.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    writes[7]=writes[3];writes[7].dstBinding=7;writes[7].pImageInfo=&modelInfo;
    const VkDescriptorImageInfo studioInfo{decodedSampler_,paintStudio_.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo courseInfo{decodedSampler_,paintCourse_.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    writes[8]=writes[3];writes[8].dstBinding=8;writes[8].pImageInfo=&studioInfo;
    writes[9]=writes[3];writes[9].dstBinding=9;writes[9].pImageInfo=&courseInfo;
    const VkDescriptorBufferInfo nativeVertices{nativeVertexBuffer_.buffer,0,VK_WHOLE_SIZE},nativeMaterials{nativeMaterialBuffer_.buffer,0,VK_WHOLE_SIZE};
    writes[10]=writes[4];writes[10].dstBinding=10;writes[10].pBufferInfo=&nativeVertices;
    writes[11]=writes[4];writes[11].dstBinding=11;writes[11].pBufferInfo=&nativeMaterials;
    const VkDescriptorImageInfo nativeEnvInfo{decodedSampler_,nativeEnvironment_.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorBufferInfo nativeViewsInfo{nativeEnvironmentViews_.buffer,0,VK_WHOLE_SIZE};
    writes[12]=writes[3];writes[12].dstBinding=12;writes[12].pImageInfo=&nativeEnvInfo;
    writes[13]=writes[2];writes[13].dstBinding=13;writes[13].pBufferInfo=&nativeViewsInfo;
    vkUpdateDescriptorSets(device_, 14, writes, 0, nullptr);
    // The source pass must not bind its own color attachment for sampling.
    for (auto& write : writes) write.dstSet = mirrorSourceSet_;
    writes[6].pImageInfo = &handInfo;
    writes[9].pImageInfo = &studioInfo;
    writes[12].pImageInfo = &decodedInfo;
    vkUpdateDescriptorSets(device_, 14, writes, 0, nullptr);

    VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(FrameParams)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &setLayout_;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    Check(vkCreatePipelineLayout(device_, &pli, nullptr, &pipelineLayout_), "vkCreatePipelineLayout");
    CreatePipelineSet(VK_SAMPLE_COUNT_1_BIT, pipelines_);
    CreatePipelineSet(VK_SAMPLE_COUNT_1_BIT, modelPipelines_, false, 0, false, false, true);
    CreatePipelineSet(VK_SAMPLE_COUNT_1_BIT,modelCullPipelines_,false,0,false,false,true,false,true);
    CreatePipelineSet(VK_SAMPLE_COUNT_1_BIT, uiPipelines_, false, 0, false, false, false, true);
    // Keep the menu shader and track capture shaders independent.
    CreatePipelineSet(VK_SAMPLE_COUNT_1_BIT, cachedMenuPipelines_, false, 0, true);
    CreatePipelineSet(VK_SAMPLE_COUNT_1_BIT,environmentPipelines_,false,0,true,false,false,false,false,true);
}

void VkSceneRenderer::DestroyPipelineSet(VkPipeline set[9]) {
    for (uint32_t i = 0; i < 9; i++) {
        if (set[i]) vkDestroyPipeline(device_, set[i], nullptr);
        set[i] = VK_NULL_HANDLE;
    }
}

void VkSceneRenderer::CreatePipelineSet(VkSampleCountFlagBits samples, VkPipeline out[9], bool stereo, uint32_t viewMask, bool cachedOnly, bool cachedHud, bool modelOnly, bool uiOnly, bool backCull, bool environmentOnly) {
    auto makeModule = [&](const uint32_t* code, size_t bytes) {
        VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smi.codeSize = bytes;
        smi.pCode = code;
        VkShaderModule m;
        Check(vkCreateShaderModule(device_, &smi, nullptr, &m), "vkCreateShaderModule");
        return m;
    };
    VkShaderModule vs;
    if(environmentOnly)vs=makeModule(kEnvironmentVertSpv,sizeof(kEnvironmentVertSpv));
    else if (cachedOnly && stereo) vs = viewMask ? makeModule(kCachedStereoMultiviewVertSpv, sizeof(kCachedStereoMultiviewVertSpv))
                                 : makeModule(kCachedStereoVertSpv, sizeof(kCachedStereoVertSpv));
    else if (stereo) vs = viewMask ? makeModule(kStereoMultiviewVertSpv, sizeof(kStereoMultiviewVertSpv))
                                  : makeModule(kStereoVertSpv, sizeof(kStereoVertSpv));
    else vs = makeModule(kVertSpv, sizeof(kVertSpv));
    VkShaderModule fs = uiOnly ? makeModule(kUiFragSpv, sizeof(kUiFragSpv)) : modelOnly ? makeModule(kModelFragSpv, sizeof(kModelFragSpv)) : cachedOnly ? makeModule(kCachedFragSpv, sizeof(kCachedFragSpv)) : makeModule(kFragSpv, sizeof(kFragSpv));
    const VkShaderModule cachedBlendFs=cachedOnly ? makeModule(kCachedBlendFragSpv,sizeof(kCachedBlendFragSpv)) : VK_NULL_HANDLE;
    const VkShaderModule glassFs=modelOnly ? makeModule(kModelGlassFragSpv,sizeof(kModelGlassFragSpv)) : VK_NULL_HANDLE;
    const VkShaderModule diffuseFs=modelOnly ? makeModule(kModelDiffuseFragSpv,sizeof(kModelDiffuseFragSpv)) : VK_NULL_HANDLE;


    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";
    const VkBool32 hudSpecialization = cachedHud;
    const VkSpecializationMapEntry cacheEntry{0, 0, sizeof(VkBool32)};
    const VkSpecializationInfo cacheInfo{1, &cacheEntry, sizeof(hudSpecialization), &hudSpecialization};
    stages[1].pSpecializationInfo = cachedOnly ? &cacheInfo : nullptr;

    // Binding 1: the UV rectangle of each vertex's triangle (SetVertices), for the smooth texture filter.
    const VkVertexInputBindingDescription vbind[3] = {{0, sizeof(SceneVertex), VK_VERTEX_INPUT_RATE_VERTEX},
                                                      {1, sizeof(float) * 4, VK_VERTEX_INPUT_RATE_VERTEX},
                                                      {2,sizeof(uint32_t),VK_VERTEX_INPUT_RATE_VERTEX}};
    VkVertexInputAttributeDescription attrs[8] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SceneVertex, pos)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(SceneVertex, texel)},
        {2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SceneVertex, color)},
        {3, 0, VK_FORMAT_R32_UINT, offsetof(SceneVertex, page)},
        {4, 0, VK_FORMAT_R32_UINT, offsetof(SceneVertex, clut)},
        {5, 0, VK_FORMAT_R32_UINT, offsetof(SceneVertex, flags)},
        {6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
        {7,2,VK_FORMAT_R32_UINT,0}};
    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vin.vertexBindingDescriptionCount = environmentOnly ? 3 : 2;
    vin.pVertexBindingDescriptions = vbind;
    vin.vertexAttributeDescriptionCount = environmentOnly ? 8 : 7;
    vin.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = backCull ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE; // per-polygon culling in the fragment shader (kCullBack)
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; // = the original's NCLIP < 0 on the y-down screen
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = samples;
    ms.alphaToCoverageEnable = samples != VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL; // reversed Z
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                                  VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = stereo && context_.ShadingRate() && cmdSetShadingRate_ ? 5u : 4u;
    dyn.pDynamicStates = dynStates;

    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = stereo ? &stereoFormat_ : &colorFormat_;
    rendering.depthAttachmentFormat = kDepthFormat;
    rendering.viewMask = viewMask; // multiview: one pass writes both array layers

    VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    VkPipelineFragmentShadingRateStateCreateInfoKHR rateState{VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR};
    rateState.fragmentSize = {1, 1};
    rateState.combinerOps[0] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
    rateState.combinerOps[1] = ((cachedOnly || modelOnly) && !cachedHud) ? VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR : VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
    if (stereo && context_.ShadingRate()) {
        rendering.pNext = &rateState;
        gpi.flags |= VK_PIPELINE_CREATE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR;
    }
    gpi.pNext = &rendering;
    gpi.stageCount = 2;
    gpi.pStages = stages;
    gpi.pVertexInputState = &vin;
    gpi.pInputAssemblyState = &ia;
    gpi.pViewportState = &vp;
    gpi.pRasterizationState = &rs;
    gpi.pMultisampleState = &ms;
    gpi.pDepthStencilState = &ds;
    gpi.pColorBlendState = &cb;
    gpi.pDynamicState = &dyn;
    gpi.layout = pipelineLayout_;
    // PS1 semi-transparency as fixed-function blending: the constant factor (set per draw) carries the 1/2 and
    // 1/4 weights. Blended draws test depth but do not write it, like the original's painter's order.
    struct BlendMode { VkBlendFactor src, dst; VkBlendOp op; };
    const BlendMode modes[4] = {{VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_OP_ADD},
                                {VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD},
                                {VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_REVERSE_SUBTRACT},
                                {VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD}};
    for (uint32_t i = 0; i < 9; i++) {
        stages[1].module=cachedBlendFs && (i<4 || i==6 || i==7 || i==8) ? cachedBlendFs : i==6 && glassFs ? glassFs : (i==4 || i==5) && diffuseFs ? diffuseFs : fs;
        ds.depthCompareOp=VK_COMPARE_OP_GREATER_OR_EQUAL;
        if (i < 4) {
            cba.blendEnable = VK_TRUE;
            cba.srcColorBlendFactor = modelOnly && i==1?VK_BLEND_FACTOR_DST_ALPHA:modes[i].src;
            cba.dstColorBlendFactor = modes[i].dst;
            cba.colorBlendOp = modes[i].op;
            cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            cba.alphaBlendOp = VK_BLEND_OP_ADD;
            ds.depthWriteEnable = VK_FALSE;
            rs.depthBiasEnable = VK_TRUE; // blended layers lie on opaque surfaces (shadows on the road): pull them nearer
        } else {
            cba.blendEnable = i >= 5;
            cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            cba.colorBlendOp = VK_BLEND_OP_ADD;
            cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            cba.alphaBlendOp = VK_BLEND_OP_ADD;
            ds.depthWriteEnable = i == 6 ? VK_FALSE : VK_TRUE;
            rs.depthBiasEnable = VK_FALSE;
        }
        if(i==7) {
            // Only overlays matched to an opaque depth-writing base use this
            // path. Alpha-tested holes have no surface depth, so a reflection
            // cannot fill them with the background's destination alpha.
            ds.depthCompareOp=VK_COMPARE_OP_EQUAL;ds.depthWriteEnable=VK_FALSE;
            rs.depthBiasEnable=VK_FALSE;cba.blendEnable=VK_TRUE;
            cba.srcColorBlendFactor=VK_BLEND_FACTOR_DST_ALPHA;
            cba.dstColorBlendFactor=VK_BLEND_FACTOR_ONE;cba.colorBlendOp=VK_BLEND_OP_ADD;
            cba.srcAlphaBlendFactor=VK_BLEND_FACTOR_ZERO;cba.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE;
        }
        if(i==8) {
            // The reflection geometry repeats the stock body exactly. Depth
            // equality retains texture-cut wheel arches and prevents decal
            // slope bias from lifting hidden panels through wheels/bodywork.
            ds.depthCompareOp=VK_COMPARE_OP_EQUAL;ds.depthWriteEnable=VK_FALSE;
            rs.depthBiasEnable=VK_FALSE;cba.blendEnable=VK_TRUE;
            cba.srcColorBlendFactor=cba.dstColorBlendFactor=VK_BLEND_FACTOR_ONE;
            cba.colorBlendOp=VK_BLEND_OP_ADD;
            cba.srcAlphaBlendFactor=VK_BLEND_FACTOR_ZERO;cba.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE;
        }
        // Native GS alpha stores reflectivity; it is not a sample coverage mask.
        ms.alphaToCoverageEnable = !modelOnly && i < 5 && samples != VK_SAMPLE_COUNT_1_BIT;
        Check(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gpi, nullptr, &out[i]), "vkCreateGraphicsPipelines");
    }
    vkDestroyShaderModule(device_, vs, nullptr);
    vkDestroyShaderModule(device_, fs, nullptr);
    if(cachedBlendFs)vkDestroyShaderModule(device_,cachedBlendFs,nullptr);
    if(glassFs)vkDestroyShaderModule(device_,glassFs,nullptr);
    if(diffuseFs)vkDestroyShaderModule(device_,diffuseFs,nullptr);
}

void VkSceneRenderer::WriteBuffer(Buffer& buffer, size_t offset, const void* data, size_t bytes) {
    if (!bytes) return;
    if (deferUploads_) {
        if (pendingWriteCount_ == pendingWrites_.size()) pendingWrites_.emplace_back();
        auto& write = pendingWrites_[pendingWriteCount_++];
        write.buffer = &buffer; write.offset = offset; write.bytes.resize(bytes);
        std::memcpy(write.bytes.data(), data, bytes);
    } else {
        WaitFrame();
        std::memcpy(static_cast<uint8_t*>(buffer.mapped) + offset, data, bytes);
    }
}
void VkSceneRenderer::FlushFrameUploads() {
    // Preserve call order, including overlapping dynamic ranges and VRAM rows.
    for (size_t i = 0; i < pendingWriteCount_; ++i) {
        const auto& write = pendingWrites_[i];
        std::memcpy(static_cast<uint8_t*>(write.buffer->mapped) + write.offset, write.bytes.data(), write.bytes.size());
    }
    pendingWriteCount_ = 0;
    deferUploads_ = false;
}
void VkSceneRenderer::SetVertices(uint32_t first, const std::vector<SceneVertex>& vertices) {
    if (first>=kModelSetVertexBase && !vertices.empty()) {
        PromoteModelTextures(first,vertices); return;
    }
    SetVerticesRaw(first,vertices);
}
std::vector<SceneVertex> VkSceneRenderer::VerticesForAudit(const DrawItem& item) const {
    if(uint64_t(item.firstVertex)+item.vertexCount>kMaxVertices)throw std::runtime_error("audit vertex range");
    const auto* first=static_cast<const SceneVertex*>(vertexBuffer_.mapped)+item.firstVertex;
    return {first,first+item.vertexCount};
}
void VkSceneRenderer::SetNativeModelVertices(uint32_t first,const std::vector<NativeModelVertex>& vertices) {
    if(uint64_t(first)+vertices.size()>uint64_t(8)*kModelSetVertexStride)throw std::runtime_error("native vertex range");
    WriteBuffer(nativeVertexBuffer_,size_t(first)*sizeof(NativeModelVertex),vertices.data(),vertices.size()*sizeof(NativeModelVertex));
}
void VkSceneRenderer::SetNativeModelMaterials(uint32_t first,const std::vector<NativeModelMaterial>& materials) {
    if(uint64_t(first)+materials.size()>uint64_t(8)*kNativeMaterialsPerSlot)throw std::runtime_error("native material range");
    WriteBuffer(nativeMaterialBuffer_,size_t(first)*sizeof(NativeModelMaterial),materials.data(),materials.size()*sizeof(NativeModelMaterial));
}
void VkSceneRenderer::SetVerticesRaw(uint32_t first, const std::vector<SceneVertex>& vertices) {
    if (uint64_t(first) + vertices.size() > kMaxVertices) throw std::runtime_error("SetVertices: vertex buffer is full");
    boundsCache_.Invalidate(first, uint32_t(vertices.size()));
    if(vertexFlags_.size()<size_t(first)+vertices.size())vertexFlags_.resize(size_t(first)+vertices.size());
    for(size_t i=0;i<vertices.size();++i)vertexFlags_[size_t(first)+i]=vertices[i].flags;
    for(auto it=environmentCodedRanges_.begin();it!=environmentCodedRanges_.end();) {
        const uint32_t start=uint32_t(it->first>>32),count=uint32_t(it->first);
        if(uint64_t(first)<uint64_t(start)+count && uint64_t(start)<uint64_t(first)+vertices.size())it=environmentCodedRanges_.erase(it);else ++it;
    }
    for(auto it=modelRasterRuns_.begin();it!=modelRasterRuns_.end();) {
        const uint32_t start=uint32_t(it->first>>32),count=uint32_t(it->first);
        if(uint64_t(first)<uint64_t(start)+count && uint64_t(start)<uint64_t(first)+vertices.size())it=modelRasterRuns_.erase(it);else ++it;
    }
    for(auto it=modelRanges_.begin();it!=modelRanges_.end();) {
        const uint32_t start=uint32_t(it->first>>32),count=uint32_t(it->first);
        if(uint64_t(first)<uint64_t(start)+count && uint64_t(start)<uint64_t(first)+vertices.size())it=modelRanges_.erase(it);
        else ++it;
    }
    for (auto it = materialRanges_.begin(); it != materialRanges_.end();) {
        const uint32_t start = uint32_t(it->first >> 32), count = uint32_t(it->first);
        if (uint64_t(first) < uint64_t(start) + count && uint64_t(start) < uint64_t(first) + vertices.size()) it = materialRanges_.erase(it);
        else ++it;
    }
    WriteBuffer(vertexBuffer_, size_t(first) * sizeof(SceneVertex), vertices.data(), vertices.size() * sizeof(SceneVertex));
    // Every caller writes triangle lists starting at `first`: the UV rectangle of each triangle for the smooth filter.
    rectScratch_.resize(vertices.size() * 4);
    float* rect = rectScratch_.data();
    for (size_t t = 0; t < vertices.size(); t += 3) {
        const size_t n = std::min<size_t>(3, vertices.size() - t);
        float r[4] = {vertices[t].texel[0], vertices[t].texel[1], vertices[t].texel[0], vertices[t].texel[1]};
        for (size_t k = 1; k < n; k++) {
            r[0] = std::min(r[0], vertices[t + k].texel[0]);
            r[1] = std::min(r[1], vertices[t + k].texel[1]);
            r[2] = std::max(r[2], vertices[t + k].texel[0]);
            r[3] = std::max(r[3], vertices[t + k].texel[1]);
        }
        for (size_t k = 0; k < n; k++) std::memcpy(rect + (t + k) * 4, r, sizeof(r));
    }
    WriteBuffer(rectBuffer_, size_t(first) * 4 * sizeof(float), rect, rectScratch_.size() * sizeof(float));
}

void VkSceneRenderer::UploadVram(uint32_t firstRow, uint32_t rowCount, const uint16_t* words) {
    if (uint64_t(firstRow) + rowCount > kVramRows) throw std::runtime_error("UploadVram: rows out of range");
    if (!rowCount) return;
    if (vramKnown_.empty()) {
        vramKnown_.resize(kVramRows, false);
        vramShadow_.resize(size_t(kVramRows) * kVramWidth);
    }
    auto changed = [&](uint32_t row) {
        const size_t source = size_t(row) * kVramWidth, target = size_t(firstRow + row) * kVramWidth;
        return !vramKnown_[firstRow + row] || !std::equal(words + source, words + source + kVramWidth, vramShadow_.data() + target);
    };
    for (uint32_t row = 0; row < rowCount;) {
        if (!changed(row)) { ++row; continue; }
        const uint32_t begin = row++;
        while (row < rowCount && changed(row)) ++row;
        const size_t source = size_t(begin) * kVramWidth, count = size_t(row - begin) * kVramWidth;
        const size_t target = size_t(firstRow + begin) * kVramWidth;
        const std::vector<uint32_t> converted(words + source, words + source + count);
        WriteBuffer(textureBuffer_, target * sizeof(uint32_t), converted.data(), converted.size() * sizeof(uint32_t));
        std::copy(words + source, words + source + count, vramShadow_.data() + target);
        std::fill(vramKnown_.begin() + firstRow + begin, vramKnown_.begin() + firstRow + row, true);
        decoded_.Invalidate(firstRow + begin, row - begin);
        environmentCodedRanges_.clear();
        for(auto it=hdUiPages_.begin();it!=hdUiPages_.end();) {
            const uint32_t y=(it->first>>16)&0x7fff;
            if(firstRow+begin<y+256 && firstRow+row>y) it=hdUiPages_.erase(it); else ++it;
        }
    }
}

void VkSceneRenderer::UploadExternalTexture(uint32_t firstTexel, uint32_t count, const uint32_t* rgba) {
    if (uint64_t(firstTexel) + count > kExternalTexels) throw std::runtime_error("UploadExternalTexture: texels out of range");
    WriteBuffer(externalBuffer_, size_t(firstTexel) * sizeof(uint32_t), rgba, size_t(count) * sizeof(uint32_t));
    if (firstTexel>=kModelSetTexelBase) {
        modelSourcePixels_[firstTexel].assign(rgba,rgba+count);
        modelAtlas_.Invalidate(firstTexel);
        if(const auto it=modelSourceSizes_.find(firstTexel);it!=modelSourceSizes_.end()) {
            uint32_t origin=0;
            modelAtlas_.Insert(firstTexel,it->second&65535u,it->second>>16,modelSourcePixels_[firstTexel],origin,nativeTextureSources_.contains(firstTexel));
        }
    }
}

void VkSceneRenderer::Draw(const std::vector<DrawItem>& items, const std::string& screenshotPath, size_t sceneItems) {
    if (!screenshotPath.empty())
        if (const char* path = std::getenv("GT2_RENDER_CAPTURE")) SaveCapture(path, items, sceneItems);
    if (!swapchain_ && !offscreen_) {
        CreateSwapchain();
        if (!swapchain_) return;
    }
    // A timeout/device error is not completion: do not overwrite uploads or
    // reset the command buffer while the prior submission may still use them.
    WaitFrame();
    FlushFrameUploads();
    PrepareFrameTextures(items,sceneItems);
    if(gpuQueries_){
        gpuMs_=-1;
    }

    uint32_t imageIndex = 0;
    if (!offscreen_) {
        VkResult acq = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, acquired_, VK_NULL_HANDLE, &imageIndex);
        if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
            vkDeviceWaitIdle(device_);
            DestroySwapchain();
            return;
        }
        if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) Check(acq, "vkAcquireNextImageKHR");
    }
    Check(vkResetFences(device_, 1, &fence_), "vkResetFences");

    const bool capture = !screenshotPath.empty();
    const VkDeviceSize captureBytes = VkDeviceSize(extent_.width) * extent_.height * 4;
    if (capture && readback_.size < captureBytes) {
        DestroyBuffer(readback_);
        readback_ = CreateBuffer(captureBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    }

    Check(vkResetCommandBuffer(cmd_, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    Check(vkBeginCommandBuffer(cmd_, &bi), "vkBeginCommandBuffer");
    if(gpuQueries_){
        vkCmdResetQueryPool(cmd_,gpuQueries_,0,4);
        vkCmdWriteTimestamp(cmd_,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,gpuQueries_,0);
    }
    UploadHandImage();
    UploadModelImage();
    UploadPaintEnvironment();UploadPaintProbe();
    UploadDecodedTextures();

    if(gpuQueries_)vkCmdWriteTimestamp(cmd_,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,gpuQueries_,2);
    RecordNativeEnvironment();
    if(gpuQueries_)vkCmdWriteTimestamp(cmd_,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,gpuQueries_,3);
    RecordMirrorPass(items, sceneItems);
    sceneItems = std::min(sceneItems, items.size());
    if (sceneItems > 0 && sceneTargets_) {
        DrawScenePass(items, sceneItems, imageIndex);
    } else {
        Transition(cmd_, images_[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        Transition(cmd_, depthImage_, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);
        BeginTargetRendering(WindowTarget(imageIndex), VK_ATTACHMENT_LOAD_OP_CLEAR);
        RecordItems(items, 0, items.size(), extent_, pipelines_, sceneItems);
        cmdEndRendering_(cmd_);
    }
    FinishFrame(imageIndex, screenshotPath);
}

// The window's target: the swapchain image `imageIndex` and the window's depth buffer.
RenderTarget VkSceneRenderer::WindowTarget(uint32_t imageIndex) const {
    RenderTarget target;
    target.colorView = views_[imageIndex];
    target.colorFormat = colorFormat_;
    target.depthView = depthView_;
    target.depthFormat = kDepthFormat;
    target.extent = extent_;
    target.layers = 1;
    return target;
}

// Dynamic rendering into `target` (depth cleared and not stored), colour cleared or kept.
void VkSceneRenderer::BeginTargetRendering(const RenderTarget& target, VkAttachmentLoadOp colorLoad) {
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = target.colorView;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = colorLoad;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{clearColor[0], clearColor[1], clearColor[2], recordNativeEnvironment_ ? 0.0f : 1.0f}};
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = target.depthView;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.clearValue.depthStencil = {0.0f, 0}; // reversed Z: 0 = infinitely far
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, target.extent};
    ri.layerCount = target.layers;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    ri.pDepthAttachment = target.depthView ? &depth : nullptr;
    cmdBeginRendering_(cmd_, &ri);
}

void VkSceneRenderer::RecordItems(const std::vector<DrawItem>& items, size_t first, size_t last, VkExtent2D extent, const VkPipeline pipelines[9],
                                  size_t sceneItems) {
    VkViewport viewport{0, 0, float(extent.width), float(extent.height), 0, .5f};
    VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(cmd_, 0, 1, &viewport);
    vkCmdSetScissor(cmd_, 0, 1, &scissor);
    const VkDescriptorSet descriptors = recordMirror_ ? mirrorSourceSet_ : descSet_;
    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &descriptors, 0, nullptr);
    const VkBuffer vertexBuffers[3] = {vertexBuffer_.buffer, rectBuffer_.buffer,environmentCodes_.buffer};
    const VkDeviceSize zero[3] = {0, 0,0};
    vkCmdBindVertexBuffers(cmd_, 0, 3, vertexBuffers, zero);
    const uint32_t sceneOptions = (options_.smoothTextures ? kOptionSmoothTextures : 0u) | (options_.affine ? kOptionAffine : 0u) | (options_.mipmaps ? kOptionMipmaps : 0u);
    uint32_t boundPipeline = ~0u;
    int boundShadingRate = -1;
    int boundDepthBias = -1;
    int boundCabinDepth = 0;
    bool clipped = false;
    for (size_t i = first; i < last; i++) {
        const DrawItem& item = items[i];
        if ((item.space == kSpaceMirrorSource) != recordMirror_) continue;
        if (item.vertexCount == 0) continue;
        // Reversed Z: disjoint ranges preserve depth within each layer and
        // guarantee that grass/curbs cannot overwrite occupied interior pixels.
        const int depthLayer=recordStereo_ && item.space==kSpaceScreen?3:int(std::min(item.cabinDepth,2u));
        if(boundCabinDepth!=depthLayer) {
            // Screen HUD must also remain in front of the cabin. Preserve
            // reversed-Z ordering within each layer, including its alpha draws.
            viewport.minDepth=depthLayer==3?.875f:depthLayer==2?.75f:depthLayer==1?.5f:0.f;
            viewport.maxDepth=depthLayer==3?1.f:depthLayer==2?.875f:depthLayer==1?.75f:.5f;
            vkCmdSetViewport(cmd_,0,1,&viewport);
            boundCabinDepth=depthLayer;
        }
        // GS culling was previously delayed until after texture sampling. Split
        // immutable runs without sorting/removing triangles, and reject their
        // backfaces before rasterization. The hint occupies unused brake bits.
        if(!recordMirror_ && !(item.brakeLit & 0x40000000u) && item.firstVertex>=kModelSetVertexBase &&
           uint64_t(item.firstVertex)+item.vertexCount<=kMaxVertices && !std::getenv("GT2_AUDIT_NO_NATIVE_CULL")) {
            const uint64_t key=(uint64_t(item.firstVertex)<<32)|item.vertexCount;
            auto [found,added]=modelRasterRuns_.try_emplace(key);
            if(added) {
                bool native=false,supported=true;
                for(uint32_t v=0;v<item.vertexCount;v+=3) {
                    const uint32_t flags=vertexFlags_.at(item.firstVertex+v);native|=(flags & kModelNative)!=0;
                    if((flags & kTextured) && !(flags & kModelAtlasTexture))supported=false;
                    const bool cull=(flags & (kModelNative|kCullBack))==(kModelNative|kCullBack);
                    if(found->second.empty() || found->second.back().cull!=cull)found->second.push_back({item.firstVertex+v,3,cull});
                    else found->second.back().count+=3;
                }
                if(!native || !supported)found->second.clear();
            }
            if(!found->second.empty()) {
                std::vector<DrawItem> parts;for(const auto& run:found->second) {
                    auto part=item;part.firstVertex=run.first;part.vertexCount=run.count;
                    part.brakeLit|=0x40000000u|(run.cull?0x80000000u:0u);parts.push_back(part);
                }
                RecordItems(parts,0,parts.size(),extent,pipelines,i<sceneItems?parts.size():0);
                // Recursive recording changes bindings, scissor and bias.
                boundPipeline=~0u;boundDepthBias=-1;boundCabinDepth=-1;boundShadingRate=-1;clipped=true;continue;
            }
        }

        if (recordStereo_ && item.space == kSpaceWorld && !item.clearDepth &&
            uint64_t(item.firstVertex) + item.vertexCount <= kMaxVertices) {
            ++culling_.tested;
            const auto& bounds = boundsCache_.Get(static_cast<const SceneVertex*>(vertexBuffer_.mapped), item.firstVertex, item.vertexCount);
            if (!BoundsInStereo(bounds, stereoViews_.worldVP, item.mvp)) { ++culling_.culled; continue; }
        }
        if(recordNativeEnvironment_ && !item.clearDepth && uint64_t(item.firstVertex)+item.vertexCount<=kMaxVertices) {
            const auto& bounds=boundsCache_.Get(static_cast<const SceneVertex*>(vertexBuffer_.mapped),item.firstVertex,item.vertexCount);
            const float identity[]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
            if(!BoundsInView(bounds,identity,item.mvp))continue;
        }
        const bool hud = recordStereo_ && item.space == kSpaceScreen;
        const bool hasClip = item.scissor[2] > item.scissor[0] && item.scissor[3] > item.scissor[1];
        const bool wantClip = hasClip && !hud;
        if (wantClip) {
            const float W = float(extent.width), H = float(extent.height);
            const int32_t x0 = std::clamp(int32_t(std::lround(item.scissor[0] * W)), 0, int32_t(extent.width));
            const int32_t y0 = std::clamp(int32_t(std::lround(item.scissor[1] * H)), 0, int32_t(extent.height));
            const int32_t x1 = std::clamp(int32_t(std::lround(item.scissor[2] * W)), x0, int32_t(extent.width));
            const int32_t y1 = std::clamp(int32_t(std::lround(item.scissor[3] * H)), y0, int32_t(extent.height));
            const VkRect2D rect{{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
            vkCmdSetScissor(cmd_, 0, 1, &rect);
            clipped = true;
        } else if (clipped) {
            vkCmdSetScissor(cmd_, 0, 1, &scissor);
            clipped = false;
        }
        if (item.clearDepth) { // a sub-view: its own depth inside the scissor rectangle (reversed Z: 0 = far)
            VkClearAttachment clear{};
            clear.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            clear.clearValue.depthStencil = {0.0f, 0};
            VkClearRect rect{};
            rect.rect = scissor;
            if (wantClip) {
                const float W = float(extent.width), H = float(extent.height);
                const int32_t x0 = std::clamp(int32_t(std::lround(item.scissor[0] * W)), 0, int32_t(extent.width));
                const int32_t y0 = std::clamp(int32_t(std::lround(item.scissor[1] * H)), 0, int32_t(extent.height));
                const int32_t x1 = std::clamp(int32_t(std::lround(item.scissor[2] * W)), x0, int32_t(extent.width));
                const int32_t y1 = std::clamp(int32_t(std::lround(item.scissor[3] * H)), y0, int32_t(extent.height));
                rect.rect = {{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
            }
            rect.layerCount = recordLayers_;
            if (rect.rect.extent.width && rect.rect.extent.height) vkCmdClearAttachments(cmd_, 1, &clear, 1, &rect);
        }
        uint32_t which = item.blend==kBlendStockSurfaceEnvironment?8u:item.blend==kBlendNativeSurfaceEnvironment?7u:item.blend==kBlendNativeEnvironment?1u:item.blend < 4 ? item.blend : item.blend == kBlendAlpha ? 6u : (i >= sceneItems ? 5u : 4u);
        const bool cached = ((recordStereo_ && !recordMirror_) || recordNativeEnvironment_) && i < cachedDraws_.size() && cachedDraws_[i];
        // Menu body/wheel ranges carry kCarPaint on their first vertex. They are
        // single-sample screen draws even when the race scene uses MSAA. Leave
        // 2D UI, shadows/reflections, external textures and cache misses on the
        // general shader; the complete cached material path is shared with VR.
        const bool cachedMenu = cachedMenuShader_ && !recordStereo_ && !recordMirror_ && i >= sceneItems &&
            i < cachedDraws_.size() && cachedDraws_[i] && item.firstVertex < kMaxVertices &&
            (static_cast<const SceneVertex*>(vertexBuffer_.mapped)[item.firstVertex].flags & kCarPaint);
        bool modelOnly=false;
        if(!recordMirror_ && (recordStereo_ || pipelines==pipelines_ || pipelines==msaaPipelines_) && item.firstVertex>=kModelSetVertexBase &&
           uint64_t(item.firstVertex)+item.vertexCount<=kMaxVertices && !std::getenv("GT2_AUDIT_GENERAL_MODEL_SHADER")) {
            const uint64_t range=(uint64_t(item.firstVertex)<<32)|item.vertexCount;
            auto [found,added]=modelRanges_.try_emplace(range,true);
            if(added)for(uint32_t v=0;v<item.vertexCount;++v){
                const auto flags=vertexFlags_.at(size_t(item.firstVertex)+v);
                if((flags & kTextured) && !(flags & kModelAtlasTexture)){found->second=false;break;}
            }
            modelOnly=found->second;
        }
        // Native diffuse alpha stores the GS environment-pass weight, not
        // body coverage. Menu placement must not turn an opaque model into
        // an alpha-composited UI quad. Glass has its own explicit blend pass.
        if(modelOnly && item.blend==kBlendOpaque)which=4u;
        // Palette-indexed HD UI uses the same equations in a small shader.
        // Mixing its contour path with the general model shader can hang Adreno.
        // Restrict this pipeline to mono UI ranges and compatible vertex flags.
        bool uiOnly=!recordStereo_ && !recordMirror_ && pipelines==pipelines_ && i>=sceneItems &&
            item.firstVertex>=TitleView::kVertexBase &&
            uint64_t(item.firstVertex)+item.vertexCount<=uint64_t(TitleView::kVertexBase)+TitleView::kVertexLimit &&
            !std::getenv("GT2_AUDIT_GENERAL_UI_SHADER");
        constexpr uint32_t uiFlags=kTextured|kRawTexture|kOverlay|kSemiTransparent|kCullBack|(3u<<kNearTierShift)|
            kIntegerModulate|kClampTextureRect|kHdUi|kSmoothUi|kUiMap|kReconstructedUi|(3u<<8);
        if(uiOnly)for(uint32_t v=0;v<item.vertexCount;++v)if(vertexFlags_.at(size_t(item.firstVertex)+v) & ~uiFlags){uiOnly=false;break;}
        const bool hardwareCull=(item.brakeLit & 0x80000000u)!=0;
        const uint32_t pipelineKey = (hardwareCull?128u:0u)+which + (uiOnly ? 80 : modelOnly ? 64 : cachedMenu ? 48 : cached ? (hud ? 32 : 16) : 0);
        if (pipelineKey != boundPipeline) {
            vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, uiOnly ? uiPipelines_[which] : modelOnly ? (recordStereo_ ? (hardwareCull?stereoModelCullPipelines_[which]:stereoModelPipelines_[which]) : pipelines==msaaPipelines_ ? (hardwareCull?msaaModelCullPipelines_[which]:msaaModelPipelines_[which]) : hardwareCull?modelCullPipelines_[which]:modelPipelines_[which]) : cachedMenu ? cachedMenuPipelines_[which] : cached ? (recordNativeEnvironment_ ? environmentPipelines_[which] : hud ? cachedHudPipelines_[which] : cachedPipelines_[which]) : pipelines[which]);
            const float k = which == 0 ? 0.5f : 0.25f;
            const float constants[4] = {k, k, k, 1.0f};
            vkCmdSetBlendConstants(cmd_, constants);
            boundPipeline = pipelineKey;
        }
        if (recordStereo_ && !recordMirror_ && context_.ShadingRate() && cmdSetShadingRate_) {
            const bool coarse = item.softParticle && which == 1 && item.stpPass == 2 &&
                                (softParticleRateSamples_ & stereoSamples_) != 0;
            const int rate = coarse ? 2 : ((cached || modelOnly) && !hud ? 1 : 0);
            if (rate != boundShadingRate) {
                const VkExtent2D size{coarse ? 2u : 1u, coarse ? 2u : 1u};
                const VkFragmentShadingRateCombinerOpKHR ops[2] = {VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR,
                    rate == 1 ? VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR : VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR};
                cmdSetShadingRate_(cmd_, &size, ops);
                boundShadingRate = rate;
            }
        }
        const bool modelPaint = item.firstVertex < kMaxVertices &&
            (static_cast<const SceneVertex*>(vertexBuffer_.mapped)[item.firstVertex].flags & kModelReflection);
        // Imported clearcoat reuses the exact opaque surface. The PS1 decal
        // bias can reveal hidden panels through the body, and its slope term
        // grows without bound on nearly edge-on triangles. Keep the native
        // paint pass at its authored depth; retain decal bias for PS1 layers.
        const int depthBias = which < 4 && !modelPaint;
        if (depthBias != boundDepthBias) {
            vkCmdSetDepthBias(cmd_, depthBias ? 4096.0f : 0.0f, 0.0f, depthBias ? 8.0f : 0.0f);
            boundDepthBias = depthBias;
        }
        FrameParams params;
        std::memcpy(params.mvp, item.mvp, sizeof(params.mvp));
        params.nativeModelRotation=item.nativeModelRotation;
        params.paint = item.paint;
        params.brakeLit = item.brakeLit & 0x3fffffffu;
        params.stpPass = item.stpPass;
        params.options = i < sceneItems ? sceneOptions : 0u;
        if(i<sceneItems && nativeCameraActive_)params.options|=16u;
        if (!recordMirror_ && (recordStereo_ ? stereoSamples_ : msaaSamples_) != VK_SAMPLE_COUNT_1_BIT) params.options |= 4u;
        params.space = item.space;   // read by the stereo shader only
        params.eye = recordEye_;
        if (hud) for (int k = 0; k < 4; ++k) params.hudClip[k] = hasClip ? item.scissor[k] * 2 - 1 : (k < 2 ? -1.0f : 1.0f);
        vkCmdPushConstants(cmd_, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(FrameParams), &params);
        vkCmdDraw(cmd_, item.vertexCount, 1, item.firstVertex, 0);
    }
}

// A fixed-size mono target replaces the HUD mirror pass in cockpit mode.
// It uses the same low-detail draw list, with no CPU readback or extra eye passes.
void VkSceneRenderer::RecordMirrorPass(const std::vector<DrawItem>& items, size_t sceneItems) {
    size_t first = 0, last = 0;
    for (size_t i = 0; i < std::min(sceneItems, items.size()); ++i) {
        if (items[i].space != kSpaceMirrorSource) continue;
        if (!last) first = i;
        last = i + 1;
    }
    if (!last && mirrorInitialized_) return;
    Transition(cmd_, mirrorColor_.image, VK_IMAGE_ASPECT_COLOR_BIT,
        mirrorInitialized_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, mirrorInitialized_ ? VkAccessFlags(VK_ACCESS_SHADER_READ_BIT) : VkAccessFlags(0),
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, mirrorInitialized_ ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    Transition(cmd_, mirrorDepth_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);
    RenderTarget target;
    target.colorView = mirrorColor_.view; target.colorFormat = colorFormat_;
    target.depthView = mirrorDepth_.view; target.depthFormat = kDepthFormat;
    target.extent = kMirrorExtent; target.layers = 1;
    BeginTargetRendering(target, VK_ATTACHMENT_LOAD_OP_CLEAR);
    recordMirror_ = true;
    RecordItems(items, first, last, kMirrorExtent, pipelines_, sceneItems);
    recordMirror_ = false;
    cmdEndRendering_(cmd_);
    Transition(cmd_, mirrorColor_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    mirrorInitialized_ = true;
}

void VkSceneRenderer::UploadPaintEnvironment() {
    if(!paintPending_)return;
    auto barrier=[&](Image& image,VkImageLayout from,VkImageLayout to,VkAccessFlags src,VkAccessFlags dst,uint32_t mip,uint32_t count) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=image.image;
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        b.oldLayout=from;b.newLayout=to;b.srcAccessMask=src;b.dstAccessMask=dst;
        b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,mip,count,0,6};
        vkCmdPipelineBarrier(cmd_,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);
    };
    barrier(paintStudio_,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,0,kPaintEnvironmentMips);
    VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,6};copy.imageExtent={kPaintEnvironmentSize,kPaintEnvironmentSize,1};
    vkCmdCopyBufferToImage(cmd_,paintUpload_.buffer,paintStudio_.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    for(uint32_t mip=0;mip<kPaintEnvironmentMips;++mip) {
        barrier(paintStudio_,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,mip,1);
        if(mip+1==kPaintEnvironmentMips)break;
        VkImageBlit blit{};blit.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,mip,0,6};blit.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,mip+1,0,6};
        blit.srcOffsets[1]={int32_t(kPaintEnvironmentSize>>mip),int32_t(kPaintEnvironmentSize>>mip),1};
        blit.dstOffsets[1]={int32_t(kPaintEnvironmentSize>>(mip+1)),int32_t(kPaintEnvironmentSize>>(mip+1)),1};
        vkCmdBlitImage(cmd_,paintStudio_.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,paintStudio_.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,VK_FILTER_LINEAR);
    }
    barrier(paintStudio_,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_SHADER_READ_BIT,0,kPaintEnvironmentMips);
    barrier(paintCourse_,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,0,kPaintEnvironmentMips);
    const VkClearColorValue sky{{.35f,.43f,.55f,1}};const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,kPaintEnvironmentMips,0,6};
    vkCmdClearColorImage(cmd_,paintCourse_.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&sky,1,&range);
    barrier(paintCourse_,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,0,kPaintEnvironmentMips);
    paintPending_=false;paintCourseInitialized_=true;
}

void VkSceneRenderer::BakePaintEnvironment(const std::array<std::vector<DrawItem>,6>& faces) {
    // One small static probe per course load. No extra race-frame submissions.
    WaitFrame();FlushFrameUploads();
    std::vector<DrawItem> combined;for(const auto& face:faces)combined.insert(combined.end(),face.begin(),face.end());
    PrepareDecodedTextures(combined,combined.size());
    Check(vkResetFences(device_,1,&fence_),"vkResetFences(paint)");
    Check(vkResetCommandBuffer(cmd_,0),"vkResetCommandBuffer(paint)");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};Check(vkBeginCommandBuffer(cmd_,&begin),"vkBeginCommandBuffer(paint)");
    UploadHandImage();UploadModelImage();UploadDecodedTextures();UploadPaintEnvironment();
    auto barrier=[&](VkImageLayout from,VkImageLayout to,VkAccessFlags src,VkAccessFlags dst,uint32_t mip,uint32_t count) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=paintCourse_.image;
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        b.oldLayout=from;b.newLayout=to;b.srcAccessMask=src;b.dstAccessMask=dst;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,mip,count,0,6};
        vkCmdPipelineBarrier(cmd_,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);
    };
    barrier(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_SHADER_READ_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,0,kPaintEnvironmentMips);
    barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,0,1);
    const VkExtent2D size{kPaintEnvironmentSize,kPaintEnvironmentSize};
    auto depth=CreateImage(size,kDepthFormat,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,VK_SAMPLE_COUNT_1_BIT,VK_IMAGE_ASPECT_DEPTH_BIT);
    Transition(cmd_,depth.image,VK_IMAGE_ASPECT_DEPTH_BIT,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,0,VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);
    const std::array<float,3> saved{clearColor[0],clearColor[1],clearColor[2]};
    recordMirror_=true;
    for(size_t f=0;f<6;++f) {
        RenderTarget target;target.colorView=paintCourse_.layer[f];target.colorFormat=colorFormat_;target.depthView=depth.view;target.depthFormat=kDepthFormat;target.extent=size;
        BeginTargetRendering(target,VK_ATTACHMENT_LOAD_OP_CLEAR);
        auto items=faces[f];for(auto& item:items)item.space=kSpaceMirrorSource;
        RecordItems(items,0,items.size(),size,pipelines_,items.size());cmdEndRendering_(cmd_);
        VkMemoryBarrier sync{VK_STRUCTURE_TYPE_MEMORY_BARRIER};sync.srcAccessMask=sync.dstAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(cmd_,VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,0,1,&sync,0,nullptr,0,nullptr);
    }
    recordMirror_=false;std::copy(saved.begin(),saved.end(),clearColor);
    barrier(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,0,1);
    for(uint32_t mip=1;mip<kPaintEnvironmentMips;++mip) {
        VkImageBlit blit{};blit.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,mip-1,0,6};blit.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,mip,0,6};
        blit.srcOffsets[1]={int32_t(kPaintEnvironmentSize>>(mip-1)),int32_t(kPaintEnvironmentSize>>(mip-1)),1};blit.dstOffsets[1]={int32_t(kPaintEnvironmentSize>>mip),int32_t(kPaintEnvironmentSize>>mip),1};
        vkCmdBlitImage(cmd_,paintCourse_.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,paintCourse_.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,VK_FILTER_LINEAR);
        barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,mip,1);
    }
    barrier(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_SHADER_READ_BIT,0,kPaintEnvironmentMips);
    Check(vkEndCommandBuffer(cmd_),"vkEndCommandBuffer(paint)");VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&cmd_;
    Check(vkQueueSubmit(queue_,1,&submit,fence_),"vkQueueSubmit(paint)");WaitFrame();DestroyImage(depth);
    std::printf("native scene: paint environment captured six 128px course faces\n");
}

namespace {
std::vector<VkBufferImageCopy> PaintProbeCopies() {
    std::vector<VkBufferImageCopy> copies;VkDeviceSize offset=0;
    for(uint32_t mip=0;mip<kPaintEnvironmentMips;++mip) {
        const uint32_t side=kPaintEnvironmentSize>>mip;
        VkBufferImageCopy c{};c.bufferOffset=offset;c.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,mip,0,6};c.imageExtent={side,side,1};
        copies.push_back(c);offset+=VkDeviceSize(side)*side*6*4;
    }
    return copies;
}
}
void VkSceneRenderer::ClearPaintProbes() {
    paintProbes_.clear();paintProbeSelected_=-1;paintProbePending_=false;
    nativeEnvironmentPending_=false;for(auto& face:nativeEnvironmentFaces_)face.clear();
    const float inactive[4]{};WriteBuffer(nativeEnvironmentViews_,3*64,inactive,16);nativeCameraActive_=false;
}
std::vector<uint8_t> VkSceneRenderer::NativeEnvironmentForAudit() {
    WaitFrame();if(!nativeEnvironmentInitialized_)throw std::runtime_error("native environment not rendered");
    const size_t bytes=size_t(kNativeEnvironmentWidth)*kNativeEnvironmentHeight*3*4;
    if(readback_.size<bytes){DestroyBuffer(readback_);readback_=CreateBuffer(bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT);}
    Check(vkResetFences(device_,1,&fence_),"native environment audit fence");
    Check(vkResetCommandBuffer(cmd_,0),"native environment audit command");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};Check(vkBeginCommandBuffer(cmd_,&begin),"native environment audit begin");
    Transition(cmd_,nativeEnvironment_.image,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_SHADER_READ_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,3);
    VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,3};copy.imageExtent={kNativeEnvironmentWidth,kNativeEnvironmentHeight,1};
    vkCmdCopyImageToBuffer(cmd_,nativeEnvironment_.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback_.buffer,1,&copy);
    Transition(cmd_,nativeEnvironment_.image,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,3);
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd_,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
    Check(vkEndCommandBuffer(cmd_),"native environment audit end");VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&cmd_;
    Check(vkQueueSubmit(queue_,1,&submit,fence_),"native environment audit submit");WaitFrame();
    const auto* p=static_cast<const uint8_t*>(readback_.mapped);return {p,p+bytes};
}
void VkSceneRenderer::SetNativeSceneLighting(const NativeSceneLighting& light) {
    WriteBuffer(nativeEnvironmentViews_,3*64+16,&light,sizeof(light));
}
void VkSceneRenderer::QueueNativeEnvironment(const std::array<std::vector<DrawItem>,3>& faces,
        const std::array<std::array<float,16>,3>& cameras) {
    nativeEnvironmentFaces_=faces;nativeEnvironmentPending_=true;
    // This UBO transforms reflection directions (w=0); capture translations
    // never participate. Avoid dirtying immutable shader constants per frame.
    auto directions=cameras;for(auto& m:directions)m[12]=m[13]=m[14]=0;
    if(!nativeCameraKnown_ || directions!=nativeCameraShadow_) {
        WriteBuffer(nativeEnvironmentViews_,0,directions.data(),3*64);
        nativeCameraShadow_=directions;nativeCameraKnown_=true;
    }
    if(!nativeCameraActive_) {
        const float active[]{1,0,0,0};WriteBuffer(nativeEnvironmentViews_,3*64,active,16);nativeCameraActive_=true;
    }
}
void VkSceneRenderer::RecordNativeEnvironment() {
    // This capture belongs to the current frame's command buffer, with no
    // separate submission/wait and no map from a different chunk centre.
    if(!nativeEnvironmentInitialized_ || nativeEnvironmentPending_) {
        Transition(cmd_,nativeEnvironment_.image,VK_IMAGE_ASPECT_COLOR_BIT,
            nativeEnvironmentInitialized_?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,nativeEnvironmentInitialized_?VK_ACCESS_SHADER_READ_BIT:0,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,3);
        Transition(cmd_,nativeEnvironmentDepth_.image,VK_IMAGE_ASPECT_DEPTH_BIT,VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,0,VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);
        const std::array<float,3> saved{clearColor[0],clearColor[1],clearColor[2]};
        clearColor[0]=clearColor[1]=clearColor[2]=0;recordMirror_=true;recordNativeEnvironment_=true;
        auto mainCached=std::move(cachedDraws_);
        for(size_t f=0;f<3;++f) {
            RenderTarget target;target.colorView=nativeEnvironment_.layer[f];target.colorFormat=colorFormat_;
            target.depthView=nativeEnvironmentDepth_.view;target.depthFormat=kDepthFormat;
            target.extent={kNativeEnvironmentWidth,kNativeEnvironmentHeight};
            BeginTargetRendering(target,VK_ATTACHMENT_LOAD_OP_CLEAR);
            const auto& items=nativeEnvironmentFaces_[f];
            cachedDraws_=nativeEnvironmentCached_[f];
            if(std::getenv("GT2_DEBUG_NATIVE_ENV")) {
                size_t inside=0,count=0;const auto* vertices=static_cast<const SceneVertex*>(vertexBuffer_.mapped);
                for(const auto& item:items)for(uint32_t j=0;j<item.vertexCount;++j) {
                    const auto& v=vertices[item.firstVertex+j];float p[4]{};
                    for(int k=0;k<4;++k)p[k]=item.mvp[k]*v.pos[0]+item.mvp[4+k]*v.pos[1]+item.mvp[8+k]*v.pos[2]+item.mvp[12+k];
                    if(p[3]>0 && p[2]>=0 && p[2]<=p[3] && std::abs(p[0])<=p[3] && std::abs(p[1])<=p[3])++inside;
                    ++count;
                }
                std::printf("Env face %zu: %zu items %zu inside/%zu vertices\n",f,items.size(),inside,count);
            }

            RecordItems(items,0,items.size(),target.extent,pipelines_,items.size());cmdEndRendering_(cmd_);
            VkMemoryBarrier sync{VK_STRUCTURE_TYPE_MEMORY_BARRIER};sync.srcAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            sync.dstAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            vkCmdPipelineBarrier(cmd_,VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,0,1,&sync,0,nullptr,0,nullptr);
        }
        recordNativeEnvironment_=false;cachedDraws_=std::move(mainCached);
        recordMirror_=false;std::copy(saved.begin(),saved.end(),clearColor);
        Transition(cmd_,nativeEnvironment_.image,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,3);
        nativeEnvironmentInitialized_=true;nativeEnvironmentPending_=false;
    }
}
void VkSceneRenderer::CachePaintProbe(int key,const std::array<std::vector<DrawItem>,6>& faces) {
    BakePaintEnvironment(faces);
    const auto copies=PaintProbeCopies();const auto& last=copies.back();
    const size_t bytes=size_t(last.bufferOffset)+size_t(last.imageExtent.width)*last.imageExtent.height*24;
    if(!paintProbeUpload_.buffer)paintProbeUpload_=CreateBuffer(bytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    Check(vkResetFences(device_,1,&fence_),"vkResetFences(probe cache)");
    Check(vkResetCommandBuffer(cmd_,0),"vkResetCommandBuffer(probe cache)");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};Check(vkBeginCommandBuffer(cmd_,&begin),"vkBeginCommandBuffer(probe cache)");
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=paintCourse_.image;
    b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,kPaintEnvironmentMips,0,6};
    b.oldLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;b.srcAccessMask=VK_ACCESS_SHADER_READ_BIT;b.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd_,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
    vkCmdCopyImageToBuffer(cmd_,paintCourse_.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,paintProbeUpload_.buffer,uint32_t(copies.size()),copies.data());
    std::swap(b.oldLayout,b.newLayout);b.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd_,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,1,&b);
    Check(vkEndCommandBuffer(cmd_),"vkEndCommandBuffer(probe cache)");VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&cmd_;
    Check(vkQueueSubmit(queue_,1,&submit,fence_),"vkQueueSubmit(probe cache)");WaitFrame();
    const auto* p=static_cast<const uint8_t*>(paintProbeUpload_.mapped);paintProbes_[key].assign(p,p+bytes);
    paintProbeSelected_=-1;
}
void VkSceneRenderer::SelectPaintProbe(int key) {
    if(key==paintProbeSelected_)return;
    const auto it=paintProbes_.find(key);if(it==paintProbes_.end())return;
    WriteBuffer(paintProbeUpload_,0,it->second.data(),it->second.size());paintProbePending_=true;paintProbeSelected_=key;
}
void VkSceneRenderer::UploadPaintProbe() {
    if(!paintProbePending_)return;
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=paintCourse_.image;
    b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,kPaintEnvironmentMips,0,6};
    b.oldLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b.srcAccessMask=VK_ACCESS_SHADER_READ_BIT;b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd_,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
    const auto copies=PaintProbeCopies();vkCmdCopyBufferToImage(cmd_,paintProbeUpload_.buffer,paintCourse_.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,uint32_t(copies.size()),copies.data());
    std::swap(b.oldLayout,b.newLayout);b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd_,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&b);paintProbePending_=false;
}

// The scene at its own resolution / sample count: items[0, sceneItems) into the scene target (MSAA resolved into
// sceneColor_), sceneColor_ blitted into the swapchain image (linear when scaled), then the 2D layers over it at the
// window's resolution with a fresh depth buffer (they are drawn at z = 1, always in front).
void VkSceneRenderer::DrawScenePass(const std::vector<DrawItem>& items, size_t sceneItems, uint32_t imageIndex) {
    const bool msaa = sceneMsaa_.image != VK_NULL_HANDLE;
    Transition(cmd_, sceneColor_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    if (msaa)
        Transition(cmd_, sceneMsaa_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    Transition(cmd_, sceneDepth_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0,
               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = msaa ? sceneMsaa_.view : sceneColor_.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = msaa ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{clearColor[0], clearColor[1], clearColor[2], recordNativeEnvironment_ ? 0.0f : 1.0f}};
    if (msaa) {
        color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        color.resolveImageView = sceneColor_.view;
        color.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = sceneDepth_.view;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.clearValue.depthStencil = {0.0f, 0};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, sceneExtent_};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    ri.pDepthAttachment = &depth;
    cmdBeginRendering_(cmd_, &ri);
    RecordItems(items, 0, sceneItems, sceneExtent_, msaa ? msaaPipelines_ : pipelines_, sceneItems);
    cmdEndRendering_(cmd_);

    Transition(cmd_, sceneColor_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    Transition(cmd_, images_[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {int32_t(sceneExtent_.width), int32_t(sceneExtent_.height), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[1] = {int32_t(extent_.width), int32_t(extent_.height), 1};
    const bool same = sceneExtent_.width == extent_.width && sceneExtent_.height == extent_.height;
    vkCmdBlitImage(cmd_, sceneColor_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, images_[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                   (!same && blitLinear_) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    Transition(cmd_, images_[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    Transition(cmd_, depthImage_, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0,
               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);
    BeginTargetRendering(WindowTarget(imageIndex), VK_ATTACHMENT_LOAD_OP_LOAD);
    RecordItems(items, sceneItems, items.size(), extent_, pipelines_, sceneItems);
    cmdEndRendering_(cmd_);
}

void VkSceneRenderer::FinishFrame(uint32_t imageIndex, const std::string& screenshotPath) {
    const bool capture = !screenshotPath.empty();
    VkImageLayout layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAccessFlags access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (capture) {
        Transition(cmd_, images_[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   access, VK_ACCESS_TRANSFER_READ_BIT, stage, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {extent_.width, extent_.height, 1};
        vkCmdCopyImageToBuffer(cmd_, images_[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_.buffer, 1, &region);
        layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        access = VK_ACCESS_TRANSFER_READ_BIT;
        stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    // The XR path has no presentation: the frame is left ready to be copied into the compositor's swapchain image.
    Transition(cmd_, images_[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT, layout,
               offscreen_ ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, access,
               offscreen_ ? VkAccessFlags(VK_ACCESS_TRANSFER_READ_BIT) : VkAccessFlags(0), stage,
               offscreen_ ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    if(gpuQueries_)vkCmdWriteTimestamp(cmd_,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,gpuQueries_,1);
    Check(vkEndCommandBuffer(cmd_), "vkEndCommandBuffer");

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = offscreen_ ? 0u : 1u;
    si.pWaitSemaphores = &acquired_;
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_;
    si.signalSemaphoreCount = offscreen_ ? 0u : 1u;
    si.pSignalSemaphores = &rendered_;
    Check(vkQueueSubmit(queue_, 1, &si, fence_), "vkQueueSubmit");
    gpuQueryPending_=gpuQueries_!=VK_NULL_HANDLE;

    if (capture) {
        WaitFrame();
        SaveScreenshot(screenshotPath);
    }
    if (offscreen_) return;

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &rendered_;
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &imageIndex;
    VkResult pres = vkQueuePresentKHR(queue_, &pi);
    VkExtent2D drawable{};
    const bool resized = pres == VK_SUBOPTIMAL_KHR && context_.WindowExtent(drawable) && drawable.width && drawable.height &&
                         (drawable.width != extent_.width || drawable.height != extent_.height);
    // SUBOPTIMAL is still a successful presentation. Cocoa can report it while
    // entering fullscreen without changing the drawable; avoid rebuilding every frame.
    if (pres == VK_ERROR_OUT_OF_DATE_KHR || resized) {
        vkDeviceWaitIdle(device_);
        DestroySwapchain();
    } else if (pres != VK_SUBOPTIMAL_KHR) {
        Check(pres, "vkQueuePresentKHR");
    }
}

void VkSceneRenderer::SaveScreenshot(const std::string& path) { SaveReadback(path, extent_, colorFormat_); }

void VkSceneRenderer::SaveReadback(const std::string& path, VkExtent2D extent, VkFormat format) {
    const size_t pixels = size_t(extent.width) * extent.height;
    std::vector<uint8_t> rgba(pixels * 4);
    const auto* src = static_cast<const uint8_t*>(readback_.mapped);
    const bool bgr = format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
    for (size_t i = 0; i < pixels; i++) {
        rgba[i * 4 + 0] = src[i * 4 + (bgr ? 2 : 0)];
        rgba[i * 4 + 1] = src[i * 4 + 1];
        rgba[i * 4 + 2] = src[i * 4 + (bgr ? 0 : 2)];
        rgba[i * 4 + 3] = 255;
    }
    gt2::WritePngRgba(path, int(extent.width), int(extent.height), rgba);
}

// ---------------------------------------------------------------- stereo (docs/research/vr_port_plan.md)

void VkSceneRenderer::CreateStereoTarget(VkExtent2D extent, VkFormat format, bool multiview) {
    if (extent.width == 0 || extent.height == 0) throw std::runtime_error("CreateStereoTarget: empty extent");
    vkDeviceWaitIdle(device_);
    DestroyStereoTarget();
    stereoExtent_ = extent;
    stereoFormat_ = format;
    stereoMultiview_ = multiview;
    stereoSamples_ = ClampSamples(options_.msaa);
    stereoColor_ = CreateImage(stereoExtent_, stereoFormat_, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                               VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT, 2);
    if (stereoSamples_ != VK_SAMPLE_COUNT_1_BIT)
        stereoMsaa_ = CreateImage(stereoExtent_, stereoFormat_, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT, stereoSamples_, VK_IMAGE_ASPECT_COLOR_BIT, 2);
    const VkImageUsageFlags depthUsage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
        (stereoSamples_ == VK_SAMPLE_COUNT_1_BIT ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT);
    stereoDepth_ = CreateImage(stereoExtent_, kDepthFormat, depthUsage,
                               stereoSamples_, VK_IMAGE_ASPECT_DEPTH_BIT, 2);
    DestroyPipelineSet(stereoPipelines_);
    DestroyPipelineSet(cachedPipelines_);
    DestroyPipelineSet(cachedHudPipelines_);
    CreatePipelineSet(stereoSamples_, stereoPipelines_, true, multiview ? 0x3u : 0u);
    DestroyPipelineSet(stereoModelPipelines_);
    DestroyPipelineSet(stereoModelCullPipelines_);
    CreatePipelineSet(stereoSamples_, stereoModelPipelines_, true, multiview ? 0x3u : 0u, false, false, true);
    CreatePipelineSet(stereoSamples_,stereoModelCullPipelines_,true,multiview?0x3u:0u,false,false,true,false,true);
    CreatePipelineSet(stereoSamples_, cachedPipelines_, true, multiview ? 0x3u : 0u, true);
    CreatePipelineSet(stereoSamples_, cachedHudPipelines_, true, multiview ? 0x3u : 0u, true, true);
    stereoReady_ = true;
    CreateRateImage();
}

void VkSceneRenderer::SetExternalStereoImage(VkImage image) {
    externalStereoImage_ = image;
    if (!image || externalStereoViews_.count(image)) return;
    Image target; target.image = image; target.ownsImage = false; target.layers = 2;
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = image; view.format = stereoFormat_; view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2};
    try {
        Check(vkCreateImageView(device_, &view, nullptr, &target.view), "direct stereo view");
        for (uint32_t eye = 0; eye < 2; ++eye) {
            view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.subresourceRange.baseArrayLayer = eye; view.subresourceRange.layerCount = 1;
            Check(vkCreateImageView(device_, &view, nullptr, &target.layer[eye]), "direct stereo eye view");
        }
        externalStereoViews_.emplace(image, target);
    } catch (...) { DestroyImage(target); externalStereoImage_ = VK_NULL_HANDLE; throw; }
}

void VkSceneRenderer::DestroyStereoTarget() {
    DestroyImage(rateImage_); DestroyBuffer(rateUpload_); rateUploaded_ = false;
    externalStereoImage_ = VK_NULL_HANDLE;
    for (auto& [image, views] : externalStereoViews_) { (void)image; DestroyImage(views); }
    externalStereoViews_.clear();
    DestroyImage(stereoColor_);
    DestroyImage(stereoMsaa_);
    DestroyImage(stereoDepth_);
    stereoReady_ = false;
}

// One pass of a stereo frame: `layer` < 0 = a single multiview pass (viewMask 0b11) writing both array layers,
// otherwise one array layer with the eye index in the push constant.
void VkSceneRenderer::RecordStereoPass(const std::vector<DrawItem>& items, size_t sceneItems, int layer) {
    const Image& colorTarget = externalStereoImage_ ? externalStereoViews_.at(externalStereoImage_) : stereoColor_;
    const bool msaa = stereoMsaa_.image != VK_NULL_HANDLE;
    const bool all = layer < 0;
    const size_t eye = all ? 0 : size_t(layer);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = msaa ? (all ? stereoMsaa_.view : stereoMsaa_.layer[eye]) : (all ? colorTarget.view : colorTarget.layer[eye]);
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = msaa ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{clearColor[0], clearColor[1], clearColor[2], recordNativeEnvironment_ ? 0.0f : 1.0f}};
    if (msaa) {
        color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        color.resolveImageView = all ? colorTarget.view : colorTarget.layer[eye];
        color.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = all ? stereoDepth_.view : stereoDepth_.layer[eye];
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    // Only single-sample depth is submitted to OpenXR. Multisample depth is tile-local scratch.
    depth.storeOp = msaa ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
    depth.clearValue.depthStencil = {0.0f, 0}; // reversed Z: 0 = infinitely far
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, stereoExtent_};
    ri.layerCount = 1; // with a view mask the layer count is ignored; a per-eye pass writes its own layer view
    ri.viewMask = all ? 0x3u : 0u;
    VkRenderingFragmentShadingRateAttachmentInfoKHR rateAttachment{VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR};
    if (rateImage_.image) {
        rateAttachment.imageView = rateImage_.view; // one identical rate map is broadcast to both eyes
        rateAttachment.imageLayout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
        rateAttachment.shadingRateAttachmentTexelSize = rateTexel_;
        ri.pNext = &rateAttachment;
    }
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    ri.pDepthAttachment = &depth;
    cmdBeginRendering_(cmd_, &ri);
    recordEye_ = uint32_t(eye);
    recordLayers_ = 1;
    recordStereo_ = true;
    RecordItems(items, 0, items.size(), stereoExtent_, stereoPipelines_, sceneItems);
    recordStereo_ = false;
    recordEye_ = 0;
    cmdEndRendering_(cmd_);
}

std::vector<uint8_t> VkSceneRenderer::CaptureStereoForAudit(const std::vector<DrawItem>& items,size_t sceneItems) {
    if(externalStereoImage_)throw std::runtime_error("audit requires owned stereo target");
    auditStereoReadback_=true;
    try {DrawStereo(items,{},sceneItems);}catch(...){auditStereoReadback_=false;throw;}
    auditStereoReadback_=false;
    const size_t bytes=size_t(stereoExtent_.width)*stereoExtent_.height*2*4;
    const auto* p=static_cast<const uint8_t*>(readback_.mapped);
    return {p,p+bytes};
}
void VkSceneRenderer::DrawStereo(const std::vector<DrawItem>& items, const std::string& screenshotPath, size_t sceneItems) {
    if (!stereoReady_) throw std::runtime_error("DrawStereo: no stereo target");
    const Image& colorTarget = externalStereoImage_ ? externalStereoViews_.at(externalStereoImage_) : stereoColor_;
    culling_ = {};
    WaitFrame();
    FlushFrameUploads();
    PrepareFrameTextures(items,sceneItems);
    Check(vkResetFences(device_, 1, &fence_), "vkResetFences");
    std::memcpy(viewBuffer_.mapped, &stereoViews_, sizeof(StereoViews)); // the frame in flight has finished above

    const bool capture = !screenshotPath.empty() || auditStereoReadback_;
    const VkDeviceSize captureBytes = VkDeviceSize(stereoExtent_.width) * stereoExtent_.height * 4 * (auditStereoReadback_?2:1);
    if (capture && readback_.size < captureBytes) {
        DestroyBuffer(readback_);
        readback_ = CreateBuffer(captureBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    }

    Check(vkResetCommandBuffer(cmd_, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    Check(vkBeginCommandBuffer(cmd_, &bi), "vkBeginCommandBuffer");
    UploadHandImage();
    UploadModelImage();
    UploadPaintEnvironment();UploadPaintProbe();
    UploadDecodedTextures();
    UploadRateImage();
    if (gpuQueries_) {
        vkCmdResetQueryPool(cmd_, gpuQueries_, 0, 4);
        vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, gpuQueries_, 0);
    }
    if(gpuQueries_)vkCmdWriteTimestamp(cmd_,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,gpuQueries_,2);
    RecordNativeEnvironment();
    if(gpuQueries_)vkCmdWriteTimestamp(cmd_,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,gpuQueries_,3);
    RecordMirrorPass(items, sceneItems);
    Transition(cmd_, colorTarget.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 2);
    if (stereoMsaa_.image)
        Transition(cmd_, stereoMsaa_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 2);
    Transition(cmd_, stereoDepth_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0,
               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 2);

    sceneItems = std::min(sceneItems, items.size());
    if (stereoMultiview_) {
        RecordStereoPass(items, sceneItems, -1);
    } else {
        RecordStereoPass(items, sceneItems, 0);
        RecordStereoPass(items, sceneItems, 1);
    }

    if (!externalStereoImage_ || capture) Transition(cmd_, colorTarget.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_PIPELINE_STAGE_TRANSFER_BIT, 2);
    if (stereoSamples_ == VK_SAMPLE_COUNT_1_BIT) Transition(cmd_, stereoDepth_.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
               VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 2);
    if (capture) { // Normal screenshots retain the left eye; the audit reads both.
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, auditStereoReadback_?2u:1u};
        region.imageExtent = {stereoExtent_.width, stereoExtent_.height, 1};
        vkCmdCopyImageToBuffer(cmd_, colorTarget.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_.buffer, 1, &region);
    }
    if (externalStereoImage_ && capture)
        Transition(cmd_, colorTarget.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 2);
    if (gpuQueries_) vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, gpuQueries_, 1);
    Check(vkEndCommandBuffer(cmd_), "vkEndCommandBuffer");

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_;
    Check(vkQueueSubmit(queue_, 1, &si, fence_), "vkQueueSubmit(stereo)");
    gpuQueryPending_ = gpuQueries_ != VK_NULL_HANDLE;
    if (capture) {
        WaitFrame();
        if(!screenshotPath.empty())SaveReadback(screenshotPath, stereoExtent_, stereoFormat_);
    }
}

} // namespace gt2view
