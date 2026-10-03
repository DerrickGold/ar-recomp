#version 450
layout(std430,set=0,binding=0) readonly buffer Vertices { vec4 values[]; } vertices;
layout(std140,set=1,binding=0) uniform Target { vec4 size; } target;
layout(location=0) out vec4 color;
void main() {
    vec2 point=vertices.values[gl_VertexIndex*2].xy;
    gl_Position=vec4(point/target.size.xy*vec2(2,-2)+vec2(-1,1),0,1);
    color=vertices.values[gl_VertexIndex*2+1];
}
