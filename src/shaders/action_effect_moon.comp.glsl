#version 450
#extension GL_GOOGLE_include_directive : require
layout(local_size_x = 64) in;
struct LightJob {
    vec4 meta, light, receiver, clip, selection, transport, surface;
    vec4 rays[6], sources[9], points[2193];
};
layout(std430,set=0,binding=0) readonly buffer Job { LightJob data; } job;
layout(std430,set=0,binding=1) readonly buffer Motion { ivec4 values[]; } motion;
layout(std430,set=0,binding=2) readonly buffer Occluders { vec4 values[]; } occluders;
layout(std430,set=1,binding=0) buffer Workspace { vec4 values[]; } workspace;
layout(std430,set=1,binding=1) buffer Coverage { uint values[]; } mask;
layout(std430,set=1,binding=2) buffer Lighting { float values[]; } lighting;
#include "action_effect_context.glsl"
#include "action_effect_projection.glsl"

shared vec4 limits[64];
shared vec2 origin;
shared float scale;
shared uint valid;
float cross2(vec2 a,vec2 b){return a.x*b.y-a.y*b.x;}
bool mapped(vec4 mapping,vec2 local,out vec2 point) {
    return project_effect(int(mapping.z),mapping.xy+local,mapping.y,(uint(mapping.w)&2u)!=0u,point);
}
float falloff(float v){float t=max(0,1-v*v);return t*t;}
float exposure(vec2 point) {
    vec4 p=workspace.values[2],basis=workspace.values[3];
    vec2 direction=point-p.xy;
    float down=cross2(basis.xy,direction);
    if(abs(down)<=.0001 || down*p.z<=0)return 0;
    float slope=cross2(direction,basis.zw)/down,light=0;
    for(int i=0;i<6;++i) {
        vec3 ray=job.data.rays[i].xyz;
        light+=ray.z*falloff((slope-ray.x)/ray.y);
    }
    return light*falloff(slope/job.data.meta.z);
}
float coverage(vec2 point) {
    vec2 q=point-.5;
    if(any(lessThan(q,vec2(0)))||q.x>=799||q.y>=223)return 0;
    uvec2 ij=uvec2(q);vec2 f=q-vec2(ij);uint at=ij.y*800+ij.x;
    vec4 p=vec4(min(mask.values[at],255u),min(mask.values[at+1],255u),
                min(mask.values[at+800],255u),min(mask.values[at+801],255u));
    float a=p.x+(p.y-p.x)*f.x,b=p.z+(p.w-p.z)*f.x;
    return (a+(b-a)*f.y)/255;
}
void main() {
    uint id=gl_GlobalInvocationID.x, lane=gl_LocalInvocationID.x;
    int kind=int(job.data.meta.y),count=int(job.data.meta.x);
    if(settings.clock.w==0) {
        if(kind!=3 && id<800*224)mask.values[id]=0;
        if(gl_WorkGroupID.x!=0)return;
        if(lane==0) {
            valid=1;vec2 below,rays[3];
            if(!mapped(job.data.light,vec2(0),origin)||!mapped(job.data.light,vec2(0,20),below))valid=0;
            scale=length(below-origin)/20;
            if(scale<.0001)valid=0;
            for(int i=0;i<3;++i) {
                vec2 p;
                if(!mapped(job.data.light,vec2((i-1)*256,128),p))valid=0;
                rays[i]=p-origin;
            }
            float determinant=cross2(rays[2],rays[0]);
            float ratio=abs(determinant)>=.0001?2*cross2(rays[1],rays[0])/determinant:0;
            vec2 axis=(ratio*rays[2]-rays[1])*.5;
            workspace.values[2]=vec4(origin,cross2(axis,rays[1]),abs(determinant)>=.0001?1:0);
            workspace.values[3]=vec4(axis,rays[1]);
        }
        barrier();
        vec4 bounds=vec4(0);
        if(lane<9) {
            vec2 point;
            if(!mapped(job.data.light,job.data.sources[lane].xy,point))atomicAnd(valid,0u);
            point=(point-origin)/scale;
            workspace.values[4+lane]=vec4(point,0,0);
            bounds=vec4(min(bounds.xy,point),max(bounds.zw,point));
        }
        vec4 clip=effect_bounds(int(job.data.receiver.z),job.data.receiver.xy,
                                uint(job.data.receiver.w),true);
        clip=vec4(max(clip.xy,job.data.clip.xy),min(clip.zw,job.data.clip.zw));
        for(uint at=lane;at<uint(count);at+=64) {
            vec2 local=job.data.points[at].xy,point;
            bool selected_point=true;
            if(kind==1)selected_point=local.x>=floor((clip.x+job.data.selection.x)/4)*4-job.data.selection.x &&
                local.x<=ceil((clip.z+job.data.selection.x)/4)*4-job.data.selection.x;
            if(kind==2)selected_point=local.x>=clip.x-8 && local.x<=clip.z+8 && local.y>=clip.y-1 && local.y<=clip.w;
            if(!mapped(job.data.receiver,local,point))atomicAnd(valid,0u);
            workspace.values[16+at]=vec4(point,selected_point?1:0,0);
            if(selected_point){point=(point-origin)/scale;bounds=vec4(min(bounds.xy,point),max(bounds.zw,point));}
        }
        limits[lane]=bounds;
        barrier();
        for(uint stride=32;stride>0;stride/=2) {
            if(lane<stride)limits[lane]=vec4(min(limits[lane].xy,limits[lane+stride].xy),
                                           max(limits[lane].zw,limits[lane+stride].zw));
            barrier();
        }
        if(lane==0) {
            vec2 start=floor(limits[0].xy)-8;
            float step=max(1,max((limits[0].z-start.x+8)/800,(limits[0].w-start.y+8)/224));
            workspace.values[0]=vec4(origin,scale,step);
            workspace.values[1]=vec4(start,valid!=0?1:0,0);
        }
        return;
    }
    vec4 frame=workspace.values[0],field=workspace.values[1];
    if(field.z==0) {
        if(settings.clock.w==2 && id<uint(count))lighting.values[uint(settings.moon.x)+id]=0;
        return;
    }
    if(settings.clock.w==1) {
        uint quad=gl_WorkGroupID.x;
        if(quad>=uint(settings.moon.y))return;
        vec4 r=occluders.values[quad],bounds=source_bounds(1);
        r=vec4(max(r.xy,bounds.xy),min(r.zw,bounds.zw));
        if(any(greaterThanEqual(r.xy,r.zw)))return;
        vec2 q[4];
        if(!project_point(1,r.xy,q[0])||!project_point(1,r.zy,q[1])||!project_point(1,r.zw,q[2])||!project_point(1,r.xw,q[3]))return;
        float top=224,bottom=0;
        for(int j=0;j<4;++j) {
            q[j]=((q[j]-frame.xy)/frame.z-field.xy)/frame.w;
            top=min(top,q[j].y);bottom=max(bottom,q[j].y);
        }
        for(int y=int(floor(max(0,top)))+int(lane);y<int(ceil(min(224,bottom)));y+=64) {
            for(int tap=0;tap<2;++tap) {
                float scan=y+.25+.5*tap,left=800,right=0;int hits=0;
                for(int e=0;e<4;++e) {
                    vec2 a=q[e],b=q[(e+1)&3];
                    if((a.y<=scan&&b.y>scan)||(b.y<=scan&&a.y>scan)) {
                        float x=a.x+(b.x-a.x)*(scan-a.y)/(b.y-a.y);
                        left=min(left,x);right=max(right,x);hits++;
                    }
                }
                if(hits!=2||left>=right)continue;
                for(int x=int(floor(max(0,left)));x<int(ceil(min(800,right)));++x)
                    atomicAdd(mask.values[y*800+x],uint((min(x+1,right)-max(x,left))*127.5+.5));
            }
        }
        return;
    }
    if(id>=uint(count))return;
    vec4 receiver_sample=workspace.values[16+id];
    vec2 point=((receiver_sample.xy-frame.xy)/frame.z-field.xy)/frame.w;
    float blocked=0;
    if(kind!=3)for(int s=0;s<9;++s) {
        vec2 light=(workspace.values[4+s].xy-field.xy)/frame.w;
        if(kind==0)for(int depth=0;depth<3;++depth)
            blocked+=coverage(light+(point-light)*job.data.transport[depth])*(1.0/27);
        else blocked+=coverage(light*job.data.surface.x+point*job.data.surface.y)/9;
    }
    float result=kind==0?1-blocked:exposure(receiver_sample.xy)*max(0,1-blocked);
    lighting.values[uint(settings.moon.x)+id]=receiver_sample.z!=0?result:0;
}
