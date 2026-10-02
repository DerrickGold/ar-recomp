#version 450
layout(local_size_x = 64) in;
layout(std430, set = 0, binding = 0) readonly buffer Costs { uint values[]; } costs;
layout(std430, set = 1, binding = 0) writeonly buffer Directions { ivec4 values[]; } output_field;
// Per OBJ priority: width, height, atlas source slot, analyze flag.
layout(std140, set = 2, binding = 0) uniform Settings { vec4 planes[4]; } settings;
shared ivec2 vectors[880];
shared uint occupied[880];
shared uint reliable[880];

uint cost(uint base, ivec2 delta) {
    return costs.values[base+1u+uint((delta.y+7)*15+delta.x+7)];
}
bool better(uint candidate_cost,ivec2 candidate,uint best_cost,ivec2 best) {
    if(candidate_cost!=best_cost)return candidate_cost<best_cost;
    int a=abs(candidate.x)+abs(candidate.y),b=abs(best.x)+abs(best.y);
    if(a!=b)return a<b;
    return candidate.y!=best.y?candidate.y<best.y:candidate.x<best.x;
}
void main() {
    int plane=int(gl_WorkGroupID.x),direction=int(gl_WorkGroupID.y),tid=int(gl_LocalInvocationID.x);
    ivec2 size=ivec2(settings.planes[plane].xy),blocks=(size+15)/16;
    int count=blocks.x*blocks.y;
    bool enabled=settings.planes[plane].w!=0.0;
    for(int at=tid;at<880;at+=64) {
        vectors[at]=ivec2(0);occupied[at]=0u;reliable[at]=0u;
        if(enabled && at<count) {
            occupied[at]=costs.values[uint(((plane*2+direction)*880+at)*226)];
        }
    }
    barrier();
    // Anti-diagonals preserve the CPU's left/top neighbour seeding exactly.
    // Every dependency is in the preceding diagonal of this one workgroup.
    for(int diagonal=0;enabled && diagonal<blocks.x+blocks.y-1;++diagonal) {
        int bx=tid,by=diagonal-bx,at=by*blocks.x+bx;
        if(bx<blocks.x && by>=0 && by<blocks.y && occupied[at]!=0u) {
            ivec2 best=ivec2(0);int contributors=0;
            if(bx>0){best+=vectors[at-1];++contributors;}
            if(by>0){best+=vectors[at-blocks.x];++contributors;}
            if(contributors>1)best/=contributors;
            best=clamp(best,ivec2(-7),ivec2(7));
            uint base=uint(((plane*2+direction)*880+at)*226);
            uint stationary=cost(base,ivec2(0));
            uint best_cost=cost(base,best);
            for(int step=4;step>=1;step/=2) {
                ivec2 center=best;
                for(int oy=-1;oy<=1;++oy)for(int ox=-1;ox<=1;++ox) {
                    ivec2 candidate=clamp(center+ivec2(ox,oy)*step,ivec2(-7),ivec2(7));
                    uint c=cost(base,candidate);
                    if(better(c,candidate,best_cost,best)){best=candidate;best_cost=c;}
                }
            }
            bool accepted=any(notEqual(best,ivec2(0))) && best_cost<stationary &&
                stationary-best_cost>=max(64u,(stationary*12u+99u)/100u);
            vectors[at]=accepted?best:ivec2(0);reliable[at]=accepted?1u:0u;
        }
        barrier();
    }
    const ivec2 neighbours[4]=ivec2[4](ivec2(-1,0),ivec2(1,0),ivec2(0,-1),ivec2(0,1));
    for(int at=tid;at<880;at+=64) {
        ivec2 motion=vectors[at];uint accepted=reliable[at];
        if(enabled && at<count && occupied[at]==0u) {
            ivec2 sum=ivec2(0),b=ivec2(at%blocks.x,at/blocks.x);int n=0;
            for(int i=0;i<4;++i) {
                ivec2 adjacent=b+neighbours[i];int other=adjacent.y*blocks.x+adjacent.x;
                if(all(greaterThanEqual(adjacent,ivec2(0))) && all(lessThan(adjacent,blocks)) &&
                    occupied[other]!=0u && reliable[other]!=0u){sum+=vectors[other];++n;}
            }
            if(n!=0){motion=sum/n;accepted=1u;}
        }
        output_field.values[(plane*2+direction)*880+at]=ivec4(motion,int(accepted),0);
    }
}
