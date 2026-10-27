#version 450
#extension GL_GOOGLE_include_directive : require
// This pipeline never writes depth: early tests cannot close alpha-mask holes.
layout(early_fragment_tests) in;
#define GT2_MODEL_GLASS_ONLY
#include "scene_model_common.glsl"
