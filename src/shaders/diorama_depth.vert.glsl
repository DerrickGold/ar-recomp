#version 450
// Owned scenery has a world receiver independent of its texture atlas slot.
// Keep the shared Sim3D/native vertex contract unchanged for other pipelines.
layout(location=0) in vec4 position;
layout(location=1) in vec4 color;
layout(location=2) in vec2 uv;
layout(location=3) in vec2 heat_world_xy;
layout(location=0) out vec4 v_color;
layout(location=1) out vec2 v_uv;
layout(location=3) out vec2 v_heat_world_xy;
void main() {
    gl_Position=position;
    v_color=color;
    v_uv=uv;
    v_heat_world_xy=heat_world_xy;
}
