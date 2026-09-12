#version 450
layout(location = 0) in vec4 source; // unit normal.xyz, limb opacity
layout(set = 1, binding = 0, std140) uniform BodyView {
    mat4 matrix;
    vec4 basis[3];
    vec4 centre_radius;
    vec4 texture_basis[3];
    vec4 rotation;
};
layout(location = 0) out vec3 direction;
layout(location = 1) out float opacity;
void main() {
    vec3 n = source.xyz;
    precise vec3 world = vec3(dot(basis[0].xyz,n), dot(basis[1].xyz,n), dot(basis[2].xyz,n));
    world = world * centre_radius.w + centre_radius.xyz;
    precise vec4 clip = matrix[1] * world.y;
    clip = fma(matrix[0], vec4(world.x), clip);
    clip = fma(matrix[2], vec4(world.z), clip);
    clip = clip + matrix[3];
    gl_Position = vec4(clip.xy, clip.z * 0.5 + clip.w * 0.5, clip.w);
    n = vec3(dot(texture_basis[0].xyz,n), dot(texture_basis[1].xyz,n), dot(texture_basis[2].xyz,n));
    float x = n.x * rotation.x + n.z * rotation.y;
    float z = n.z * rotation.x - n.x * rotation.y;
    direction = vec3(x, n.y * rotation.z + z * rotation.w, z * rotation.z - n.y * rotation.w);
    opacity = source.w * clip.w;
}
