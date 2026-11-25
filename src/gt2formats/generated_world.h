#pragma once
#include <filesystem>
#include "gt2formats/generated_sponsor_aliases.h"
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
// Fixed course adverts and race-selected sponsors share replacement artwork.
inline std::string SponsorImageFor(std::string_view file) {
    constexpr std::string_view hex="0123456789abcdef";
    for(size_t i=0;i+40<=file.size();++i) {
        if(i && file[i-1]!='-')continue;
        const auto hash=file.substr(i,40);
        if(hash.find_first_not_of(hex)!=std::string_view::npos ||
           (i+40<file.size() && file[i+40]!='-' && file[i+40]!='.'))continue;
        for(const auto& alias:sponsorAliases)
            if(hash==alias.source)return std::string(alias.replacement)+".png";
        return std::string(hash)+".png";
    }
    return {};
}
inline std::filesystem::path CourseImagePath(const std::string& course,const std::string& file) {
    const auto local=root/course/file;
    if(std::filesystem::is_regular_file(local))return local;
    {
        const auto sponsor=SponsorImageFor(file);
        if(!sponsor.empty()) {
            const auto sharedSponsor=root/"_sponsors"/sponsor;
            if(std::filesystem::is_regular_file(sharedSponsor))return sharedSponsor;
        }
    }
    return root/"_shared"/file;
}
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
