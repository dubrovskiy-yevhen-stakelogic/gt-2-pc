#include "gt2view/vk_scene_renderer.h"
#include "gt2view/shading_rate.h"
#include "gt2view/title_view.h"
#include "gt2view/hd_picture.h"
#include <filesystem>
#include "gt2view/vr_driving_visuals.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <memory>
#include <vector>
#include <fstream>

static void Check(VkResult result) { if (result != VK_SUCCESS) throw std::runtime_error("Vulkan error " + std::to_string(result)); }
static int validationErrors = 0;
static VKAPI_ATTR VkBool32 VKAPI_CALL Validation(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++validationErrors;
    std::fprintf(stderr, "validation: %s\n", data->pMessage); return VK_FALSE;
}
struct Device {
    gt2view::VkContext::Existing e;
    VkDebugUtilsMessengerEXT debug = VK_NULL_HANDLE;
    ~Device() { if (debug) reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(e.instance, "vkDestroyDebugUtilsMessengerEXT"))(e.instance, debug, nullptr); if (e.device) vkDestroyDevice(e.device, nullptr); if (e.instance) vkDestroyInstance(e.instance, nullptr); }
    Device() {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.pApplicationName = "GT2 render benchmark"; app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ci.pApplicationInfo = &app;
        const char* layer = "VK_LAYER_KHRONOS_validation";
        const char* extension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        if (std::getenv("GT2_VK_VALIDATION")) { ci.enabledLayerCount = 1; ci.ppEnabledLayerNames = &layer; ci.enabledExtensionCount = 1; ci.ppEnabledExtensionNames = &extension; }
        Check(vkCreateInstance(&ci, nullptr, &e.instance));
        if (ci.enabledLayerCount) {
            VkDebugUtilsMessengerCreateInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
            info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            info.pfnUserCallback = Validation;
            Check(reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(e.instance, "vkCreateDebugUtilsMessengerEXT"))(e.instance, &info, nullptr, &debug));
        }
        uint32_t n = 0; Check(vkEnumeratePhysicalDevices(e.instance, &n, nullptr));
        std::vector<VkPhysicalDevice> devices(n); Check(vkEnumeratePhysicalDevices(e.instance, &n, devices.data()));
        for (auto d : devices) {
            VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(d, &p);
            if (p.apiVersion < VK_API_VERSION_1_3) continue;
            e.physical = d;
            if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) break;
        }
        if (!e.physical) throw std::runtime_error("no Vulkan 1.3 device");
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(e.physical, &p); std::printf("GPU: %s\n", p.deviceName);
        vkGetPhysicalDeviceQueueFamilyProperties(e.physical, &n, nullptr);
        std::vector<VkQueueFamilyProperties> queues(n); vkGetPhysicalDeviceQueueFamilyProperties(e.physical, &n, queues.data());
        while (e.queueFamily < n && !(queues[e.queueFamily].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++e.queueFamily;
        if (e.queueFamily == n) throw std::runtime_error("no graphics queue");
        float priority = 1;
        VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qi.queueFamilyIndex = e.queueFamily; qi.queueCount = 1; qi.pQueuePriorities = &priority;
        VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES}; v13.dynamicRendering = VK_TRUE;
        VkPhysicalDeviceMultiviewFeatures mv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES}; mv.multiview = VK_TRUE; mv.pNext = &v13;
        VkPhysicalDeviceFragmentShadingRateFeaturesKHR shadingRate{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
        e.shadingRate = gt2view::ShadingRateFeatures(e.physical, shadingRate);
        const char* rateExtension = VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME;
        if (e.shadingRate) v13.pNext = &shadingRate;
        VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        if (e.shadingRate) { dc.enabledExtensionCount = 1; dc.ppEnabledExtensionNames = &rateExtension; } dc.pNext = &mv; dc.queueCreateInfoCount = 1; dc.pQueueCreateInfos = &qi;
        Check(vkCreateDevice(e.physical, &dc, nullptr, &e.device)); vkGetDeviceQueue(e.device, e.queueFamily, 0, &e.queue);
    }
};
int main(int argc, char** argv) {
    try {
        if (argc >= 4 && std::string(argv[1]) == "--menu-upload") {
            const std::filesystem::path root=argv[2];
            std::string profile; std::ifstream(root/"hd/profile.txt")>>profile;
            gt2::hd::SetRoot(root.string(),profile);
            if(gt2::hd::Asset("images/title.png").empty()) throw std::runtime_error("menu benchmark needs HD title image");
            const int count=std::clamp(std::atoi(argv[3]),1,300);
            Device device; gt2view::VkContext context(device.e);
            gt2view::VkSceneRenderer renderer(context,{1280,960},VK_FORMAT_R8G8B8A8_UNORM);
            gt2view::TitleView view(renderer); gt2::MenuVram vram;
            view.UploadVram(vram);
            const auto start=std::chrono::steady_clock::now();
            for(int i=0;i<count;++i) {
                view.UploadVram(vram);
                if(argc>4 && std::atoi(argv[4])) gt2view::UploadHdPicture(renderer,"title.png",352,480);
            }
            const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()/count;
            std::printf("Menu VRAM upload: %.3f ms/update, %d updates, repeated HD decode %s\n",ms,count,argc>4&&std::atoi(argv[4])?"on":"off");
            return validationErrors ? 2 : 0;
        }
        if (argc < 2) { std::puts("gt2renderbench capture [scale%=175] [MSAA=2] [frames=300] [screenshot.png] [smooth=1] [direct=0] [foveation=0]"); return 1; }
        const int scale = argc > 2 ? std::atoi(argv[2]) : 175, samples = argc > 3 ? std::atoi(argv[3]) : 2;
        const int frames = argc > 4 ? std::atoi(argv[4]) : 300;
        if (scale < 50 || scale > 200 || frames < 1 || frames > 10000 || (samples != 1 && samples != 2 && samples != 4 && samples != 8)) throw std::runtime_error("invalid benchmark arguments");
        Device device; gt2view::VkContext context(device.e);
        gt2view::VkSceneRenderer renderer(context, {1280, 960}, VK_FORMAT_R8G8B8A8_UNORM);
        std::vector<gt2view::DrawItem> items; size_t sceneCount = 0;
        renderer.LoadCapture(argv[1], items, sceneCount);
        if(const char* hdRoot=std::getenv("GT2_BENCH_HD_ROOT")) {
            std::string profile; std::ifstream(std::filesystem::path(hdRoot)/"hd/profile.txt")>>profile;
            gt2::hd::SetRoot(hdRoot,profile); gt2::hd::SetEnabled(true);
            std::ifstream input(argv[1],std::ios::binary);uint32_t h[7]{};
            input.read(reinterpret_cast<char*>(h),sizeof(h));input.seekg(40);
            std::vector<gt2view::SceneVertex> captured(h[2]),ui;
            input.read(reinterpret_cast<char*>(captured.data()),captured.size()*sizeof(captured[0]));
            std::vector<uint32_t> packed(h[3]);
            input.read(reinterpret_cast<char*>(packed.data()),packed.size()*sizeof(uint32_t));
            if(!input) throw std::runtime_error("cannot read HD UI fixture");
            std::vector<uint16_t> words(packed.begin(),packed.end());
            renderer.UploadVram(0,h[3]/1024,words.data());
            for(const auto& item:items) ui.insert(ui.end(),captured.begin()+item.firstVertex,captured.begin()+item.firstVertex+item.vertexCount);
            renderer.ApplyHdUi(ui);
            const auto reconstructed=std::count_if(ui.begin(),ui.end(),[](const auto& v){return (v.flags&gt2view::kReconstructedUi)!=0;});
            if(!reconstructed) throw std::runtime_error("HD UI fixture loaded no reconstructed text");
            size_t at=0;
            for(const auto& item:items) {
                std::vector<gt2view::SceneVertex> range(ui.begin()+at,ui.begin()+at+item.vertexCount);
                renderer.SetVertices(item.firstVertex,range);at+=item.vertexCount;
            }
            std::printf("HD UI live load: %zu reconstructed vertices\n",size_t(reconstructed));
        }
        // Repeat the same captured camera in both eyes; this is a GPU workload replay,
        // not an OpenXR/compositor or gameplay benchmark. Keep the original clip matrices.
        gt2view::StereoViews views;
        for (int eye = 0; eye < 2; ++eye) for (int i = 0; i < 4; ++i)
            views.worldVP[eye][i * 5] = views.skyVP[eye][i * 5] = views.hudVP[eye][i * 5] = 1;
        // Preserve sky/world/HUD classification: it selects the real texture and clipping paths.
        renderer.SetStereoViews(views);
        // Replay live buffer updates to compare immediate/staged uploads, including
        // two overlapping writes. Fixtures come only from the supplied local capture.
        std::vector<gt2view::SceneVertex> reupload;
        std::vector<uint16_t> vramRow;
        uint32_t firstVertex=0, externalTexel=0;
        const char* uploadMode=std::getenv("GT2_BENCH_REUPLOAD");
        const bool overlapVram=uploadMode && std::atoi(uploadMode)>1;
        if (uploadMode && !items.empty()) {
            std::ifstream file(argv[1],std::ios::binary); uint32_t header[7];
            file.read(reinterpret_cast<char*>(header),sizeof(header));
            firstVertex=items[0].firstVertex; reupload.resize(items[0].vertexCount);
            file.seekg(40+uint64_t(firstVertex)*sizeof(gt2view::SceneVertex));
            file.read(reinterpret_cast<char*>(reupload.data()),reupload.size()*sizeof(gt2view::SceneVertex));
            file.seekg(40+uint64_t(header[2])*sizeof(gt2view::SceneVertex));
            uint32_t row[1024];file.read(reinterpret_cast<char*>(row),sizeof(row));
            vramRow.assign(std::begin(row),std::end(row));
            file.seekg(40+uint64_t(header[2])*sizeof(gt2view::SceneVertex)+uint64_t(header[3])*4);
            file.read(reinterpret_cast<char*>(&externalTexel),4);
            if(!file)throw std::runtime_error("cannot read upload fixture");
        }
        const auto capturedItems=items; const size_t capturedScene=sceneCount;
        const char* handMode=std::getenv("GT2_BENCH_HANDS");
        const bool combined=handMode && std::atoi(handMode)>=2;
        const bool animated=handMode && std::atoi(handMode)==3;
        const char* deferMode=std::getenv("GT2_BENCH_DEFER");
        const bool deferUploads=!deferMode || std::atoi(deferMode)!=0;
        std::unique_ptr<gt2view::VrDrivingVisuals> handCheck;
        gt2::vr::TrackedControllers tracking;
        gt2::vr::DrivingController driving; gt2::vr::DrivingSettings settings; settings.mode = 1;
        float handMatrix[16]{}; handMatrix[0]=1;handMatrix[5]=-1;handMatrix[11]=-1;handMatrix[14]=.05f;
        if (std::getenv("GT2_BENCH_HANDS")) {
            handCheck = std::make_unique<gt2view::VrDrivingVisuals>(renderer);

            for (int h = 0; h < 2; ++h) {
                tracking.gripValid[h] = tracking.aimValid[h] = true;
                tracking.gripPose[h].position[0] = h == 0 ? -.18f : .18f;
                tracking.gripPose[h].position[1] = -.28f; tracking.gripPose[h].position[2] = -.38f;
                tracking.grip[h] = 1;
            }
            driving.Update(tracking,settings,true);
            // Identity camera facing -Z, with a symmetric 90-degree Vulkan projection.

            items = combined ? capturedItems : std::vector<gt2view::DrawItem>{}; handCheck->Append(items,tracking,driving,settings,handMatrix);sceneCount=combined ? capturedScene : items.size();
        }
        renderer.EnableDecodedTextures(true);
        renderer.SetFoveation(argc > 8 ? std::atoi(argv[8]) : 0);
        gt2view::RenderOptions options; options.msaa = uint32_t(samples); options.smoothTextures = argc < 7 || std::atoi(argv[6]) != 0;
        const char* mipOption=std::getenv("GT2_BENCH_MIPS");
        options.mipmaps=!mipOption || std::atoi(mipOption)!=0;
        renderer.SetOptions(options);
        renderer.CreateStereoTarget({uint32_t(1680 * scale / 100), uint32_t(1760 * scale / 100)}, VK_FORMAT_R8G8B8A8_UNORM, true);
        if (argc > 7 && std::atoi(argv[7])) renderer.SetExternalStereoImage(renderer.StereoImage());
        std::vector<double> gpu; double elapsed = 0, handBuild = 0;
        for (int f = -60; f < frames; ++f) {
            const auto start = std::chrono::steady_clock::now();
            if (deferUploads) renderer.BeginFrameUploads();
            if (!reupload.empty() && (!overlapVram || f == -60 || f >= 0)) {
                auto changed=reupload;for(auto& vertex:changed)vertex.pos[0]+=1000;
                renderer.SetVertices(firstVertex,changed);
                renderer.SetVertices(firstVertex,reupload);
                if (overlapVram) {
                    auto changedRow=vramRow;changedRow[0]^=1;
                    renderer.UploadVram(0,1,changedRow.data());
                }
                renderer.UploadVram(0,1,vramRow.data());
                renderer.UploadExternalTexture(0,1,&externalTexel);
            }
            if (handCheck) {
                if (animated) for (int h=0;h<2;++h) tracking.trigger[h]=.5f+.5f*std::sin(float(f)*.19f);
                items = combined ? capturedItems : std::vector<gt2view::DrawItem>{}; handCheck->Append(items,tracking,driving,settings,handMatrix); sceneCount=combined ? capturedScene : items.size();
                if (f>=0) handBuild += std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            }
            if (std::getenv("GT2_BENCH_FLAT")) renderer.Draw(items, {}, sceneCount);
            else renderer.DrawStereo(items, {}, sceneCount); renderer.WaitFrame();
            if (f >= 0) {
                gpu.push_back(renderer.GpuMilliseconds());
                elapsed += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            }
        }
        if (handCheck) std::printf("Hands build/upload: %.3f ms/frame\n",handBuild/frames);
        double sum = 0; for (double t : gpu) sum += t;
        std::sort(gpu.begin(), gpu.end());
        std::printf("%d%% %dxMSAA smooth=%d draws=%zu scene=%zu: GPU avg %.3f ms p95 %.3f p99 %.3f; CPU+GPU %.3f ms\n",
            scale, samples, int(options.smoothTextures), items.size(), sceneCount, sum / frames, gpu[size_t(frames * 95 / 100)], gpu[size_t(frames * 99 / 100)], elapsed / frames);
        std::printf("Cached materials: %u / %u\n", renderer.CachedMaterials(), renderer.CachedMaterials() + renderer.UncachedMaterials());
        if (std::getenv("GT2_VK_VALIDATION")) {
            for (int level : {0, 1, 3, 2}) { renderer.SetFoveation(level); renderer.DrawStereo(items, {}, sceneCount); renderer.WaitFrame(); }
            for (uint32_t msaa : {1u, 4u, 2u}) { options.msaa = msaa; renderer.SetOptions(options); renderer.DrawStereo(items, {}, sceneCount); renderer.WaitFrame(); }
            options.msaa = uint32_t(samples); renderer.SetOptions(options);
            renderer.SetFoveation(argc > 8 ? std::atoi(argv[8]) : 0);
        }
        if (argc > 5 && argv[5][0]) { renderer.DrawStereo(items, argv[5], sceneCount); renderer.WaitFrame(); }
        return validationErrors ? 2 : 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}
