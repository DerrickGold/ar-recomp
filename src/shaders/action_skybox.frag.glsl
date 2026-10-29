#version 450
layout(location=0) in vec4 vertex_color;
layout(location=1) in vec2 texture_uv;
layout(location=2) flat in vec2 cloud_offset;
layout(location=0) out vec4 result;
layout(set=2,binding=0) uniform sampler2D image;
layout(std140,set=3,binding=0) uniform Material { vec4 texel; vec4 cloud_source; vec4 cloud_meta; vec4 cloud_regions[24]; vec4 cloud_background_dim; } settings;
#define CLOUD_REGIONS settings.cloud_regions
#include "cloud_light.glsl"
void main() {
    if(settings.texel.z<=0) result=texture(image,texture_uv);
    else {
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
    result=sum*.1;
    }
    if(settings.cloud_meta.x>0.0)result.rgb=cloud_lighting(result.rgb,texture_uv,
        settings.cloud_source-vec4(cloud_offset,0,0),settings.cloud_meta,settings.cloud_background_dim);
    result*=vertex_color;
}
