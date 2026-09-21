#pragma once
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#include <windows.h>
#endif

namespace gt2::shell {
inline std::string sharedVrSettingsPath;
inline bool SharedVrKey(const std::string& key) {
    if(key.starts_with("vr_")) return true;
    for(const char* name : {"hd_assets","units","adaptive","rumble","profiler","frame_rate","frame_cap","vsync",
                           "render_scale","render_height","msaa","texture_filter","texture_mapping","scenery_detail","draw_distance"})
        if(key==name) return true;
    return false;
}
inline std::string VrPreferences(const std::string& text) {
    std::istringstream in(text); std::ostringstream out; std::string line;
    while(std::getline(in,line)) {
        if(!line.empty() && line.back()=='\r') line.pop_back();
        const auto eq=line.find('=');
        if(eq!=std::string::npos && SharedVrKey(line.substr(0,eq))) out<<line<<'\n';
    }
    return out.str();
}
inline std::string ReadPreferences(const std::filesystem::path& path) {
    std::ifstream in(path); std::ostringstream out; out<<in.rdbuf(); return out.str();
}
inline void WritePreferences(const std::filesystem::path& path,const std::string& text) {
    if(!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    auto temp=path; temp += ".tmp";
    { std::ofstream out(temp,std::ios::trunc); out<<text; out.flush(); if(!out) throw std::runtime_error("cannot write preferences"); }
#ifdef _WIN32
    if(!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("cannot replace preferences");
#else
    std::filesystem::rename(temp,path);
#endif
}
inline void InitializeSharedVrSettings(const std::filesystem::path& saves,const std::string& defaults,const std::string& overlayDefaults) {
    sharedVrSettingsPath=(saves/"vr-settings.txt").string();
    if(std::filesystem::is_regular_file(sharedVrSettingsPath)) return;
    std::filesystem::path latest;
    for(const char* disc : {"arcade","simulation"}) {
        const auto path=saves/disc/"settings.txt.overlay";
        if(std::filesystem::is_regular_file(path) && (latest.empty() || std::filesystem::last_write_time(path)>std::filesystem::last_write_time(latest))) latest=path;
    }
    const auto base=latest.empty()?defaults:ReadPreferences(latest.parent_path()/"settings.txt");
    const auto overlay=latest.empty()?overlayDefaults:ReadPreferences(latest);
    WritePreferences(sharedVrSettingsPath,VrPreferences(defaults)+VrPreferences(base)+VrPreferences(overlay));
}
}
