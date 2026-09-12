#version 450
layout(set = 2, binding = 0) uniform sampler2D atlas_texture;
layout(set = 3, binding = 0, std140) uniform BodySample {
    vec4 rotation_unused;
    vec4 offset_extent;
    vec4 atlas;
    vec4 tint;
};
layout(location = 0) in vec3 direction;
layout(location = 1) in float opacity;
layout(location = 0) out vec4 output_color;
const float pi = 3.14159265358979323846;
void main() {
    vec3 n = dot(direction,direction) > 0.0 ? normalize(direction) : vec3(0.0,1.0,0.0);
    float longitude = n.x == 0.0 && n.z == 0.0 ? 0.0 : atan(n.z,n.x);
    vec2 uv = vec2((longitude + pi) / (2.0*pi), acos(clamp(n.y,-1.0,1.0)) / pi) + offset_extent.xy;
    uv = vec2(fract(uv.x), clamp(uv.y,0.0,1.0));
    uv = (atlas.xy + uv * (atlas.zw-1.0) + 0.5) / offset_extent.zw;
    output_color = texture(atlas_texture,uv) * tint;
    output_color.a *= opacity * gl_FragCoord.w;
    if (output_color.a <= 0.5/255.0) discard;
}
