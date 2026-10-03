#version 450
#extension GL_GOOGLE_include_directive : require
layout(local_size_x = 64) in;


layout(std430,set=0,binding=0) readonly buffer Occluders { vec4 values[]; } occluders;
layout(std430,set=0,binding=1) readonly buffer Motion { ivec4 values[]; } motion;
layout(std430,set=1,binding=0) buffer Shadows { uint values[]; } shadows;

#include "action_effect_context.glsl"

#include "action_effect_projection.glsl"
void main() {
    uint id=gl_GlobalInvocationID.x;
    // Initial dispatch clears coverage and computes its exact projected frame.
    if(settings.clock.w==0) {
        if(id<400*176)shadows.values[4+id]=0;
        if(id!=0)return;
        vec4 r=occluders.values[0];
        vec2 p[4];
        bool valid=project_point(1,r.xy,p[0])&&project_point(1,r.zy,p[1])&&project_point(1,r.zw,p[2])&&project_point(1,r.xw,p[3]);
        if(!valid) {
            shadows.values[0]=0; shadows.values[1]=0; shadows.values[2]=0;
            return;
        }
        vec2 lo=min(min(p[0],p[1]),min(p[2],p[3])),hi=max(max(p[0],p[1]),max(p[2],p[3]));
        vec2 start=floor(lo)-2;
        float step=valid?max(.01,max((hi.x-start.x+2)/400,(hi.y-start.y+2)/176)):0;
        shadows.values[0]=floatBitsToUint(start.x);shadows.values[1]=floatBitsToUint(start.y);shadows.values[2]=floatBitsToUint(step);
        return;
    }
    // A workgroup owns one quad; positive integer coverage additions commute.
    uint quad=gl_WorkGroupID.x;
    if(quad>=uint(settings.clock.z))return;
    vec4 r=occluders.values[quad+1],bounds=source_bounds(1);
    r=vec4(max(r.xy,bounds.xy),min(r.zw,bounds.zw));
    if(any(greaterThanEqual(r.xy,r.zw)))return;
    vec2 q[4];
    if(!project_point(1,r.xy,q[0])||!project_point(1,r.zy,q[1])||!project_point(1,r.zw,q[2])||!project_point(1,r.xw,q[3]))return;
    vec3 frame=vec3(uintBitsToFloat(shadows.values[0]),uintBitsToFloat(shadows.values[1]),uintBitsToFloat(shadows.values[2]));
    if(frame.z<=0)return;
    float top=176,bottom=0;
    for(int j=0;j<4;j++){q[j]=(q[j]-frame.xy)/frame.z;top=min(top,q[j].y);bottom=max(bottom,q[j].y);}
    for(int y=int(floor(max(0,top)))+int(gl_LocalInvocationID.x);y<int(ceil(min(176,bottom)));y+=64) {
        for(int tap=0;tap<2;tap++) {
            float scan=y+.25+.5*tap,left=400,right=0;int hits=0;
            for(int e=0;e<4;e++) {
                vec2 a=q[e],b=q[(e+1)&3];
                if((a.y<=scan&&b.y>scan)||(b.y<=scan&&a.y>scan)) {
                    float x=a.x+(b.x-a.x)*(scan-a.y)/(b.y-a.y);
                    left=min(left,x);right=max(right,x);hits++;
                }
            }
            if(hits!=2||left>=right)continue;
            for(int x=int(floor(max(0,left)));x<int(ceil(min(400,right)));x++)
                atomicAdd(shadows.values[4+y*400+x],uint(127.5*(min(x+1,right)-max(x,left))+.5));
        }
    }
}
