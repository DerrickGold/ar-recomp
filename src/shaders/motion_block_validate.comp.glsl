#version 450
layout(local_size_x = 64) in;
layout(std430, set = 0, binding = 0) readonly buffer Directions { ivec4 values[]; } directions;
layout(std430, set = 1, binding = 0) writeonly buffer Motion { ivec4 values[]; } motion;
shared uint accepted[64];
void main() {
    uint tid=gl_LocalInvocationID.x,plane=gl_WorkGroupID.y;
    uint any_valid=0u;
    for(uint at=tid;at<880u;at+=64u) {
    ivec4 a=directions.values[(plane*2u)*880u+at],b=directions.values[(plane*2u+1u)*880u+at];
    bool valid=a.z!=0 && b.z!=0 && any(notEqual(a.xy,ivec2(0))) &&
        any(notEqual(b.xy,ivec2(0))) && all(lessThanEqual(abs(a.xy+b.xy),ivec2(1)));
    motion.values[plane*880u+at]=valid?ivec4(a.xy,b.xy):ivec4(0);
    if(valid)any_valid=1u;
    }
    accepted[tid]=any_valid;barrier();
    for(uint step=32u;step>0u;step/=2u){if(tid<step)accepted[tid]|=accepted[tid+step];barrier();}
    if(tid==0u)motion.values[3520u+plane]=ivec4(int(accepted[0]),0,0,0);
}
