#version 450
#extension GL_EXT_multiview : require
// Checkerboard whose two colours depend on the eye (view index + eyeBase) and the frame (set by the app).
layout(push_constant) uniform PC {
    vec4 colA[2];
    vec4 colB[2];
    int eyeBase;
    int cell;
} pc;
layout(location = 0) out vec4 outColor;
void main() {
    int eye = int(gl_ViewIndex) + pc.eyeBase;
    ivec2 c = ivec2(gl_FragCoord.xy) / pc.cell;
    outColor = (((c.x + c.y) & 1) != 0) ? pc.colB[eye] : pc.colA[eye];
}