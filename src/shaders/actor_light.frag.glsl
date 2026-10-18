#version 450
layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(set = 2, binding = 0) uniform sampler2D u_texture;
layout(set = 2, binding = 1) uniform sampler2D u_attributes;
layout(set = 2, binding = 2) uniform sampler2D u_illumination;
layout(set = 2, binding = 3) uniform sampler2D u_enemy_light;
layout(set = 3, binding = 0) uniform Context {
    vec4 filter_info; // source texel XY, DOF radius, rim strength
    vec4 light_info; // screen pixel to illumination UV
    vec4 attribute_info; // compact artwork UV to original capture UV
    vec4 bounds; // lower content boundary
};
vec3 receiver_data(vec2 uv) {
    vec4 attributes = texture(u_attributes, attribute_info.xy + uv * attribute_info.zw);
    // Recover the coverage-weighted values at filtered transparent edges.
    return attributes.a > .00001 ? attributes.rgb / attributes.a : vec3(64.0/255.0,0,0);
}
vec4 art(out vec3 data) {
    if (filter_info.z <= 0.0) {
        data = receiver_data(v_uv);
        return texture(u_texture, v_uv);
    }
    data = vec3(0);
    if (bounds.x > 0.0 && v_uv.y >= bounds.x) return vec4(0);
    vec4 sum = vec4(0);
    for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x) {
        vec2 uv = v_uv + vec2(x,y) * filter_info.xy * filter_info.z;
        if (bounds.x > 0.0) uv.y = min(uv.y, bounds.x - .5 * filter_info.y);
        vec4 c = texture(u_texture, uv);
        float weight = x == 0 && y == 0 ? 2.0 : 1.0;
        sum += vec4(c.rgb * c.a, c.a) * weight;
        // The blurred silhouette extends beyond the original owner mask.
        // Filter its attributes with the artwork's taps and coverage too.
        data += receiver_data(uv) * c.a * weight;
    }
    data = sum.a > .00001 ? data / sum.a : vec3(0);
    return vec4(sum.a > .00001 ? sum.rgb / sum.a : vec3(0), sum.a * .1);
}
void main() {
    vec3 data;
    vec4 c=art(data);
    float ambient=data.r*(255.0/64.0);
    vec2 uv=gl_FragCoord.xy*light_info.xy+light_info.zw;
    vec3 incoming=4.0*(texture(u_illumination,uv).rgb*data.g+
        texture(u_enemy_light,uv).rgb*data.b);
    vec3 lit=c.rgb*clamp(vec3(ambient)+incoming,vec3(0),vec3(4));
    if(filter_info.w>0.0) {
        float neighbor=min(min(texture(u_texture,v_uv+vec2(filter_info.x,0)).a,
            texture(u_texture,v_uv-vec2(filter_info.x,0)).a),
            min(texture(u_texture,v_uv+vec2(0,filter_info.y)).a,
            texture(u_texture,v_uv-vec2(0,filter_info.y)).a));
        lit*=1.0+c.a*max(0.0,c.a-neighbor)*filter_info.w;
    }
    o_color=vec4(lit*v_color.rgb,c.a*v_color.a);
}
