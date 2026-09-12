#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec4 in_color;
layout(set = 1, binding = 0, std140) uniform ModelView {
    mat4 matrix;
    vec4 viewport; // Same private layout as the legacy model experiment.
};
layout(location = 0) out vec4 vertex_color;

void main() {
    precise vec4 clip = matrix[1] * in_position.y;
    clip = fma(matrix[0], vec4(in_position.x), clip);
    clip = fma(matrix[2], vec4(in_position.z), clip);
    clip = clip + matrix[3];
    // Scene math uses -W..W depth; SDL GPU uses 0..W. Never divide here:
    // vertices behind the eye remain finite inputs to hardware clipping.
    precise float depth = clip.z * 0.5 + clip.w * 0.5;
    gl_Position = vec4(clip.xy, depth, clip.w);
    // Carry affine attributes through ordinary perspective interpolation.
    // This remains finite through hardware clip intersections at W=0, where
    // native noperspective interpolation can lose even constant colors.
    vertex_color = in_color * clip.w;
}
