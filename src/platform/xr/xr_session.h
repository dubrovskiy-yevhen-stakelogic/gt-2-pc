#pragma once
// The OpenXR session of the game (docs/research/vr_port_plan.md, M1): the runtime's instance and system, the Vulkan
// instance and device the runtime requires (XR_KHR_vulkan_enable2), the session's state machine, the reference
// spaces, the swapchain of the cinema quad and the frame loop (xrWaitFrame / xrBeginFrame / xrEndFrame).
//
// Portable between Windows and Android except for the two pieces marked below (the loader's file name and the
// XrTime <-> performance counter conversion); the game's window backend (tools/gt2game/game_window_xr.cpp) is the
// only user.
//
// The loader is loaded at run time: without an `openxr_loader.dll` (or without a runtime behind it) the constructor
// throws with the reason - `--vr` fails, it never falls back to the desktop path silently.
//
// M1 renders every screen into one offscreen image (gt2view::VkSceneRenderer offscreen mode) and shows it on a
// world-locked quad layer 2 m ahead: SubmitFrame copies that image into the quad swapchain. M2 added the stereo
// projection layer next to it (CreateStereoSwapchains / LocateViews / SubmitStereoFrame): the race scene is drawn
// into a two-layer colour image and submitted as an XrCompositionLayerProjection, with its depth image when the
// runtime offers XR_KHR_composition_layer_depth. One frame uses one or the other, never both.
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "platform/xr/vr_rig.h"
#include "platform/xr/vr_driving.h"
#include "platform/input/ps1_pad.h"

namespace gt2::xr {

struct SessionOptions {
    std::string appName = "gt2game";
    // The cinema quad: the offscreen frame's size, its width in metres, and how far ahead of the latched head pose it
    // hangs (docs/research/vr_port_plan.md section 2: 1280 x 960, 2.56 m wide, 2 m ahead).
    uint32_t quadWidth = 1280, quadHeight = 960;
    float quadMetres = 2.56f, quadDistance = 2.0f;
    float refreshHz = 0;      // > 0: ask XR_FB_display_refresh_rate for this rate
    bool mirrorWindow = false; // GT2_XR_MIRROR=1: the Vulkan instance / device also carry the surface and swapchain
                               // extensions, so that the same image can be shown in a window on the PC
    bool vulkanValidation = false; // GT2_VK_VALIDATION=1: the Khronos validation layer on the runtime's instance
    // Android only (docs/research/vr_port_plan.md, M6): the two Java objects the OpenXR loader and the runtime need -
    // ANativeActivity::vm (JavaVM*) and ANativeActivity::clazz (the activity instance). android_main passes them;
    // without them the session cannot be created on the headset and says so.
    void* androidVm = nullptr;
    void* androidActivity = nullptr;
};

// What the runtime reports about itself, for the log.
struct SessionInfo {
    std::string runtimeName, systemName;
    uint32_t viewWidth = 0, viewHeight = 0; // recommended per-eye image size (M2 uses it)
    double refreshHz = 0;
    std::vector<double> refreshRates;
    bool refreshRateExtension = false, performanceExtension = false, timeConversion = false;
    bool stageSpace = false;
    bool shadingRate = false;
    bool depthLayer = false; // XR_KHR_composition_layer_depth (M2: the stereo layer carries its depth image)
};

class Session {
public:
    // Creates everything up to a session that is ready to run. Throws std::runtime_error on any failure.
    explicit Session(const SessionOptions& options);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    const SessionInfo& Info() const { return info_; }

    // The Vulkan objects the runtime made (gt2view::VkContext::Existing adopts them; the session owns them).
    VkInstance VulkanInstance() const { return vkInstance_; }
    VkPhysicalDevice PhysicalDevice() const { return vkPhysical_; }
    VkDevice Device() const { return vkDevice_; }
    uint32_t QueueFamily() const { return vkQueueFamily_; }
    VkQueue Queue() const { return vkQueue_; }
    // The colour format of the quad swapchain, and the format the frame must be rendered in so that its bytes reach
    // the compositor unchanged (the *_UNORM twin of an sRGB swapchain format: our colours are display-referred).
    VkFormat QuadFormat() const { return quadFormat_; }
    VkFormat RenderFormat() const { return renderFormat_; }

    // Services the runtime's event queue and the session state machine (xrBeginSession on READY, xrEndSession on
    // STOPPING). False once the runtime told the app to quit (EXITING / instance loss).
    bool PollEvents();
#ifdef __ANDROID__
    bool ReadController(input::Ps1PadFrame& pad, float vibration);
    vr::TrackedControllers ControllerTracking();
#endif
    bool Running() const { return running_; }    // between xrBeginSession and xrEndSession
    bool Quit() const { return quit_; }
    void RequestExit();                          // xrRequestExitSession (the game's own quit)
    int State() const { return int(state_); }    // XrSessionState
    const char* StateName() const;

    // One compositor frame. BeginFrame blocks in xrWaitFrame until the runtime wants the next frame; false when the
    // session is not running (the caller then services events and tries again). ShouldRender() says whether the
    // runtime shows anything; SubmitFrame ends the frame, with the quad layer when `image` is not null (that image
    // must be in VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, of RenderFormat() and the quad's size, and its queue work
    // must already have completed).
    bool BeginFrame();
    bool ShouldRender() const { return shouldRender_; }
    void SubmitFrame(VkImage image);

    // ---- stereo (docs/research/vr_port_plan.md, M2) ----
    // Creates the projection layer's swapchains: one colour swapchain of arraySize 2 at `width` x `height` per eye
    // and, when the runtime offers XR_KHR_composition_layer_depth, a depth swapchain of the same shape.
    void CreateStereoSwapchains(uint32_t width, uint32_t height, bool direct = false);
    VkImage AcquireStereoRenderImage();
    bool DirectStereo() const { return directStereo_; }
    bool StereoReady() const { return stereoReady_; }
    uint32_t StereoWidth() const { return stereoWidth_; }
    uint32_t StereoHeight() const { return stereoHeight_; }
    VkFormat StereoRenderFormat() const { return stereoRenderFormat_; }
    VkFormat StereoDepthFormat() const { return stereoDepthFormat_; }
    bool StereoDepth() const { return stereoDepth_ != nullptr; }
    // The head and the two eyes of the frame BeginFrame opened, in LOCAL space at its predictedDisplayTime. False
    // when the runtime could not track them (the caller keeps the previous pose or falls back to the cinema quad).
    bool LocateViews(vr::Pose& head, vr::EyeView eyes[2]);
    // Ends the frame with an XrCompositionLayerProjection instead of the cinema quad. `color` (and `depth`, which
    // may be VK_NULL_HANDLE) are two-layer images in VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL whose queue work has
    // completed. Direct images from AcquireStereoRenderImage instead require submitted commands on Queue(),
    // ending in COLOR_ATTACHMENT_OPTIMAL; enable2 synchronizes pending writes with the runtime.
    // `eyes` are the poses and fields of view the frame was actually rendered with, in LOCAL space.
    void SubmitStereoFrame(VkImage color, VkImage depth, const vr::EyeView eyes[2], float nearZ);
    // True once after the runtime recentred its reference space (and once at the start).
    bool TakeRecenter();

    // The frame the last SubmitFrame showed, on the steady clock, and the display period (game_window.h
    // VBlankTiming: the race's display-rate loop schedules its frames on them).
    std::chrono::steady_clock::time_point LastDisplayTime() const { return lastDisplay_; }
    std::chrono::steady_clock::duration Period() const { return period_; }
    // The number of frames submitted so far (the frame index of the runtime's log).
    long long FrameIndex() const { return frameIndex_; }

    // The cinema quad is world-locked with the yaw the head had when it was latched (VC's theater anchor): the next
    // rendered frame latches it again.
    void RecenterQuad() { quadLatched_ = false; }
    bool SetRefreshRate(float hz);

private:
    // Copies the rendered frame into the acquired quad swapchain image (plain copy: the bytes the game drew are what
    // the compositor shows). False when the swapchain gave no image.
    bool CopyToQuad(VkImage src);
    // The same for the projection layer's two-layer colour (and optional depth) swapchain images.
    bool CopyToStereo(VkImage color, VkImage depth);

    struct Impl;
    std::unique_ptr<Impl> impl_;

    SessionOptions options_;
    SessionInfo info_;
    VkInstance vkInstance_ = VK_NULL_HANDLE;
    VkPhysicalDevice vkPhysical_ = VK_NULL_HANDLE;
    VkDevice vkDevice_ = VK_NULL_HANDLE;
    uint32_t vkQueueFamily_ = 0;
    VkQueue vkQueue_ = VK_NULL_HANDLE;
    VkFormat quadFormat_ = VK_FORMAT_UNDEFINED, renderFormat_ = VK_FORMAT_UNDEFINED;
    // The projection layer (M2)
    bool directStereo_ = false;
    bool stereoReady_ = false, recenter_ = true;
    uint32_t stereoWidth_ = 0, stereoHeight_ = 0;
    VkFormat stereoFormat_ = VK_FORMAT_UNDEFINED, stereoRenderFormat_ = VK_FORMAT_UNDEFINED;
    VkFormat stereoDepthFormat_ = VK_FORMAT_UNDEFINED;
    void* stereoDepth_ = nullptr; // non-null when the depth swapchain exists (the handle lives in Impl)

    bool running_ = false, quit_ = false, shouldRender_ = false, frameOpen_ = false, quadLatched_ = false;
    int state_ = 0; // XrSessionState
    std::chrono::steady_clock::time_point lastDisplay_{};
    std::chrono::steady_clock::duration period_{};
    long long frameIndex_ = 0;
};

} // namespace gt2::xr
