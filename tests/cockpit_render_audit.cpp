// Development-only visual corpus: uses the production cockpit geometry and Vulkan renderer.
#include "gt2view/cockpit_eye_fit.h"
#include "gt2view/procedural_cockpit.h"
#include "gt2view/shading_rate.h"
#include "gt2vfs/gtfs.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void Check(VkResult result) {
    if (result != VK_SUCCESS) throw std::runtime_error("Vulkan error " + std::to_string(result));
}
struct Device {
    gt2view::VkContext::Existing existing;
    Device() {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "GT2 cockpit corpus"; app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; create.pApplicationInfo = &app;
        Check(vkCreateInstance(&create, nullptr, &existing.instance));
        uint32_t count = 0; Check(vkEnumeratePhysicalDevices(existing.instance, &count, nullptr));
        std::vector<VkPhysicalDevice> devices(count);
        Check(vkEnumeratePhysicalDevices(existing.instance, &count, devices.data()));
        for (auto device : devices) {
            VkPhysicalDeviceProperties properties; vkGetPhysicalDeviceProperties(device, &properties);
            if (properties.apiVersion < VK_API_VERSION_1_3) continue;
            existing.physical = device;
            if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) break;
        }
        if (!existing.physical) throw std::runtime_error("no Vulkan 1.3 device");
        VkPhysicalDeviceProperties properties; vkGetPhysicalDeviceProperties(existing.physical, &properties);
        std::printf("GPU: %s\n", properties.deviceName);
        vkGetPhysicalDeviceQueueFamilyProperties(existing.physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> queues(count);
        vkGetPhysicalDeviceQueueFamilyProperties(existing.physical, &count, queues.data());
        while (existing.queueFamily < count && !(queues[existing.queueFamily].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++existing.queueFamily;
        if (existing.queueFamily == count) throw std::runtime_error("no graphics queue");
        const float priority = 1;
        VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue.queueFamilyIndex = existing.queueFamily; queue.queueCount = 1; queue.pQueuePriorities = &priority;
        VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES}; v13.dynamicRendering = VK_TRUE;
        VkPhysicalDeviceMultiviewFeatures multiview{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES};
        multiview.multiview = VK_TRUE; multiview.pNext = &v13;
        VkPhysicalDeviceFragmentShadingRateFeaturesKHR rate{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
        existing.shadingRate = gt2view::ShadingRateFeatures(existing.physical, rate);
        const char* extension = VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME;
        if (existing.shadingRate) v13.pNext = &rate;
        VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        device.pNext = &multiview; device.queueCreateInfoCount = 1; device.pQueueCreateInfos = &queue;
        if (existing.shadingRate) { device.enabledExtensionCount = 1; device.ppEnabledExtensionNames = &extension; }
        Check(vkCreateDevice(existing.physical, &device, nullptr, &existing.device));
        vkGetDeviceQueue(existing.device, existing.queueFamily, 0, &existing.queue);
    }
    ~Device() {
        if (existing.device) vkDestroyDevice(existing.device, nullptr);
        if (existing.instance) vkDestroyInstance(existing.instance, nullptr);
    }
};

constexpr uint32_t kTextureRow = 512, kClutRow = 736;
void UploadBody(gt2view::VkSceneRenderer& renderer, const gt2view::CockpitBody& body, const gt2::CarTexture& texture) {
    using namespace gt2view;
    if (body.vertices.size() > VkSceneRenderer::kCockpitBodyVertexStride) throw std::runtime_error("cockpit body vertex budget exceeded");
    if (texture.paints.empty()) throw std::runtime_error("car texture has no paints");
    std::vector<uint16_t> vram(225 * 1024, 0);
    for (size_t i = 0; i < texture.indices.size(); ++i) {
        const size_t x = i % gt2::CarTexture::kWidth, y = i / gt2::CarTexture::kWidth;
        vram[y * 1024 + x / 4] |= uint16_t(texture.indices[i] << ((x & 3) * 4));
    }
    const auto& paint = texture.paints[0];
    for (size_t palette = 0; palette < 16; ++palette) for (size_t index = 0; index < 16; ++index) {
        const auto colour = paint.cluts[palette][index];
        vram[224 * 1024 + palette * 16 + index] = colour;
        vram[224 * 1024 + (kCockpitGlazingPalette + palette) * 16 + index] =
            (body.glassMasks[palette] & (1u << index)) ? 0 : colour;
        vram[224 * 1024 + (kCockpitLiningPalette + palette) * 16 + index] = colour ? kCockpitTrimColor : 0;
    }
    renderer.UploadVram(kTextureRow, 225, vram.data());
    std::vector<SceneVertex> vertices; vertices.reserve(body.vertices.size());
    for (const auto& source : body.vertices) {
        SceneVertex vertex{};
        std::copy_n(source.pos, 3, vertex.pos); std::copy_n(source.texel, 2, vertex.texel); std::copy_n(source.color, 3, vertex.color);
        vertex.page = kTextureRow << 16; vertex.clut = uint32_t(source.palette) * 16 | (kClutRow << 16);
        vertex.flags = (source.textured ? kTextured : 0u) | (source.rawTexture ? kRawTexture : 0u) | kCarPaint;
        vertices.push_back(vertex);
    }
    renderer.SetVertices(VkSceneRenderer::kCockpitBodyVertexBase, vertices);
}

std::array<float,16> View(const std::array<float,3>& eye, float yawDegrees, float pitchDegrees) {
    constexpr float radians = 3.14159265358979323846f / 180;
    const float yaw = yawDegrees * radians, pitch = pitchDegrees * radians;
    const float right[] = {std::cos(yaw), 0, std::sin(yaw)};
    const float up[] = {-std::sin(yaw)*std::sin(pitch), std::cos(pitch), std::cos(yaw)*std::sin(pitch)};
    const float forward[] = {std::sin(yaw)*std::cos(pitch), std::sin(pitch), -std::cos(yaw)*std::cos(pitch)};
    const float focal = 1 / std::tan(95.f * radians * .5f);
    std::array<float,16> matrix{};
    for (size_t axis = 0; axis < 3; ++axis) {
        matrix[axis*4] = right[axis] * focal;
        matrix[axis*4+1] = -up[axis] * focal;
        matrix[axis*4+3] = forward[axis];
        matrix[12] -= right[axis] * focal * eye[axis];
        matrix[13] += up[axis] * focal * eye[axis];
        matrix[15] -= forward[axis] * eye[axis];
    }
    matrix[14] = .025f; // Infinite reversed-Z projection, matching the renderer's depth convention.
    return matrix;
}

gt2view::DrawItem Checker(gt2view::VkSceneRenderer& renderer) {
    std::vector<gt2view::SceneVertex> vertices;
    constexpr float colours[2][3] = {{.10f, .70f, .90f}, {.85f, .64f, .12f}};
    for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) {
        for (const auto& corner : std::array<std::array<int,2>,6>{{{0,0},{1,0},{1,1},{0,0},{1,1},{0,1}}}) {
            gt2view::SceneVertex vertex{};
            vertex.pos[0] = -1.f + float(x+corner[0])*.25f;
            vertex.pos[1] = -1.f + float(y+corner[1])*.25f;
            std::copy_n(colours[(x+y)&1], 3, vertex.color); vertices.push_back(vertex);
        }
    }
    renderer.SetVertices(0, vertices);
    gt2view::DrawItem item; item.vertexCount = uint32_t(vertices.size());
    item.mvp[0] = item.mvp[5] = item.mvp[10] = item.mvp[15] = 1;
    return item;
}
}

int main(int argc, char** argv) {
    try {
        if (argc < 3 || argc > 4) {
            std::puts("gt2cockpitaudit disc output-under-work [car-id-substring]"); return 1;
        }
        const auto output = std::filesystem::absolute(argv[2]).lexically_normal();
        const auto work = std::filesystem::absolute("work").lexically_normal();
        const auto relative = output.lexically_relative(work);
        if (relative.empty() || *relative.begin() == "..") throw std::runtime_error("screenshots must remain under the workspace work directory");
        std::filesystem::create_directories(output / "frames");
        gt2::DiscImage disc(argv[1]); gt2::GtfsVolume volume(disc);
        Device device; gt2view::VkContext context(device.existing);
        gt2view::VkSceneRenderer renderer(context, {320,320}, VK_FORMAT_R8G8B8A8_UNORM);
        gt2view::RenderOptions options; options.msaa = 1; options.smoothTextures = true; renderer.SetOptions(options);
        renderer.EnableDecodedTextures(true);
        const auto checker = Checker(renderer);
        gt2view::ProceduralCockpit cockpit(renderer);
        std::ofstream csv(output / "audit.csv");
        if (!csv) throw std::runtime_error("cannot open audit report");
        csv << "car,body_vertices,window_triangles,eye_x,eye_y,eye_z,side_sill,open_top,status\n";
        size_t cars = 0, failures = 0, skipped = 0;
        const auto started = std::chrono::steady_clock::now();
        for (const auto& entry : volume.Files()) {
            if (!entry.path.ends_with(".cdo.gz") && !entry.path.ends_with(".cdo")) continue;
            const auto slash = entry.path.find_last_of('/'), dot = entry.path.find(".cdo");
            const std::string id = entry.path.substr(slash == std::string::npos ? 0 : slash+1, dot-(slash == std::string::npos ? 0 : slash+1));
            if (argc > 3 && id.find(argv[3]) == std::string::npos) continue;
            try {
                const auto model = gt2::ParseCarModel(volume.Read(entry));
                auto fit = gt2view::FitCockpit(model);
                if (!fit.valid) { ++skipped; continue; }
                auto path = entry.path; path.replace(path.find(".cdo"), 4, ".cdp");
                const auto texture = gt2::ParseCarTexture(volume.Read(path));
                const auto body = gt2view::BuildCockpitBody(model, texture, fit);
                gt2view::RefineCockpitEye(fit, body.vertices, texture, body.glassMasks);
                gt2view::FitCockpitSideSills(fit, body.windowOpenings);
                gt2view::FitCockpitMirror(fit, model);
                UploadBody(renderer, body, texture);
                auto eye = fit.eye; eye[2] += .15f;
                struct Angle { const char* name; float yaw, pitch; };
                constexpr Angle angles[] = {{"front",0,-8},{"left",-90,-5},{"right",90,-5},{"seat",60,-65}};
                for (const auto& angle : angles) {
                    const auto matrix = View(eye, angle.yaw, angle.pitch);
                    gt2view::DrawItem bodyItem;
                    bodyItem.firstVertex = gt2view::VkSceneRenderer::kCockpitBodyVertexBase;
                    bodyItem.vertexCount = uint32_t(body.vertices.size());
                    std::copy(matrix.begin(), matrix.end(), bodyItem.mvp);
                    std::vector<gt2view::DrawItem> items{checker, bodyItem};
                    cockpit.Append(items, fit, matrix.data(), 0, 0, 850, 1, true, 0, .15f);
                    renderer.Draw(items, (output / "frames" / (id+"-"+angle.name+".png")).string(), items.size());
                    renderer.WaitFrame();
                }
                csv << id << ',' << body.vertices.size() << ',' << body.windowTriangles << ',' << eye[0] << ',' << eye[1] << ',' << eye[2]
                    << ',' << fit.sideSillValid << ',' << fit.openTop << ",ok\n";
                ++cars;
                if (cars % 25 == 0) { std::printf("rendered %zu cars (%zu screenshots), failures %zu\n", cars, cars*4, failures); std::fflush(stdout); csv.flush(); }
            } catch (const std::exception& error) {
                ++failures; csv << id << ",,,,,,,,error\n";
                std::fprintf(stderr, "%s: %s\n", id.c_str(), error.what());
            }
        }
        csv.flush();
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
        std::ofstream summary(output / "summary.json");
        summary << "{\n  \"cars\": " << cars << ",\n  \"screenshots\": " << cars*4 << ",\n  \"failures\": " << failures
                << ",\n  \"non_car_assets_skipped\": " << skipped << ",\n  \"seconds\": " << seconds
                << ",\n  \"image_size\": [320, 320],\n  \"field_of_view_degrees\": 95,\n  \"seat_back_metres\": 0.15,\n  \"paint_index\": 0\n}\n";
        std::printf("AUDIT cars=%zu screenshots=%zu failures=%zu skipped=%zu seconds=%.1f\n", cars, cars*4, failures, skipped, seconds);
        return failures || !cars ? 2 : 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
