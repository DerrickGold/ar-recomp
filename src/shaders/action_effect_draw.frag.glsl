#version 450
layout(location=0) in vec4 color;
layout(location=1) in vec2 uv;
layout(set=2,binding=0) uniform sampler2D masks;
layout(location=0) out vec4 result;
void main() {
    result=color;
    // Old primitives are constant white; no texture fetch for those draws.
    if(any(notEqual(uv,vec2(0))))result.a*=texture(masks,uv).a;
}
