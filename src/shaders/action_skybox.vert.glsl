#version 450
#include "action_skybox_mapping.glsl"
layout(location=0) in vec2 position;
layout(location=1) in vec4 color;
layout(location=2) in vec2 uv;
layout(location=0) out vec4 vertex_color;
layout(location=1) out vec2 texture_uv;
layout(std430,set=0,binding=0) readonly buffer Motion { ivec4 values[]; } motion;
layout(std140,set=1,binding=0) uniform Context {
    vec4 target; // target W/H, viewport origin X/Y
    vec4 motion_info; // motion slot, phase, periodic, raw band index
    SkyMapping mapping;
} settings;
void main() {
    vec2 offset=vec2(0);
    int slot=int(settings.motion_info.x);
    if(slot>=0&&motion.values[slot*2+1].x!=0) {
        vec4 m=vec4(motion.values[slot*2]);float phase=settings.motion_info.y;
        offset=phase<.5?m.zw+m.xy*phase:m.zw*(1-phase);
    }
    texture_uv=uv; vec2 pixel=position;
    if(settings.motion_info.z!=0) texture_uv-=offset/256.0;
    else if(settings.mapping.meta.x>0) {
        vec4 bounds; vec2 rows;
        bool valid=resolve_sky_band(settings.mapping,int(settings.motion_info.w),offset.y,bounds,rows);
        texture_uv=vec2(position.x==0?bounds.x:bounds.z,position.y==0?bounds.y:bounds.w);
        pixel.y=(position.y==0?rows.x:rows.y)*settings.mapping.output_size.y;
        if(!valid)pixel=vec2(-1);
    }
    vertex_color=color;
    gl_Position=vec4((pixel+settings.target.zw)/settings.target.xy*vec2(2,-2)+vec2(-1,1),0,1);
}
