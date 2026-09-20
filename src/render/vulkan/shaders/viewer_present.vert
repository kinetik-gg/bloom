#version 460
// Bloom viewer presentation vertex shader (offline compiled; do not load at runtime).
// Generates one fullscreen triangle; the fragment shader renders the background, the resident
// display image under an explicit affine destination rectangle, and an optional overlay. No vertex
// buffer is bound.

layout(location = 0) out vec2 vTargetUv01;

void main()
{
    const vec2 kPositions[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    const vec2 position = kPositions[gl_VertexIndex];
    vTargetUv01 = position * 0.5 + 0.5;
    gl_Position = vec4(position, 0.0, 1.0);
}
