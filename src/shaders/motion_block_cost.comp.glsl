#version 450
// Search costs are independent of neighbour seeds. Compute them in parallel
// per occupied block, caching the 27x27 search footprint in workgroup memory.
// The small ordered search then reproduces the CPU algorithm using these costs.
layout(local_size_x = 64) in;
layout(set = 0, binding = 0) uniform sampler2D previous_frame;
layout(set = 0, binding = 1) uniform sampler2D current_frame;
layout(std430, set = 1, binding = 0) writeonly buffer Costs { uint values[]; } costs;
layout(std140, set = 2, binding = 0) uniform Settings { vec4 planes[4]; } settings;
shared uint source_samples[16];
shared uint target_samples[729];
shared uint signal_bits[64];
uint pixel(bool forward, bool source, ivec2 p, ivec2 size, int slot) {
    if (any(lessThan(p,ivec2(0))) || any(greaterThanEqual(p,size))) return 0u;
    p.y+=slot*size.y;
    uvec4 c=uvec4(round((forward==source?texelFetch(previous_frame,p,0):
        texelFetch(current_frame,p,0))*255.0));
    return c.x|(c.y<<8)|(c.z<<16)|(c.w<<24);
}
uint difference(uint a,uint b) {
    ivec4 x=ivec4(a&255u,(a>>8)&255u,(a>>16)&255u,a>>24);
    ivec4 y=ivec4(b&255u,(b>>8)&255u,(b>>16)&255u,b>>24);
    ivec4 d=abs(x-y);return uint(d.x+d.y+d.z+d.w*2);
}
void main() {
    uint block=gl_WorkGroupID.x,direction=gl_WorkGroupID.y,plane=direction/2u;
    uint lane=gl_LocalInvocationIndex,base=(direction*880u+block)*226u;
    ivec2 size=ivec2(settings.planes[plane].xy),blocks=(size+15)/16;
    bool enabled=settings.planes[plane].w!=0.0 && block<uint(blocks.x*blocks.y);
    if(!enabled){if(lane==0u)costs.values[base]=0u;return;}
    ivec2 origin=ivec2(int(block)%blocks.x,int(block)/blocks.x)*16;
    int slot=int(settings.planes[plane].z);bool forward=(direction&1u)==0u;
    uint occupied=0u;
    for(uint i=lane;i<256u;i+=64u) {
        ivec2 p=ivec2(int(i)%16,int(i)/16);
        uint c=pixel(forward,true,origin+p,size,slot);occupied|=c;
        if((p.x&3)==0 && (p.y&3)==0)source_samples[(p.y/4)*4+p.x/4]=c;
    }
    signal_bits[lane]=occupied;barrier();
    for(uint step=32u;step>0u;step/=2u){if(lane<step)signal_bits[lane]|=signal_bits[lane+step];barrier();}
    if(lane==0u)costs.values[base]=signal_bits[0]!=0u?1u:0u;
    if(signal_bits[0]==0u)return;
    for(uint i=lane;i<729u;i+=64u)
        target_samples[i]=pixel(forward,false,origin+ivec2(int(i)%27-7,int(i)/27-7),size,slot);
    barrier();
    for(uint candidate=lane;candidate<225u;candidate+=64u) {
        ivec2 delta=ivec2(int(candidate)%15-7,int(candidate)/15-7);
        uint sum=uint(abs(delta.x)+abs(delta.y))*12u;
        for(int y=0;y<4 && origin.y+y*4<size.y;++y)
            for(int x=0;x<4 && origin.x+x*4<size.x;++x)
                sum+=difference(source_samples[y*4+x],target_samples[(y*4+delta.y+7)*27+x*4+delta.x+7]);
        costs.values[base+1u+candidate]=sum;
    }
}
