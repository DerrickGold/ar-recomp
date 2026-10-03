// Shared math for the native effect compute stages. No resource declarations.
vec2 motion_offset(int slot) {
    if (slot < 0 || (uint(settings.clock.y) & (1u << uint(slot))) == 0u || motion.values[slot*2+1].x == 0)
        return vec2(0);
    vec4 m = vec4(motion.values[slot*2]);
    return settings.clock.x < .5 ? m.zw + m.xy * settings.clock.x : m.zw * (1-settings.clock.x);
}
float sky_shift(EffectPlane p) {
    if (p.sky.w == 0) return 0;
    float dy = motion_offset(int(p.offset.z)).y;
    return clamp(settings.follow.w + (settings.follow.z + dy), settings.follow.x, settings.follow.y) -
           clamp(settings.follow.w + settings.follow.z, settings.follow.x, settings.follow.y);
}
SkyBand sky_band(int index) {
    SkyBand band = settings.sky_bands[index];
    if(settings.sky_mapping.meta.x>0) {
        vec4 uv; vec2 rows;
        bool valid=resolve_sky_band(settings.sky_mapping,index,
            motion_offset(int(settings.planes[2].offset.z)).y,uv,rows);
        band.bounds=valid ? uv*settings.sky_mapping.meta.yzyz-settings.sky_mapping.output_size.zwzw : vec4(0);
        band.rows.xy=rows;
    } else band.bounds.yw += vec2(sky_shift(settings.planes[2]));
    return band;
}
int sky_band_at(float y) {
    int selected = 0;
    float nearest = 1e30;
    for (int i=0;i<int(settings.sky_meta.x);++i) {
        SkyBand b=sky_band(i);
        if(b.rows.y<=b.rows.x)continue;
        float d=max(0,max(b.bounds.y-y,y-b.bounds.w));
        if(d<nearest){nearest=d;selected=i;}
    }
    return selected;
}
vec4 sky_bounds(float anchor, bool anchored) {
    int first=settings.sky_meta.y<0?0:int(settings.sky_meta.y);
    int end=settings.sky_meta.y<0?int(settings.sky_meta.x):first+1;
    vec4 bounds=vec4(1e30,1e30,-1e30,-1e30);
    vec2 rows=vec2(1,-1);
    for(int i=first;i<end;++i) {
        SkyBand b=sky_band(i);
        if(b.rows.y<=b.rows.x)continue;
        bounds=vec4(min(bounds.xy,b.bounds.xy),max(bounds.zw,b.bounds.zw));
        rows=vec2(min(rows.x,b.rows.x),max(rows.y,b.rows.y));
    }
    if(anchored) {
        SkyBand b=sky_band(sky_band_at(anchor));
        if(b.rows.y<=b.rows.x)return bounds;
        float height=(b.bounds.w-b.bounds.y)/(b.rows.y-b.rows.x);
        bounds=vec4(b.bounds.x,b.bounds.y+(rows.x-b.rows.x)*height,
                    b.bounds.z,b.bounds.y+(rows.y-b.rows.x)*height);
    }
    return bounds;
}
vec4 source_bounds(int plane) {
    EffectPlane p = settings.planes[plane];
    vec4 bounds = p.sky.z != 0 ? sky_bounds(0,false) : p.bounds;
    return bounds - motion_offset(int(p.offset.z)).xyxy;
}
vec4 effect_bounds(int plane, vec2 origin, uint flags, bool clipping) {
    EffectPlane p=settings.planes[plane];
    if(!clipping && (flags&5u)!=0u)return vec4(-1e9,-1e9,1e9,1e9);
    vec2 motion=motion_offset(int(p.offset.z));
    vec4 bounds=p.sky.z!=0?sky_bounds(origin.y+motion.y,(flags&2u)!=0u):p.bounds;
    if(!clipping && p.sky.z==0 && p.fold.x!=0)bounds.w=1e9;
    return bounds-motion.xyxy-origin.xyxy;
}
bool project_effect(int plane, vec2 source, float anchor, bool anchored, out vec2 pixel) {
    EffectPlane p = settings.planes[plane];
    if (p.offset.w == 0) return false;
    vec2 motion=motion_offset(int(p.offset.z));
    source += motion;
    if (p.sky.z == 1) {
        int selected=anchored?sky_band_at(anchor+motion.y):
            (settings.sky_meta.y<0?sky_band_at(source.y):int(settings.sky_meta.y));
        SkyBand band=sky_band(selected);
        if(band.rows.y<=band.rows.x)return false;
        vec2 q=(source-band.bounds.xy)/(band.bounds.zw-band.bounds.xy);
        pixel=settings.viewport.xy+vec2(q.x,band.rows.x+q.y*(band.rows.y-band.rows.x))*settings.viewport.zw;
        return true;
    }
    source += p.offset.xy;
    vec2 uv = (source + vec2(settings.capture.z,0))/settings.capture.xy;
    vec2 st = (uv-p.uv.xy)/(p.uv.zw-p.uv.xy);
    float t = clamp(st.y,0,1);
    float z = p.shape.x + p.shape.y*t + p.shape.z*t*t;
    precise float wx = (st.x-.5)*settings.capture.w;
    precise float wy = (.5-st.y)*settings.geometry.x;
    if(p.fold.x!=0 && st.y>p.fold.y && p.fold.z>0) {
        float ft=clamp((st.y-p.fold.y)*settings.geometry.x/p.fold.z,0,1);
        float overlap=clamp(p.fold.w,0,1),y_top=(.5-p.fold.y)*settings.geometry.x;
        float tt=clamp(p.fold.y,0,1),z_top=p.shape.x+p.shape.y*tt+p.shape.z*tt*tt;
        float linear_y=y_top-p.fold.z*ft;
        if(overlap>=1 || ft<=overlap) {
            float q=overlap>0?ft/overlap:0;
            wy=linear_y; z=z_top+(p.front.x-z_top)*q;
        } else {
            float u=(ft-overlap)/(1-overlap),bend=u*u*(3-2*u);
            float front_y=y_top-p.fold.z*overlap-p.front.z*u;
            wy=linear_y+(front_y-linear_y)*bend;
            z=p.front.x+(p.front.y-p.front.x)*bend;
        }
    }
    wy+=p.shape.w;
    precise vec4 c = settings.matrix[0]*wx + settings.matrix[1]*wy + settings.matrix[2]*z + settings.matrix[3];
    if (c.w <= .0001) return false;
    pixel = settings.viewport.xy + vec2(c.x/c.w*.5+.5,1-(c.y/c.w*.5+.5))*settings.viewport.zw;
    return true;
}

bool project_point(int plane, vec2 source, out vec2 pixel) {
    return project_effect(plane,source,0,false,pixel);
}
