#version 450
layout(location=0) in vec4 vertex_color;
layout(location=1) in vec2 texture_uv;
layout(location=0) out vec4 result;
layout(set=2,binding=0) uniform sampler2D image;
layout(std140,set=3,binding=0) uniform Material { vec4 texel; } settings;
void main() {
    if(settings.texel.z<=0) { result=texture(image,texture_uv)*vertex_color; return; }
    vec2 d=settings.texel.xy*settings.texel.z;
    vec4 sum=vec4(0);
    sum+=texture(image,texture_uv+vec2(-d.x,-d.y));
    sum+=texture(image,texture_uv+vec2(0,-d.y));
    sum+=texture(image,texture_uv+vec2(d.x,-d.y));
    sum+=texture(image,texture_uv+vec2(-d.x,0));
    sum+=texture(image,texture_uv)*2;
    sum+=texture(image,texture_uv+vec2(d.x,0));
    sum+=texture(image,texture_uv+vec2(-d.x,d.y));
    sum+=texture(image,texture_uv+vec2(0,d.y));
    sum+=texture(image,texture_uv+vec2(d.x,d.y));
    result=sum*.1*vertex_color;
}
