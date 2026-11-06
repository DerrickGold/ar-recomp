#version 450
layout(location=0) in vec4 position;
layout(location=1) in vec4 color;
layout(location=2) in vec2 uv;
layout(location=3) in float ordinal;
layout(location=0) out vec4 v_color;
layout(location=1) out vec2 v_uv;
layout(location=2) flat out float v_ordinal;
void main() {
    gl_Position = position;
    v_color = color;
    v_uv = uv;
    v_ordinal = ordinal;
}
