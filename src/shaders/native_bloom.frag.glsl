#version 450
layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(set = 2, binding = 0) uniform sampler2D u_texture;
layout(set = 3, binding = 0) uniform Context {
    vec4 filter_info; // normalized step XY, extraction threshold, mode
    vec4 grade_info;  // saturation, contrast, glow gain
};
vec3 grade(vec3 c) {
    float l=dot(c,vec3(.2126,.7152,.0722));
    c=max(vec3(0),mix(vec3(l),c,grade_info.x));
    // Black remains black; a shoulder protects bright native star cores.
    c=clamp(c,0.0,1.0);
    return clamp(c+(c*c-c)*(grade_info.y-1.0)*2.0,0.0,1.0);
}
vec3 sample_light(vec2 uv) {
    // Empty space beyond the projected viewport cannot emit a clamped edge.
    if(any(lessThan(uv,vec2(0))) || any(greaterThan(uv,vec2(1)))) return vec3(0);
    vec4 c=textureLod(u_texture,uv,0.0);
    if(filter_info.w>.5) return c.rgb;
    vec3 g=grade(c.rgb);
    float peak=max(g.r,max(g.g,g.b));
    float gate=smoothstep(filter_info.z,min(1.0,filter_info.z+.25),peak);
    return g*c.a*gate;
}
void main() {
    if(filter_info.w>1.5) {
        vec4 c=texture(u_texture,v_uv);
        // Glow intensity can exceed one; normalized vertex/texture tints
        // cannot express it. Keep receiver radiance independent of this gain.
        o_color=vec4(filter_info.w>2.5 ? c.rgb*grade_info.z : grade(c.rgb),c.a)*v_color;
        return;
    }
    // Separable, normalized Gaussian. Both passes run on a bounded small
    // target; the untouched original art is composed at output resolution.
    vec2 d=filter_info.xy;
    vec3 c=sample_light(v_uv)*.227027;
    c+=(sample_light(v_uv+d*1.384615)+sample_light(v_uv-d*1.384615))*.316216;
    c+=(sample_light(v_uv+d*3.230769)+sample_light(v_uv-d*3.230769))*.070270;
    o_color=vec4(c,1)*v_color;
}
