// Mirrors DioramaSkyboxSourceMapping; shared by sky sampling and effect projection.
struct SkyMapping {
    vec4 meta; vec4 output_size; vec4 vertical; vec4 follow; vec4 fit;
    vec4 bands[11];
};
float rounded_divide(float x, float y) {
    precise float q=x/y;
    precise float residual=fma(-q,y,x);
    precise float corrected=q+residual/y;
    return corrected;
}
bool resolve_sky_band(SkyMapping m, int index, float dy, out vec4 uv, out vec2 rows) {
    precise vec4 window=m.vertical;
    if(m.fit.x>0) {
        precise float top=clamp(m.follow.w+(m.follow.z+dy),m.follow.x,m.follow.y);
        window=vec4(top,top+m.fit.x,rounded_divide(top,m.meta.z),rounded_divide(top+m.fit.x,m.meta.z));
    }
    float available=1e30;
    for(int i=0;i<int(m.meta.x);++i) {
        vec4 b=m.bands[i];
        if(b.y>b.x && b.w>window.x && b.z<window.y)
            available=min(available,(b.y-b.x)*m.meta.y);
    }
    precise float height=(window.w-window.z)*m.meta.z;
    precise float aspect=rounded_divide(m.output_size.x,m.output_size.y);
    precise float width=height*rounded_divide(aspect,m.meta.w);
    if(width>available) {
        precise float inset=.5*(1-rounded_divide(available,width));
        precise float capture_inset=(window.y-window.x)*inset;
        precise float texture_inset=(window.w-window.z)*inset;
        window+=vec4(capture_inset,-capture_inset,texture_inset,-texture_inset);
        width=available;
    }
    vec4 band=m.bands[index];
    precise float y0=max(band.z,window.x), y1=min(band.w,window.y);
    if(y1<=y0 || width<=0 || band.y<=band.x) {uv=vec4(0);rows=vec2(0);return false;}
    rows=vec2(rounded_divide(y0-window.x,window.y-window.x),rounded_divide(y1-window.x,window.y-window.x));
    precise float center=.5*(band.x+band.y);
    precise float half_width=rounded_divide(.5*width,m.meta.y);
    precise float v0=window.z+(window.w-window.z)*rows.x;
    precise float v1=window.z+(window.w-window.z)*rows.y;
    uv=vec4(center-half_width,v0,center+half_width,v1);
    return true;
}
