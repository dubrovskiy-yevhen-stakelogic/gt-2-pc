#pragma once
// Compile-time platform selection. All game/scene builders use this contract.
#ifdef __EMSCRIPTEN__
#include "gt2view/webgl_scene_renderer.h"
namespace gt2view { using SceneRenderer = WebGlSceneRenderer; }
#else
#include "gt2view/vk_scene_renderer.h"
namespace gt2view { using SceneRenderer = VkSceneRenderer; }
#endif
