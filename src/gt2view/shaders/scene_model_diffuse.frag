#version 450
#extension GL_GOOGLE_include_directive : require
// Depth-writing body/cabin draws contain no type 2/4 reflection geometry.
// Keep their shader independent of the environment-view matrices and sampling.
#define GT2_MODEL_GLASS_ONLY
#include "scene_model_common.glsl"
