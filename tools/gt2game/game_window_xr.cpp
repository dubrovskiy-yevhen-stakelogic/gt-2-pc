// Windows OpenXR: theatre menus, stereo races and tracked driving controls.
#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR // the desktop mirror's surface
#include <vulkan/vulkan.h>

#include "game_window.h"
#include "pc_overlay.h"
#include "game/audio/pause.h"
#include "gt2view/vr_driving_visuals.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>

#include "gt2view/vk_context.h"
#include "platform/xr/vr_rig.h"
#include "platform/xr/xr_session.h"

namespace gt2game {
namespace {

using Clock = std::chrono::steady_clock;

// The desktop mirror: the same image the headset gets, blitted into a swapchain on the program's window. It is only
// a monitor - it never blocks the compositor's frame (no FIFO presentation) and any failure just switches it off.
class MirrorWindow {
public:
    MirrorWindow(gt2::xr::Session& session, void* hwnd) : session_(session), hwnd_(static_cast<HWND>(hwnd)) {
        VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        sci.hinstance = GetModuleHandleA(nullptr);
        sci.hwnd = hwnd_;
        if (vkCreateWin32SurfaceKHR(session_.VulkanInstance(), &sci, nullptr, &surface_) != VK_SUCCESS) {
            std::printf("xr: no desktop mirror (the runtime's Vulkan instance has no Win32 surface support)\n");
            surface_ = VK_NULL_HANDLE;
            return;
        }
        VkBool32 supported = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(session_.PhysicalDevice(), session_.QueueFamily(), surface_, &supported);
        if (!supported) {
            std::printf("xr: no desktop mirror (the runtime's GPU cannot present to this window)\n");
            Destroy();
            return;
        }
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = session_.QueueFamily();
        vkCreateCommandPool(session_.Device(), &pci, nullptr, &pool_);
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = pool_;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        vkAllocateCommandBuffers(session_.Device(), &cai, &cmd_);
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vkCreateFence(session_.Device(), &fci, nullptr, &fence_);
        VkSemaphoreCreateInfo smi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(session_.Device(), &smi, nullptr, &acquired_);
        vkCreateSemaphore(session_.Device(), &smi, nullptr, &blitted_);
        Create();
    }
    ~MirrorWindow() { Destroy(); }
    MirrorWindow(const MirrorWindow&) = delete;
    MirrorWindow& operator=(const MirrorWindow&) = delete;

    bool Active() const { return swapchain_ != VK_NULL_HANDLE; }

    // `src` is the frame in VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL whose queue work has completed.
    void Show(VkImage src, VkExtent2D srcExtent);

private:
    void Create();
    void DestroySwapchain();
    void Destroy();

    gt2::xr::Session& session_;
    HWND hwnd_ = nullptr;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkExtent2D extent_{};
    std::vector<VkImage> images_;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkSemaphore acquired_ = VK_NULL_HANDLE, blitted_ = VK_NULL_HANDLE;
    bool linear_ = false;
};

void MirrorWindow::Create() {
    if (!surface_) return;
    VkSurfaceCapabilitiesKHR caps{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(session_.PhysicalDevice(), surface_, &caps) != VK_SUCCESS) return;
    extent_ = caps.currentExtent;
    if (extent_.width == 0xFFFFFFFFu) {
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        extent_ = {uint32_t(rc.right - rc.left), uint32_t(rc.bottom - rc.top)};
    }
    if (extent_.width == 0 || extent_.height == 0) return; // minimized
    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(session_.PhysicalDevice(), surface_, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(session_.PhysicalDevice(), surface_, &formatCount, formats.data());
    if (formats.empty()) return;
    VkSurfaceFormatKHR chosen = formats[0];
    for (const VkSurfaceFormatKHR& f : formats)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) chosen = f;
    // Never FIFO: the mirror must not pace the compositor's frames.
    uint32_t modeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(session_.PhysicalDevice(), surface_, &modeCount, nullptr);
    std::vector<VkPresentModeKHR> modes(modeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(session_.PhysicalDevice(), surface_, &modeCount, modes.data());
    VkPresentModeKHR present = VK_PRESENT_MODE_MAX_ENUM_KHR;
    for (VkPresentModeKHR m : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR})
        if (present == VK_PRESENT_MODE_MAX_ENUM_KHR && std::find(modes.begin(), modes.end(), m) != modes.end()) present = m;
    if (present == VK_PRESENT_MODE_MAX_ENUM_KHR) {
        std::printf("xr: no desktop mirror (the window offers only blocking presentation)\n");
        return;
    }
    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = surface_;
    sci.minImageCount = std::max(caps.minImageCount, 3u);
    if (caps.maxImageCount) sci.minImageCount = std::min(sci.minImageCount, caps.maxImageCount);
    sci.imageFormat = chosen.format;
    sci.imageColorSpace = chosen.colorSpace;
    sci.imageExtent = extent_;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = present;
    sci.clipped = VK_TRUE;
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return;
    if (vkCreateSwapchainKHR(session_.Device(), &sci, nullptr, &swapchain_) != VK_SUCCESS) {
        swapchain_ = VK_NULL_HANDLE;
        return;
    }
    uint32_t n = 0;
    vkGetSwapchainImagesKHR(session_.Device(), swapchain_, &n, nullptr);
    images_.resize(n);
    vkGetSwapchainImagesKHR(session_.Device(), swapchain_, &n, images_.data());
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(session_.PhysicalDevice(), chosen.format, &fp);
    linear_ = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
}

void MirrorWindow::DestroySwapchain() {
    if (swapchain_) vkDestroySwapchainKHR(session_.Device(), swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
    images_.clear();
}

void MirrorWindow::Destroy() {
    if (session_.Device()) {
        vkDeviceWaitIdle(session_.Device());
        DestroySwapchain();
        if (acquired_) vkDestroySemaphore(session_.Device(), acquired_, nullptr);
        if (blitted_) vkDestroySemaphore(session_.Device(), blitted_, nullptr);
        if (fence_) vkDestroyFence(session_.Device(), fence_, nullptr);
        if (pool_) vkDestroyCommandPool(session_.Device(), pool_, nullptr);
    }
    acquired_ = blitted_ = VK_NULL_HANDLE;
    fence_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    if (surface_) vkDestroySurfaceKHR(session_.VulkanInstance(), surface_, nullptr);
    surface_ = VK_NULL_HANDLE;
}

void MirrorWindow::Show(VkImage src, VkExtent2D srcExtent) {
    if (!swapchain_) {
        Create();
        if (!swapchain_) return;
    }
    uint32_t index = 0;
    const VkResult acq = vkAcquireNextImageKHR(session_.Device(), swapchain_, UINT64_MAX, acquired_, VK_NULL_HANDLE, &index);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
        vkDeviceWaitIdle(session_.Device());
        DestroySwapchain();
        return;
    }
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) return;

    vkResetCommandBuffer(cmd_, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd_, &bi);
    auto barrier = [&](VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess, VkAccessFlags dstAccess) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = srcAccess;
        b.dstAccessMask = dstAccess;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = images_[index];
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {int32_t(srcExtent.width), int32_t(srcExtent.height), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[1] = {int32_t(extent_.width), int32_t(extent_.height), 1};
    vkCmdBlitImage(cmd_, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, images_[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                   linear_ ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
    vkEndCommandBuffer(cmd_);

    const VkPipelineStageFlags wait = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &acquired_;
    si.pWaitDstStageMask = &wait;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &blitted_;
    vkResetFences(session_.Device(), 1, &fence_);
    vkQueueSubmit(session_.Queue(), 1, &si, fence_);
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &blitted_;
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &index;
    const VkResult pres = vkQueuePresentKHR(session_.Queue(), &pi);
    vkWaitForFences(session_.Device(), 1, &fence_, VK_TRUE, UINT64_MAX); // the session's own copies share this queue
    if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR) {
        vkDeviceWaitIdle(session_.Device());
        DestroySwapchain();
    }
}

struct XrPadState {
    gt2::input::Ps1PadFrame frame;
    float vibration = 0;
    bool active = false;
};
class XrPadDevice final : public gt2::input::Device {
public:
    explicit XrPadDevice(std::shared_ptr<XrPadState> state) : state_(std::move(state)) {}
    bool Poll(int, gt2::input::Ps1PadFrame& out) override { out = state_->frame; return true; }
    void SetMotors(uint8_t small, uint8_t large, int strength) override {
        state_->vibration = std::max(small ? 0.35f : 0.f, float(large)/255.f) * float(strength)/100.f;
    }
    bool HasMotors() const override { return true; }
    void SetActive(bool active) override { state_->active = active; }
    std::string Name() const override { return "OpenXR controllers"; }
private:
    std::shared_ptr<XrPadState> state_;
};

class XrWindow final : public WindowBackend {
public:
    XrWindow(const std::string& title, int clientWidth, int clientHeight, bool deterministic);
    ~XrWindow() override;

    gt2view::VkSceneRenderer& Renderer() override { return *renderer_; }
    void* NativeHandle() const override { return desktop_->NativeHandle(); }
    void SetTitle(const std::string& title) override { desktop_->SetTitle(title); }

    void Pump() override;
    bool TakeTimingReset() override { const bool reset = timingReset_; timingReset_ = false; return reset; }
    void ConfigureVr() override;
    void SetDrivingActive(bool active) override { drivingActive_ = active; if (!active) driving_.Reset(); }
    void SetVrMenuActive(bool active) override { vrMenuActive_ = active; driving_.Reset(); steering_ = 0; }
    bool PhysicalSteering(float& value) const override {
        value = steering_; return pad_->active && drivingActive_ && !vrMenuActive_ && OverlayDrivingSettings().mode != 0;
    }
    void AppendDrivingVisuals(std::vector<gt2view::DrawItem>& items, const float* vehicleFrame) override {
        if (stereoFrame_ && pad_->active && !vrMenuActive_ && drivingActive_) hands_->Append(items, tracking_, driving_, OverlayDrivingSettings(), vehicleFrame ? vehicleFrame : drivingMatrix_);
    }
    std::unique_ptr<gt2::input::Device> CreateInputDevice() override {
        return std::make_unique<XrPadDevice>(pad_);
    }

    void BeginRenderFrame() override;
    void EndRenderFrame() override;
    bool Closed() const override { return closed_ || desktop_->Closed(); }
    void Close() override {
        closed_ = true;
        desktop_->Close();
    }
    bool Focused() const override { return session_->State() == 5; }
    std::vector<int> TakeKeyDowns() override { auto keys = desktop_->TakeKeyDowns(); return desktop_->Focused() ? keys : std::vector<int>{}; }
    bool KeyDown(int key) const override { return desktop_->Focused() && desktop_->KeyDown(key); }
    bool TakeDevicesChanged() override { return desktop_->TakeDevicesChanged(); }

    bool XrPaced() const override { return !deterministic_; }
    std::vector<int> RefreshRates() const override {
        std::vector<int> rates;
        for (double rate : session_->Info().refreshRates) rates.push_back(int(rate + 0.5));
        return rates;
    }
    bool SetRefreshRate(int hz) override { return session_->SetRefreshRate(float(hz)); }
    Clock::time_point DisplayTime() const override { return session_->LastDisplayTime(); }
    void SleepUntil(Clock::time_point t) override;
    bool VBlankTiming(Clock::time_point& vblank, Clock::duration& period) const override;

    bool StereoAvailable() const override { return stereo_ && session_->StereoReady() && renderer_->StereoReady(); }
    float StereoAspect() const override {
        const VkExtent2D e = renderer_->StereoExtent();
        return e.height ? float(e.width) / float(e.height) : 4.0f / 3.0f;
    }
    bool BeginStereoScene(const gt2::vr::Camera& camera, gt2::vr::View& view) override;
    void DrawStereoScene(const std::vector<gt2view::DrawItem>& items, size_t sceneItems, const std::string& shotPath) override {
        renderer_->DrawStereo(items, shotPath, sceneItems);
    }
    void CancelStereoScene() override { stereoFrame_ = false; }

private:
    // Waits for the runtime to let the session run (it is READY within a few event polls); false when it asked the
    // app to quit instead.
    bool WaitForRunning();
    // One compositor frame that shows the image the game drew last (while the game is between fields or loading).
    void IdleFrame();

    void UpdateDriving() {
        tracking_ = session_->ControllerTracking();
        if (recenter_.Pending()) tracking_ = {};
        else for (int h = 0; h < 2; ++h) {
            tracking_.gripPose[h] = recenter_.Apply(tracking_.gripPose[h]);
            tracking_.aimPose[h] = recenter_.Apply(tracking_.aimPose[h]);
        }
        steering_ = driving_.Update(tracking_,OverlayDrivingSettings(),drivingActive_ && !vrMenuActive_);
    }
    gt2::vr::DrivingController driving_;
    gt2::vr::TrackedControllers tracking_;
    std::unique_ptr<gt2view::VrDrivingVisuals> hands_;
    bool drivingActive_ = false, vrMenuActive_ = false;
    float steering_ = 0, drivingMatrix_[16]{};

    std::shared_ptr<XrPadState> pad_ = std::make_shared<XrPadState>();
    std::unique_ptr<WindowBackend> desktop_; // the Win32 window: keyboard, focus, controllers, high-resolution sleep
    std::unique_ptr<MirrorWindow> mirror_;   // ... and the same image on the PC's screen
    std::unique_ptr<gt2::xr::Session> session_;
    std::unique_ptr<gt2view::VkContext> vulkan_;
    std::unique_ptr<gt2view::VkSceneRenderer> renderer_;
    bool deterministic_ = false;
    bool closed_ = false;
    bool timingReset_ = false;
    bool frameOpen_ = false;  // a compositor frame is between xrBeginFrame and xrEndFrame
    bool everDrawn_ = false;  // the offscreen image holds a frame (idle frames may resubmit it)

    // Stereo (docs/research/vr_port_plan.md, M2)
    bool stereo_ = false;        // the projection layer exists (vr_stereo / --vr-mono)
    bool stereoFrame_ = false;   // this compositor frame is a stereo one: EndRenderFrame submits the projection layer
    bool lastStereo_ = false;    // the last submitted frame was the projection layer (what an idle frame repeats)
    gt2::vr::Recenter recenter_;
    gt2::vr::Settings rig_;
    gt2::vr::EyeView eyes_[2];   // what the last BeginStereoScene rendered with (LOCAL space, submitted as they are)
    std::FILE* poseLog_ = nullptr;
};

XrWindow::XrWindow(const std::string& title, int clientWidth, int clientHeight, bool deterministic) : deterministic_(deterministic) {
    gt2::xr::SessionOptions options;
    options.appName = "GT2 VR";
    options.alternateMenuChord = true;
    options.refreshHz = float(OverlayRefreshRate());
    options.quadWidth = uint32_t(kCinemaWidth);
    options.quadHeight = uint32_t(kCinemaHeight);
    // The desktop mirror is off unless GT2_XR_MIRROR=1 asks for it: in VR the player looks through the headset, and
    // a second window that draws (and, on an interactive launch, takes the keyboard) is in the way on the PC.
    const char* mirror = std::getenv("GT2_XR_MIRROR");
    options.mirrorWindow = mirror && mirror[0] == '1';
    const char* validation = std::getenv("GT2_VK_VALIDATION");
    options.vulkanValidation = validation && validation[0] == '1';
    session_ = std::make_unique<gt2::xr::Session>(options);
    const gt2::xr::SessionInfo& info = session_->Info();
    std::printf("xr: runtime \"%s\", system \"%s\", views %ux%u, %.1f Hz%s%s%s\n", info.runtimeName.c_str(), info.systemName.c_str(), info.viewWidth,
                info.viewHeight, info.refreshHz, info.refreshRateExtension ? ", FB display refresh rate" : "",
                info.performanceExtension ? ", EXT performance settings" : "", info.stageSpace ? ", STAGE space" : "");
    std::printf("xr: cinema quad %dx%d, %.2f m wide, %.1f m ahead%s\n", kCinemaWidth, kCinemaHeight, double(options.quadMetres),
                double(options.quadDistance), deterministic_ ? ", deterministic (one field per compositor frame)" : "");

    const gt2view::VkContext::Existing existing{session_->VulkanInstance(), session_->PhysicalDevice(), session_->Device(), session_->QueueFamily(),
                                                session_->Queue()};
    vulkan_ = std::make_unique<gt2view::VkContext>(existing);
    renderer_ = std::make_unique<gt2view::VkSceneRenderer>(*vulkan_, VkExtent2D{uint32_t(kCinemaWidth), uint32_t(kCinemaHeight)}, session_->RenderFormat());
    hands_ = std::make_unique<gt2view::VrDrivingVisuals>(*renderer_);
    ConfigureVr();
    const VrOptions& vr = VrOptionsInUse();
    if (!vr.poseLog.empty()) {
        poseLog_ = std::fopen(vr.poseLog.c_str(), "w");
        if (poseLog_)
            std::fprintf(poseLog_, "frame,field_alpha,cam_x,cam_y,cam_z,cam_rx,cam_ry,cam_rz,cam_ux,cam_uy,cam_uz,cam_fx,cam_fy,cam_fz,"
                                   "head_x,head_y,head_z,head_qx,head_qy,head_qz,head_qw,ref_x,ref_y,ref_z,"
                                   "e0_x,e0_y,e0_z,e0_qx,e0_qy,e0_qz,e0_qw,e1_x,e1_y,e1_z,e1_qx,e1_qy,e1_qz,e1_qw,ipd,"
                                   "fov0_l,fov0_r,fov0_u,fov0_d\n");
        else std::printf("xr: cannot write the pose log %s\n", vr.poseLog.c_str());
    }

    desktop_ = CreateInputWindowBackend(title, clientWidth, clientHeight);
    if (options.mirrorWindow) {
        mirror_ = std::make_unique<MirrorWindow>(*session_, desktop_->NativeHandle());
        if (!mirror_->Active()) mirror_.reset();
        else std::printf("xr: desktop mirror in the program's window (GT2_XR_MIRROR=1)\n");
    }
}

void XrWindow::ConfigureVr() {
    const auto& info = session_->Info();
    renderer_->WaitFrame();
    session_->SetRefreshRate(float(OverlayRefreshRate()));
    renderer_->SetFoveation(OverlayFoveation());
    const VrOptions& vr = VrOptionsInUse();
    rig_.horizonLock = vr.horizonLock;
    rig_.worldScale = vr.worldScale;
    rig_.nearZ = vr.nearZ;
    rig_.ipd = vr.ipd;
    rig_.originalFov = vr.originalFov;
    for (int i = 0; i < 3; i++) rig_.seat[i] = vr.seat[i];
    if (vr.stereo) {
        uint32_t w = vr.eyeWidth > 0 ? uint32_t(vr.eyeWidth) : uint32_t(std::lround(double(info.viewWidth) * double(vr.renderScale)));
        uint32_t h = vr.eyeHeight > 0 ? uint32_t(vr.eyeHeight) : uint32_t(std::lround(double(info.viewHeight) * double(vr.renderScale)));
        w = std::max(w, 16u);
        h = std::max(h, 16u);
        session_->CreateStereoSwapchains(w, h);
        renderer_->CreateStereoTarget(VkExtent2D{w, h}, session_->StereoRenderFormat(), vr.multiview);
        stereo_ = true;
        std::printf("xr: stereo race %ux%u per eye, %s, near %.2f m, horizon lock %.0f %%, world scale %.2f%s%s\n", w, h,
                    vr.multiview ? "multiview (one pass)" : "two passes", double(vr.nearZ), double(vr.horizonLock) * 100.0,
                    double(vr.worldScale), session_->StereoDepth() ? ", depth layer" : "",
                    vr.originalFov ? ", the original camera's field of view" : "");
        if (vr.ipd >= 0) std::printf("xr: eye separation forced to %.4f m (a check)\n", double(vr.ipd));
    } else {
        std::printf("xr: stereo off (vr_stereo=0 / --vr-mono): the race stays on the cinema quad\n");
    }
}

XrWindow::~XrWindow() {
    if (renderer_) renderer_->WaitFrame();
    // A clean exit: ask the runtime to end the session and walk its state machine down (FOCUSED -> ... -> STOPPING,
    // xrEndSession, IDLE -> EXITING) before anything Vulkan is destroyed.
    if (session_) {
        session_->RequestExit();
        const auto deadline = Clock::now() + std::chrono::seconds(3);
        while (!session_->Quit() && Clock::now() < deadline) {
            if (!session_->PollEvents()) break;
            if (session_->Running()) {
                if (session_->BeginFrame()) session_->SubmitFrame(VK_NULL_HANDLE);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        std::printf("xr: %lld compositor frames, session %s\n", session_->FrameIndex(), session_->StateName());
    }
    if (poseLog_) std::fclose(poseLog_);
    poseLog_ = nullptr;
    mirror_.reset();
    hands_.reset();
    renderer_.reset();
    vulkan_.reset();
    session_.reset();
    desktop_.reset();
}

void XrWindow::Pump() {
    desktop_->Pump();
    if (!session_->PollEvents()) closed_ = true;
    if (session_->Running() && !Focused() && !Closed()) {
        gt2::audio::ScopedMixPause pause;
        pad_->frame = {}; pad_->vibration = 0;
        driving_.Reset(); steering_ = 0;
        while (!Focused() && !Closed()) {
            desktop_->Pump();
            if (!session_->PollEvents()) { closed_ = true; break; }
            IdleFrame();
            if (!session_->Running()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        timingReset_ = true;
    }
    gt2::input::Ps1PadFrame pad;
    session_->ReadController(pad, pad_->vibration);
    UpdateDriving();
    if (!vrMenuActive_ && drivingActive_ && OverlayControlBindings().steeringStick == 1) {
        std::swap(pad.analog[0],pad.analog[2]); std::swap(pad.analog[1],pad.analog[3]);
        pad.buttons &= ~(gt2::input::ps1::kLeft|gt2::input::ps1::kRight|gt2::input::ps1::kUp|gt2::input::ps1::kDown);
    }
    if (!vrMenuActive_ && drivingActive_ && OverlayDrivingSettings().mode != 0) {
        pad.analog = {128,128,128,128};
        pad.buttons &= ~(gt2::input::ps1::kLeft | gt2::input::ps1::kRight | gt2::input::ps1::kUp | gt2::input::ps1::kDown);
    }
    pad_->frame = pad;
}

bool XrWindow::WaitForRunning() {
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    while (!session_->Running()) {
        if (!session_->PollEvents()) {
            closed_ = true;
            return false;
        }
        if (session_->Running()) break;
        if (Clock::now() > deadline) {
            std::printf("xr: the runtime did not start the session within 10 s (state %s)\n", session_->StateName());
            closed_ = true;
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

void XrWindow::BeginRenderFrame() {
    if (frameOpen_) return; // idempotent: the race view opens the frame itself before it builds a stereo one
    if (closed_) return;
    if (!session_->Running() && !WaitForRunning()) return;
    frameOpen_ = session_->BeginFrame();
}

// One stereo frame: the runtime's eye poses, the recentred origin, and the rig (vr_rig.h) on the original camera.
bool XrWindow::BeginStereoScene(const gt2::vr::Camera& camera, gt2::vr::View& view) {
    stereoFrame_ = false;
    if (!StereoAvailable() || !frameOpen_ || !session_->ShouldRender()) return false;
    gt2::vr::Pose head;
    gt2::vr::EyeView eyes[2];
    if (!session_->LocateViews(head, eyes)) return false;
    if (session_->TakeRecenter() || recenter_.Pending()) {
        recenter_.Latch(head);
        driving_.Reset();
        std::printf("xr: recentred on the head (yaw %.1f deg)\n", double(recenter_.Yaw()) * 180.0 / 3.14159265358979323846);
    }
    gt2::vr::EyeView recentred[2] = {eyes[0], eyes[1]};
    for (int v = 0; v < 2; v++) recentred[v].pose = recenter_.Apply(eyes[v].pose);
    view = gt2::vr::Build(camera, recentred, rig_);
    if (!view.valid) return false;
    UpdateDriving();
    std::fill(std::begin(drivingMatrix_),std::end(drivingMatrix_),0.f); drivingMatrix_[15] = 1;
    for (int axis = 0; axis < 3; ++axis) for (int k = 0; k < 3; ++k)
        drivingMatrix_[axis*4+k] = view.levelled[axis*3+k]*rig_.worldScale;
    for (int k = 0; k < 3; ++k) {
        drivingMatrix_[12+k] = camera.eye[k]-view.refEye[k];
        for (int axis = 0; axis < 3; ++axis) drivingMatrix_[12+k] += view.levelled[axis*3+k]*rig_.seat[axis];
    }
    gt2view::StereoViews matrices;
    gt2::vr::HudProjection(head, eyes, matrices.hudVP);
    std::memcpy(matrices.worldVP, view.worldVP, sizeof(matrices.worldVP));
    std::memcpy(matrices.skyVP, view.skyVP, sizeof(matrices.skyVP));
    renderer_->SetStereoViews(matrices);
    renderer_->BeginFrameUploads();
    for (int v = 0; v < 2; v++) { // submitted as the runtime reported them: the compositor reprojects on the real head
        eyes_[v].pose = eyes[v].pose;
        eyes_[v].fov = view.fov[v];
    }
    if (poseLog_) {
        const gt2::vr::Pose h = recenter_.Apply(head);
        std::fprintf(poseLog_, "%lld,%.6f", session_->FrameIndex(), 0.0);
        std::fprintf(poseLog_, ",%.6f,%.6f,%.6f", double(camera.eye[0]), double(camera.eye[1]), double(camera.eye[2]));
        std::fprintf(poseLog_, ",%.6f,%.6f,%.6f", double(camera.right[0]), double(camera.right[1]), double(camera.right[2]));
        std::fprintf(poseLog_, ",%.6f,%.6f,%.6f", double(camera.up[0]), double(camera.up[1]), double(camera.up[2]));
        std::fprintf(poseLog_, ",%.6f,%.6f,%.6f", double(camera.forward[0]), double(camera.forward[1]), double(camera.forward[2]));
        std::fprintf(poseLog_, ",%.6f,%.6f,%.6f", double(h.position[0]), double(h.position[1]), double(h.position[2]));
        std::fprintf(poseLog_, ",%.6f,%.6f,%.6f,%.6f", double(h.orientation[0]), double(h.orientation[1]), double(h.orientation[2]),
                     double(h.orientation[3]));
        std::fprintf(poseLog_, ",%.6f,%.6f,%.6f", double(view.refEye[0]), double(view.refEye[1]), double(view.refEye[2]));
        for (int v = 0; v < 2; v++) {
            std::fprintf(poseLog_, ",%.6f,%.6f,%.6f", double(view.eyeWorld[v][0]), double(view.eyeWorld[v][1]), double(view.eyeWorld[v][2]));
            std::fprintf(poseLog_, ",%.6f,%.6f,%.6f,%.6f", double(view.eyeQuat[v][0]), double(view.eyeQuat[v][1]), double(view.eyeQuat[v][2]),
                         double(view.eyeQuat[v][3]));
        }
        std::fprintf(poseLog_, ",%.6f,%.6f,%.6f,%.6f,%.6f\n", double(view.ipd), double(view.fov[0].left), double(view.fov[0].right),
                     double(view.fov[0].up), double(view.fov[0].down));
    }
    stereoFrame_ = true;
    return true;
}

void XrWindow::EndRenderFrame() {
    everDrawn_ = !stereoFrame_ || everDrawn_; // the offscreen image still holds the last mono frame
    renderer_->WaitFrame(); // the frame's queue work is done before the compositor's image is copied from it
    if (frameOpen_) {
        if (stereoFrame_) {
            // With MSAA the depth attachment is multisampled and cannot be handed to the compositor as it is.
            const VkImage depth = renderer_->EffectiveMsaa() == 1 ? renderer_->StereoDepthImage() : VK_NULL_HANDLE;
            session_->SubmitStereoFrame(renderer_->StereoImage(), depth, eyes_, rig_.nearZ);
        } else {
            session_->SubmitFrame(renderer_->OffscreenImage());
        }
    }
    const bool wasStereo = stereoFrame_;
    lastStereo_ = wasStereo;
    stereoFrame_ = false;
    frameOpen_ = false;
    if (mirror_) {
        if (wasStereo) mirror_->Show(renderer_->StereoImage(), renderer_->StereoExtent()); // the left eye
        else mirror_->Show(renderer_->OffscreenImage(), renderer_->Extent());
    }
}

// A compositor frame that shows what the game drew last, while the game is between fields or loading. A stereo
// frame is resubmitted as the projection layer with the poses it was rendered for - which is exactly what the
// compositor's reprojection expects - and not as the (stale) mono image of the cinema quad.
void XrWindow::IdleFrame() {
    if (!session_->Running() || !session_->BeginFrame()) return;
    if (lastStereo_ && renderer_->StereoReady()) {
        const VkImage depth = renderer_->EffectiveMsaa() == 1 ? renderer_->StereoDepthImage() : VK_NULL_HANDLE;
        session_->SubmitStereoFrame(renderer_->StereoImage(), depth, eyes_, rig_.nearZ);
        return;
    }
    session_->SubmitFrame(everDrawn_ ? renderer_->OffscreenImage() : VK_NULL_HANDLE);
}

void XrWindow::SleepUntil(Clock::time_point t) {
    if (deterministic_) return; // one compositor frame per field, no clock of our own
    // The compositor's frames are the clock: while the last frame submitted will be shown BEFORE the field's time,
    // the display in between would show nothing new - submit the same image again. That is one frame per display
    // period (60 fields/s on a 72 Hz headset become 72 compositor frames/s, none of them skipped), and the field
    // clock keeps the original's 60 Hz because the loop stops as soon as a frame reaches past `t`.
    const Clock::duration period = session_->Period();
    while (session_->Running() && !closed_ && period > Clock::duration::zero() && session_->LastDisplayTime() < t) {
        IdleFrame();
        if (!session_->PollEvents()) {
            closed_ = true;
            return;
        }
    }
    desktop_->SleepUntil(t);
}

bool XrWindow::VBlankTiming(Clock::time_point& vblank, Clock::duration& period) const {
    if (!session_->Running() || session_->Period() <= Clock::duration::zero()) return false;
    vblank = session_->LastDisplayTime();
    period = session_->Period();
    return period > std::chrono::milliseconds(2) && period < std::chrono::milliseconds(50);
}

} // namespace

std::unique_ptr<WindowBackend> CreateXrWindowBackend(const std::string& title, int clientWidth, int clientHeight) {
    return std::make_unique<XrWindow>(title, clientWidth, clientHeight, VrDeterministic());
}

} // namespace gt2game
