#include "gt2view/scene_types.h"
#include "gt2formats/hd_media.h"
#include <cstdio>
#include <stdexcept>
struct Renderer : gt2view::SceneRendererState {
    void UploadExternalTexture(uint32_t, uint32_t, const uint32_t*) override {}
    void Prepare() {
        hdUiGeneration_ = gt2::hd::Generation();
        hdUiEnabled_ = true;
        options_.smoothTextures = true;
        vramShadow_.resize(size_t(kVramRows)*1024);
        hdUiSlots_.push_back({"fixture",0,false});
        hdUiPages_[uint64_t(512)] = 0;
    }
};
int main() {
    Renderer renderer;
    renderer.Prepare();
    gt2view::SceneVertex original{};
    original.page=512; original.texel[0]=12; original.texel[1]=16;
    original.flags=gt2view::kTextured;
    auto nearest=original;
    nearest.flags|=gt2view::kNearestUi;
    std::vector<gt2view::SceneVertex> vertices{original,nearest};
    renderer.ApplyHdUi(vertices);
    if (!(vertices[0].flags & gt2view::kHdUi) || vertices[0].texel[0]!=48) return 1;
    const auto& kept=vertices[1];
    if (kept.page!=nearest.page || kept.texel[0]!=12 || kept.texel[1]!=16 || kept.flags!=nearest.flags) return 2;
    std::puts("PASS: ordinary HUD gets HD; nearest input HUD retains original page, UV and flags");
}
