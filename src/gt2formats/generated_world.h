#pragma once
#include <filesystem>
#include <array>
#include <string_view>
#include <string>

namespace gt2::generated {
// Independent from offline HD media/upscaling. Opt-in, presentation only.
inline bool enabled = false;
inline bool allowed = true;
enum Category : unsigned { Road, Vegetation, Buildings, Trackside, Signs, Horizon, Hud, Count };
inline constexpr std::array<std::string_view,Count> keys{"road","vegetation","buildings","trackside","signs","horizon","hud"};
inline constexpr std::array<const char*,Count> labels{"Road and terrain","Vegetation","Buildings","Trackside objects","Signs and markings","Sky and horizon","Race HUD"};
inline constexpr unsigned allCategories=(1u << Count)-1;
inline unsigned categories=allCategories;
inline unsigned ActiveMask() { return enabled && allowed ? categories : 0; }
inline std::filesystem::path root;
inline bool Enabled() { return enabled && allowed; }
inline void SetEnabled(bool value) { enabled = value; }
inline void SetRoot(const std::string& discPath) {
    auto path = std::filesystem::path(discPath);
    if (!std::filesystem::is_directory(path)) path = path.parent_path();
    // The selected texture pack is authoritative: missing images must not fall back
    // to another HD pack. Original game textures remain the per-image fallback.
    const auto common=path.parent_path()/"textures";
    const auto local=path/"textures";
    const auto mode=path.filename();
    if(std::filesystem::is_directory(local))root=local;
    else if(std::filesystem::is_directory(common))root=std::filesystem::is_directory(common/mode)?common/mode:common;
    else root=path/"generated-world"; // existing development installs

}
}
