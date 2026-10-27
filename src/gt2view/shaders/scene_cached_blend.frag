#version 450
#extension GL_GOOGLE_include_directive : require
// Blended pipelines never write depth; early tests preserve alpha-mask holes.
layout(early_fragment_tests) in;
#include "scene_cached_common.glsl"
