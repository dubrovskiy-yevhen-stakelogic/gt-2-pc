#pragma once
#include "gt2formats/model_set.h"
#include <memory>

namespace gt2 {
// CPU-only preparation: no renderer/device calls from the worker. One worker,
// up to 96 queued cars and 256 MiB of retained geometry/textures.
void StartModelSetPreload(const std::filesystem::path& root, const std::filesystem::path& history,
                          const std::vector<std::string>& priority = {});
std::unique_ptr<ModelSetCar> CopyPreloadedModel(const std::filesystem::path& root, const std::string& id);
std::unique_ptr<GltfMesh> CopyPreloadedAuthoredMesh(const std::filesystem::path& root, const std::string& id);
void RememberModelSetCar(const std::filesystem::path& root, const std::string& id);
void StopModelSetPreload();
}
