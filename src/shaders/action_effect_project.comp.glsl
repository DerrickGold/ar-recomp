#version 450
#extension GL_GOOGLE_include_directive : require
layout(local_size_x = 64) in;


struct Primitive { uvec4 meta; vec4 origin; vec4 clip; vec4 points[6]; vec4 colors[3]; vec4 extra; };
layout(std430,set=0,binding=0) readonly buffer Primitives { Primitive values[]; } primitives;
layout(std430,set=0,binding=1) readonly buffer Motion { ivec4 values[]; } motion;
layout(std430,set=0,binding=2) readonly buffer Shadows { uint values[]; } shadows;
layout(std430,set=0,binding=3) readonly buffer Lighting { float values[]; } lighting;
// Screen-space output, consumed directly by action_effect_draw.vert.glsl.
layout(std430,set=1,binding=0) writeonly buffer Vertices { vec4 values[]; } vertices;

#include "action_effect_context.glsl"

#include "action_effect_projection.glsl"
struct ClipVertex { vec2 point; vec4 color; };
float distance_to(vec2 p,vec4 b,int edge) {
    if(edge==0)return p.x-b.x;
    if(edge==1)return b.z-p.x;
    if(edge==2)return p.y-b.y;
    return b.w-p.y;
}
ClipVertex intersection(ClipVertex a,ClipVertex b,vec4 bounds,int edge) {
    if(a.point.x>b.point.x || (a.point.x==b.point.x && a.point.y>b.point.y)) {
        ClipVertex swap=a;a=b;b=swap;
    }
    float da=distance_to(a.point,bounds,edge),db=distance_to(b.point,bounds,edge);
    float t=da/(da-db);
    ClipVertex r=ClipVertex(a.point+(b.point-a.point)*t,a.color+(b.color-a.color)*t);
    if(edge==0)r.point.x=bounds.x;
    if(edge==1)r.point.x=bounds.z;
    if(edge==2)r.point.y=bounds.y;
    if(edge==3)r.point.y=bounds.w;
    return r;
}
void push_vertex(inout ClipVertex list[8],inout int count,ClipVertex v) {
    if(count>0 && all(equal(list[count-1].point,v.point)))return;
    if(count<8)list[count++]=v;
}
float coverage(vec2 point) {
    vec3 frame=vec3(uintBitsToFloat(shadows.values[0]),uintBitsToFloat(shadows.values[1]),uintBitsToFloat(shadows.values[2]));
    if(frame.z<=0)return 0;
    vec2 q=(point-frame.xy)/frame.z-.5;
    if(any(lessThan(q,vec2(0)))||q.x>=399||q.y>=175)return 0;
    uvec2 ij=uvec2(q);vec2 f=q-vec2(ij);uint at=4+ij.y*400+ij.x;
    vec4 p=vec4(min(shadows.values[at],255u),min(shadows.values[at+1],255u),
                min(shadows.values[at+400],255u),min(shadows.values[at+401],255u));
    return mix(mix(p.x,p.y,f.x),mix(p.z,p.w,f.x),f.y)/255;
}
void emit_vertex(uint at,vec2 point,vec4 color) {
    vertices.values[at*2]=vec4(point,0,0);
    vertices.values[at*2+1]=color;
}
bool local_point(Primitive p, vec4 bounds, vec2 point, out vec2 screen) {
    uint flags=uint(p.origin.z);
    if((flags&4u)==0u && (any(lessThan(point,p.clip.xy)) || any(greaterThan(point,p.clip.zw))))return false;
    if((flags&5u)==0u) {
        EffectPlane plane=settings.planes[p.meta.y];
        if(plane.sky.z!=0) {
            if(any(lessThan(point,bounds.xy)) || any(greaterThan(point,bounds.zw)))return false;
        } else {
            // Match the reference's normalized point predicate. Multiplying a
            // UV edge back into pixels can round inward and drop a whole glow
            // exactly at that edge (e.g. 64/640 == .1 in Marahna's torch room).
            precise vec2 source=p.origin.xy+point+motion_offset(int(plane.offset.z))+plane.offset.xy;
            vec2 uv=vec2(rounded_divide(source.x+settings.capture.z,settings.capture.x),
                         rounded_divide(source.y,settings.capture.y));
            if(any(lessThan(uv,min(plane.uv.xy,plane.uv.zw))) ||
               uv.x>max(plane.uv.x,plane.uv.z) ||
               (uv.y>max(plane.uv.y,plane.uv.w) && plane.fold.x==0))return false;
        }
    }
    return project_effect(int(p.meta.y), p.origin.xy+point, p.origin.y, (flags&2u)!=0u, screen);
}
bool local_scale(Primitive p, vec4 bounds, vec2 local, out vec2 point, out float scale) {
    vec2 x,y;
    if(!local_point(p,bounds,local,point) || !local_point(p,bounds,local+vec2(1,0),x) ||
       !local_point(p,bounds,local+vec2(0,1),y))return false;
    vec2 sizes=vec2(length(x-point),length(y-point));
    scale=max(.5,(sizes.x+sizes.y)*.5);
    return all(greaterThan(sizes,vec2(0)));
}
void main() {
    uint id=gl_GlobalInvocationID.x;
    if(id>=uint(settings.geometry.w))return;
    Primitive p=primitives.values[id];
    uint base=id*15;
    for(uint i=0;i<15;i++)emit_vertex(base+i,vec2(0),vec4(0));
    int plane=int(p.meta.y);
    vec4 bounds=effect_bounds(plane,p.origin.xy,uint(p.origin.z),(p.meta.x==0u || p.meta.x==8u || p.meta.x==9u));
    bounds=vec4(max(bounds.xy,p.clip.xy),min(bounds.zw,p.clip.zw));
    if(any(greaterThanEqual(bounds.xy,bounds.zw)))return;
    if((uint(p.origin.z)&8u)!=0u) {
        float world=p.points[5].x,cell=p.points[5].y,step=p.points[5].z;
        float first=(floor((bounds.x+world)/step)-1)*step;
        if(cell<first || cell>=ceil(bounds.z+world))return;
    }
    if((p.meta.x==0u || p.meta.x==8u)) {
        ClipVertex a[8],b[8];int n=3;
        for(int j=0;j<3;j++) {
            a[j]=ClipVertex(p.points[j].xy,p.colors[j]);
            if(p.meta.x==8u) {
                float light=p.points[j].z>0?lighting.values[uint(p.points[j].z)-1]:0;
                a[j].color.a=min(p.extra.x,p.points[j].w+p.colors[j].a*light);
            }
        }
        bool clipping=false;
        for(int j=0;j<3;j++)clipping=clipping||any(lessThan(a[j].point,bounds.xy))||any(greaterThan(a[j].point,bounds.zw));
        for(int edge=0;clipping&&edge<4&&n>0;edge++) {
            int next=0;
            for(int j=0;j<n;j++) {
                ClipVertex x=a[j],y=a[(j+1)%n];
                bool xi=distance_to(x.point,bounds,edge)>=0,yi=distance_to(y.point,bounds,edge)>=0;
                if(xi!=yi)push_vertex(b,next,intersection(x,y,bounds,edge));
                if(yi)push_vertex(b,next,y);
            }
            if(next>1&&all(equal(b[0].point,b[next-1].point)))next--;
            n=next;for(int j=0;j<n;j++)a[j]=b[j];
        }
        if(n<3||n>7)return;
        vec2 screen[8];
        for(int j=0;j<n;j++)if(!project_effect(plane,p.origin.xy+a[j].point,p.origin.y,(uint(p.origin.z)&2u)!=0u,screen[j]))return;
        if(p.meta.w!=0u&&settings.clock.z>0) {
            vec2 light;
            if(project_effect(plane,p.origin.xy+p.extra.xy,p.origin.y,(uint(p.origin.z)&2u)!=0u,light))for(int j=0;j<n;j++) {
                float blocked=coverage(light+(screen[j]-light)*.74)+coverage(light+(screen[j]-light)*.85)+coverage(light+(screen[j]-light)*.96);
                if(p.meta.w==1u)a[j].color.rgb*=1-.86*blocked/3;
                else a[j].color.a*=1-.86*blocked/3;
            }
        }
        for(int j=1;j<n-1;j++) {
            emit_vertex(base+uint((j-1)*3),screen[0],a[0].color);
            emit_vertex(base+uint((j-1)*3+1),screen[j],a[j].color);
            emit_vertex(base+uint((j-1)*3+2),screen[j+1],a[j+1].color);
        }
    } else if(p.meta.x==1u || p.meta.x==6u) {
        vec2 local[4]=vec2[4](p.points[0].xy,p.points[0].xy+vec2(1,0),p.points[0].xy+vec2(0,1),p.points[1].xy);
        vec2 screen[4];
        for(int j=0;j<4;j++) {
            if(!local_point(p,bounds,local[j],screen[j]))return;
        }
        vec2 sizes=vec2(length(screen[1]-screen[0]),length(screen[2]-screen[0]));
        if(any(lessThanEqual(sizes,vec2(0))))return;
        float scale=max(.5,(sizes.x+sizes.y)*.5);
        vec2 heading=screen[0]-screen[3];float len=length(heading);
        heading=len<.001?vec2(0,-1):heading/len;
        float width=p.meta.x==6u?max(.4,p.extra.x*scale):p.extra.x*scale;
        float reach=p.meta.x==6u?width*p.extra.y:p.extra.y*scale;
        vec2 wide=vec2(-heading.y,heading.x)*width,longer=heading*reach;
        vec2 points[4]=vec2[4](screen[0]+longer,screen[0]+wide,screen[0]-longer,screen[0]-wide);
        const int ix[6]=int[6](0,1,2,0,2,3);
        for(uint j=0;j<6;j++)emit_vertex(base+j,points[ix[j]],p.colors[0]);
    } else if(p.meta.x==2u) {
        vec2 points[6];
        for(int j=0;j<6;j++) {
            if(!local_point(p,bounds,p.points[j].xy,points[j]))return;
        }
        for(int j=1;j<5;j++) {
            emit_vertex(base+uint((j-1)*3),points[0],p.colors[0]);
            emit_vertex(base+uint((j-1)*3+1),points[j],p.colors[0]);
            emit_vertex(base+uint((j-1)*3+2),points[j+1],p.colors[0]);
        }
        emit_vertex(base+12,points[1],p.colors[1]);emit_vertex(base+13,points[2],p.colors[1]);
        emit_vertex(base+14,points[1]*.72+points[5]*.28,p.colors[1]);
    } else if(p.meta.x==3u) {
        vec2 points[4];
        for(int j=0;j<4;j++)if(!local_point(p,bounds,p.points[j].xy,points[j]))return;
        const int ix[6]=int[6](0,1,2,0,2,3);
        for(uint j=0;j<6;j++) {
            int at=ix[j];
            emit_vertex(base+j,points[at],at<3?p.colors[at]:p.extra);
        }
    } else if(p.meta.x==4u || p.meta.x==5u) {
        vec2 centre,sx,sy;
        if(!local_point(p,bounds,p.points[0].xy,centre) ||
           !local_point(p,bounds,p.points[0].xy+vec2(1,0),sx) ||
           !local_point(p,bounds,p.points[0].xy+vec2(0,1),sy))return;
        vec2 lengths=vec2(length(sx-centre),length(sy-centre));
        if(any(lessThanEqual(lengths,vec2(0))))return;
        if(p.meta.x==5u) {
            vec2 radius=max(p.points[1].zw,p.points[1].xy*lengths);
            for(uint j=0;j<3;j++) {
                vec4 c=p.points[2+j];
                emit_vertex(base+j,centre+vec2(dot(c.xy,radius),dot(c.zw,radius)),p.colors[j]);
            }
        } else {
            if(any(lessThan(lengths,vec2(.001))))return;
            vec2 x=(sx-centre)/lengths.x,y=(sy-centre)/lengths.y;
            vec2 longer=p.extra.x*lengths*vec2(1,1.35);
            vec2 thin=max(vec2(.45),p.extra.x*.23*lengths);
            vec2 points[8]=vec2[8](centre+x*longer.x,centre+y*thin.y,
                centre-x*longer.x,centre-y*thin.y,centre+y*longer.y,
                centre+x*thin.x,centre-y*longer.y,centre-x*thin.x);
            const int ix[12]=int[12](0,1,2,0,2,3,4,5,6,4,6,7);
            for(uint j=0;j<12;j++)emit_vertex(base+j,points[ix[j]],p.colors[0]);
        }
    } else if(p.meta.x==7u) {
        // Reference ribbons reject the whole path if a joint cannot be projected.
        // Paths are capped at 64 joints and all packets for a path are contiguous.
        vec2 a,b; float sa,sb;
        for(uint j=uint(p.extra.z);j<uint(p.extra.z+p.extra.w);++j) {
            Primitive joint=primitives.values[j]; vec2 q; float scale;
            if(!local_scale(joint,bounds,joint.points[0].xy,q,scale))return;
            if(j+1==uint(p.extra.z+p.extra.w) && !local_scale(joint,bounds,joint.points[1].xy,q,scale))return;
        }
        if(!local_scale(p,bounds,p.points[0].xy,a,sa) || !local_scale(p,bounds,p.points[1].xy,b,sb))return;
        vec2 direction=b-a;float len=length(direction);if(len<.001)return;
        vec2 normal=vec2(-direction.y,direction.x)/len;
        vec2 points[4]=vec2[4](a+normal*(p.extra.x*sa),a-normal*(p.extra.x*sa),
            b-normal*(p.extra.y*sb),b+normal*(p.extra.y*sb));
        const int ix[6]=int[6](0,1,2,0,2,3);
        for(uint j=0;j<6;j++)emit_vertex(base+j,points[ix[j]],p.colors[0]);
    } else if(p.meta.x==9u) {
        for(uint j=0;j<3;j++) {
            float x=clamp(p.points[j].x,bounds.x,bounds.z),wx=x+p.points[4].y;
            float phase=p.points[3].z,seed=p.points[3].w,slice=p.points[4].x;
            float billow=.73+.16*sin(wx*.053+phase+seed+slice)+.11*cos(wx*.097-phase+slice);
            float top=p.points[3].y*(.9-.15*slice)*billow;
            float y=clamp(p.points[3].x-top*(1-p.points[j].y*.5),bounds.y,bounds.w);
            float rise=clamp((p.points[3].x-y)/top,0,1);
            float edge=clamp(min(x-p.points[5].x,p.points[5].y-x)/12,0,1);
            vec4 color=p.colors[0];
            color.a=min(1,p.points[4].z*(.14+.08*billow)*(1-rise)*(1-rise)*edge);
            vec2 point;
            if(!project_effect(plane,p.origin.xy+vec2(x,y),p.origin.y,(uint(p.origin.z)&2u)!=0u,point))return;
            emit_vertex(base+j,point,color);
        }
    }
}
