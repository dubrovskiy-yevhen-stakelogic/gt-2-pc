// xrsim - the OpenXR entry points implemented by the runtime (namespace xs, same names as the API).
#pragma once
#include "rt.h"

namespace xs {

// rt_main.cpp: loader, instance, system, paths, events
XrResult XRAPI_CALL xrGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function);
XrResult XRAPI_CALL xrEnumerateApiLayerProperties(uint32_t cap, uint32_t* count, XrApiLayerProperties* props);
XrResult XRAPI_CALL xrEnumerateInstanceExtensionProperties(const char* layerName, uint32_t cap, uint32_t* count, XrExtensionProperties* props);
XrResult XRAPI_CALL xrCreateInstance(const XrInstanceCreateInfo* createInfo, XrInstance* instance);
XrResult XRAPI_CALL xrDestroyInstance(XrInstance instance);
XrResult XRAPI_CALL xrGetInstanceProperties(XrInstance instance, XrInstanceProperties* props);
XrResult XRAPI_CALL xrPollEvent(XrInstance instance, XrEventDataBuffer* eventData);
XrResult XRAPI_CALL xrResultToString(XrInstance instance, XrResult value, char buffer[XR_MAX_RESULT_STRING_SIZE]);
XrResult XRAPI_CALL xrStructureTypeToString(XrInstance instance, XrStructureType value, char buffer[XR_MAX_STRUCTURE_NAME_SIZE]);
XrResult XRAPI_CALL xrGetSystem(XrInstance instance, const XrSystemGetInfo* getInfo, XrSystemId* systemId);
XrResult XRAPI_CALL xrGetSystemProperties(XrInstance instance, XrSystemId systemId, XrSystemProperties* props);
XrResult XRAPI_CALL xrEnumerateEnvironmentBlendModes(XrInstance instance, XrSystemId systemId, XrViewConfigurationType vct, uint32_t cap, uint32_t* count, XrEnvironmentBlendMode* modes);
XrResult XRAPI_CALL xrEnumerateViewConfigurations(XrInstance instance, XrSystemId systemId, uint32_t cap, uint32_t* count, XrViewConfigurationType* types);
XrResult XRAPI_CALL xrGetViewConfigurationProperties(XrInstance instance, XrSystemId systemId, XrViewConfigurationType vct, XrViewConfigurationProperties* props);
XrResult XRAPI_CALL xrEnumerateViewConfigurationViews(XrInstance instance, XrSystemId systemId, XrViewConfigurationType vct, uint32_t cap, uint32_t* count, XrViewConfigurationView* views);
XrResult XRAPI_CALL xrStringToPath(XrInstance instance, const char* pathString, XrPath* path);
XrResult XRAPI_CALL xrPathToString(XrInstance instance, XrPath path, uint32_t cap, uint32_t* count, char* buffer);
XrResult XRAPI_CALL xrConvertWin32PerformanceCounterToTimeKHR(XrInstance instance, const LARGE_INTEGER* pc, XrTime* time);
XrResult XRAPI_CALL xrConvertTimeToWin32PerformanceCounterKHR(XrInstance instance, XrTime time, LARGE_INTEGER* pc);

// rt_session.cpp: session, frame loop, spaces, views, refresh rate, performance settings
XrResult XRAPI_CALL xrCreateSession(XrInstance instance, const XrSessionCreateInfo* createInfo, XrSession* session);
XrResult XRAPI_CALL xrDestroySession(XrSession session);
XrResult XRAPI_CALL xrBeginSession(XrSession session, const XrSessionBeginInfo* beginInfo);
XrResult XRAPI_CALL xrEndSession(XrSession session);
XrResult XRAPI_CALL xrRequestExitSession(XrSession session);
XrResult XRAPI_CALL xrWaitFrame(XrSession session, const XrFrameWaitInfo* info, XrFrameState* state);
XrResult XRAPI_CALL xrBeginFrame(XrSession session, const XrFrameBeginInfo* info);
XrResult XRAPI_CALL xrEndFrame(XrSession session, const XrFrameEndInfo* info);
XrResult XRAPI_CALL xrLocateViews(XrSession session, const XrViewLocateInfo* info, XrViewState* viewState, uint32_t cap, uint32_t* count, XrView* views);
XrResult XRAPI_CALL xrEnumerateReferenceSpaces(XrSession session, uint32_t cap, uint32_t* count, XrReferenceSpaceType* spaces);
XrResult XRAPI_CALL xrCreateReferenceSpace(XrSession session, const XrReferenceSpaceCreateInfo* createInfo, XrSpace* space);
XrResult XRAPI_CALL xrGetReferenceSpaceBoundsRect(XrSession session, XrReferenceSpaceType type, XrExtent2Df* bounds);
XrResult XRAPI_CALL xrCreateActionSpace(XrSession session, const XrActionSpaceCreateInfo* createInfo, XrSpace* space);
XrResult XRAPI_CALL xrLocateSpace(XrSpace space, XrSpace baseSpace, XrTime time, XrSpaceLocation* location);
XrResult XRAPI_CALL xrLocateSpaces(XrSession session, const XrSpacesLocateInfo* info, XrSpaceLocations* locations);
XrResult XRAPI_CALL xrDestroySpace(XrSpace space);
XrResult XRAPI_CALL xrEnumerateDisplayRefreshRatesFB(XrSession session, uint32_t cap, uint32_t* count, float* rates);
XrResult XRAPI_CALL xrGetDisplayRefreshRateFB(XrSession session, float* rate);
XrResult XRAPI_CALL xrRequestDisplayRefreshRateFB(XrSession session, float rate);
XrResult XRAPI_CALL xrPerfSettingsSetPerformanceLevelEXT(XrSession session, XrPerfSettingsDomainEXT domain, XrPerfSettingsLevelEXT level);

// rt_vulkan.cpp: graphics binding, swapchains
XrResult XRAPI_CALL xrGetVulkanGraphicsRequirementsKHR(XrInstance instance, XrSystemId systemId, XrGraphicsRequirementsVulkanKHR* req);
XrResult XRAPI_CALL xrGetVulkanGraphicsRequirements2KHR(XrInstance instance, XrSystemId systemId, XrGraphicsRequirementsVulkanKHR* req);
XrResult XRAPI_CALL xrGetVulkanInstanceExtensionsKHR(XrInstance instance, XrSystemId systemId, uint32_t cap, uint32_t* count, char* buffer);
XrResult XRAPI_CALL xrGetVulkanDeviceExtensionsKHR(XrInstance instance, XrSystemId systemId, uint32_t cap, uint32_t* count, char* buffer);
XrResult XRAPI_CALL xrGetVulkanGraphicsDeviceKHR(XrInstance instance, XrSystemId systemId, VkInstance vkInstance, VkPhysicalDevice* phys);
XrResult XRAPI_CALL xrGetVulkanGraphicsDevice2KHR(XrInstance instance, const XrVulkanGraphicsDeviceGetInfoKHR* info, VkPhysicalDevice* phys);
XrResult XRAPI_CALL xrCreateVulkanInstanceKHR(XrInstance instance, const XrVulkanInstanceCreateInfoKHR* ci, VkInstance* vkInstance, VkResult* vkResult);
XrResult XRAPI_CALL xrCreateVulkanDeviceKHR(XrInstance instance, const XrVulkanDeviceCreateInfoKHR* ci, VkDevice* vkDevice, VkResult* vkResult);
XrResult XRAPI_CALL xrEnumerateSwapchainFormats(XrSession session, uint32_t cap, uint32_t* count, int64_t* formats);
XrResult XRAPI_CALL xrCreateSwapchain(XrSession session, const XrSwapchainCreateInfo* createInfo, XrSwapchain* swapchain);
XrResult XRAPI_CALL xrDestroySwapchain(XrSwapchain swapchain);
XrResult XRAPI_CALL xrEnumerateSwapchainImages(XrSwapchain swapchain, uint32_t cap, uint32_t* count, XrSwapchainImageBaseHeader* images);
XrResult XRAPI_CALL xrAcquireSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageAcquireInfo* info, uint32_t* index);
XrResult XRAPI_CALL xrWaitSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageWaitInfo* info);
XrResult XRAPI_CALL xrReleaseSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo* info);

// rt_input.cpp: actions
XrResult XRAPI_CALL xrCreateActionSet(XrInstance instance, const XrActionSetCreateInfo* createInfo, XrActionSet* actionSet);
XrResult XRAPI_CALL xrDestroyActionSet(XrActionSet actionSet);
XrResult XRAPI_CALL xrCreateAction(XrActionSet actionSet, const XrActionCreateInfo* createInfo, XrAction* action);
XrResult XRAPI_CALL xrDestroyAction(XrAction action);
XrResult XRAPI_CALL xrSuggestInteractionProfileBindings(XrInstance instance, const XrInteractionProfileSuggestedBinding* suggested);
XrResult XRAPI_CALL xrAttachSessionActionSets(XrSession session, const XrSessionActionSetsAttachInfo* attachInfo);
XrResult XRAPI_CALL xrGetCurrentInteractionProfile(XrSession session, XrPath topLevelUserPath, XrInteractionProfileState* profile);
XrResult XRAPI_CALL xrGetActionStateBoolean(XrSession session, const XrActionStateGetInfo* getInfo, XrActionStateBoolean* state);
XrResult XRAPI_CALL xrGetActionStateFloat(XrSession session, const XrActionStateGetInfo* getInfo, XrActionStateFloat* state);
XrResult XRAPI_CALL xrGetActionStateVector2f(XrSession session, const XrActionStateGetInfo* getInfo, XrActionStateVector2f* state);
XrResult XRAPI_CALL xrGetActionStatePose(XrSession session, const XrActionStateGetInfo* getInfo, XrActionStatePose* state);
XrResult XRAPI_CALL xrSyncActions(XrSession session, const XrActionsSyncInfo* syncInfo);
XrResult XRAPI_CALL xrEnumerateBoundSourcesForAction(XrSession session, const XrBoundSourcesForActionEnumerateInfo* info, uint32_t cap, uint32_t* count, XrPath* sources);
XrResult XRAPI_CALL xrGetInputSourceLocalizedName(XrSession session, const XrInputSourceLocalizedNameGetInfo* info, uint32_t cap, uint32_t* count, char* buffer);
XrResult XRAPI_CALL xrApplyHapticFeedback(XrSession session, const XrHapticActionInfo* info, const XrHapticBaseHeader* feedback);
XrResult XRAPI_CALL xrStopHapticFeedback(XrSession session, const XrHapticActionInfo* info);

} // namespace xs
