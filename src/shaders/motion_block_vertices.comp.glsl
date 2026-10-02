#version 450
layout(local_size_x = 64) in;
layout(std430, set = 0, binding = 0) readonly buffer Motion { ivec4 values[]; } motion;
struct Vertex { vec4 position; vec4 color; vec4 uv; };
layout(std430, set = 1, binding = 0) writeonly buffer Vertices { Vertex values[]; } vertices;
// width, height, plane, interpolation phase; output atlas width/height.
layout(std140, set = 2, binding = 0) uniform Settings { vec4 plane; vec4 atlas; } settings;
void main() {
    ivec2 size=ivec2(settings.plane.xy),blocks=(size+15)/16;
    uint index=gl_GlobalInvocationID.x;
    if(index>=uint((blocks.x+1)*(blocks.y+1)))return;
    ivec2 grid=ivec2(int(index)%(blocks.x+1),int(index)/(blocks.x+1));
    ivec2 source=min(grid*16,size),sample_at=min(source,size-1);
    ivec2 point=max(sample_at-8,ivec2(0)),b=min(point/16,blocks-1),b1=min(b+1,blocks-1);
    ivec2 fraction=ivec2(b.x==b1.x?0:point.x%16,b.y==b1.y?0:point.y%16);
    int plane=int(settings.plane.z),base=plane*880;
    ivec4 top=motion.values[base+b.y*blocks.x+b.x]*(16-fraction.x)+
        motion.values[base+b.y*blocks.x+b1.x]*fraction.x;
    ivec4 bottom=motion.values[base+b1.y*blocks.x+b.x]*(16-fraction.x)+
        motion.values[base+b1.y*blocks.x+b1.x]*fraction.x;
    vec4 field=vec4(top*(16-fraction.y)+bottom*fraction.y)/256.0;
    bool forward=settings.plane.w<0.5 && motion.values[3520+plane].x!=0;
    vec2 offset=forward?field.xy*settings.plane.w:field.zw*(1.0-settings.plane.w);
    vec2 position=vec2(source)+offset+vec2(0,plane*size.y);
    vertices.values[index].position=vec4(position.x/settings.atlas.x*2.0-1.0,
        1.0-position.y/settings.atlas.y*2.0,0,1);
    vertices.values[index].color=vec4(forward?1.0:0.0,1,1,1);
    vertices.values[index].uv=vec4(vec2(source),0,0);
}
