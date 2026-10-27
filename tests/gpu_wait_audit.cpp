// Inject a failed GPU wait into the real renderer; no pending command buffer
// may be reset after failure, and screenshots must not read incomplete data.
#include "offscreen_car_audit.h"
#include <filesystem>
#include <iostream>
#include <string>

namespace {
int waitsUntilFailure = 0;
bool waitFailed = false;
int resetsAfterFailure = 0;
}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL __real_vkWaitForFences(VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL __real_vkResetCommandBuffer(VkCommandBuffer, VkCommandBufferResetFlags);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkWaitForFences(VkDevice device, uint32_t count, const VkFence* fences, VkBool32 all, uint64_t timeout) {
    if (waitsUntilFailure > 0 && --waitsUntilFailure == 0) { waitFailed = true; return VK_TIMEOUT; }
    return __real_vkWaitForFences(device, count, fences, all, timeout);
}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkResetCommandBuffer(VkCommandBuffer buffer, VkCommandBufferResetFlags flags) {
    if (waitFailed) ++resetsAfterFailure;
    return __real_vkResetCommandBuffer(buffer, flags);
}
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("GPU wait audit output directory required");
        const auto output = std::filesystem::absolute(argv[1]).lexically_normal();
        if (!output.string().starts_with(std::filesystem::absolute("work").string()+std::string(1,std::filesystem::path::preferred_separator)))
            throw std::runtime_error("output must be under work");
        std::filesystem::create_directories(output);
        Device device; gt2view::VkContext context(device.existing);
        for (const int failureAt : {1, 2}) {
            waitFailed = false; resetsAfterFailure = 0;
            gt2view::VkSceneRenderer renderer(context, {64, 64}, VK_FORMAT_R8G8B8A8_UNORM);
            const auto shot = failureAt == 2 ? (output/"must-not-read.png").string() : std::string();
            if (!shot.empty() && std::filesystem::exists(shot)) throw std::runtime_error("use a fresh audit output directory");
            waitsUntilFailure = failureAt;
            bool rejected = false;
            try { renderer.Draw({}, shot); }
            catch (const std::runtime_error& e) { rejected = std::string(e.what()).find("vkWaitForFences failed (2)") != std::string::npos; }
            waitsUntilFailure = 0;
            if (!rejected || !waitFailed || resetsAfterFailure != 0 || (!shot.empty() && std::filesystem::exists(shot)))
                throw std::runtime_error("renderer reused/read a frame after failed GPU completion");
            waitFailed = false;
        }
        std::cout << "Failed GPU wait blocks command reuse and screenshot readback: passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
