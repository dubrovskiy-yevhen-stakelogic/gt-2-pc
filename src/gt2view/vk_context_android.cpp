#include "gt2view/vk_context.h"
#include <stdexcept>
namespace gt2view {
VkContext::VkContext(void*, void*) { throw std::runtime_error("Android rendering requires an OpenXR Vulkan context"); }
VkContext::VkContext(const Existing& e) : instance_(e.instance), physical_(e.physical), device_(e.device),
    queueFamily_(e.queueFamily), queue_(e.queue), owns_(false), shadingRate_(e.shadingRate) {}
VkContext::~VkContext() = default;
bool VkContext::WindowExtent(VkExtent2D&) const { return false; }
}
