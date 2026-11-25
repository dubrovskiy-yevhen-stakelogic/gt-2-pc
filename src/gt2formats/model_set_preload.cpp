#include "gt2formats/model_set_preload.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <chrono>
#include <set>

#if defined(__EMSCRIPTEN__)
namespace gt2 {
void StartModelSetPreload(const std::filesystem::path&, const std::filesystem::path&, const std::vector<std::string>&) {}
std::unique_ptr<ModelSetCar> CopyPreloadedModel(const std::filesystem::path&, const std::string&) { return {}; }
std::unique_ptr<GltfMesh> CopyPreloadedAuthoredMesh(const std::filesystem::path&, const std::string&) { return {}; }
void RememberModelSetCar(const std::filesystem::path&, const std::string&) {}
void StopModelSetPreload() {}
}
#else
namespace gt2 {
namespace {
constexpr size_t kBudget = 256 * 1024 * 1024, kCars = 96, kRecent = 8;
bool ValidId(const std::string& id) {
    return !id.empty() && id.size() <= 16 && id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-_") == std::string::npos;
}
size_t Bytes(const ModelSetCar& car) {
    size_t bytes = sizeof(car);
    for (const auto* mesh : {&car.exterior, &car.cockpit, &car.lamps, &car.reflections, &car.attachments}) {
        for (const auto& p : mesh->primitives) bytes += p.vertices.size()*sizeof(GltfVertex) + p.indices.size()*sizeof(uint32_t);
        for (const auto& image : mesh->images) bytes += image.rgba.size();
        bytes += mesh->materials.size()*sizeof(GltfMaterial);
    }
    for (const auto& row : car.nativePaintMaterials) bytes += row.size()*sizeof(std::array<float,17>);
    return bytes;
}
uint32_t CompactImages(ModelSetCar& car) {
    // Native submeshes share the same decoded texture table. Store that table
    // once in the preload cache, then restore independent mutable copies on use.
    uint32_t aliases=0, bit=1;
    for (auto* mesh : {&car.cockpit,&car.lamps,&car.reflections,&car.attachments}) {
        bool same=mesh->images.size()==car.exterior.images.size();
        for(size_t i=0;same && i<mesh->images.size();++i) {
            const auto& a=mesh->images[i];const auto& b=car.exterior.images[i];
            same=a.width==b.width && a.height==b.height && a.rgba==b.rgba;
        }
        if(same) {mesh->images.clear();aliases|=bit;}
        bit<<=1;
    }
    return aliases;
}
struct Preload {
    std::mutex mutex;
    std::condition_variable wake;
    std::filesystem::path root, history;
    std::vector<std::string> recent;
    std::deque<std::string> pending;
    std::map<std::string,std::unique_ptr<ModelSetCar>> ready;
    std::map<std::string,uint32_t> imageAliases;
    std::set<std::string> authored;
    size_t bytes = 0;
    // Declared last: joins before the fields used by the worker are destroyed.
    std::atomic<bool> stop{false};
    std::thread worker;
    ~Preload() { if (worker.joinable()) { { std::lock_guard lock(mutex); stop = true; } wake.notify_all(); worker.join(); } }
};
Preload& State() { static Preload state; return state; }
void Enqueue(Preload& s, const std::string& id) {
    if (!ValidId(id) || s.ready.contains(id) || std::find(s.pending.begin(),s.pending.end(),id)!=s.pending.end()) return;
    if (s.pending.size()<kCars) s.pending.push_back(id);
}
}
void StopModelSetPreload() {
    auto& s = State();
    if (s.worker.joinable()) { { std::lock_guard lock(s.mutex); s.stop = true; } s.wake.notify_all(); s.worker.join(); }
    std::lock_guard lock(s.mutex);
    s.ready.clear(); s.authored.clear(); s.imageAliases.clear(); s.pending.clear(); s.recent.clear(); s.bytes=0; s.root.clear(); s.history.clear();
}
void StartModelSetPreload(const std::filesystem::path& root, const std::filesystem::path& history,
                          const std::vector<std::string>& priority) {
    auto& s = State();
    {
        std::lock_guard lock(s.mutex);
        if (s.worker.joinable() && s.root==root) {
            for(const auto& id:priority) Enqueue(s,id);
            s.wake.notify_one(); return;
        }
    }
    StopModelSetPreload();
    s.root=root; s.history=history;
    for(const auto& id:priority) Enqueue(s,id);
    std::ifstream file(history); std::string id;
    while(s.recent.size()<kRecent && std::getline(file,id)) if(ValidId(id)) {
        s.recent.push_back(id); Enqueue(s,id);
    }
    s.stop = false;
    s.worker=std::thread([&s,root] {
        ModelSet models(root);
        while(!s.stop.load()) {
            std::string next;
            {
                std::unique_lock lock(s.mutex);
                s.wake.wait(lock,[&]{return s.stop.load() || !s.pending.empty();});
                if(s.stop.load()) break;
                next=std::move(s.pending.front()); s.pending.pop_front();
                if(s.ready.contains(next)) continue;
            }
            const auto start=std::chrono::steady_clock::now();
            try {
                const auto manifest=root/"authored"/next/"manifest.json";
                const bool authored=std::filesystem::is_regular_file(manifest);
                auto car=std::make_unique<ModelSetCar>();
                if(authored) {
                    const auto doc=json::ReadFile(manifest.string());
                    if(doc.StringOr("format","")!="gt2-authored-car" || doc.IntOr("version",0)!=1 || doc.StringOr("car","")!=next)
                        throw std::runtime_error("incompatible authored preload manifest");
                    car->exterior=ReadGltf(ModelSetAsset(manifest.parent_path(),doc.Require("mesh").AsString()).string());
                } else {
                    if(!models.Has(next))continue;
                    *car=models.Read(next,true);
                }
                const auto aliases=CompactImages(*car);
                const auto bytes=Bytes(*car);
                if (bytes > kBudget) continue;
                std::unique_lock lock(s.mutex);
                // Retain priorities within a fixed budget, never expand to the entire GT4 pack.
                if(!s.stop.load() && !s.ready.contains(next) && bytes<=kBudget-s.bytes) {
                    s.ready.emplace(next,std::move(car));s.imageAliases[next]=aliases;s.bytes+=bytes;
                    if(authored)s.authored.insert(next);
                    std::printf("GT4 preload: %s ready, %.1f ms, retained %zu KiB\n",next.c_str(),
                        std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count(),s.bytes/1024);
                }
            } catch(const std::exception& e) { std::printf("GT4 preload: %s deferred (%s)\n",next.c_str(),e.what()); }
        }
    });
}
std::unique_ptr<ModelSetCar> CopyPreloadedModel(const std::filesystem::path& root,const std::string& id) {
    auto& s=State(); std::lock_guard lock(s.mutex);
    if(s.root!=root || s.authored.contains(id)) return {};
    auto found=s.ready.find(id);if(found==s.ready.end())return {};
    // Preview and race each mutate their own copy (pose/alignment). Keep the
    // decoded source so opening the showroom cannot consume the race preload.
    std::printf("GT4 preload: reused %s\n",id.c_str());
    auto car=std::make_unique<ModelSetCar>(*found->second);
    const uint32_t aliases=s.imageAliases.at(id);uint32_t bit=1;
    for(auto* mesh:{&car->cockpit,&car->lamps,&car->reflections,&car->attachments}) {
        if(aliases & bit)mesh->images=car->exterior.images;
        bit<<=1;
    }
    return car;
}
std::unique_ptr<GltfMesh> CopyPreloadedAuthoredMesh(const std::filesystem::path& root,const std::string& id) {
    auto& s=State();std::lock_guard lock(s.mutex);
    if(s.root!=root || !s.authored.contains(id))return {};
    auto found=s.ready.find(id);if(found==s.ready.end())return {};
    std::printf("GT4 preload: reused authored %s\n",id.c_str());
    return std::make_unique<GltfMesh>(found->second->exterior);
}
void RememberModelSetCar(const std::filesystem::path& root,const std::string& id) {
    if(!ValidId(id))return;
    auto& s=State();std::lock_guard lock(s.mutex);
    if(s.root!=root || s.history.empty())return;
    s.recent.erase(std::remove(s.recent.begin(),s.recent.end(),id),s.recent.end());
    s.recent.insert(s.recent.begin(),id);if(s.recent.size()>kRecent)s.recent.resize(kRecent);
    // A tiny advisory file only: a failed write must never affect a model or a save.
    std::ofstream out(s.history); for(const auto& item:s.recent)out<<item<<'\n';
}
}

#endif
