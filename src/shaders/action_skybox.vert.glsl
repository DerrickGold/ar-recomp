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
    vec4 heat_source;
    vec4 heat_bounds[4];
    vec4 heat_motion[4];
} settings;
// Matches DioramaHeat_Uv; evaluated only on the fixed, bounded heat grid.
vec2 heat_uv(vec2 value) {
    if(settings.heat_source.z<=0||settings.heat_source.w<=0)return value;
    vec2 world=value*settings.heat_source.zw+settings.heat_source.xy;
    float strength=0,phase=0;
    for(int i=0;i<4;++i) {
        vec4 r=settings.heat_bounds[i];
        if(settings.heat_motion[i].x<=0||r.z<=r.x||r.w<=r.y)continue;
        vec2 p=(world-r.xy)/(r.zw-r.xy);
        if(any(lessThanEqual(p,vec2(0)))||any(greaterThanEqual(p,vec2(1))))continue;
        float edge=min(1,8*min(min(p.x,1-p.x),min(p.y,1-p.y)));
        float a=settings.heat_motion[i].x*edge*edge*(3-2*edge);
        if(a>strength){strength=a;phase=settings.heat_motion[i].y;}
    }
    value.x+=strength*(sin(world.y*.12+phase*3)+.4*sin(world.x*.08-world.y*.05+phase*2))/settings.heat_source.z;
    value.y+=strength*.18*sin(world.x*.09+world.y*.07+phase*2)/settings.heat_source.w;
    return value;
}
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
        texture_uv=mix(bounds.xy,bounds.zw,uv);
        texture_uv=clamp(heat_uv(texture_uv),bounds.xy,bounds.zw);
        pixel.y=mix(rows.x,rows.y,uv.y)*settings.mapping.output_size.y;
        if(!valid)pixel=vec2(-1);
    }
    vertex_color=color;
    gl_Position=vec4((pixel+settings.target.zw)/settings.target.xy*vec2(2,-2)+vec2(-1,1),0,1);
}
