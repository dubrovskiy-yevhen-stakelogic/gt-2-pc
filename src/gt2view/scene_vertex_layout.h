#ifndef GT2_SCENE_VERTEX_LAYOUT_H
#define GT2_SCENE_VERTEX_LAYOUT_H
// Shared with GLSL. The C++ layout assertion catches changes to preceding ranges.
#define GT2_COCKPIT_VERTEX_LIMIT 49152
#define GT2_NATIVE_MODEL_VERTEX_BASE (1048576 + 16384 + 8192 + 65536 + 65536 + GT2_COCKPIT_VERTEX_LIMIT)
#endif
